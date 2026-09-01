use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    env, fs,
    io::{BufRead, BufReader, Write},
    path::{Path, PathBuf},
    process::{Child, ChildStdin, Command, Stdio},
    sync::mpsc::{self, Receiver, RecvTimeoutError},
    thread::{self, JoinHandle},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

const RESPONSE_TIMEOUT: Duration = Duration::from_secs(8);
const MAX_RESPONSE_BYTES: usize = 2 * 1024 * 1024;
const MAX_LINE_BYTES: usize = 1024 * 1024;
const MAX_CURSOR_BYTES: usize = 4096;
const TURN_PAGE_SIZE: u64 = 20;
const THREAD_LIST_PAGE_SIZE: u64 = 25;
const MAX_SEARCH_CHARS: usize = 200;
const MAX_SEARCH_BYTES: usize = 512;

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

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexThreadListRequest {
    cursor: Option<String>,
    archived: bool,
    source_group: String,
    search_term: Option<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexThreadSummary {
    thread_id: String,
    name: Option<String>,
    source_kind: String,
    source_label: String,
    reported_status: String,
    created_at: Option<i64>,
    updated_at: Option<i64>,
    workspace_name: Option<String>,
    is_pinned: bool,
    archived: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexThreadListPage {
    threads: Vec<CodexThreadSummary>,
    next_cursor: Option<String>,
    observed_at: i64,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexTurnSummary {
    id: String,
    status: String,
    started_at: Option<i64>,
    completed_at: Option<i64>,
    duration_ms: Option<i64>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexThreadPage {
    thread_id: String,
    name: Option<String>,
    source_kind: String,
    source_label: String,
    reported_status: String,
    created_at: Option<i64>,
    updated_at: Option<i64>,
    history_mode: Option<String>,
    turns: Vec<CodexTurnSummary>,
    next_cursor: Option<String>,
    history_state: String,
    history_message: Option<String>,
    observed_at: i64,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub(crate) struct CodexHistoryError {
    pub(crate) code: String,
    pub(crate) message: String,
}

impl CodexHistoryError {
    pub(crate) fn new(code: &str, message: &str) -> Self {
        Self {
            code: code.to_string(),
            message: message.to_string(),
        }
    }
}

enum ReaderEvent {
    Line(String),
    Error(CodexHistoryError),
    Eof,
}

pub(crate) struct AppServer {
    child: Child,
    stdin: Option<ChildStdin>,
    receiver: Option<Receiver<ReaderEvent>>,
    reader: Option<JoinHandle<()>>,
}

impl AppServer {
    pub(crate) fn start(executable: &Path) -> Result<Self, CodexHistoryError> {
        Self::start_with_response_limit(executable, Some(MAX_RESPONSE_BYTES))
    }

    pub(crate) fn start_streaming(executable: &Path) -> Result<Self, CodexHistoryError> {
        Self::start_with_response_limit(executable, None)
    }

    fn start_with_response_limit(
        executable: &Path,
        response_limit: Option<usize>,
    ) -> Result<Self, CodexHistoryError> {
        let mut command = Command::new(executable);
        command
            .arg("app-server")
            .arg("--stdio")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x0800_0000);
        }
        let mut child = command.spawn().map_err(|_| {
            CodexHistoryError::new("provider_start_failed", "无法启动本机 Codex 官方接口")
        })?;
        let stdin = child.stdin.take().ok_or_else(|| {
            CodexHistoryError::new("provider_start_failed", "Codex 官方接口输入管道不可用")
        })?;
        let stdout = child.stdout.take().ok_or_else(|| {
            CodexHistoryError::new("provider_start_failed", "Codex 官方接口输出管道不可用")
        })?;
        // A bounded queue prevents a noisy provider from growing this process without limit.
        let (sender, receiver) = mpsc::sync_channel(256);
        let reader = thread::spawn(move || {
            let mut reader = BufReader::new(stdout);
            let mut total = 0usize;
            loop {
                let mut bytes = Vec::new();
                match reader.read_until(b'\n', &mut bytes) {
                    Ok(0) => {
                        let _ = sender.send(ReaderEvent::Eof);
                        return;
                    }
                    Ok(_) => {
                        total = total.saturating_add(bytes.len());
                        if bytes.len() > MAX_LINE_BYTES
                            || response_limit.is_some_and(|limit| total > limit)
                        {
                            let _ = sender.send(ReaderEvent::Error(CodexHistoryError::new(
                                "response_too_large",
                                "Codex 官方接口响应超过安全上限",
                            )));
                            return;
                        }
                        while matches!(bytes.last(), Some(b'\n' | b'\r')) {
                            bytes.pop();
                        }
                        if bytes.is_empty() {
                            continue;
                        }
                        match String::from_utf8(bytes) {
                            Ok(line) => {
                                if sender.send(ReaderEvent::Line(line)).is_err() {
                                    return;
                                }
                            }
                            Err(_) => {
                                let _ = sender.send(ReaderEvent::Error(CodexHistoryError::new(
                                    "invalid_encoding",
                                    "Codex 官方接口返回了无效文本",
                                )));
                                return;
                            }
                        }
                    }
                    Err(_) => {
                        let _ = sender.send(ReaderEvent::Error(CodexHistoryError::new(
                            "read_failed",
                            "Codex 官方接口读取失败",
                        )));
                        return;
                    }
                }
            }
        });
        Ok(Self {
            child,
            stdin: Some(stdin),
            receiver: Some(receiver),
            reader: Some(reader),
        })
    }

    pub(crate) fn send(&mut self, value: &Value) -> Result<(), CodexHistoryError> {
        let stdin = self
            .stdin
            .as_mut()
            .ok_or_else(|| CodexHistoryError::new("write_failed", "Codex 官方接口输入已关闭"))?;
        serde_json::to_writer(&mut *stdin, value)
            .map_err(|_| CodexHistoryError::new("request_failed", "无法生成 Codex 官方接口请求"))?;
        stdin
            .write_all(b"\n")
            .and_then(|_| stdin.flush())
            .map_err(|_| CodexHistoryError::new("write_failed", "Codex 官方接口请求发送失败"))
    }

    pub(crate) fn wait_for(
        &self,
        ids: &[i64],
        deadline: Instant,
    ) -> Result<Vec<Value>, CodexHistoryError> {
        let receiver = self
            .receiver
            .as_ref()
            .ok_or_else(|| CodexHistoryError::new("provider_exited", "Codex 官方接口连接已关闭"))?;
        let mut responses: Vec<Option<Value>> = vec![None; ids.len()];
        while responses.iter().any(Option::is_none) {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err(CodexHistoryError::new("timeout", "Codex 官方接口读取超时"));
            }
            match receiver.recv_timeout(remaining) {
                Ok(ReaderEvent::Line(line)) => {
                    let value: Value = serde_json::from_str(&line).map_err(|_| {
                        CodexHistoryError::new("protocol_changed", "Codex 官方接口响应格式已变化")
                    })?;
                    let Some(id) = value.get("id").and_then(Value::as_i64) else {
                        continue;
                    };
                    if let Some(index) = ids.iter().position(|candidate| *candidate == id) {
                        responses[index] = Some(value);
                    }
                }
                Ok(ReaderEvent::Error(error)) => return Err(error),
                Ok(ReaderEvent::Eof) => {
                    return Err(CodexHistoryError::new(
                        "provider_exited",
                        "Codex 官方接口提前退出",
                    ))
                }
                Err(RecvTimeoutError::Timeout) => {
                    return Err(CodexHistoryError::new("timeout", "Codex 官方接口读取超时"))
                }
                Err(RecvTimeoutError::Disconnected) => {
                    return Err(CodexHistoryError::new(
                        "provider_exited",
                        "Codex 官方接口连接已关闭",
                    ))
                }
            }
        }
        Ok(responses.into_iter().flatten().collect())
    }

    pub(crate) fn receive(&self, timeout: Duration) -> Result<Option<Value>, CodexHistoryError> {
        let receiver = self
            .receiver
            .as_ref()
            .ok_or_else(|| CodexHistoryError::new("provider_exited", "Codex 官方接口连接已关闭"))?;
        match receiver.recv_timeout(timeout) {
            Ok(ReaderEvent::Line(line)) => serde_json::from_str(&line).map(Some).map_err(|_| {
                CodexHistoryError::new("protocol_changed", "Codex 官方接口响应格式已变化")
            }),
            Ok(ReaderEvent::Error(error)) => Err(error),
            Ok(ReaderEvent::Eof) => Err(CodexHistoryError::new(
                "provider_exited",
                "Codex 官方接口提前退出",
            )),
            Err(RecvTimeoutError::Timeout) => Ok(None),
            Err(RecvTimeoutError::Disconnected) => Err(CodexHistoryError::new(
                "provider_exited",
                "Codex 官方接口连接已关闭",
            )),
        }
    }

    pub(crate) fn stop(mut self) {
        self.stdin.take();
        self.receiver.take();
        let _ = self.child.kill();
        let _ = self.child.wait();
        if let Some(reader) = self.reader.take() {
            let _ = reader.join();
        }
    }
}

impl Drop for AppServer {
    fn drop(&mut self) {
        self.stdin.take();
        self.receiver.take();
        let _ = self.child.kill();
        let _ = self.child.wait();
        if let Some(reader) = self.reader.take() {
            let _ = reader.join();
        }
    }
}

pub(crate) fn validate_thread_id(thread_id: &str) -> Result<(), CodexHistoryError> {
    if thread_id.is_empty()
        || thread_id.len() > 128
        || !thread_id
            .chars()
            .all(|value| value.is_ascii_alphanumeric() || matches!(value, '-' | '_'))
    {
        return Err(CodexHistoryError::new(
            "invalid_thread_id",
            "Codex 任务编号格式无效",
        ));
    }
    Ok(())
}

fn validate_cursor(cursor: Option<&str>) -> Result<(), CodexHistoryError> {
    if cursor.is_some_and(|value| value.len() > MAX_CURSOR_BYTES) {
        return Err(CodexHistoryError::new(
            "invalid_cursor",
            "Codex 历史游标超过安全上限",
        ));
    }
    Ok(())
}

fn source_kinds(source_group: &str) -> Result<Vec<&'static str>, CodexHistoryError> {
    let values = match source_group {
        "all" => ALL_SOURCE_KINDS.to_vec(),
        "interactive" => vec!["cli", "vscode"],
        "automation" => vec!["exec", "appServer"],
        "subagents" => vec![
            "subAgent",
            "subAgentReview",
            "subAgentCompact",
            "subAgentThreadSpawn",
            "subAgentOther",
        ],
        _ => {
            return Err(CodexHistoryError::new(
                "invalid_source_group",
                "Codex 任务来源筛选无效",
            ))
        }
    };
    Ok(values)
}

fn validated_search_term(value: Option<&str>) -> Result<Option<String>, CodexHistoryError> {
    let Some(value) = value.map(str::trim).filter(|value| !value.is_empty()) else {
        return Ok(None);
    };
    if value.len() > MAX_SEARCH_BYTES
        || value.chars().count() > MAX_SEARCH_CHARS
        || value.chars().any(char::is_control)
    {
        return Err(CodexHistoryError::new(
            "invalid_search_term",
            "Codex 任务搜索内容无效或过长",
        ));
    }
    Ok(Some(value.to_string()))
}

fn thread_list_request_value(request: &CodexThreadListRequest) -> Result<Value, CodexHistoryError> {
    validate_cursor(request.cursor.as_deref())?;
    let kinds = source_kinds(&request.source_group)?;
    let search_term = validated_search_term(request.search_term.as_deref())?;
    Ok(json!({
        "id": 2,
        "method": "thread/list",
        "params": {
            "cursor": request.cursor,
            "limit": THREAD_LIST_PAGE_SIZE,
            "sortKey": "recency_at",
            "sortDirection": "desc",
            "sourceKinds": kinds,
            "archived": request.archived,
            "useStateDbOnly": false,
            "searchTerm": search_term
        }
    }))
}

fn path_is_executable(path: &Path) -> bool {
    let Ok(metadata) = fs::metadata(path) else {
        return false;
    };
    if !metadata.is_file() {
        return false;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        metadata.permissions().mode() & 0o111 != 0
    }
    #[cfg(not(unix))]
    {
        true
    }
}

pub(crate) fn codex_executable() -> Result<PathBuf, CodexHistoryError> {
    if let Some(configured) = env::var_os("CODEX_TASK_CENTER_CODEX_EXECUTABLE") {
        let path = PathBuf::from(configured);
        return if path_is_executable(&path) {
            Ok(path)
        } else {
            Err(CodexHistoryError::new(
                "provider_unavailable",
                "已配置的 Codex 官方接口不可执行",
            ))
        };
    }

    let mut candidates = Vec::new();
    #[cfg(target_os = "macos")]
    {
        candidates.push(PathBuf::from(
            "/Applications/ChatGPT.app/Contents/Resources/codex",
        ));
        candidates.push(PathBuf::from(
            "/Applications/Codex.app/Contents/Resources/codex",
        ));
        if let Some(home) = env::var_os("HOME") {
            let home = PathBuf::from(home);
            candidates.push(home.join("Applications/ChatGPT.app/Contents/Resources/codex"));
            candidates.push(home.join("Applications/Codex.app/Contents/Resources/codex"));
        }
    }
    #[cfg(windows)]
    {
        if let Some(local) = env::var_os("LOCALAPPDATA") {
            candidates.push(PathBuf::from(local).join("Programs/Codex/codex.exe"));
        }
        if let Some(program_files) = env::var_os("ProgramFiles") {
            candidates.push(PathBuf::from(program_files).join("Codex/codex.exe"));
        }
    }
    if let Some(path) = env::var_os("PATH") {
        for directory in env::split_paths(&path) {
            #[cfg(windows)]
            candidates.push(directory.join("codex.exe"));
            #[cfg(not(windows))]
            candidates.push(directory.join("codex"));
        }
    }
    candidates
        .into_iter()
        .find(|path| path_is_executable(path))
        .ok_or_else(|| CodexHistoryError::new("provider_unavailable", "未找到本机 Codex 官方接口"))
}

fn response_error(value: &Value) -> Option<(i64, String)> {
    let error = value.get("error")?;
    Some((
        error.get("code").and_then(Value::as_i64).unwrap_or(0),
        error
            .get("message")
            .and_then(Value::as_str)
            .unwrap_or("Codex 官方接口返回错误")
            .to_string(),
    ))
}

pub(crate) fn checked_result(value: &Value) -> Result<&Value, CodexHistoryError> {
    if let Some((code, message)) = response_error(value) {
        let category = if code == -32601 {
            "method_unsupported"
        } else if message.to_ascii_lowercase().contains("not found") {
            "thread_not_found"
        } else {
            "provider_error"
        };
        let user_message = match category {
            "method_unsupported" => "当前 Codex 版本不支持该历史接口",
            "thread_not_found" => "没有找到该 Codex 任务历史",
            _ => "Codex 官方接口未能读取该任务",
        };
        return Err(CodexHistoryError::new(category, user_message));
    }
    value
        .get("result")
        .ok_or_else(|| CodexHistoryError::new("protocol_changed", "Codex 官方接口响应缺少结果字段"))
}

fn sanitized_kind(value: &str) -> String {
    value
        .chars()
        .filter(|character| character.is_ascii_alphanumeric() || matches!(character, '-' | '_'))
        .take(48)
        .collect()
}

fn source_descriptor(value: Option<&Value>) -> (String, String) {
    let Some(value) = value else {
        return ("unknown".to_string(), "未知来源".to_string());
    };
    if let Some(source) = value.as_str() {
        let label = match source {
            "cli" => "Codex 命令行",
            "vscode" => "Codex 桌面版或编辑器",
            "exec" => "Codex 批处理",
            "mcp" => "Codex MCP",
            "appServer" => "Codex App Server",
            "subAgent" => "Codex 子智能体",
            "subAgentReview" => "Codex 子智能体审查",
            "subAgentCompact" => "Codex 记忆整理",
            "subAgentThreadSpawn" => "Codex 子任务启动",
            "subAgentOther" => "Codex 其他子智能体",
            "unknown" => "未知来源",
            _ => "其他 Codex 来源",
        };
        let kind = sanitized_kind(source);
        return (
            if kind.is_empty() {
                "unknown".to_string()
            } else {
                kind
            },
            label.to_string(),
        );
    }
    if let Some(source) = value.get("subagent").or_else(|| value.get("subAgent")) {
        let (kind, label) = match source.as_str() {
            Some("review") => ("subAgentReview", "Codex 子智能体审查"),
            Some("compact" | "memory_consolidation") => ("subAgentCompact", "Codex 记忆整理"),
            _ if source.get("thread_spawn").is_some() => {
                ("subAgentThreadSpawn", "Codex 子任务启动")
            }
            _ if source.get("other").is_some() => ("subAgentOther", "Codex 其他子智能体"),
            _ => ("subAgent", "Codex 子智能体"),
        };
        return (kind.to_string(), label.to_string());
    }
    if value.get("internal").is_some() {
        return ("subAgentCompact".to_string(), "Codex 记忆整理".to_string());
    }
    if value.get("custom").is_some() {
        return ("custom".to_string(), "自定义 Codex 来源".to_string());
    }
    ("unknown".to_string(), "其他 Codex 来源".to_string())
}

fn status_kind(value: Option<&Value>) -> String {
    let raw = value
        .and_then(|status| status.get("type").or(Some(status)))
        .and_then(Value::as_str)
        .unwrap_or("unknown");
    let sanitized = sanitized_kind(raw);
    if sanitized.is_empty() {
        "unknown".to_string()
    } else {
        sanitized
    }
}

pub(crate) fn bounded_text(value: &str, limit: usize) -> Option<String> {
    let text: String = value
        .chars()
        .filter(|character| !character.is_control())
        .take(limit)
        .collect();
    let text = text.trim();
    (!text.is_empty()).then(|| text.to_string())
}

fn workspace_name(value: Option<&Value>) -> Option<String> {
    let path = value.and_then(Value::as_str)?;
    Path::new(path)
        .file_name()
        .and_then(|name| name.to_str())
        .and_then(|name| bounded_text(name, 96))
}

pub(crate) fn observed_at() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs() as i64)
        .unwrap_or_default()
}

