use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
    env,
    fs::{self, File, OpenOptions},
    io::{BufRead, BufReader, Read, Write},
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};
use tauri::Manager;
use time::{format_description::well_known::Rfc3339, OffsetDateTime};

const MAX_FRONTMATTER_BYTES: usize = 128 * 1024;
const MAX_BODY_BYTES: u64 = 2 * 1024 * 1024;
const MAX_EVENT_FILE_BYTES: u64 = 8 * 1024 * 1024;
const WRITABLE_PRIORITIES: [&str; 3] = ["high", "medium", "low"];

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct RawTaskSource {
    file_token: String,
    frontmatter: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct ProjectMapping {
    id: String,
    name: String,
    workdirs: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct TaskEvent {
    id: String,
    task_id: String,
    event_type: String,
    occurred_at: String,
    previous_task_status: Option<String>,
    new_task_status: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PriorityEditRequest {
    file_token: String,
    new_priority: String,
    expected_hash: String,
    confirmed: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct PriorityEditPreview {
    file_token: String,
    task_id: String,
    before_priority: String,
    after_priority: String,
    expected_hash: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct PriorityEditReceipt {
    file_token: String,
    task_id: String,
    previous_priority: String,
    new_priority: String,
    file_hash: String,
    event_id: String,
    event_file: String,
    verified: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
struct WriteError {
    code: &'static str,
    message: &'static str,
}

impl WriteError {
    const fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

fn write_error(code: &'static str, message: &'static str) -> WriteError {
    WriteError::new(code, message)
}

fn default_task_root() -> PathBuf {
    if let Ok(root) = env::var("CODEX_TASK_CENTER_TASK_ROOT") {
        return PathBuf::from(root);
    }
    let home = env::var_os("HOME")
        .or_else(|| env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .unwrap_or_default();
    home.join("Documents")
        .join("01-小烈刀-AI协作库-默认可读")
        .join("30-领域与项目")
        .join("任务中枢")
        .join("任务")
}

fn ensure_child(root: &Path, candidate: &Path) -> Result<PathBuf, String> {
    let root = root
        .canonicalize()
        .map_err(|_| "任务根目录不可用".to_string())?;
    let candidate = candidate
        .canonicalize()
        .map_err(|_| "任务文件不可用".to_string())?;
    if !candidate.starts_with(&root) {
        return Err("拒绝读取任务根目录之外的路径".to_string());
    }
    Ok(candidate)
}

fn safe_file(root: &Path, file_token: &str, extension: &str) -> Result<PathBuf, String> {
    let token_path = Path::new(file_token);
    if token_path.components().count() != 1
        || token_path.extension().and_then(|v| v.to_str()) != Some(extension)
    {
        return Err("无效的文件令牌".to_string());
    }
    ensure_child(root, &root.join(token_path))
}

fn read_frontmatter(path: &Path) -> Result<String, String> {
    let file = File::open(path).map_err(|_| "无法打开任务元数据".to_string())?;
    let mut reader = BufReader::new(file);
    let mut line = String::new();
    reader
        .read_line(&mut line)
        .map_err(|_| "无法读取任务元数据".to_string())?;
    if line.trim_end() != "---" {
        return Err("缺少 frontmatter 起始标记".to_string());
    }
    let mut result = String::new();
    loop {
        line.clear();
        let bytes = reader
            .read_line(&mut line)
            .map_err(|_| "无法读取任务元数据".to_string())?;
        if bytes == 0 {
            return Err("缺少 frontmatter 结束标记".to_string());
        }
        if line.trim_end() == "---" {
            return Ok(result);
        }
        if result.len() + bytes > MAX_FRONTMATTER_BYTES {
            return Err("frontmatter 超过安全上限".to_string());
        }
        result.push_str(&line);
    }
}

fn frontmatter_scalar<'a>(frontmatter: &'a str, key: &str) -> Option<&'a str> {
    frontmatter.lines().find_map(|line| {
        let (candidate, value) = line.split_once(':')?;
        if candidate.trim() != key {
            return None;
        }
        Some(value.trim().trim_matches(['"', '\'']))
    })
}

fn frontmatter_allows_read(frontmatter: &str) -> bool {
    let privacy = frontmatter_scalar(frontmatter, "privacy");
    let access = frontmatter_scalar(frontmatter, "codex_access");
    privacy == Some("general")
        && !matches!(access, None | Some("forbidden") | Some("explicit_only"))
}

fn scan_metadata(root: &Path) -> Result<Vec<RawTaskSource>, String> {
    let canonical_root = root
        .canonicalize()
        .map_err(|_| "正式任务目录不可用".to_string())?;
    let mut entries: Vec<_> = fs::read_dir(&canonical_root)
        .map_err(|_| "无法列出正式任务目录".to_string())?
        .filter_map(Result::ok)
        .collect();
    entries.sort_by_key(|entry| entry.file_name());
    Ok(entries
        .into_iter()
        .filter_map(|entry| {
            let file_token = entry.file_name().to_string_lossy().into_owned();
            if !file_token.starts_with("tsk_") || !file_token.ends_with(".md") {
                return None;
            }
            let path = match ensure_child(&canonical_root, &entry.path()) {
                Ok(path) => path,
                Err(error) => {
                    return Some(RawTaskSource {
                        file_token,
                        frontmatter: String::new(),
                        error: Some(error),
                    })
                }
            };
            match read_frontmatter(&path) {
                Ok(frontmatter) if frontmatter_allows_read(&frontmatter) => Some(RawTaskSource {
                    file_token,
                    frontmatter,
                    error: None,
                }),
                Ok(_) => Some(RawTaskSource {
                    file_token,
                    frontmatter: String::new(),
                    error: Some("任务受隐私或访问规则限制".to_string()),
                }),
                Err(error) => Some(RawTaskSource {
                    file_token,
                    frontmatter: String::new(),
                    error: Some(error),
                }),
            }
        })
        .collect())
}

fn frontmatter_allows_body(frontmatter: &str) -> bool {
    frontmatter_allows_read(frontmatter)
}

fn read_body(root: &Path, file_token: &str) -> Result<String, String> {
    let path = safe_file(root, file_token, "md")?;
    let metadata = fs::metadata(&path).map_err(|_| "任务文件不可用".to_string())?;
    if metadata.len() > MAX_BODY_BYTES {
        return Err("正文超过 2 MiB 安全上限".to_string());
    }
    let frontmatter = read_frontmatter(&path)?;
    if !frontmatter_allows_body(&frontmatter) {
        return Err("正文受隐私或访问规则限制".to_string());
    }
    let mut content = String::new();
    File::open(path)
        .and_then(|mut file| file.read_to_string(&mut content))
        .map_err(|_| "正文读取失败".to_string())?;
    let mut offset = 0;
    let mut lines = content.split_inclusive('\n');
    let first = lines.next().ok_or_else(|| "正文边界无效".to_string())?;
    if first.trim_end_matches(['\r', '\n']) != "---" {
        return Err("正文边界无效".to_string());
    }
    offset += first.len();
    for line in lines {
        offset += line.len();
        if line.trim_end_matches(['\r', '\n']) == "---" {
            return Ok(content[offset..]
                .trim_start_matches(['\r', '\n'])
                .to_string());
        }
    }
    Err("正文边界无效".to_string())
}

fn events_root(task_root: &Path) -> PathBuf {
    task_root.parent().unwrap_or(task_root).join("事件")
}

fn read_events(root: &Path, task_id: &str) -> Result<Vec<TaskEvent>, String> {
    if !task_id.starts_with("tsk_") || task_id.len() > 128 {
        return Err("无效的任务编号".to_string());
    }
    let root = root
        .canonicalize()
        .map_err(|_| "事件目录不可用".to_string())?;
    let mut files: Vec<_> = fs::read_dir(&root)
        .map_err(|_| "无法列出事件目录".to_string())?
        .filter_map(Result::ok)
        .filter(|entry| entry.file_name().to_string_lossy().ends_with(".jsonl"))
        .collect();
    files.sort_by_key(|entry| entry.file_name());
    let mut result = Vec::new();
    for entry in files {
        let path = ensure_child(&root, &entry.path())?;
        let file = match File::open(path) {
            Ok(file) => file,
            Err(_) => continue,
        };
        for line in BufReader::new(file).lines().map_while(Result::ok) {
            let value: Value = match serde_json::from_str(&line) {
                Ok(value) => value,
                Err(_) => continue,
            };
            if value.get("task_id").and_then(Value::as_str) != Some(task_id)
                || value
                    .get("privacy")
                    .and_then(Value::as_str)
                    .unwrap_or("pending_classification")
                    != "general"
            {
                continue;
            }
            let Some(id) = value.get("id").and_then(Value::as_str) else {
                continue;
            };
            let Some(event_type) = value.get("event_type").and_then(Value::as_str) else {
                continue;
            };
            let Some(occurred_at) = value.get("occurred_at").and_then(Value::as_str) else {
                continue;
            };
            result.push(TaskEvent {
                id: id.to_string(),
                task_id: task_id.to_string(),
                event_type: event_type.to_string(),
                occurred_at: occurred_at.to_string(),
                previous_task_status: value
                    .get("previous_task_status")
                    .and_then(Value::as_str)
                    .map(str::to_string),
                new_task_status: value
                    .get("new_task_status")
                    .and_then(Value::as_str)
                    .map(str::to_string),
            });
        }
    }
    result.sort_by(|a, b| b.occurred_at.cmp(&a.occurred_at));
    Ok(result)
}

fn project_config_path() -> Option<PathBuf> {
    if let Ok(path) = env::var("CODEX_TASK_CENTER_PROJECT_CONFIG") {
        return Some(PathBuf::from(path));
    }
    env::var_os("HOME")
        .or_else(|| env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .map(|home| {
            home.join(".codex-monitor")
                .join("task-center-projects.json")
        })
}

fn read_project_mappings(path: Option<&Path>) -> Result<Vec<ProjectMapping>, String> {
    let Some(path) = path else {
        return Ok(Vec::new());
    };
    if !path.exists() {
        return Ok(Vec::new());
    }
    let metadata = fs::metadata(path).map_err(|_| "项目映射配置不可用".to_string())?;
    if metadata.len() > 256 * 1024 {
        return Err("项目映射配置过大".to_string());
    }
    let mappings: Vec<ProjectMapping> =
        serde_json::from_reader(File::open(path).map_err(|_| "项目映射配置不可用".to_string())?)
            .map_err(|_| "项目映射配置格式错误".to_string())?;
    if mappings.iter().any(|item| {
        item.id.trim().is_empty()
            || item.name.trim().is_empty()
            || item
                .workdirs
                .iter()
                .any(|path| !Path::new(path).is_absolute())
    }) {
        return Err("项目映射必须包含编号、名称和绝对工作目录".to_string());
    }
    Ok(mappings)
}

fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    digest.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn read_writable_task(root: &Path, file_token: &str) -> Result<(PathBuf, String), WriteError> {
    let path = safe_file(root, file_token, "md")
        .map_err(|_| write_error("invalid_file", "任务文件不可用或超出允许目录"))?;
    let metadata = fs::metadata(&path)
        .map_err(|_| write_error("invalid_file", "任务文件不可用或超出允许目录"))?;
    if metadata.permissions().readonly() {
        return Err(write_error("read_only", "任务文件为只读，未执行写入"));
    }
    if metadata.len() > MAX_BODY_BYTES {
        return Err(write_error("too_large", "任务文件超过安全写入上限"));
    }
    let mut content = String::new();
    File::open(&path)
        .and_then(|mut file| file.read_to_string(&mut content))
        .map_err(|_| write_error("read_failed", "任务文件读取失败"))?;
    let frontmatter = read_frontmatter(&path)
        .map_err(|_| write_error("bad_frontmatter", "任务元数据格式无效，未执行写入"))?;
    if !frontmatter_allows_read(&frontmatter) {
        return Err(write_error("restricted", "受限隐私或访问规则禁止写入"));
    }
    if frontmatter_scalar(&frontmatter, "task_id").is_none()
        && frontmatter_scalar(&frontmatter, "id").is_none()
    {
        return Err(write_error("missing_id", "任务缺少稳定编号，未执行写入"));
    }
    Ok((path, content))
}

fn frontmatter_range(content: &str) -> Result<(usize, usize), WriteError> {
    let mut lines = content.split_inclusive('\n');
    let first = lines
        .next()
        .ok_or_else(|| write_error("bad_frontmatter", "任务元数据格式无效，未执行写入"))?;
    if first.trim_end_matches(['\r', '\n']) != "---" {
        return Err(write_error(
            "bad_frontmatter",
            "任务元数据格式无效，未执行写入",
        ));
    }
    let start = first.len();
    let mut offset = start;
    for line in lines {
        if line.trim_end_matches(['\r', '\n']) == "---" {
            return Ok((start, offset));
        }
        offset += line.len();
    }
    Err(write_error(
        "bad_frontmatter",
        "任务元数据格式无效，未执行写入",
    ))
}

fn replace_frontmatter_scalar(content: &str, key: &str, value: &str) -> Result<String, WriteError> {
    let (start, end) = frontmatter_range(content)?;
    let frontmatter = &content[start..end];
    let mut output = String::with_capacity(content.len() + value.len());
    output.push_str(&content[..start]);
    let mut replaced = false;
    for line in frontmatter.split_inclusive('\n') {
        let body = line.trim_end_matches(['\r', '\n']);
        let ending = &line[body.len()..];
        let is_target = body
            .split_once(':')
            .map(|(candidate, _)| candidate.trim() == key)
            .unwrap_or(false);
        if !is_target {
            output.push_str(line);
            continue;
        }
        if replaced {
            return Err(write_error("duplicate_field", "任务字段重复，未执行写入"));
        }
        let colon = body
            .find(':')
            .ok_or_else(|| write_error("bad_frontmatter", "任务字段格式无效"))?;
        let after_colon = &body[colon + 1..];
        let leading_len = after_colon.len() - after_colon.trim_start().len();
        let leading = if leading_len == 0 {
            " "
        } else {
            &after_colon[..leading_len]
        };
        let comment = after_colon.find(" #").map(|index| &after_colon[index..]);
        output.push_str(&body[..=colon]);
        output.push_str(leading);
        output.push_str(value);
        if let Some(comment) = comment {
            output.push_str(comment);
        }
        output.push_str(ending);
        replaced = true;
    }
    if !replaced {
        output.push_str(key);
        output.push_str(": ");
        output.push_str(value);
        output.push('\n');
    }
    output.push_str(&content[end..]);
    Ok(output)
}

fn task_id_and_priority(content: &str) -> Result<(String, String), WriteError> {
    let (start, end) = frontmatter_range(content)?;
    let frontmatter = &content[start..end];
    let task_id = frontmatter_scalar(frontmatter, "task_id")
        .or_else(|| frontmatter_scalar(frontmatter, "id"))
        .ok_or_else(|| write_error("missing_id", "任务缺少稳定编号，未执行写入"))?;
    let priority = frontmatter_scalar(frontmatter, "priority").unwrap_or("unknown");
    Ok((task_id.to_string(), priority.to_string()))
}

fn validate_priority(value: &str) -> Result<(), WriteError> {
    if WRITABLE_PRIORITIES.contains(&value) {
        Ok(())
    } else {
        Err(write_error(
            "unsupported_value",
            "正式结构暂不支持该优先级，未执行写入",
        ))
    }
}

fn preview_priority_edit_at(
    root: &Path,
    file_token: &str,
    new_priority: &str,
) -> Result<PriorityEditPreview, WriteError> {
    validate_priority(new_priority)?;
    let (_, content) = read_writable_task(root, file_token)?;
    let (task_id, before_priority) = task_id_and_priority(&content)?;
    let _ = replace_frontmatter_scalar(&content, "priority", new_priority)?;
    Ok(PriorityEditPreview {
        file_token: file_token.to_string(),
        task_id,
        before_priority,
        after_priority: new_priority.to_string(),
        expected_hash: sha256_hex(content.as_bytes()),
    })
}

fn unique_sidecar_path(path: &Path, suffix: &str) -> Result<PathBuf, WriteError> {
    let parent = path
        .parent()
        .ok_or_else(|| write_error("invalid_file", "任务文件目录不可用"))?;
    let name = path
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or_else(|| write_error("invalid_file", "任务文件名不可用"))?;
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| write_error("clock_error", "系统时间不可用"))?
        .as_nanos();
    Ok(parent.join(format!(".{name}.task-center-{nonce}.{suffix}")))
}

fn write_prepared_file(
    path: &Path,
    content: &[u8],
    permissions: Option<fs::Permissions>,
) -> Result<(), WriteError> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(|_| write_error("prepare_failed", "无法准备安全写入文件"))?;
    file.write_all(content)
        .and_then(|_| file.sync_all())
        .map_err(|_| write_error("prepare_failed", "无法准备安全写入文件"))?;
    if let Some(permissions) = permissions {
        fs::set_permissions(path, permissions)
            .map_err(|_| write_error("prepare_failed", "无法保留任务文件权限"))?;
    }
    Ok(())
}

fn current_event_time() -> Result<(String, String), WriteError> {
    let now = OffsetDateTime::now_utc();
    let occurred_at = now
        .format(&Rfc3339)
        .map_err(|_| write_error("clock_error", "无法生成事件时间"))?;
    let month = format!("{:04}-{:02}", now.year(), u8::from(now.month()));
    Ok((occurred_at, month))
}

fn rollback_file(path: &Path, backup: Option<&Path>) -> Result<(), WriteError> {
    if let Some(backup) = backup {
        fs::rename(backup, path)
            .map_err(|_| write_error("rollback_failed", "写入失败且无法自动恢复原文件"))?;
    } else if path.exists() {
        fs::remove_file(path)
            .map_err(|_| write_error("rollback_failed", "写入失败且无法移除新增事件"))?;
    }
    Ok(())
}

fn apply_priority_edit_at(
    root: &Path,
    events_root: &Path,
    request: PriorityEditRequest,
    occurred_at: &str,
    event_month: &str,
) -> Result<PriorityEditReceipt, WriteError> {
    if !request.confirmed {
        return Err(write_error(
            "confirmation_required",
            "用户未确认，未执行写入",
        ));
    }
    validate_priority(&request.new_priority)?;
    let (task_path, original) = read_writable_task(root, &request.file_token)?;
    let current_hash = sha256_hex(original.as_bytes());
    if current_hash != request.expected_hash {
        return Err(write_error(
            "conflict",
            "任务已被其他操作修改，请重新读取后确认",
        ));
    }
    let (task_id, previous_priority) = task_id_and_priority(&original)?;
    if previous_priority == request.new_priority {
        return Err(write_error("no_change", "修改前后相同，未执行写入"));
    }
    let updated = replace_frontmatter_scalar(&original, "priority", &request.new_priority)?;
    let updated_hash = sha256_hex(updated.as_bytes());

    fs::create_dir_all(events_root)
        .map_err(|_| write_error("event_prepare_failed", "事件目录不可用，未执行写入"))?;
    let canonical_events = events_root
        .canonicalize()
        .map_err(|_| write_error("event_prepare_failed", "事件目录不可用，未执行写入"))?;
    let event_file_name = format!("{event_month}.jsonl");
    let event_path = canonical_events.join(&event_file_name);
    if event_path.exists() {
        let metadata = fs::metadata(&event_path)
            .map_err(|_| write_error("event_prepare_failed", "事件文件不可用，未执行写入"))?;
        if !metadata.is_file()
            || metadata.permissions().readonly()
            || metadata.len() > MAX_EVENT_FILE_BYTES
        {
            return Err(write_error(
                "event_prepare_failed",
                "事件文件不可安全追加，未执行写入",
            ));
        }
    }
    let existing_events = if event_path.exists() {
        fs::read(&event_path)
            .map_err(|_| write_error("event_prepare_failed", "事件文件读取失败，未执行写入"))?
    } else {
        Vec::new()
    };
    if !existing_events.is_empty() && !existing_events.ends_with(b"\n") {
        return Err(write_error(
            "event_prepare_failed",
            "事件文件末行不完整，未执行写入",
        ));
    }
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| write_error("clock_error", "系统时间不可用"))?
        .as_nanos();
    let event_id = format!("evt_task_center_{nonce}_{}", &updated_hash[..12]);
    let event = serde_json::json!({
        "id": event_id,
        "task_id": task_id,
        "event_type": "priority_changed",
        "occurred_at": occurred_at,
        "source_refs": ["task-center-ui"],
        "confirmed_by": "user_ui_confirmation",
        "previous_priority": previous_priority,
        "new_priority": request.new_priority,
        "privacy": "general"
    });
    let mut next_events = existing_events.clone();
    next_events.extend_from_slice(
        serde_json::to_string(&event)
            .map_err(|_| write_error("event_prepare_failed", "事件序列化失败，未执行写入"))?
            .as_bytes(),
    );
    next_events.push(b'\n');

    let task_temp = unique_sidecar_path(&task_path, "tmp")?;
    let event_temp = unique_sidecar_path(&event_path, "tmp")?;
    let task_backup = unique_sidecar_path(&task_path, "bak")?;
    let event_backup = if event_path.exists() {
        Some(unique_sidecar_path(&event_path, "bak")?)
    } else {
        None
    };
    let task_permissions = fs::metadata(&task_path)
        .ok()
        .map(|value| value.permissions());
    let event_permissions = fs::metadata(&event_path)
        .ok()
        .map(|value| value.permissions());
    if let Err(error) = write_prepared_file(&task_temp, updated.as_bytes(), task_permissions) {
        let _ = fs::remove_file(&task_temp);
        return Err(error);
    }
    if let Err(error) = write_prepared_file(&event_temp, &next_events, event_permissions) {
        let _ = fs::remove_file(&task_temp);
        let _ = fs::remove_file(&event_temp);
        return Err(error);
    }
    if fs::copy(&task_path, &task_backup).is_err() {
        let _ = fs::remove_file(&task_temp);
        let _ = fs::remove_file(&event_temp);
        return Err(write_error("backup_failed", "无法创建恢复副本，未执行写入"));
    }
    if let Some(backup) = event_backup.as_deref() {
        if fs::copy(&event_path, backup).is_err() {
            let _ = fs::remove_file(&task_temp);
            let _ = fs::remove_file(&event_temp);
            let _ = fs::remove_file(&task_backup);
            return Err(write_error(
                "backup_failed",
                "无法创建事件恢复副本，未执行写入",
            ));
        }
    }

    let commit_result = (|| -> Result<(), WriteError> {
        fs::rename(&task_temp, &task_path)
            .map_err(|_| write_error("task_commit_failed", "任务原子替换失败"))?;
        fs::rename(&event_temp, &event_path)
            .map_err(|_| write_error("event_commit_failed", "事件追加失败"))?;
        let written_task =
            fs::read(&task_path).map_err(|_| write_error("readback_failed", "写后任务回读失败"))?;
        if sha256_hex(&written_task) != updated_hash {
            return Err(write_error("readback_mismatch", "写后任务内容核对不一致"));
        }
        let written_events = fs::read(&event_path)
            .map_err(|_| write_error("readback_failed", "写后事件回读失败"))?;
        if !written_events.ends_with(
            format!("{}\n", serde_json::to_string(&event).unwrap_or_default()).as_bytes(),
        ) {
            return Err(write_error("event_readback_mismatch", "写后事件核对不一致"));
        }
        Ok(())
    })();

    if let Err(error) = commit_result {
        let task_rollback = rollback_file(&task_path, Some(&task_backup));
        let event_rollback = rollback_file(&event_path, event_backup.as_deref());
        let _ = fs::remove_file(&task_temp);
        let _ = fs::remove_file(&event_temp);
        if task_rollback.is_err() || event_rollback.is_err() {
            return Err(write_error(
                "rollback_failed",
                "写入失败且自动恢复未完成，请停止继续写入",
            ));
        }
        return Err(error);
    }

    let _ = fs::remove_file(&task_backup);
    if let Some(backup) = event_backup.as_deref() {
        let _ = fs::remove_file(backup);
    }
    Ok(PriorityEditReceipt {
        file_token: request.file_token,
        task_id,
        previous_priority,
        new_priority: request.new_priority,
        file_hash: updated_hash,
        event_id,
        event_file: event_file_name,
        verified: true,
    })
}

#[tauri::command]
fn preview_priority_edit(
    file_token: String,
    new_priority: String,
) -> Result<PriorityEditPreview, WriteError> {
    preview_priority_edit_at(&default_task_root(), &file_token, &new_priority)
}

#[tauri::command]
fn apply_priority_edit(request: PriorityEditRequest) -> Result<PriorityEditReceipt, WriteError> {
    let (occurred_at, month) = current_event_time()?;
    let root = default_task_root();
    apply_priority_edit_at(&root, &events_root(&root), request, &occurred_at, &month)
}

#[tauri::command]
fn load_task_metadata() -> Result<Vec<RawTaskSource>, String> {
    scan_metadata(&default_task_root())
}

#[tauri::command]
fn load_task_body(file_token: String) -> Result<String, String> {
    read_body(&default_task_root(), &file_token)
}

#[tauri::command]
fn load_task_events(task_id: String) -> Result<Vec<TaskEvent>, String> {
    read_events(&events_root(&default_task_root()), &task_id)
}

#[tauri::command]
fn load_project_mappings() -> Result<Vec<ProjectMapping>, String> {
    read_project_mappings(project_config_path().as_deref())
}

pub fn read_only_diagnostic() -> i32 {
    let root = default_task_root();
    match scan_metadata(&root) {
        Ok(tasks) => {
            let mappings =
                read_project_mappings(project_config_path().as_deref()).unwrap_or_default();
            println!(
                "{}",
                serde_json::json!({
                    "ok": true,
                    "metadata_files": tasks.len(),
                    "project_mappings": mappings.len(),
                    "body_bytes_read": 0,
                    "write_operations": 0
                })
            );
            0
        }
        Err(error) => {
            println!("{}", serde_json::json!({ "ok": false, "error": error }));
            1
        }
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            load_task_metadata,
            load_task_body,
            load_task_events,
            load_project_mappings,
            preview_priority_edit,
            apply_priority_edit
        ])
        .on_window_event(|window, event| {
            if matches!(event, tauri::WindowEvent::CloseRequested { .. }) {
                window.app_handle().exit(0);
            }
        })
        .run(tauri::generate_context!())
        .expect("任务中心启动失败");
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use tempfile::tempdir;

    fn writable_task(priority: &str) -> String {
        format!(
            "---\n\
task_id: tsk_write_test\n\
schema_version: 1\n\
record_type: task\n\
task_status: doing\n\
title: Write test\n\
domain: test\n\
priority: {priority}\n\
unknown_extension: keep-me\n\
privacy: general\n\
codex_access: proposal_only\n\
source_refs: []\n\
verification_status: human_confirmed\n\
---\n\
# Body\n\nKeep --- body formatting.\n"
        )
    }

    #[test]
    fn frontmatter_scan_stops_before_body() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("tsk_test.md");
        fs::write(
            &path,
            "---\ntask_id: tsk_test\nprivacy: general\ncodex_access: proposal_only\n---\nSECRET_BODY_MARKER",
        )
        .unwrap();
        let rows = scan_metadata(dir.path()).unwrap();
        assert_eq!(rows.len(), 1);
        assert!(!rows[0].frontmatter.contains("SECRET_BODY_MARKER"));
        assert!(rows[0].error.is_none());
    }

    #[test]
    fn restricted_frontmatter_never_crosses_the_rust_boundary() {
        let dir = tempdir().unwrap();
        fs::write(
            dir.path().join("tsk_private.md"),
            "---\ntask_id: tsk_private\nprivacy: private\ncodex_access: explicit_only\nprivate_note: DO_NOT_EXPOSE\n---\nSECRET",
        ).unwrap();
        let rows = scan_metadata(dir.path()).unwrap();
        assert_eq!(rows.len(), 1);
        assert!(rows[0].frontmatter.is_empty());
        assert!(!format!("{:?}", rows[0]).contains("DO_NOT_EXPOSE"));
        assert!(rows[0].error.is_some());
    }

    #[test]
    fn rejects_traversal_and_symlink_escape() {
        let dir = tempdir().unwrap();
        assert!(safe_file(dir.path(), "../outside.md", "md").is_err());
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink("/etc/hosts", dir.path().join("tsk_escape.md")).unwrap();
            assert!(safe_file(dir.path(), "tsk_escape.md", "md").is_err());
        }
    }

    #[test]
    fn bad_event_line_isolated() {
        let dir = tempdir().unwrap();
        let mut file = File::create(dir.path().join("2026-08.jsonl")).unwrap();
        writeln!(file, "not json").unwrap();
        writeln!(file, "{{\"id\":\"e1\",\"task_id\":\"tsk_one\",\"event_type\":\"created\",\"occurred_at\":\"2026-08-22T00:00:00+08:00\",\"privacy\":\"general\"}}").unwrap();
        assert_eq!(read_events(dir.path(), "tsk_one").unwrap().len(), 1);
    }

    #[test]
    fn project_mapping_requires_absolute_workdirs() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("projects.json");
        fs::write(&path, r#"[{"id":"p","name":"P","workdirs":["relative"]}]"#).unwrap();
        assert!(read_project_mappings(Some(&path)).is_err());
    }

    #[test]
    fn body_permission_is_enforced_in_rust_layer() {
        let dir = tempdir().unwrap();
        fs::write(
            dir.path().join("tsk_private.md"),
            "---\ntask_id: tsk_private\nprivacy: general\ncodex_access: explicit_only\n---\nSECRET",
        )
        .unwrap();
        assert!(read_body(dir.path(), "tsk_private.md").is_err());
        fs::write(dir.path().join("tsk_general.md"), "---\ntask_id: tsk_general\nprivacy: general\ncodex_access: proposal_only\n---\nVISIBLE").unwrap();
        assert_eq!(read_body(dir.path(), "tsk_general.md").unwrap(), "VISIBLE");
    }

    #[test]
    fn body_delimiter_must_be_its_own_line() {
        let dir = tempdir().unwrap();
        fs::write(
            dir.path().join("tsk_delimiter.md"),
            "---\ntask_id: tsk_delimiter\nprivacy: general\ncodex_access: proposal_only\ntitle: contains---dashes\n---\nVISIBLE---BODY",
        ).unwrap();
        assert_eq!(
            read_body(dir.path(), "tsk_delimiter.md").unwrap(),
            "VISIBLE---BODY"
        );
    }

    #[test]
    fn malformed_frontmatter_is_reported_not_silently_dropped() {
        let dir = tempdir().unwrap();
        fs::write(dir.path().join("tsk_broken.md"), "---\ntask_id: broken").unwrap();
        let rows = scan_metadata(dir.path()).unwrap();
        assert_eq!(rows.len(), 1);
        assert!(rows[0].error.is_some());
    }

    #[test]
    fn priority_edit_requires_confirmation_and_preserves_original() {
        let dir = tempdir().unwrap();
        let task_root = dir.path().join("任务");
        let events = dir.path().join("事件");
        fs::create_dir_all(&task_root).unwrap();
        let path = task_root.join("tsk_write_test.md");
        let original = writable_task("medium");
        fs::write(&path, &original).unwrap();
        let preview = preview_priority_edit_at(&task_root, "tsk_write_test.md", "high").unwrap();
        let error = apply_priority_edit_at(
            &task_root,
            &events,
            PriorityEditRequest {
                file_token: preview.file_token,
                new_priority: preview.after_priority,
                expected_hash: preview.expected_hash,
                confirmed: false,
            },
            "2026-08-24T08:00:00Z",
            "2026-08",
        )
        .unwrap_err();
        assert_eq!(error.code, "confirmation_required");
        assert_eq!(fs::read_to_string(path).unwrap(), original);
        assert!(!events.exists());
    }

    #[test]
    fn priority_edit_is_atomic_appends_event_and_preserves_unknown_content() {
        let dir = tempdir().unwrap();
        let task_root = dir.path().join("任务");
        let events = dir.path().join("事件");
        fs::create_dir_all(&task_root).unwrap();
        fs::create_dir_all(&events).unwrap();
        let path = task_root.join("tsk_write_test.md");
        let original = writable_task("medium");
        fs::write(&path, &original).unwrap();
        fs::write(
            events.join("2026-08.jsonl"),
            "{\"id\":\"existing\",\"task_id\":\"tsk_write_test\"}\n",
        )
        .unwrap();
        let preview = preview_priority_edit_at(&task_root, "tsk_write_test.md", "high").unwrap();
        assert_eq!(preview.before_priority, "medium");
        let receipt = apply_priority_edit_at(
            &task_root,
            &events,
            PriorityEditRequest {
                file_token: preview.file_token,
                new_priority: preview.after_priority,
                expected_hash: preview.expected_hash,
                confirmed: true,
            },
            "2026-08-24T08:00:00Z",
            "2026-08",
        )
        .unwrap();
        assert!(receipt.verified);
        assert_eq!(receipt.previous_priority, "medium");
        assert_eq!(receipt.new_priority, "high");
        let written = fs::read_to_string(&path).unwrap();
        assert_eq!(
            written,
            original.replacen("priority: medium", "priority: high", 1)
        );
        assert!(written.contains("unknown_extension: keep-me"));
        assert!(written.ends_with("# Body\n\nKeep --- body formatting.\n"));
        let rows: Vec<Value> = fs::read_to_string(events.join("2026-08.jsonl"))
            .unwrap()
            .lines()
            .map(|line| serde_json::from_str(line).unwrap())
            .collect();
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0]["id"], "existing");
        assert_eq!(rows[1]["event_type"], "priority_changed");
        assert_eq!(rows[1]["previous_priority"], "medium");
        assert_eq!(rows[1]["new_priority"], "high");
        assert_eq!(rows[1]["confirmed_by"], "user_ui_confirmation");
        assert!(!task_root.read_dir().unwrap().any(|entry| entry
            .unwrap()
            .file_name()
            .to_string_lossy()
            .contains("task-center")));
    }

    #[test]
    fn concurrent_change_stops_without_overwrite_or_event() {
        let dir = tempdir().unwrap();
        let task_root = dir.path().join("任务");
        let events = dir.path().join("事件");
        fs::create_dir_all(&task_root).unwrap();
        let path = task_root.join("tsk_write_test.md");
        fs::write(&path, writable_task("medium")).unwrap();
        let preview = preview_priority_edit_at(&task_root, "tsk_write_test.md", "high").unwrap();
        let concurrent = writable_task("medium").replace(
            "unknown_extension: keep-me",
            "unknown_extension: changed-elsewhere",
        );
        fs::write(&path, &concurrent).unwrap();
        let error = apply_priority_edit_at(
            &task_root,
            &events,
            PriorityEditRequest {
                file_token: preview.file_token,
                new_priority: preview.after_priority,
                expected_hash: preview.expected_hash,
                confirmed: true,
            },
            "2026-08-24T08:00:00Z",
            "2026-08",
        )
        .unwrap_err();
        assert_eq!(error.code, "conflict");
        assert_eq!(fs::read_to_string(path).unwrap(), concurrent);
        assert!(!events.exists());
    }

    #[test]
    fn event_prepare_failure_leaves_task_unchanged() {
        let dir = tempdir().unwrap();
        let task_root = dir.path().join("任务");
        let events = dir.path().join("事件");
        fs::create_dir_all(&task_root).unwrap();
        fs::create_dir_all(events.join("2026-08.jsonl")).unwrap();
        let path = task_root.join("tsk_write_test.md");
        let original = writable_task("medium");
        fs::write(&path, &original).unwrap();
        let preview = preview_priority_edit_at(&task_root, "tsk_write_test.md", "high").unwrap();
        let error = apply_priority_edit_at(
            &task_root,
            &events,
            PriorityEditRequest {
                file_token: preview.file_token,
                new_priority: preview.after_priority,
                expected_hash: preview.expected_hash,
                confirmed: true,
            },
            "2026-08-24T08:00:00Z",
            "2026-08",
        )
        .unwrap_err();
        assert_eq!(error.code, "event_prepare_failed");
        assert_eq!(fs::read_to_string(path).unwrap(), original);
    }

    #[test]
    fn restricted_and_unsupported_priority_writes_are_rejected() {
        let dir = tempdir().unwrap();
        let private_path = dir.path().join("tsk_private.md");
        fs::write(
            &private_path,
            "---\ntask_id: tsk_private\npriority: medium\nprivacy: private\ncodex_access: explicit_only\n---\nSECRET",
        )
        .unwrap();
        assert_eq!(
            preview_priority_edit_at(dir.path(), "tsk_private.md", "high")
                .unwrap_err()
                .code,
            "restricted"
        );
        assert_eq!(
            preview_priority_edit_at(dir.path(), "tsk_private.md", "urgent")
                .unwrap_err()
                .code,
            "unsupported_value"
        );
    }
}
