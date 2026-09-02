use crate::codex_history::{
    checked_result, codex_executable, observed_at, validate_thread_id, AppServer, CodexHistoryError,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};
use std::{
    collections::{BTreeMap, HashMap, HashSet},
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc, Mutex,
    },
    time::{Duration, Instant},
};

const RESPONSE_TIMEOUT: Duration = Duration::from_secs(10);
const PREVIEW_LIFETIME: Duration = Duration::from_secs(5 * 60);
const MAX_BATCH_THREADS: usize = 500;
const THREAD_PAGE_SIZE: u64 = 100;
const MODEL_PAGE_SIZE: u64 = 100;

static NEXT_PREVIEW_ID: AtomicU64 = AtomicU64::new(1);

const ALL_SOURCE_KINDS: [&str; 10] = [
    "cli",
    "vscode",
    "exec",
    "appServer",
    "subAgent",
    "subAgentReview",
    "subAgentCompact",
    "subAgentThreadSpawn",
    "subAgentOther",
    "unknown",
];

#[derive(Debug, Clone, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexGlobalSettingsRequest {
    scope: String,
    thread_ids: Vec<String>,
    reasoning_selection: String,
    speed_selection: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexRuntimeOption {
    id: String,
    label: String,
    description: Option<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexRuntimeCapabilities {
    reasoning_efforts: Vec<CodexRuntimeOption>,
    speed_tiers: Vec<CodexRuntimeOption>,
    model_count: usize,
    observed_at: i64,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexGlobalSettingsModelCount {
    model: String,
    count: usize,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexGlobalSettingsPreview {
    preview_id: String,
    request: CodexGlobalSettingsRequest,
    discovered_count: usize,
    changeable_count: usize,
    unchanged_count: usize,
    partial_count: usize,
    skipped_count: usize,
    models: Vec<CodexGlobalSettingsModelCount>,
    warnings: Vec<String>,
    expires_at: i64,
}

#[derive(Debug, Clone, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexThreadRuntimeSetting {
    thread_id: String,
    model: String,
    effort: Option<String>,
    service_tier: Option<String>,
    effort_changed: bool,
    service_tier_changed: bool,
}

#[derive(Debug, Clone, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexThreadRuntimeOverride {
    thread_id: String,
    effort_set: bool,
    effort: Option<String>,
    service_tier_set: bool,
    service_tier: Option<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexGlobalSettingsFailure {
    thread_id: String,
    message: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexGlobalSettingsReceipt {
    changed_count: usize,
    unchanged_count: usize,
    failed_count: usize,
    failures: Vec<CodexGlobalSettingsFailure>,
    previous: Vec<CodexThreadRuntimeSetting>,
    applied: Vec<CodexThreadRuntimeOverride>,
    applied_at: i64,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexGlobalSettingsRestoreReceipt {
    restored_count: usize,
    failed_count: usize,
    failures: Vec<CodexGlobalSettingsFailure>,
    remaining: Vec<CodexThreadRuntimeSetting>,
    restored: Vec<CodexThreadRuntimeOverride>,
    restored_at: i64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct SpeedTier {
    id: String,
    name: String,
    description: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct ModelCapability {
    model: String,
    display_name: String,
    efforts: Vec<String>,
    default_effort: String,
    speed_tiers: Vec<SpeedTier>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct CurrentThreadSettings {
    thread_id: String,
    model: String,
    effort: Option<String>,
    service_tier: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct SettingTarget {
    thread_id: String,
    model: String,
    before_effort: Option<String>,
    before_service_tier: Option<String>,
    effort_change: Option<Option<String>>,
    service_tier_change: Option<Option<String>>,
}

#[derive(Debug, Clone)]
struct PendingPreview {
    public: CodexGlobalSettingsPreview,
    targets: Vec<SettingTarget>,
    created: Instant,
}

#[derive(Clone, Default)]
pub(crate) struct CodexSettingsManager {
    pending: Arc<Mutex<Option<PendingPreview>>>,
}

enum Resolution {
    Keep,
    Set(Option<String>),
    Unsupported,
}

impl CodexSettingsManager {
    pub(crate) fn preview(
        &self,
        request: CodexGlobalSettingsRequest,
    ) -> Result<CodexGlobalSettingsPreview, CodexHistoryError> {
        validate_request(&request)?;
        let executable = codex_executable()?;
        let mut server = AppServer::start_streaming(&executable)?;
        let result = preview_with_server(&mut server, request);
        server.stop();
        let pending = result?;
        let public = pending.public.clone();
        let mut guard = self.pending.lock().map_err(|_| {
            CodexHistoryError::new("runtime_unavailable", "全局运行配置预览暂不可用")
        })?;
        *guard = Some(pending);
        Ok(public)
    }

    pub(crate) fn apply(
        &self,
        preview_id: String,
    ) -> Result<CodexGlobalSettingsReceipt, CodexHistoryError> {
        let pending = {
            let mut guard = self.pending.lock().map_err(|_| {
                CodexHistoryError::new("runtime_unavailable", "全局运行配置预览暂不可用")
            })?;
            let current = guard.as_ref().ok_or_else(|| {
                CodexHistoryError::new("preview_missing", "请先重新预览全局运行配置")
            })?;
            if current.public.preview_id != preview_id {
                return Err(CodexHistoryError::new(
                    "preview_changed",
                    "配置预览已经变化，请重新确认",
                ));
            }
            if current.created.elapsed() > PREVIEW_LIFETIME {
                *guard = None;
                return Err(CodexHistoryError::new(
                    "preview_expired",
                    "配置预览已超过5分钟，请重新预览",
                ));
            }
            guard.take().expect("checked pending preview")
        };
        apply_targets(pending)
    }

    pub(crate) fn restore(
        &self,
        previous: Vec<CodexThreadRuntimeSetting>,
    ) -> Result<CodexGlobalSettingsRestoreReceipt, CodexHistoryError> {
        validate_restore_settings(&previous)?;
        restore_settings(previous)
    }
}

pub(crate) fn load_capabilities() -> Result<CodexRuntimeCapabilities, CodexHistoryError> {
    let executable = codex_executable()?;
    let mut server = AppServer::start_streaming(&executable)?;
    let result = (|| {
        initialize(&mut server)?;
        let mut request_id = 2i64;
        let models = load_models(&mut server, &mut request_id)?;
        Ok(capabilities_from_models(&models))
    })();
    server.stop();
    result
}

fn initialize(server: &mut AppServer) -> Result<(), CodexHistoryError> {
    server.send(&json!({
        "id": 1,
        "method": "initialize",
        "params": {
            "clientInfo": {
                "name": "codex-monitor-task-center",
                "title": "Codex Monitor Task Center",
                "version": env!("CARGO_PKG_VERSION")
            },
            "capabilities": {
                "experimentalApi": true,
                "optOutNotificationMethods": [
                    "item/agentMessage/delta",
                    "item/started",
                    "item/completed",
                    "rawResponseItem/completed",
                    "rawResponse/completed",
                    "remoteControl/status/changed"
                ]
            }
        }
    }))?;
    let response = server.wait_for(&[1], Instant::now() + RESPONSE_TIMEOUT)?;
    checked_result(&response[0])?;
    server.send(&json!({"method": "initialized", "params": {}}))?;
    Ok(())
}

fn load_models(
    server: &mut AppServer,
    request_id: &mut i64,
) -> Result<Vec<ModelCapability>, CodexHistoryError> {
    let mut models = Vec::new();
    let mut cursor: Option<String> = None;
    loop {
        let id = take_request_id(request_id);
        server.send(&json!({
            "id": id,
            "method": "model/list",
            "params": {"cursor": cursor, "limit": MODEL_PAGE_SIZE, "includeHidden": false}
        }))?;
        let response = server.wait_for(&[id], Instant::now() + RESPONSE_TIMEOUT)?;
        let result = checked_result(&response[0])?;
        let data = result
            .get("data")
            .and_then(Value::as_array)
            .ok_or_else(|| {
                CodexHistoryError::new("protocol_changed", "Codex 模型目录格式已变化")
            })?;
        for item in data {
            let Some(model) = parse_model(item) else {
                continue;
            };
            models.push(model);
        }
        cursor = result
            .get("nextCursor")
            .and_then(Value::as_str)
            .filter(|value| value.len() <= 4096)
            .map(str::to_string);
        if cursor.is_none() || models.len() >= 200 {
            break;
        }
    }
    if models.is_empty() {
        return Err(CodexHistoryError::new(
            "capability_unavailable",
            "当前 Codex 没有返回可用的模型配置",
        ));
    }
    Ok(models)
}

fn parse_model(value: &Value) -> Option<ModelCapability> {
    let model = value.get("model").and_then(Value::as_str)?;
    if !safe_model_identifier(model) {
        return None;
    }
    let display_name = value
        .get("displayName")
        .and_then(Value::as_str)
        .filter(|name| name.len() <= 120 && !name.chars().any(char::is_control))
        .unwrap_or(model)
        .to_string();
    let efforts: Vec<String> = value
        .get("supportedReasoningEfforts")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(|option| option.get("reasoningEffort").and_then(Value::as_str))
        .filter(|effort| safe_identifier(effort))
        .map(str::to_string)
        .collect();
    let default_effort = value
        .get("defaultReasoningEffort")
        .and_then(Value::as_str)
        .filter(|effort| efforts.iter().any(|candidate| candidate == effort))?
        .to_string();
    let speed_tiers = value
        .get("serviceTiers")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(|tier| {
            let id = tier.get("id").and_then(Value::as_str)?;
            if !safe_identifier(id) {
                return None;
            }
            Some(SpeedTier {
                id: id.to_string(),
                name: bounded_plain_text(tier.get("name").and_then(Value::as_str), id, 80),
                description: bounded_plain_text(
                    tier.get("description").and_then(Value::as_str),
                    "",
                    160,
                ),
            })
        })
        .collect();
    Some(ModelCapability {
        model: model.to_string(),
        display_name,
        efforts,
        default_effort,
        speed_tiers,
    })
}

fn capabilities_from_models(models: &[ModelCapability]) -> CodexRuntimeCapabilities {
    let mut effort_ids = Vec::new();
    let mut effort_seen = HashSet::new();
    let mut speed_by_id: BTreeMap<String, SpeedTier> = BTreeMap::new();
    for model in models {
        for effort in &model.efforts {
            if effort_seen.insert(effort.clone()) {
                effort_ids.push(effort.clone());
            }
        }
        for tier in &model.speed_tiers {
            speed_by_id
                .entry(tier.id.clone())
                .or_insert_with(|| tier.clone());
        }
    }
    CodexRuntimeCapabilities {
        reasoning_efforts: effort_ids
            .into_iter()
            .map(|id| CodexRuntimeOption {
                label: effort_label(&id),
                description: effort_description(&id),
                id,
            })
            .collect(),
        speed_tiers: speed_by_id
            .into_values()
            .map(|tier| CodexRuntimeOption {
                id: tier.id,
                label: if tier.name.eq_ignore_ascii_case("fast") {
                    "快速（Fast）".to_string()
                } else {
                    tier.name
                },
                description: (!tier.description.is_empty()).then_some(tier.description),
            })
            .collect(),
        model_count: models.len(),
        observed_at: observed_at(),
    }
}

fn preview_with_server(
    server: &mut AppServer,
    request: CodexGlobalSettingsRequest,
) -> Result<PendingPreview, CodexHistoryError> {
    initialize(server)?;
    let mut request_id = 2i64;
    let models = load_models(server, &mut request_id)?;
    let model_map: HashMap<&str, &ModelCapability> = models
        .iter()
        .map(|model| (model.model.as_str(), model))
        .collect();
    let (thread_ids, truncated) = resolve_thread_ids(server, &request, &mut request_id)?;
    let discovered_count = thread_ids.len();
    let mut targets = Vec::new();
    let mut unchanged_count = 0usize;
    let mut partial_count = 0usize;
    let mut skipped_count = 0usize;
    let mut unsupported_effort = 0usize;
    let mut unsupported_speed = 0usize;
    let mut unreadable = 0usize;
    let mut model_counts: BTreeMap<String, usize> = BTreeMap::new();

    for thread_id in thread_ids {
        let current = match resume_thread(server, &thread_id, &mut request_id) {
            Ok(value) => value,
            Err(_) => {
                unreadable += 1;
                skipped_count += 1;
                continue;
            }
        };
        let model_capability = model_map.get(current.model.as_str()).copied();
        let model_label = model_capability
            .map(|model| model.display_name.clone())
            .unwrap_or_else(|| current.model.clone());
        *model_counts.entry(model_label).or_default() += 1;

        let effort = resolve_effort(
            &request.reasoning_selection,
            model_capability,
            current.effort.as_deref(),
        );
        let speed = resolve_speed(
            &request.speed_selection,
            model_capability,
            current.service_tier.as_deref(),
        );
        let effort_unsupported = matches!(effort, Resolution::Unsupported);
        let speed_unsupported = matches!(speed, Resolution::Unsupported);
        unsupported_effort += usize::from(effort_unsupported);
        unsupported_speed += usize::from(speed_unsupported);

        let effort_change = change_from_resolution(effort, current.effort.as_deref());
        let service_tier_change = change_from_resolution(speed, current.service_tier.as_deref());
        if effort_change.is_none() && service_tier_change.is_none() {
            if effort_unsupported || speed_unsupported {
                skipped_count += 1;
            } else {
                unchanged_count += 1;
            }
            continue;
        }
        if effort_unsupported || speed_unsupported {
            partial_count += 1;
        }
        targets.push(SettingTarget {
            thread_id: current.thread_id,
            model: current.model,
            before_effort: current.effort,
            before_service_tier: current.service_tier,
            effort_change,
            service_tier_change,
        });
    }

    let mut warnings = vec![
        "正在运行的回合不会改变；新设置只用于后续回合。".to_string(),
        "速度设置会在任务中心以后启动回合时再次带入；Codex 界面内直接继续仍以其当时设置为准。"
            .to_string(),
    ];
    if truncated {
        warnings.push(format!(
            "任务数量超过安全上限，本次只处理最近 {MAX_BATCH_THREADS} 个任务。"
        ));
    }
    if unsupported_effort > 0 {
        warnings.push(format!(
            "{unsupported_effort} 个任务的模型不支持所选推理强度。"
        ));
    }
    if unsupported_speed > 0 {
        warnings.push(format!("{unsupported_speed} 个任务的模型不支持所选速度。"));
    }
    if unreadable > 0 {
        warnings.push(format!("{unreadable} 个任务暂时无法读取，未纳入修改。"));
    }
    let now = observed_at();
    let preview_id = format!(
        "settings-{now}-{}",
        NEXT_PREVIEW_ID.fetch_add(1, Ordering::Relaxed)
    );
    let public = CodexGlobalSettingsPreview {
        preview_id,
        request,
        discovered_count,
        changeable_count: targets.len(),
        unchanged_count,
        partial_count,
        skipped_count,
        models: model_counts
            .into_iter()
            .map(|(model, count)| CodexGlobalSettingsModelCount { model, count })
            .collect(),
        warnings,
        expires_at: now + PREVIEW_LIFETIME.as_secs() as i64,
    };
    Ok(PendingPreview {
        public,
        targets,
        created: Instant::now(),
    })
}

fn resolve_thread_ids(
    server: &mut AppServer,
    request: &CodexGlobalSettingsRequest,
    request_id: &mut i64,
) -> Result<(Vec<String>, bool), CodexHistoryError> {
    if request.scope == "currentView" {
        let mut seen = HashSet::new();
        let ids = request
            .thread_ids
            .iter()
            .filter(|id| seen.insert((*id).clone()))
            .cloned()
            .collect();
        return Ok((ids, false));
    }
    let archived_values: &[bool] = if request.scope == "allIncludingArchived" {
        &[false, true]
    } else {
        &[false]
    };
    let mut ids = Vec::new();
    let mut seen = HashSet::new();
    let mut truncated = false;
    for archived in archived_values {
        let mut cursor: Option<String> = None;
        loop {
            let id = take_request_id(request_id);
            server.send(&json!({
                "id": id,
                "method": "thread/list",
                "params": {
                    "cursor": cursor,
                    "limit": THREAD_PAGE_SIZE,
                    "sortKey": "recency_at",
                    "sortDirection": "desc",
                    "sourceKinds": ALL_SOURCE_KINDS,
                    "archived": archived,
                    "useStateDbOnly": false
                }
            }))?;
            let response = server.wait_for(&[id], Instant::now() + RESPONSE_TIMEOUT)?;
            let result = checked_result(&response[0])?;
            let data = result
                .get("data")
                .and_then(Value::as_array)
                .ok_or_else(|| {
                    CodexHistoryError::new("protocol_changed", "Codex 任务列表格式已变化")
                })?;
            for thread in data {
                let Some(thread_id) = thread.get("id").and_then(Value::as_str) else {
                    continue;
                };
                if validate_thread_id(thread_id).is_ok() && seen.insert(thread_id.to_string()) {
                    if ids.len() >= MAX_BATCH_THREADS {
                        truncated = true;
                        break;
                    }
                    ids.push(thread_id.to_string());
                }
            }
            if truncated {
                break;
            }
            cursor = result
                .get("nextCursor")
                .and_then(Value::as_str)
                .filter(|value| value.len() <= 4096)
                .map(str::to_string);
            if cursor.is_none() {
                break;
            }
        }
        if truncated {
            break;
        }
    }
    Ok((ids, truncated))
}

fn resume_thread(
    server: &mut AppServer,
    thread_id: &str,
    request_id: &mut i64,
) -> Result<CurrentThreadSettings, CodexHistoryError> {
    validate_thread_id(thread_id)?;
    let id = take_request_id(request_id);
    server.send(&json!({
        "id": id,
        "method": "thread/resume",
        "params": {"threadId": thread_id, "excludeTurns": true}
    }))?;
    let response = server.wait_for(&[id], Instant::now() + RESPONSE_TIMEOUT)?;
    let result = checked_result(&response[0])?;
    let returned_id = result
        .get("thread")
        .and_then(|thread| thread.get("id"))
        .and_then(Value::as_str)
        .ok_or_else(|| {
            CodexHistoryError::new("protocol_changed", "Codex 任务设置响应缺少任务编号")
        })?;
    if returned_id != thread_id {
        return Err(CodexHistoryError::new(
            "protocol_changed",
            "Codex 任务设置响应编号不一致",
        ));
    }
    let model = result
        .get("model")
        .and_then(Value::as_str)
        .filter(|value| safe_model_identifier(value))
        .ok_or_else(|| CodexHistoryError::new("protocol_changed", "Codex 任务设置缺少模型信息"))?;
    let effort = optional_identifier(result.get("reasoningEffort"))?;
    let service_tier = normalized_service_tier(result.get("serviceTier"))?;
    // Deliberately discard thread preview, cwd, instruction sources, and all turn content.
    Ok(CurrentThreadSettings {
        thread_id: thread_id.to_string(),
        model: model.to_string(),
        effort,
        service_tier,
    })
}

fn apply_targets(pending: PendingPreview) -> Result<CodexGlobalSettingsReceipt, CodexHistoryError> {
    if pending.targets.is_empty() {
        return Ok(CodexGlobalSettingsReceipt {
            changed_count: 0,
            unchanged_count: pending.public.unchanged_count,
            failed_count: 0,
            failures: Vec::new(),
            previous: Vec::new(),
            applied: Vec::new(),
            applied_at: observed_at(),
        });
    }
    let executable = codex_executable()?;
    let mut server = AppServer::start_streaming(&executable)?;
    let result = (|| {
        initialize(&mut server)?;
        let mut request_id = 2i64;
        let mut changed_count = 0usize;
        let mut failures = Vec::new();
        let mut previous = Vec::new();
        let mut applied = Vec::new();
        for target in pending.targets {
            let current = match resume_thread(&mut server, &target.thread_id, &mut request_id) {
                Ok(value) => value,
                Err(error) => {
                    failures.push(public_failure(&target.thread_id, error));
                    continue;
                }
            };
            if current.model != target.model
                || current.effort != target.before_effort
                || current.service_tier != target.before_service_tier
            {
                failures.push(CodexGlobalSettingsFailure {
                    thread_id: target.thread_id,
                    message: "任务设置在确认前已经变化，请重新预览".to_string(),
                });
                continue;
            }
            let previous_setting = CodexThreadRuntimeSetting {
                thread_id: target.thread_id.clone(),
                model: target.model.clone(),
                effort: target.before_effort.clone(),
                service_tier: target.before_service_tier.clone(),
                effort_changed: target.effort_change.is_some(),
                service_tier_changed: target.service_tier_change.is_some(),
            };
            let update_id = take_request_id(&mut request_id);
            server.send(&settings_update_request(update_id, &target))?;
            let response = server.wait_for(&[update_id], Instant::now() + RESPONSE_TIMEOUT)?;
            if let Err(error) = checked_result(&response[0]) {
                failures.push(public_failure(&target.thread_id, error));
                continue;
            }
            // Keep the prior values even if readback fails, so the user still has a restore path.
            previous.push(previous_setting);
            match resume_thread(&mut server, &target.thread_id, &mut request_id) {
                Ok(readback) if target_matches(&target, &readback) => {
                    changed_count += 1;
                    applied.push(target_override(&target));
                }
                Ok(_) => failures.push(CodexGlobalSettingsFailure {
                    thread_id: target.thread_id,
                    message: "Codex 未确认新的运行配置，请重新检查".to_string(),
                }),
                Err(error) => failures.push(public_failure(&target.thread_id, error)),
            }
        }
        Ok(CodexGlobalSettingsReceipt {
            changed_count,
            unchanged_count: pending.public.unchanged_count,
            failed_count: failures.len(),
            failures,
            previous,
            applied,
            applied_at: observed_at(),
        })
    })();
    server.stop();
    result
}

fn restore_settings(
    previous: Vec<CodexThreadRuntimeSetting>,
) -> Result<CodexGlobalSettingsRestoreReceipt, CodexHistoryError> {
    if previous.is_empty() {
        return Ok(CodexGlobalSettingsRestoreReceipt {
            restored_count: 0,
            failed_count: 0,
            failures: Vec::new(),
            remaining: Vec::new(),
            restored: Vec::new(),
            restored_at: observed_at(),
        });
    }
    let executable = codex_executable()?;
    let mut server = AppServer::start_streaming(&executable)?;
    let result = (|| {
        initialize(&mut server)?;
        let mut request_id = 2i64;
        let mut restored_count = 0usize;
        let mut failures = Vec::new();
        let mut remaining = Vec::new();
        let mut restored = Vec::new();
        for setting in previous {
            let current = match resume_thread(&mut server, &setting.thread_id, &mut request_id) {
                Ok(value) => value,
                Err(error) => {
                    failures.push(public_failure(&setting.thread_id, error));
                    remaining.push(setting);
                    continue;
                }
            };
            if current.model != setting.model {
                failures.push(CodexGlobalSettingsFailure {
                    thread_id: setting.thread_id.clone(),
                    message: "任务模型已经变化，未自动恢复旧强度".to_string(),
                });
                remaining.push(setting);
                continue;
            }
            let target = SettingTarget {
                thread_id: setting.thread_id.clone(),
                model: setting.model.clone(),
                before_effort: current.effort,
                before_service_tier: current.service_tier,
                effort_change: setting.effort_changed.then(|| setting.effort.clone()),
                service_tier_change: setting
                    .service_tier_changed
                    .then(|| setting.service_tier.clone()),
            };
            let update_id = take_request_id(&mut request_id);
            server.send(&settings_update_request(update_id, &target))?;
            let response = server.wait_for(&[update_id], Instant::now() + RESPONSE_TIMEOUT)?;
            if let Err(error) = checked_result(&response[0]) {
                failures.push(public_failure(&setting.thread_id, error));
                remaining.push(setting);
                continue;
            }
            match resume_thread(&mut server, &setting.thread_id, &mut request_id) {
                Ok(readback)
                    if (!setting.effort_changed || readback.effort == setting.effort)
                        && (!setting.service_tier_changed
                            || readback.service_tier == setting.service_tier) =>
                {
                    restored_count += 1;
                    restored.push(CodexThreadRuntimeOverride {
                        thread_id: setting.thread_id.clone(),
                        effort_set: setting.effort_changed,
                        effort: setting.effort.clone(),
                        service_tier_set: setting.service_tier_changed,
                        service_tier: setting.service_tier.clone(),
                    });
                }
                Ok(_) => {
                    failures.push(CodexGlobalSettingsFailure {
                        thread_id: setting.thread_id.clone(),
                        message: "Codex 未确认恢复结果".to_string(),
                    });
                    remaining.push(setting);
                }
                Err(error) => {
                    failures.push(public_failure(&setting.thread_id, error));
                    remaining.push(setting);
                }
            }
        }
        Ok(CodexGlobalSettingsRestoreReceipt {
            restored_count,
            failed_count: failures.len(),
            failures,
            remaining,
            restored,
            restored_at: observed_at(),
        })
    })();
    server.stop();
    result
}

fn settings_update_request(id: i64, target: &SettingTarget) -> Value {
    let mut params = Map::new();
    params.insert(
        "threadId".to_string(),
        Value::String(target.thread_id.clone()),
    );
    if let Some(effort) = &target.effort_change {
        params.insert(
            "effort".to_string(),
            effort.clone().map(Value::String).unwrap_or(Value::Null),
        );
    }
    if let Some(service_tier) = &target.service_tier_change {
        params.insert(
            "serviceTier".to_string(),
            service_tier
                .clone()
                .map(Value::String)
                .unwrap_or(Value::Null),
        );
    }
    json!({"id": id, "method": "thread/settings/update", "params": params})
}

fn target_matches(target: &SettingTarget, current: &CurrentThreadSettings) -> bool {
    target
        .effort_change
        .as_ref()
        .map_or(true, |value| value == &current.effort)
        && target
            .service_tier_change
            .as_ref()
            .map_or(true, |value| value == &current.service_tier)
}

fn target_override(target: &SettingTarget) -> CodexThreadRuntimeOverride {
    CodexThreadRuntimeOverride {
        thread_id: target.thread_id.clone(),
        effort_set: target.effort_change.is_some(),
        effort: target.effort_change.clone().flatten(),
        service_tier_set: target.service_tier_change.is_some(),
        service_tier: target.service_tier_change.clone().flatten(),
    }
}

fn resolve_effort(
    selection: &str,
    model: Option<&ModelCapability>,
    current: Option<&str>,
) -> Resolution {
    if selection == "keep" {
        return Resolution::Keep;
    }
    let Some(model) = model else {
        return Resolution::Unsupported;
    };
    let target = match selection {
        "default" => Some(model.default_effort.clone()),
        "minimum" => model.efforts.first().cloned(),
        "maximum" => model.efforts.last().cloned(),
        concrete if model.efforts.iter().any(|effort| effort == concrete) => {
            Some(concrete.to_string())
        }
        _ => None,
    };
    match target {
        Some(value) if current == Some(value.as_str()) => Resolution::Keep,
        Some(value) => Resolution::Set(Some(value)),
        None => Resolution::Unsupported,
    }
}

fn resolve_speed(
    selection: &str,
    model: Option<&ModelCapability>,
    current: Option<&str>,
) -> Resolution {
    match selection {
        "keep" => Resolution::Keep,
        "standard" if current.is_none() => Resolution::Keep,
        "standard" => Resolution::Set(None),
        concrete => {
            let supported =
                model.is_some_and(|model| model.speed_tiers.iter().any(|tier| tier.id == concrete));
            if !supported {
                Resolution::Unsupported
            } else if current == Some(concrete) {
                Resolution::Keep
            } else {
                Resolution::Set(Some(concrete.to_string()))
            }
        }
    }
}

fn change_from_resolution(resolution: Resolution, current: Option<&str>) -> Option<Option<String>> {
    match resolution {
        Resolution::Keep | Resolution::Unsupported => None,
        Resolution::Set(value) if value.as_deref() == current => None,
        Resolution::Set(value) => Some(value),
    }
}

fn validate_request(request: &CodexGlobalSettingsRequest) -> Result<(), CodexHistoryError> {
    if !matches!(
        request.scope.as_str(),
        "currentView" | "allActive" | "allIncludingArchived"
    ) {
        return Err(CodexHistoryError::new(
            "invalid_scope",
            "全局运行配置的任务范围无效",
        ));
    }
    if request.thread_ids.len() > MAX_BATCH_THREADS {
        return Err(CodexHistoryError::new(
            "too_many_threads",
            "当前列表任务数量超过单次安全上限",
        ));
    }
    for thread_id in &request.thread_ids {
        validate_thread_id(thread_id)?;
    }
    if request.scope == "currentView" && request.thread_ids.is_empty() {
        return Err(CodexHistoryError::new(
            "empty_scope",
            "当前列表没有可调整的 Codex 任务",
        ));
    }
    if request.reasoning_selection == "keep" && request.speed_selection == "keep" {
        return Err(CodexHistoryError::new(
            "no_change",
            "请至少选择一项要统一调整的运行配置",
        ));
    }
    if !selection_is_safe(&request.reasoning_selection)
        || !selection_is_safe(&request.speed_selection)
    {
        return Err(CodexHistoryError::new(
            "invalid_selection",
            "全局运行配置选项无效",
        ));
    }
    Ok(())
}

fn validate_restore_settings(
    settings: &[CodexThreadRuntimeSetting],
) -> Result<(), CodexHistoryError> {
    if settings.len() > MAX_BATCH_THREADS {
        return Err(CodexHistoryError::new(
            "too_many_threads",
            "恢复任务数量超过单次安全上限",
        ));
    }
    let mut seen = HashSet::new();
    for setting in settings {
        validate_thread_id(&setting.thread_id)?;
        if !seen.insert(setting.thread_id.as_str())
            || !safe_model_identifier(&setting.model)
            || setting
                .effort
                .as_deref()
                .is_some_and(|value| !safe_identifier(value))
            || setting
                .service_tier
                .as_deref()
                .is_some_and(|value| !safe_identifier(value))
        {
            return Err(CodexHistoryError::new(
                "invalid_restore",
                "保存的旧运行配置无效，未执行恢复",
            ));
        }
    }
    Ok(())
}

fn selection_is_safe(value: &str) -> bool {
    matches!(
        value,
        "keep" | "default" | "minimum" | "maximum" | "standard"
    ) || safe_identifier(value)
}

fn safe_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 80
        && value
            .chars()
            .all(|character| character.is_ascii_alphanumeric() || matches!(character, '-' | '_'))
}

fn safe_model_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 120
        && value.chars().all(|character| {
            character.is_ascii_alphanumeric() || matches!(character, '-' | '_' | '.' | '/' | ':')
        })
}

fn optional_identifier(value: Option<&Value>) -> Result<Option<String>, CodexHistoryError> {
    let Some(value) = value else { return Ok(None) };
    if value.is_null() {
        return Ok(None);
    }
    let value = value
        .as_str()
        .filter(|value| safe_identifier(value))
        .ok_or_else(|| {
            CodexHistoryError::new("protocol_changed", "Codex 任务运行配置格式已变化")
        })?;
    Ok(Some(value.to_string()))
}

fn normalized_service_tier(value: Option<&Value>) -> Result<Option<String>, CodexHistoryError> {
    Ok(optional_identifier(value)?
        .filter(|value| !matches!(value.as_str(), "default" | "standard")))
}

fn bounded_plain_text(value: Option<&str>, fallback: &str, limit: usize) -> String {
    let value = value.unwrap_or(fallback);
    let text: String = value
        .chars()
        .filter(|character| !character.is_control())
        .take(limit)
        .collect();
    let text = text.trim();
    if text.is_empty() {
        fallback.to_string()
    } else {
        text.to_string()
    }
}

fn effort_label(id: &str) -> String {
    match id {
        "minimal" => "最少".to_string(),
        "low" => "低".to_string(),
        "medium" => "中等".to_string(),
        "high" => "高".to_string(),
        "xhigh" => "很高".to_string(),
        "max" => "最大".to_string(),
        "ultra" => "Ultra".to_string(),
        _ => id.to_string(),
    }
}

fn effort_description(id: &str) -> Option<String> {
    (id == "ultra").then(|| "最高级别；支持时可能启用主动多智能体".to_string())
}

fn take_request_id(next: &mut i64) -> i64 {
    let value = *next;
    *next = next.saturating_add(1);
    value
}

fn public_failure(thread_id: &str, error: CodexHistoryError) -> CodexGlobalSettingsFailure {
    CodexGlobalSettingsFailure {
        thread_id: thread_id.to_string(),
        message: error.message,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[ignore = "requires an installed and logged-in Codex; reads model capabilities only"]
    fn live_official_runtime_capabilities_smoke() {
        let capabilities = load_capabilities().expect("load official Codex runtime capabilities");
        assert!(capabilities.model_count > 0);
        assert!(!capabilities.reasoning_efforts.is_empty());
    }

    fn model() -> ModelCapability {
        ModelCapability {
            model: "gpt-5.6-sol".to_string(),
            display_name: "GPT-5.6-Sol".to_string(),
            efforts: ["low", "medium", "high", "xhigh", "max", "ultra"]
                .into_iter()
                .map(str::to_string)
                .collect(),
            default_effort: "medium".to_string(),
            speed_tiers: vec![SpeedTier {
                id: "priority".to_string(),
                name: "Fast".to_string(),
                description: "1.5x speed, increased usage".to_string(),
            }],
        }
    }

    #[test]
    fn capability_options_are_dynamic_and_user_facing() {
        let capabilities = capabilities_from_models(&[model()]);
        assert_eq!(capabilities.model_count, 1);
        assert!(capabilities
            .reasoning_efforts
            .iter()
            .any(|option| option.id == "ultra" && option.description.is_some()));
        assert_eq!(capabilities.speed_tiers[0].id, "priority");
        assert_eq!(capabilities.speed_tiers[0].label, "快速（Fast）");
    }

    #[test]
    fn official_dotted_model_ids_are_accepted() {
        let value = json!({
            "model": "gpt-5.6-sol",
            "displayName": "GPT-5.6-Sol",
            "supportedReasoningEfforts": [
                {"reasoningEffort": "low", "description": "Low"},
                {"reasoningEffort": "medium", "description": "Medium"},
                {"reasoningEffort": "high", "description": "High"}
            ],
            "defaultReasoningEffort": "medium",
            "serviceTiers": [
                {"id": "priority", "name": "Fast", "description": "Fast mode"}
            ]
        });
        let parsed = parse_model(&value).expect("parse current official model id");
        assert_eq!(parsed.model, "gpt-5.6-sol");
        assert_eq!(parsed.default_effort, "medium");
        assert_eq!(parsed.speed_tiers[0].id, "priority");
    }

    #[test]
    fn relative_reasoning_choices_map_to_each_models_supported_range() {
        let model = model();
        assert!(matches!(
            resolve_effort("minimum", Some(&model), Some("medium")),
            Resolution::Set(Some(value)) if value == "low"
        ));
        assert!(matches!(
            resolve_effort("maximum", Some(&model), Some("medium")),
            Resolution::Set(Some(value)) if value == "ultra"
        ));
        assert!(matches!(
            resolve_effort("medium", Some(&model), Some("medium")),
            Resolution::Keep
        ));
        assert!(matches!(
            resolve_effort("future", Some(&model), None),
            Resolution::Unsupported
        ));
    }

    #[test]
    fn standard_speed_clears_only_an_existing_fast_tier() {
        assert!(matches!(
            resolve_speed("standard", Some(&model()), None),
            Resolution::Keep
        ));
        assert!(matches!(
            resolve_speed("standard", Some(&model()), Some("priority")),
            Resolution::Set(None)
        ));
        assert!(matches!(
            resolve_speed("priority", Some(&model()), None),
            Resolution::Set(Some(value)) if value == "priority"
        ));
        assert_eq!(
            normalized_service_tier(Some(&json!("default"))).unwrap(),
            None
        );
        assert_eq!(
            normalized_service_tier(Some(&json!("priority"))).unwrap(),
            Some("priority".to_string())
        );
    }

    #[test]
    fn settings_update_includes_only_selected_fields() {
        let target = SettingTarget {
            thread_id: "019f-demo".to_string(),
            model: "gpt-5.6-sol".to_string(),
            before_effort: Some("medium".to_string()),
            before_service_tier: Some("priority".to_string()),
            effort_change: Some(Some("high".to_string())),
            service_tier_change: Some(None),
        };
        let value = settings_update_request(9, &target);
        assert_eq!(value["method"], "thread/settings/update");
        assert_eq!(value["params"]["effort"], "high");
        assert!(value["params"]["serviceTier"].is_null());
        assert!(value["params"].get("model").is_none());
    }

    #[test]
    fn no_op_and_unsafe_batch_requests_are_rejected() {
        let mut request = CodexGlobalSettingsRequest {
            scope: "allActive".to_string(),
            thread_ids: Vec::new(),
            reasoning_selection: "keep".to_string(),
            speed_selection: "keep".to_string(),
        };
        assert_eq!(validate_request(&request).unwrap_err().code, "no_change");
        request.reasoning_selection = "high".to_string();
        assert!(validate_request(&request).is_ok());
        request.speed_selection = "../../bad".to_string();
        assert_eq!(
            validate_request(&request).unwrap_err().code,
            "invalid_selection"
        );
    }

    #[test]
    fn resume_parser_discards_everything_except_safe_runtime_metadata() {
        let value = json!({
            "id": 3,
            "result": {
                "thread": {"id": "019f-demo", "preview": "private conversation"},
                "model": "gpt-5.6-sol",
                "reasoningEffort": "high",
                "serviceTier": "priority",
                "cwd": "/private/path",
                "instructionSources": ["/private/instructions"]
            }
        });
        let result = checked_result(&value).unwrap();
        let model = result.get("model").and_then(Value::as_str).unwrap();
        assert_eq!(model, "gpt-5.6-sol");
        let serialized = serde_json::to_string(&CodexThreadRuntimeSetting {
            thread_id: "019f-demo".to_string(),
            model: model.to_string(),
            effort: Some("high".to_string()),
            service_tier: Some("priority".to_string()),
            effort_changed: true,
            service_tier_changed: true,
        })
        .unwrap();
        assert!(!serialized.contains("private conversation"));
        assert!(!serialized.contains("/private/path"));
    }
}