fn parse_thread_list(
    response: &Value,
    archived: bool,
) -> Result<CodexThreadListPage, CodexHistoryError> {
    let result = checked_result(response)?;
    let data = result
        .get("data")
        .and_then(Value::as_array)
        .ok_or_else(|| CodexHistoryError::new("protocol_changed", "Codex 任务列表格式已变化"))?;
    let mut threads = Vec::with_capacity(data.len());
    for thread in data {
        let Some(thread_id) = thread.get("id").and_then(Value::as_str) else {
            continue;
        };
        if validate_thread_id(thread_id).is_err() {
            continue;
        }
        let (source_kind, source_label) = source_descriptor(thread.get("source"));
        threads.push(CodexThreadSummary {
            thread_id: thread_id.to_string(),
            name: thread
                .get("name")
                .and_then(Value::as_str)
                .and_then(|name| bounded_text(name, 240)),
            source_kind,
            source_label,
            reported_status: status_kind(thread.get("status")),
            created_at: thread.get("createdAt").and_then(Value::as_i64),
            updated_at: thread.get("updatedAt").and_then(Value::as_i64),
            workspace_name: workspace_name(thread.get("cwd")),
            is_pinned: thread
                .get("isPinned")
                .and_then(Value::as_bool)
                .unwrap_or(false),
            archived,
        });
    }
    let next_cursor = result
        .get("nextCursor")
        .and_then(Value::as_str)
        .filter(|value| value.len() <= MAX_CURSOR_BYTES)
        .map(str::to_string);
    Ok(CodexThreadListPage {
        threads,
        next_cursor,
        observed_at: observed_at(),
    })
}

fn parse_thread_page(
    thread_id: &str,
    thread_response: &Value,
    turns_response: &Value,
) -> Result<CodexThreadPage, CodexHistoryError> {
    let thread_result = checked_result(thread_response)?;
    let thread = thread_result
        .get("thread")
        .ok_or_else(|| CodexHistoryError::new("protocol_changed", "Codex 任务响应缺少任务字段"))?;
    if thread.get("id").and_then(Value::as_str) != Some(thread_id) {
        return Err(CodexHistoryError::new(
            "protocol_changed",
            "Codex 任务响应编号不一致",
        ));
    }
    let (source_kind, source_label) = source_descriptor(thread.get("source"));
    let mut turns = Vec::new();
    let mut next_cursor = None;
    let (history_state, history_message) =
        if let Some((code, message)) = response_error(turns_response) {
            let unsupported =
                code == -32601 || code == -32600 || message.contains("experimentalApi capability");
            if unsupported {
                (
                    "unsupported".to_string(),
                    Some(
                        "当前 Codex 版本未开放按需分页；为保持低负担，没有一次性读取完整历史。"
                            .to_string(),
                    ),
                )
            } else {
                (
                    "error".to_string(),
                    Some("本轮历史分页读取失败，任务元数据仍可查看。".to_string()),
                )
            }
        } else {
            let result = turns_response.get("result").ok_or_else(|| {
                CodexHistoryError::new("protocol_changed", "Codex 历史响应缺少结果字段")
            })?;
            let data = result
                .get("data")
                .and_then(Value::as_array)
                .ok_or_else(|| {
                    CodexHistoryError::new("protocol_changed", "Codex 历史分页格式已变化")
                })?;
            for turn in data {
                let Some(id) = turn.get("id").and_then(Value::as_str) else {
                    continue;
                };
                turns.push(CodexTurnSummary {
                    id: id.to_string(),
                    status: status_kind(turn.get("status")),
                    started_at: turn.get("startedAt").and_then(Value::as_i64),
                    completed_at: turn.get("completedAt").and_then(Value::as_i64),
                    duration_ms: turn.get("durationMs").and_then(Value::as_i64),
                });
            }
            next_cursor = result
                .get("nextCursor")
                .and_then(Value::as_str)
                .filter(|value| value.len() <= MAX_CURSOR_BYTES)
                .map(str::to_string);
            ("paged".to_string(), None)
        };
    Ok(CodexThreadPage {
        thread_id: thread_id.to_string(),
        name: thread
            .get("name")
            .and_then(Value::as_str)
            .map(str::to_string),
        source_kind,
        source_label,
        reported_status: status_kind(thread.get("status")),
        created_at: thread.get("createdAt").and_then(Value::as_i64),
        updated_at: thread.get("updatedAt").and_then(Value::as_i64),
        history_mode: thread
            .get("historyMode")
            .and_then(Value::as_str)
            .map(str::to_string),
        turns,
        next_cursor,
        history_state,
        history_message,
        observed_at: observed_at(),
    })
}

pub(crate) fn load_thread_list(
    request: CodexThreadListRequest,
) -> Result<CodexThreadListPage, CodexHistoryError> {
    let list_request = thread_list_request_value(&request)?;
    let executable = codex_executable()?;
    let mut server = AppServer::start(&executable)?;
    let deadline = Instant::now() + RESPONSE_TIMEOUT;
    let result = (|| {
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
                    "experimentalApi": false,
                    "optOutNotificationMethods": ["remoteControl/status/changed"]
                }
            }
        }))?;
        let initialize = server.wait_for(&[1], deadline)?;
        checked_result(&initialize[0])?;
        server.send(&json!({"method": "initialized", "params": {}}))?;
        server.send(&list_request)?;
        let responses = server.wait_for(&[2], deadline)?;
        parse_thread_list(&responses[0], request.archived)
    })();
    server.stop();
    result
}

pub(crate) fn load_thread_page(
    thread_id: &str,
    cursor: Option<&str>,
) -> Result<CodexThreadPage, CodexHistoryError> {
    validate_thread_id(thread_id)?;
    validate_cursor(cursor)?;
    let executable = codex_executable()?;
    let mut server = AppServer::start(&executable)?;
    let deadline = Instant::now() + RESPONSE_TIMEOUT;
    let result = (|| {
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
                    "optOutNotificationMethods": ["remoteControl/status/changed"]
                }
            }
        }))?;
        let initialize = server.wait_for(&[1], deadline)?;
        checked_result(&initialize[0])?;
        server.send(&json!({"method": "initialized", "params": {}}))?;
        server.send(&json!({
            "id": 2,
            "method": "thread/read",
            "params": {"threadId": thread_id, "includeTurns": false}
        }))?;
        server.send(&json!({
            "id": 3,
            "method": "thread/turns/list",
            "params": {
                "threadId": thread_id,
                "cursor": cursor,
                "limit": TURN_PAGE_SIZE,
                "sortDirection": "desc",
                "itemsView": "notLoaded"
            }
        }))?;
        let responses = server.wait_for(&[2, 3], deadline)?;
        parse_thread_page(thread_id, &responses[0], &responses[1])
    })();
    server.stop();
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    fn thread_response(source: Value) -> Value {
        json!({
            "id": 2,
            "result": {
                "thread": {
                    "id": "thread_test",
                    "name": "合成任务",
                    "source": source,
                    "status": {"type": "notLoaded"},
                    "createdAt": 10,
                    "updatedAt": 20,
                    "historyMode": "legacy"
                }
            }
        })
    }

    #[test]
    fn thread_list_exposes_only_bounded_metadata() {
        let page = parse_thread_list(
            &json!({
                "id": 2,
                "result": {
                    "data": [{
                        "id": "thread_public",
                        "name": "公开标题",
                        "preview": "绝不能跨过 Rust 边界的对话预览",
                        "cwd": "/Users/example/private-project",
                        "source": "vscode",
                        "status": {"type": "notLoaded"},
                        "createdAt": 10,
                        "updatedAt": 20,
                        "isPinned": true
                    }],
                    "nextCursor": "next-page"
                }
            }),
            false,
        )
        .unwrap();
        assert_eq!(page.threads.len(), 1);
        assert_eq!(page.threads[0].name.as_deref(), Some("公开标题"));
        assert_eq!(
            page.threads[0].workspace_name.as_deref(),
            Some("private-project")
        );
        assert_eq!(page.next_cursor.as_deref(), Some("next-page"));
        let serialized = serde_json::to_string(&page).unwrap();
        assert!(!serialized.contains("绝不能"));
        assert!(!serialized.contains("/Users/example"));
    }

    #[test]
    fn thread_list_skips_invalid_ids_and_accepts_future_sources() {
        let page = parse_thread_list(
            &json!({
                "id": 2,
                "result": {
                    "data": [
                        {"id": "../unsafe", "source": "cli"},
                        {"id": "thread_future", "source": "futureAgent", "status": "futureStatus"}
                    ],
                    "nextCursor": null
                }
            }),
            false,
        )
        .unwrap();
        assert_eq!(page.threads.len(), 1);
        assert_eq!(page.threads[0].source_kind, "futureAgent");
        assert_eq!(page.threads[0].reported_status, "futureStatus");
    }

    #[test]
    fn parses_cursor_page_without_message_content() {
        let page = parse_thread_page(
            "thread_test",
            &thread_response(json!("vscode")),
            &json!({
                "id": 3,
                "result": {
                    "data": [{
                        "id": "turn_1",
                        "status": "completed",
                        "startedAt": 11,
                        "completedAt": 12,
                        "durationMs": 1000,
                        "items": []
                    }],
                    "nextCursor": "opaque-cursor"
                }
            }),
        )
        .unwrap();
        assert_eq!(page.source_kind, "vscode");
        assert_eq!(page.source_label, "Codex 桌面版或编辑器");
        assert_eq!(page.turns.len(), 1);
        assert_eq!(page.turns[0].status, "completed");
        assert_eq!(page.next_cursor.as_deref(), Some("opaque-cursor"));
        assert_eq!(page.history_state, "paged");
    }

    #[test]
    fn accepts_subagent_and_future_sources_without_closed_enum() {
        let subagent = source_descriptor(Some(&json!({"subagent": {"other": "future"}})));
        let review = source_descriptor(Some(&json!({"subagent": "review"})));
        let future = source_descriptor(Some(&json!("guardian_review_future")));
        assert_eq!(
            subagent,
            (
                "subAgentOther".to_string(),
                "Codex 其他子智能体".to_string()
            )
        );
        assert_eq!(review.0, "subAgentReview");
        assert_eq!(future.0, "guardian_review_future");
        assert_eq!(future.1, "其他 Codex 来源");
    }

    #[test]
    fn unsupported_pagination_keeps_metadata_and_does_not_fallback_to_full_history() {
        let page = parse_thread_page(
            "thread_test",
            &thread_response(json!("cli")),
            &json!({
                "id": 3,
                "error": {"code": -32600, "message": "requires experimentalApi capability"}
            }),
        )
        .unwrap();
        assert_eq!(page.history_state, "unsupported");
        assert!(page.turns.is_empty());
        assert!(page.next_cursor.is_none());
        assert!(page
            .history_message
            .unwrap()
            .contains("没有一次性读取完整历史"));
    }

    #[test]
    fn rejects_unbounded_inputs() {
        assert_eq!(
            validate_thread_id("thread/../../secret").unwrap_err().code,
            "invalid_thread_id"
        );
        let long_cursor = "x".repeat(MAX_CURSOR_BYTES + 1);
        assert_eq!(
            validate_cursor(Some(&long_cursor)).unwrap_err().code,
            "invalid_cursor"
        );
    }

    #[test]
    fn thread_list_request_uses_official_search_source_archive_and_cursor_fields() {
        let value = thread_list_request_value(&CodexThreadListRequest {
            cursor: Some("opaque-cursor".to_string()),
            archived: true,
            source_group: "subagents".to_string(),
            search_term: Some("  目标审查  ".to_string()),
        })
        .unwrap();
        assert_eq!(value["method"], "thread/list");
        assert_eq!(value["params"]["cursor"], "opaque-cursor");
        assert_eq!(value["params"]["archived"], true);
        assert_eq!(value["params"]["useStateDbOnly"], false);
        assert_eq!(value["params"]["searchTerm"], "目标审查");
        assert_eq!(
            value["params"]["sourceKinds"],
            json!([
                "subAgent",
                "subAgentReview",
                "subAgentCompact",
                "subAgentThreadSpawn",
                "subAgentOther"
            ])
        );
    }

    #[test]
    fn thread_list_request_rejects_unknown_groups_and_unbounded_search() {
        let unknown = thread_list_request_value(&CodexThreadListRequest {
            cursor: None,
            archived: false,
            source_group: "future-group".to_string(),
            search_term: None,
        })
        .unwrap_err();
        assert_eq!(unknown.code, "invalid_source_group");
        let too_long = thread_list_request_value(&CodexThreadListRequest {
            cursor: None,
            archived: false,
            source_group: "all".to_string(),
            search_term: Some("x".repeat(MAX_SEARCH_CHARS + 1)),
        })
        .unwrap_err();
        assert_eq!(too_long.code, "invalid_search_term");
    }

    #[test]
    #[ignore = "requires an installed, logged-in Codex and an explicit local thread id"]
    fn live_official_history_smoke() {
        let thread_id = env::var("CODEX_TASK_CENTER_LIVE_THREAD_ID")
            .expect("set CODEX_TASK_CENTER_LIVE_THREAD_ID for the live smoke test");
        let page = load_thread_page(&thread_id, None).expect("official history page");
        assert_eq!(page.thread_id, thread_id);
        assert!(matches!(
            page.history_state.as_str(),
            "paged" | "unsupported" | "error"
        ));
    }

    #[test]
    #[ignore = "requires an installed and logged-in Codex"]
    fn live_official_thread_list_smoke() {
        let page = load_thread_list(CodexThreadListRequest {
            cursor: None,
            archived: false,
            source_group: "all".to_string(),
            search_term: None,
        })
        .expect("official thread list page");
        assert!(page.threads.len() <= THREAD_LIST_PAGE_SIZE as usize);
    }
}
