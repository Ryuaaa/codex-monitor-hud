mod codex_history;

use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
    env,
    fs::{self, File, OpenOptions},
    io::{BufRead, BufReader, Read, Write},
    path::{Path, PathBuf},
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tauri::Manager;
use tauri_plugin_updater::UpdaterExt;
use time::{format_description::well_known::Rfc3339, Date, Month, OffsetDateTime, UtcOffset};

const MAX_FRONTMATTER_BYTES: usize = 128 * 1024;
const MAX_BODY_BYTES: u64 = 2 * 1024 * 1024;
const MAX_EVENT_FILE_BYTES: u64 = 8 * 1024 * 1024;
const MAX_FILTER_FILE_BYTES: u64 = 128 * 1024;
const MAX_TAGS: usize = 32;
const MAX_RELATIONS: usize = 64;
const MAX_NOTE_CHARS: usize = 2_000;
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
    #[serde(skip_serializing_if = "Option::is_none")]
    message: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    author: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct SavedTaskFilter {
    id: String,
    name: String,
    project_id: String,
    status: String,
    tag: String,
    show_archived: bool,
    view: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct SavedTaskFilterDraft {
    id: Option<String>,
    name: String,
    project_id: String,
    status: String,
    tag: String,
    show_archived: bool,
    view: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
struct SavedTaskFilterFile {
    version: u32,
    filters: Vec<SavedTaskFilter>,
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

#[derive(Debug, Clone, PartialEq, Eq)]
struct AtomicWriteReceipt {
    file_hash: String,
    event_id: String,
    event_file: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CommitFault {
    None,
    #[cfg(test)]
    BeforeTaskCommitExternalChange,
    #[cfg(test)]
    BeforeEventCommitExternalChange,
    AfterTaskCommit,
    AfterEventCommit,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct TaskFieldEditRequest {
    file_token: String,
    field: String,
    new_value: Value,
    expected_hash: String,
    confirmed: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
struct TaskFieldEditPreview {
    file_token: String,
    task_id: String,
    field: String,
    before_value: Value,
    after_value: Value,
    expected_hash: String,
}

#[derive(Debug, Clone, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
struct TaskFieldEditReceipt {
    file_token: String,
    task_id: String,
    field: String,
    previous_value: Value,
    new_value: Value,
    file_hash: String,
    event_id: String,
    event_file: String,
    verified: bool,
}

#[derive(Debug, Clone)]
struct PreparedFieldEdit {
    yaml_value: String,
    before_value: Value,
    after_value: Value,
    event_type: &'static str,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct NewTaskDraft {
    title: String,
    domain: String,
    task_status: String,
    priority: String,
    assignee: String,
    deadline: String,
    tags: Vec<String>,
    parent_id: String,
    blocked_by_ids: Vec<String>,
    related_ids: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct CreateTaskPreview {
    draft: NewTaskDraft,
    task_id: String,
    file_token: String,
    created_at: String,
    occurred_at: String,
    expected_hash: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct CreateTaskRequest {
    preview: CreateTaskPreview,
    confirmed: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct CreateTaskReceipt {
    task_id: String,
    file_token: String,
    file_hash: String,
    event_id: String,
    event_file: String,
    verified: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct TaskNotePreview {
    file_token: String,
    task_id: String,
    kind: String,
    text: String,
    author: String,
    occurred_at: String,
    expected_task_hash: String,
    expected_event_hash: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct TaskNoteRequest {
    file_token: String,
    task_id: String,
    kind: String,
    text: String,
    author: String,
    occurred_at: String,
    expected_task_hash: String,
    expected_event_hash: String,
    confirmed: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct TaskNoteReceipt {
    task_id: String,
    event_id: String,
    event_file: String,
    verified: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct TaskCenterUpdateInfo {
    current_version: String,
    available: bool,
    version: Option<String>,
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
    if home.as_os_str().is_empty() {
        return PathBuf::new();
    }
    let existing_personal_root = home
        .join("Documents")
        .join("01-小烈刀-AI协作库-默认可读")
        .join("30-领域与项目")
        .join("任务中枢")
        .join("任务");
    if existing_personal_root.is_dir() {
        return existing_personal_root;
    }
    #[cfg(target_os = "macos")]
    let data_root = home
        .join("Library")
        .join("Application Support")
        .join("CodexMonitorTaskCenter");
    #[cfg(windows)]
    let data_root = env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|| home.join("AppData").join("Local"))
        .join("CodexMonitorTaskCenter");
    #[cfg(not(any(target_os = "macos", windows)))]
    let data_root = env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| home.join(".local").join("share"))
        .join("codex-monitor-task-center");
    data_root.join("任务")
}

fn initialize_task_library_at(root: &Path) -> Result<(), String> {
    if root.file_name().and_then(|name| name.to_str()) != Some("任务") {
        return Err("任务库目录必须明确指向“任务”文件夹".to_string());
    }
    if root.exists() && !root.is_dir() {
        return Err("任务库目标已存在但不是文件夹".to_string());
    }
    if let Ok(metadata) = fs::symlink_metadata(root) {
        if metadata.file_type().is_symlink() {
            return Err("任务库目标不能是符号链接".to_string());
        }
    }
    fs::create_dir_all(root).map_err(|_| "无法创建本地任务目录".to_string())?;
    let event_root = events_root(root);
    fs::create_dir_all(&event_root).map_err(|_| "无法创建本地事件目录".to_string())?;
    root.canonicalize()
        .map_err(|_| "新建任务目录无法核对".to_string())?;
    event_root
        .canonicalize()
        .map_err(|_| "新建事件目录无法核对".to_string())?;
    Ok(())
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
            let note_event = matches!(event_type, "comment_added" | "manual_activity_added");
            let message = note_event
                .then(|| value.get("message").and_then(Value::as_str))
                .flatten()
                .filter(|message| {
                    message.chars().count() <= MAX_NOTE_CHARS && !message.contains('\0')
                })
                .map(str::to_string);
            let author = note_event
                .then(|| value.get("author").and_then(Value::as_str))
                .flatten()
                .filter(|author| {
                    author.chars().count() <= 120 && !author.contains(['\r', '\n', '\0'])
                })
                .map(str::to_string);
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
                message,
                author,
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

fn validate_saved_filter(filter: &SavedTaskFilter) -> Result<(), String> {
    let valid_status = [
        "all",
        "todo",
        "doing",
        "long_term",
        "done",
        "cancelled",
        "unknown",
    ];
    if filter.id.is_empty()
        || filter.id.len() > 96
        || !filter
            .id
            .chars()
            .all(|character| character.is_ascii_alphanumeric() || matches!(character, '-' | '_'))
        || filter.name.trim().is_empty()
        || filter.name.chars().count() > 60
        || filter.name.contains(['\r', '\n', '\0'])
        || filter.project_id.chars().count() > 160
        || filter.project_id.contains(['\r', '\n', '\0'])
        || !valid_status.contains(&filter.status.as_str())
        || filter.tag.chars().count() > 40
        || filter.tag.contains(['\r', '\n', '\0'])
        || !matches!(filter.view.as_str(), "board" | "list")
    {
        return Err("保存的筛选条件无效".to_string());
    }
    Ok(())
}

fn read_saved_filters_at(path: &Path) -> Result<Vec<SavedTaskFilter>, String> {
    if !path.exists() {
        return Ok(Vec::new());
    }
    let metadata = fs::symlink_metadata(path).map_err(|_| "保存筛选配置不可用".to_string())?;
    if metadata.file_type().is_symlink()
        || !metadata.is_file()
        || metadata.len() > MAX_FILTER_FILE_BYTES
    {
        return Err("保存筛选配置不可安全读取".to_string());
    }
    let file: SavedTaskFilterFile =
        serde_json::from_reader(File::open(path).map_err(|_| "保存筛选配置不可用".to_string())?)
            .map_err(|_| "保存筛选配置格式错误".to_string())?;
    if file.version != 1 {
        return Err("保存筛选配置版本暂不支持".to_string());
    }
    if file.filters.len() > 32 {
        return Err("保存筛选数量超过安全上限".to_string());
    }
    let mut ids = Vec::new();
    for filter in &file.filters {
        validate_saved_filter(filter)?;
        if ids.iter().any(|id| id == &filter.id) {
            return Err("保存筛选编号重复".to_string());
        }
        ids.push(filter.id.clone());
    }
    Ok(file.filters)
}

fn write_saved_filters_at(path: &Path, filters: &[SavedTaskFilter]) -> Result<(), String> {
    if filters.len() > 32 {
        return Err("最多保存32个筛选方案".to_string());
    }
    for filter in filters {
        validate_saved_filter(filter)?;
    }
    let bytes = serde_json::to_vec_pretty(&SavedTaskFilterFile {
        version: 1,
        filters: filters.to_vec(),
    })
    .map_err(|_| "保存筛选配置序列化失败".to_string())?;
    if bytes.len() as u64 > MAX_FILTER_FILE_BYTES {
        return Err("保存筛选配置超过安全上限".to_string());
    }
    let parent = path
        .parent()
        .ok_or_else(|| "保存筛选目录不可用".to_string())?;
    fs::create_dir_all(parent).map_err(|_| "保存筛选目录不可用".to_string())?;
    if path.exists()
        && fs::symlink_metadata(path)
            .map(|metadata| metadata.file_type().is_symlink() || !metadata.is_file())
            .unwrap_or(true)
    {
        return Err("保存筛选配置目标不安全".to_string());
    }
    let temp = unique_sidecar_path(path, "tmp").map_err(|error| error.message.to_string())?;
    let backup = path
        .exists()
        .then(|| unique_sidecar_path(path, "bak"))
        .transpose()
        .map_err(|error| error.message.to_string())?;
    write_prepared_file(&temp, &bytes, None).map_err(|error| error.message.to_string())?;
    if let Some(backup) = backup.as_deref() {
        fs::rename(path, backup).map_err(|_| {
            let _ = fs::remove_file(&temp);
            "保存筛选配置备份失败".to_string()
        })?;
    }
    if fs::rename(&temp, path).is_err() {
        let _ = fs::remove_file(&temp);
        if let Some(backup) = backup.as_deref() {
            let _ = fs::rename(backup, path);
        }
        return Err("保存筛选配置写入失败".to_string());
    }
    let verified = read_saved_filters_at(path);
    if verified.as_ref().map(Vec::as_slice) != Ok(filters) {
        let _ = fs::remove_file(path);
        if let Some(backup) = backup.as_deref() {
            let _ = fs::rename(backup, path);
        }
        return Err("保存筛选配置回读不一致，已恢复原配置".to_string());
    }
    if let Some(backup) = backup.as_deref() {
        let _ = fs::remove_file(backup);
    }
    Ok(())
}

fn save_task_filter_at(
    path: &Path,
    draft: SavedTaskFilterDraft,
) -> Result<SavedTaskFilter, String> {
    let id = match draft.id.filter(|id| !id.is_empty()) {
        Some(id) => id,
        None => {
            let nonce = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map_err(|_| "系统时间不可用".to_string())?
                .as_nanos();
            format!("filter_{nonce}")
        }
    };
    let saved = SavedTaskFilter {
        id,
        name: draft.name.trim().to_string(),
        project_id: draft.project_id.trim().to_string(),
        status: draft.status,
        tag: draft.tag.trim().to_string(),
        show_archived: draft.show_archived,
        view: draft.view,
    };
    validate_saved_filter(&saved)?;
    let mut filters = read_saved_filters_at(path)?;
    if let Some(index) = filters.iter().position(|filter| filter.id == saved.id) {
        filters[index] = saved.clone();
    } else {
        if filters.len() >= 32 {
            return Err("最多保存32个筛选方案".to_string());
        }
        filters.push(saved.clone());
    }
    write_saved_filters_at(path, &filters)?;
    Ok(saved)
}

fn delete_task_filter_at(path: &Path, id: &str) -> Result<(), String> {
    if id.is_empty() || id.len() > 96 {
        return Err("保存筛选编号无效".to_string());
    }
    let mut filters = read_saved_filters_at(path)?;
    filters.retain(|filter| filter.id != id);
    write_saved_filters_at(path, &filters)
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
    let _: Value = yaml_serde::from_str(&frontmatter)
        .map_err(|_| write_error("invalid_frontmatter", "任务 YAML 格式无效，未执行写入"))?;
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

fn frontmatter_list(frontmatter: &str, key: &str) -> Result<Vec<String>, WriteError> {
    let lines: Vec<&str> = frontmatter.lines().collect();
    for (index, line) in lines.iter().enumerate() {
        let Some((candidate, rest)) = line.split_once(':') else {
            continue;
        };
        if candidate.trim() != key {
            continue;
        }
        let inline = rest.trim();
        if !inline.is_empty() {
            if inline == "[]" {
                return Ok(Vec::new());
            }
            return serde_json::from_str::<Vec<String>>(inline)
                .map_err(|_| write_error("unsupported_format", "列表字段格式暂不支持安全写入"));
        }
        let mut values = Vec::new();
        for child in lines.iter().skip(index + 1) {
            let trimmed = child.trim_start();
            if trimmed.is_empty() {
                continue;
            }
            if child.len() == trimmed.len() {
                break;
            }
            let Some(value) = trimmed.strip_prefix("- ") else {
                return Err(write_error(
                    "unsupported_format",
                    "列表字段格式暂不支持安全写入",
                ));
            };
            values.push(value.trim().trim_matches(['"', '\'']).to_string());
        }
        return Ok(values);
    }
    Ok(Vec::new())
}

fn replace_frontmatter_entry(
    content: &str,
    key: &str,
    yaml_value: &str,
) -> Result<String, WriteError> {
    let (start, end) = frontmatter_range(content)?;
    let frontmatter = &content[start..end];
    let lines: Vec<&str> = frontmatter.split_inclusive('\n').collect();
    let mut output = String::with_capacity(content.len() + yaml_value.len());
    output.push_str(&content[..start]);
    let mut replaced = false;
    let mut index = 0;
    while index < lines.len() {
        let line = lines[index];
        let body = line.trim_end_matches(['\r', '\n']);
        let ending = &line[body.len()..];
        let is_target = body
            .split_once(':')
            .map(|(candidate, _)| candidate.trim() == key)
            .unwrap_or(false);
        if !is_target {
            output.push_str(line);
            index += 1;
            continue;
        }
        if replaced {
            return Err(write_error("duplicate_field", "任务字段重复，未执行写入"));
        }
        let colon = body
            .find(':')
            .ok_or_else(|| write_error("bad_frontmatter", "任务字段格式无效"))?;
        output.push_str(&body[..=colon]);
        if !yaml_value.is_empty() {
            output.push(' ');
            output.push_str(yaml_value);
        }
        output.push_str(ending);
        replaced = true;
        index += 1;
        while index < lines.len() {
            let child_body = lines[index].trim_end_matches(['\r', '\n']);
            let child_trimmed = child_body.trim_start();
            if child_trimmed.is_empty() {
                break;
            }
            if child_trimmed.starts_with("- ") {
                index += 1;
                continue;
            }
            break;
        }
    }
    if !replaced {
        output.push_str(key);
        output.push(':');
        if !yaml_value.is_empty() {
            output.push(' ');
            output.push_str(yaml_value);
        }
        output.push('\n');
    }
    output.push_str(&content[end..]);
    Ok(output)
}

fn validate_plain_text(
    value: &str,
    empty_allowed: bool,
    max_chars: usize,
) -> Result<(), WriteError> {
    let length = value.chars().count();
    if (!empty_allowed && value.trim().is_empty())
        || length > max_chars
        || value.contains(['\r', '\n'])
    {
        return Err(write_error("invalid_value", "字段内容不符合正式结构要求"));
    }
    Ok(())
}

fn valid_task_id(value: &str) -> bool {
    value.starts_with("tsk_") && value.len() <= 128 && !value.contains(['\r', '\n', '\0'])
}

fn normalized_string_list(
    new_value: &Value,
    max_items: usize,
    max_chars: usize,
    item_label: &'static str,
) -> Result<Vec<String>, WriteError> {
    let values = new_value
        .as_array()
        .ok_or_else(|| write_error("invalid_value", "列表字段格式无效"))?;
    if values.len() > max_items {
        return Err(write_error("invalid_value", "列表项数超过安全上限"));
    }
    let mut normalized = Vec::new();
    for value in values {
        let value = value
            .as_str()
            .ok_or_else(|| write_error("invalid_value", "列表字段格式无效"))?
            .trim();
        if value.is_empty()
            || value.chars().count() > max_chars
            || value.contains(['\r', '\n', '\0'])
            || normalized
                .iter()
                .any(|existing: &String| existing.eq_ignore_ascii_case(value))
        {
            let _ = item_label;
            return Err(write_error(
                "invalid_value",
                "列表项为空、重复或超过安全上限",
            ));
        }
        normalized.push(value.to_string());
    }
    Ok(normalized)
}

fn normalized_relation_ids(new_value: &Value, task_id: &str) -> Result<Vec<String>, WriteError> {
    let normalized = normalized_string_list(new_value, MAX_RELATIONS, 128, "任务编号")?;
    if normalized
        .iter()
        .any(|value| !valid_task_id(value) || value == task_id)
    {
        return Err(write_error("invalid_value", "任务关系编号无效或指向自身"));
    }
    Ok(normalized)
}

fn valid_iso_date(value: &str) -> bool {
    let parts: Vec<&str> = value.split('-').collect();
    if parts.len() != 3 {
        return false;
    }
    let Ok(year) = parts[0].parse::<i32>() else {
        return false;
    };
    let Ok(month_number) = parts[1].parse::<u8>() else {
        return false;
    };
    let Ok(day) = parts[2].parse::<u8>() else {
        return false;
    };
    let Ok(month) = Month::try_from(month_number) else {
        return false;
    };
    Date::from_calendar_date(year, month, day).is_ok()
}

fn prepare_field_edit(
    content: &str,
    task_id: &str,
    field: &str,
    new_value: &Value,
) -> Result<PreparedFieldEdit, WriteError> {
    let (start, end) = frontmatter_range(content)?;
    let frontmatter = &content[start..end];
    let scalar_before = |key: &str| {
        Value::String(
            frontmatter_scalar(frontmatter, key)
                .unwrap_or("")
                .to_string(),
        )
    };
    let (yaml_value, before_value, after_value, event_type) = match field {
        "title" => {
            let value = new_value
                .as_str()
                .ok_or_else(|| write_error("invalid_value", "标题格式无效"))?;
            validate_plain_text(value, false, 200)?;
            (
                serde_json::to_string(value).unwrap(),
                scalar_before("title"),
                Value::String(value.to_string()),
                "title_changed",
            )
        }
        "task_status" => {
            let value = new_value
                .as_str()
                .ok_or_else(|| write_error("invalid_value", "任务状态格式无效"))?;
            if !["todo", "doing", "long_term", "done", "cancelled"].contains(&value) {
                return Err(write_error(
                    "unsupported_value",
                    "正式结构暂不支持该任务状态",
                ));
            }
            (
                value.to_string(),
                scalar_before("task_status"),
                Value::String(value.to_string()),
                "status_changed",
            )
        }
        "priority" => {
            let value = new_value
                .as_str()
                .ok_or_else(|| write_error("invalid_value", "优先级格式无效"))?;
            validate_priority(value)?;
            (
                value.to_string(),
                scalar_before("priority"),
                Value::String(value.to_string()),
                "priority_changed",
            )
        }
        "deadline" => {
            let value = new_value
                .as_str()
                .ok_or_else(|| write_error("invalid_value", "截止日期格式无效"))?;
            if !value.is_empty() && !valid_iso_date(value) {
                return Err(write_error(
                    "invalid_value",
                    "截止日期必须是有效的 YYYY-MM-DD",
                ));
            }
            (
                if value.is_empty() {
                    String::new()
                } else {
                    value.to_string()
                },
                scalar_before("deadline"),
                Value::String(value.to_string()),
                "deadline_changed",
            )
        }
        "assignee" => {
            let value = new_value
                .as_str()
                .ok_or_else(|| write_error("invalid_value", "负责人格式无效"))?;
            validate_plain_text(value, true, 120)?;
            (
                if value.is_empty() {
                    String::new()
                } else {
                    serde_json::to_string(value).unwrap()
                },
                scalar_before("assignee"),
                Value::String(value.to_string()),
                "assignee_changed",
            )
        }
        "tags" => {
            let normalized = normalized_string_list(new_value, MAX_TAGS, 40, "标签")?;
            let before = frontmatter_list(frontmatter, "tags")?;
            (
                serde_json::to_string(&normalized).unwrap(),
                serde_json::to_value(before).unwrap(),
                serde_json::to_value(&normalized).unwrap(),
                "tags_changed",
            )
        }
        "parent_id" => {
            let value = new_value
                .as_str()
                .ok_or_else(|| write_error("invalid_value", "父任务编号格式无效"))?
                .trim();
            if !value.is_empty() && (!valid_task_id(value) || value == task_id) {
                return Err(write_error("invalid_value", "父任务编号无效或指向自身"));
            }
            (
                value.to_string(),
                scalar_before("parent_id"),
                Value::String(value.to_string()),
                "parent_changed",
            )
        }
        "blocked_by_ids" => {
            let normalized = normalized_relation_ids(new_value, task_id)?;
            let before = frontmatter_list(frontmatter, "blocked_by_ids")?;
            (
                serde_json::to_string(&normalized).unwrap(),
                serde_json::to_value(before).unwrap(),
                serde_json::to_value(&normalized).unwrap(),
                "blockers_changed",
            )
        }
        "related_ids" => {
            let normalized = normalized_relation_ids(new_value, task_id)?;
            let before = frontmatter_list(frontmatter, "related_ids")?;
            (
                serde_json::to_string(&normalized).unwrap(),
                serde_json::to_value(before).unwrap(),
                serde_json::to_value(&normalized).unwrap(),
                "relations_changed",
            )
        }
        "record_status" => {
            let value = new_value
                .as_str()
                .ok_or_else(|| write_error("invalid_value", "记录状态格式无效"))?;
            if !["current", "archived"].contains(&value) {
                return Err(write_error(
                    "unsupported_value",
                    "正式结构暂不支持该记录状态",
                ));
            }
            let event_type = if value == "archived" {
                "archived"
            } else {
                "restored"
            };
            (
                value.to_string(),
                scalar_before("record_status"),
                Value::String(value.to_string()),
                event_type,
            )
        }
        _ => {
            return Err(write_error(
                "unsupported_field",
                "正式结构暂不支持写入该字段",
            ))
        }
    };
    Ok(PreparedFieldEdit {
        yaml_value,
        before_value,
        after_value,
        event_type,
    })
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
    let task_hub_offset = UtcOffset::from_hms(8, 0, 0)
        .map_err(|_| write_error("clock_error", "任务中枢时区不可用"))?;
    let now = OffsetDateTime::now_utc().to_offset(task_hub_offset);
    let occurred_at = now
        .format(&Rfc3339)
        .map_err(|_| write_error("clock_error", "无法生成事件时间"))?;
    let month = format!("{:04}-{:02}", now.year(), u8::from(now.month()));
    Ok((occurred_at, month))
}

fn event_month_from_timestamp(value: &str) -> Result<String, WriteError> {
    let timestamp = OffsetDateTime::parse(value, &Rfc3339)
        .map_err(|_| write_error("invalid_value", "记录时间格式无效"))?;
    Ok(format!(
        "{:04}-{:02}",
        timestamp.year(),
        u8::from(timestamp.month())
    ))
}

fn normalize_task_note(
    kind: &str,
    text: &str,
    author: &str,
) -> Result<(String, String, &'static str), WriteError> {
    let event_type = match kind {
        "comment" => "comment_added",
        "activity" => "manual_activity_added",
        _ => return Err(write_error("unsupported_value", "记录类型暂不支持")),
    };
    let normalized_text = text.replace("\r\n", "\n").replace('\r', "\n");
    let normalized_text = normalized_text.trim().to_string();
    let normalized_author = author.trim().to_string();
    if normalized_text.is_empty()
        || normalized_text.chars().count() > MAX_NOTE_CHARS
        || normalized_text.contains('\0')
    {
        return Err(write_error("invalid_value", "记录内容为空或超过2000字"));
    }
    validate_plain_text(&normalized_author, false, 120)?;
    Ok((normalized_text, normalized_author, event_type))
}

fn read_event_snapshot(events_root: &Path, event_month: &str) -> Result<Vec<u8>, WriteError> {
    if !events_root.exists() {
        return Ok(Vec::new());
    }
    let canonical_events = events_root
        .canonicalize()
        .map_err(|_| write_error("event_prepare_failed", "事件目录不可用"))?;
    let path = canonical_events.join(format!("{event_month}.jsonl"));
    if !path.exists() {
        return Ok(Vec::new());
    }
    let metadata = fs::symlink_metadata(&path)
        .map_err(|_| write_error("event_prepare_failed", "事件文件不可用"))?;
    if metadata.file_type().is_symlink()
        || !metadata.is_file()
        || metadata.permissions().readonly()
        || metadata.len() > MAX_EVENT_FILE_BYTES
    {
        return Err(write_error("event_prepare_failed", "事件文件不可安全追加"));
    }
    let bytes =
        fs::read(path).map_err(|_| write_error("event_prepare_failed", "事件文件读取失败"))?;
    if !bytes.is_empty() && !bytes.ends_with(b"\n") {
        return Err(write_error("event_prepare_failed", "事件文件末行不完整"));
    }
    Ok(bytes)
}

fn preview_task_note_at(
    root: &Path,
    events_root: &Path,
    file_token: &str,
    kind: &str,
    text: &str,
    author: &str,
    occurred_at: &str,
) -> Result<TaskNotePreview, WriteError> {
    let (_, content) = read_writable_task(root, file_token)?;
    let (task_id, _) = task_id_and_priority(&content)?;
    let (text, author, _) = normalize_task_note(kind, text, author)?;
    let month = event_month_from_timestamp(occurred_at)?;
    let events = read_event_snapshot(events_root, &month)?;
    Ok(TaskNotePreview {
        file_token: file_token.to_string(),
        task_id,
        kind: kind.to_string(),
        text,
        author,
        occurred_at: occurred_at.to_string(),
        expected_task_hash: sha256_hex(content.as_bytes()),
        expected_event_hash: sha256_hex(&events),
    })
}

fn apply_task_note_at(
    root: &Path,
    events_root: &Path,
    request: TaskNoteRequest,
) -> Result<TaskNoteReceipt, WriteError> {
    if !request.confirmed {
        return Err(write_error(
            "confirmation_required",
            "用户未确认，未追加记录",
        ));
    }
    let (text, author, event_type) =
        normalize_task_note(&request.kind, &request.text, &request.author)?;
    if text != request.text || author != request.author {
        return Err(write_error(
            "preview_mismatch",
            "记录预览已变化，请重新确认",
        ));
    }
    let (_, content) = read_writable_task(root, &request.file_token)?;
    let (task_id, _) = task_id_and_priority(&content)?;
    if task_id != request.task_id || sha256_hex(content.as_bytes()) != request.expected_task_hash {
        return Err(write_error(
            "conflict",
            "任务已被其他操作修改，请重新读取后确认",
        ));
    }
    let month = event_month_from_timestamp(&request.occurred_at)?;
    let existing_events = read_event_snapshot(events_root, &month)?;
    if sha256_hex(&existing_events) != request.expected_event_hash {
        return Err(write_error(
            "event_conflict",
            "事件文件已被其他操作修改，请重新预览",
        ));
    }
    fs::create_dir_all(events_root)
        .map_err(|_| write_error("event_prepare_failed", "事件目录不可用"))?;
    let canonical_events = events_root
        .canonicalize()
        .map_err(|_| write_error("event_prepare_failed", "事件目录不可用"))?;
    let event_file = format!("{month}.jsonl");
    let event_path = canonical_events.join(&event_file);
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| write_error("clock_error", "系统时间不可用"))?
        .as_nanos();
    let event_id = format!("evt_task_center_note_{nonce}");
    let event = serde_json::json!({
        "id": event_id,
        "task_id": task_id,
        "event_type": event_type,
        "occurred_at": request.occurred_at,
        "source_refs": ["task-center-ui"],
        "confirmed_by": "user_ui_confirmation",
        "privacy": "general",
        "author": author,
        "message": text
    });
    let event_line = serde_json::to_string(&event)
        .map_err(|_| write_error("event_prepare_failed", "记录序列化失败"))?;
    let mut next_events = existing_events.clone();
    next_events.extend_from_slice(event_line.as_bytes());
    next_events.push(b'\n');
    if next_events.len() as u64 > MAX_EVENT_FILE_BYTES {
        return Err(write_error("event_prepare_failed", "事件文件超过安全上限"));
    }
    let temp = unique_sidecar_path(&event_path, "tmp")?;
    let backup = event_path
        .exists()
        .then(|| unique_sidecar_path(&event_path, "bak"))
        .transpose()?;
    write_prepared_file(
        &temp,
        &next_events,
        fs::metadata(&event_path)
            .ok()
            .map(|value| value.permissions()),
    )?;
    if let Some(backup) = backup.as_deref() {
        if fs::copy(&event_path, backup).is_err() {
            let _ = fs::remove_file(&temp);
            return Err(write_error("backup_failed", "无法创建事件恢复副本"));
        }
    }
    if read_event_snapshot(events_root, &month)? != existing_events {
        let _ = fs::remove_file(&temp);
        if let Some(backup) = backup.as_deref() {
            let _ = fs::remove_file(backup);
        }
        return Err(write_error(
            "event_conflict",
            "事件文件已被其他操作修改，请重新预览",
        ));
    }
    if fs::rename(&temp, &event_path).is_err() {
        let _ = fs::remove_file(&temp);
        if let Some(backup) = backup.as_deref() {
            let _ = fs::remove_file(backup);
        }
        return Err(write_error("event_commit_failed", "记录追加失败"));
    }
    let written = fs::read(&event_path).map_err(|_| write_error("readback_failed", "记录回读失败"));
    if written
        .as_ref()
        .map(|bytes| bytes.ends_with(format!("{event_line}\n").as_bytes()))
        != Ok(true)
    {
        let rollback = rollback_file(&event_path, backup.as_deref());
        if rollback.is_err() {
            return Err(write_error("rollback_failed", "记录追加失败且无法自动恢复"));
        }
        return Err(write_error("event_readback_mismatch", "记录回读不一致"));
    }
    if let Some(backup) = backup.as_deref() {
        let _ = fs::remove_file(backup);
    }
    Ok(TaskNoteReceipt {
        task_id,
        event_id,
        event_file,
        verified: true,
    })
}

fn rollback_file(path: &Path, backup: Option<&Path>) -> Result<(), WriteError> {
    if let Some(backup) = backup {
        if path.exists() {
            fs::remove_file(path)
                .map_err(|_| write_error("rollback_failed", "写入失败且无法移除异常文件"))?;
        }
        fs::rename(backup, path)
            .map_err(|_| write_error("rollback_failed", "写入失败且无法自动恢复原文件"))?;
    } else if path.exists() {
        fs::remove_file(path)
            .map_err(|_| write_error("rollback_failed", "写入失败且无法移除新增事件"))?;
    }
    Ok(())
}

// Keeping every transaction input explicit makes the security boundary auditable at each call site.
#[allow(clippy::too_many_arguments)]
fn commit_existing_task_change(
    task_path: &Path,
    original: &str,
    updated: &str,
    events_root: &Path,
    task_id: &str,
    event_type: &str,
    event_details: Value,
    occurred_at: &str,
    event_month: &str,
    fault: CommitFault,
) -> Result<AtomicWriteReceipt, WriteError> {
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
    let mut event = serde_json::json!({
        "id": event_id,
        "task_id": task_id,
        "event_type": event_type,
        "occurred_at": occurred_at,
        "source_refs": ["task-center-ui"],
        "confirmed_by": "user_ui_confirmation",
        "privacy": "general"
    });
    if let (Some(target), Some(details)) = (event.as_object_mut(), event_details.as_object()) {
        for (key, value) in details {
            target.insert(key.clone(), value.clone());
        }
    }
    let event_line = serde_json::to_string(&event)
        .map_err(|_| write_error("event_prepare_failed", "事件序列化失败，未执行写入"))?;
    let mut next_events = existing_events.clone();
    next_events.extend_from_slice(event_line.as_bytes());
    next_events.push(b'\n');

    let task_temp = unique_sidecar_path(task_path, "tmp")?;
    let event_temp = unique_sidecar_path(&event_path, "tmp")?;
    let task_backup = unique_sidecar_path(task_path, "bak")?;
    let event_backup = if event_path.exists() {
        Some(unique_sidecar_path(&event_path, "bak")?)
    } else {
        None
    };
    let task_permissions = fs::metadata(task_path)
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
    if fs::copy(task_path, &task_backup).is_err() {
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

    let mut task_committed = false;
    let mut event_committed = false;
    let commit_result = (|| -> Result<(), WriteError> {
        #[cfg(test)]
        if fault == CommitFault::BeforeTaskCommitExternalChange {
            fs::write(
                task_path,
                original.replace(
                    "unknown_extension: keep-me",
                    "unknown_extension: external-change",
                ),
            )
            .map_err(|_| write_error("test_fault_failed", "无法注入测试冲突"))?;
        }
        if fs::read(task_path).map_err(|_| write_error("conflict", "任务提交前无法重新核对"))?
            != original.as_bytes()
        {
            return Err(write_error(
                "conflict",
                "任务已被其他操作修改，请重新读取后确认",
            ));
        }
        fs::rename(&task_temp, task_path)
            .map_err(|_| write_error("task_commit_failed", "任务原子替换失败"))?;
        task_committed = true;
        if fault == CommitFault::AfterTaskCommit {
            return Err(write_error("event_commit_failed", "事件追加失败"));
        }
        #[cfg(test)]
        if fault == CommitFault::BeforeEventCommitExternalChange {
            let mut external_events = existing_events.clone();
            external_events.extend_from_slice(b"{\"id\":\"external-concurrent-event\"}\n");
            fs::write(&event_path, external_events)
                .map_err(|_| write_error("test_fault_failed", "无法注入测试冲突"))?;
        }
        let event_is_unchanged = if event_path.exists() {
            fs::read(&event_path)
                .map_err(|_| write_error("event_conflict", "事件提交前无法重新核对"))?
                == existing_events
        } else {
            existing_events.is_empty()
        };
        if !event_is_unchanged {
            return Err(write_error(
                "event_conflict",
                "事件文件已被其他操作修改，请重新读取后确认",
            ));
        }
        fs::rename(&event_temp, &event_path)
            .map_err(|_| write_error("event_commit_failed", "事件追加失败"))?;
        event_committed = true;
        if fault == CommitFault::AfterEventCommit {
            return Err(write_error("readback_mismatch", "写后任务内容核对不一致"));
        }
        let written_task =
            fs::read(task_path).map_err(|_| write_error("readback_failed", "写后任务回读失败"))?;
        if sha256_hex(&written_task) != updated_hash {
            return Err(write_error("readback_mismatch", "写后任务内容核对不一致"));
        }
        let written_events = fs::read(&event_path)
            .map_err(|_| write_error("readback_failed", "写后事件回读失败"))?;
        if !written_events.ends_with(format!("{event_line}\n").as_bytes()) {
            return Err(write_error("event_readback_mismatch", "写后事件核对不一致"));
        }
        Ok(())
    })();

    if let Err(error) = commit_result {
        let task_rollback = if task_committed {
            rollback_file(task_path, Some(&task_backup))
        } else {
            let _ = fs::remove_file(&task_backup);
            Ok(())
        };
        let event_rollback = if event_committed {
            rollback_file(&event_path, event_backup.as_deref())
        } else {
            if let Some(backup) = event_backup.as_deref() {
                let _ = fs::remove_file(backup);
            }
            Ok(())
        };
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
    Ok(AtomicWriteReceipt {
        file_hash: updated_hash,
        event_id,
        event_file: event_file_name,
    })
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
    let atomic = commit_existing_task_change(
        &task_path,
        &original,
        &updated,
        events_root,
        &task_id,
        "priority_changed",
        serde_json::json!({
            "previous_priority": previous_priority,
            "new_priority": request.new_priority,
        }),
        occurred_at,
        event_month,
        CommitFault::None,
    )?;
    Ok(PriorityEditReceipt {
        file_token: request.file_token,
        task_id,
        previous_priority,
        new_priority: request.new_priority,
        file_hash: atomic.file_hash,
        event_id: atomic.event_id,
        event_file: atomic.event_file,
        verified: true,
    })
}

fn preview_task_field_edit_at(
    root: &Path,
    file_token: &str,
    field: &str,
    new_value: &Value,
) -> Result<TaskFieldEditPreview, WriteError> {
    let (_, content) = read_writable_task(root, file_token)?;
    let (task_id, _) = task_id_and_priority(&content)?;
    let prepared = prepare_field_edit(&content, &task_id, field, new_value)?;
    if prepared.before_value == prepared.after_value {
        return Err(write_error("no_change", "修改前后相同，未执行写入"));
    }
    let _ = replace_frontmatter_entry(&content, field, &prepared.yaml_value)?;
    Ok(TaskFieldEditPreview {
        file_token: file_token.to_string(),
        task_id,
        field: field.to_string(),
        before_value: prepared.before_value,
        after_value: prepared.after_value,
        expected_hash: sha256_hex(content.as_bytes()),
    })
}

fn apply_task_field_edit_at(
    root: &Path,
    events_root: &Path,
    request: TaskFieldEditRequest,
    occurred_at: &str,
    event_month: &str,
) -> Result<TaskFieldEditReceipt, WriteError> {
    if !request.confirmed {
        return Err(write_error(
            "confirmation_required",
            "用户未确认，未执行写入",
        ));
    }
    let (task_path, original) = read_writable_task(root, &request.file_token)?;
    if sha256_hex(original.as_bytes()) != request.expected_hash {
        return Err(write_error(
            "conflict",
            "任务已被其他操作修改，请重新读取后确认",
        ));
    }
    let (task_id, _) = task_id_and_priority(&original)?;
    let prepared = prepare_field_edit(&original, &task_id, &request.field, &request.new_value)?;
    if prepared.before_value == prepared.after_value {
        return Err(write_error("no_change", "修改前后相同，未执行写入"));
    }
    let updated = replace_frontmatter_entry(&original, &request.field, &prepared.yaml_value)?;
    let mut details = serde_json::json!({
        "changed_field": request.field,
        "previous_value": prepared.before_value,
        "new_value": prepared.after_value,
    });
    if request.field == "task_status" {
        if let Some(object) = details.as_object_mut() {
            object.insert(
                "previous_task_status".to_string(),
                prepared.before_value.clone(),
            );
            object.insert("new_task_status".to_string(), prepared.after_value.clone());
        }
    }
    let atomic = commit_existing_task_change(
        &task_path,
        &original,
        &updated,
        events_root,
        &task_id,
        prepared.event_type,
        details,
        occurred_at,
        event_month,
        CommitFault::None,
    )?;
    Ok(TaskFieldEditReceipt {
        file_token: request.file_token,
        task_id,
        field: request.field,
        previous_value: prepared.before_value,
        new_value: prepared.after_value,
        file_hash: atomic.file_hash,
        event_id: atomic.event_id,
        event_file: atomic.event_file,
        verified: true,
    })
}

fn validate_new_task_draft(draft: &NewTaskDraft) -> Result<(), WriteError> {
    validate_plain_text(&draft.title, false, 200)?;
    validate_plain_text(&draft.domain, false, 120)?;
    validate_plain_text(&draft.assignee, true, 120)?;
    if !["todo", "doing", "long_term", "done", "cancelled"].contains(&draft.task_status.as_str()) {
        return Err(write_error(
            "unsupported_value",
            "正式结构暂不支持该任务状态",
        ));
    }
    validate_priority(&draft.priority)?;
    if !draft.deadline.is_empty() && !valid_iso_date(&draft.deadline) {
        return Err(write_error(
            "invalid_value",
            "截止日期必须是有效的 YYYY-MM-DD",
        ));
    }
    let tags_value = serde_json::to_value(&draft.tags)
        .map_err(|_| write_error("invalid_value", "标签无法安全序列化"))?;
    let normalized_tags = normalized_string_list(&tags_value, MAX_TAGS, 40, "标签")?;
    if normalized_tags != draft.tags {
        return Err(write_error("invalid_value", "标签需要移除空白或重复项"));
    }
    if !draft.parent_id.is_empty() && !valid_task_id(&draft.parent_id) {
        return Err(write_error("invalid_value", "父任务编号无效"));
    }
    for relations in [&draft.blocked_by_ids, &draft.related_ids] {
        let value = serde_json::to_value(relations)
            .map_err(|_| write_error("invalid_value", "任务关系无法安全序列化"))?;
        let normalized = normalized_relation_ids(&value, "tsk_new_task_not_yet_assigned")?;
        if normalized != *relations {
            return Err(write_error("invalid_value", "任务关系需要移除空白或重复项"));
        }
    }
    Ok(())
}

fn safe_title_slug(title: &str) -> String {
    let mut slug = String::new();
    let mut last_dash = false;
    for character in title.chars() {
        if character.is_alphanumeric() {
            slug.push(character);
            last_dash = false;
        } else if !last_dash && !slug.is_empty() {
            slug.push('-');
            last_dash = true;
        }
        if slug.chars().count() >= 48 {
            break;
        }
    }
    slug.trim_matches('-').to_string()
}

fn render_new_task(preview: &CreateTaskPreview) -> Result<String, WriteError> {
    validate_new_task_draft(&preview.draft)?;
    if preview.draft.parent_id == preview.task_id
        || preview
            .draft
            .blocked_by_ids
            .iter()
            .chain(preview.draft.related_ids.iter())
            .any(|value| value == &preview.task_id)
    {
        return Err(write_error("invalid_value", "任务关系不能指向新任务自身"));
    }
    let title = serde_json::to_string(&preview.draft.title)
        .map_err(|_| write_error("invalid_value", "标题无法安全序列化"))?;
    let domain = serde_json::to_string(&preview.draft.domain)
        .map_err(|_| write_error("invalid_value", "归属无法安全序列化"))?;
    let assignee = if preview.draft.assignee.is_empty() {
        String::new()
    } else {
        serde_json::to_string(&preview.draft.assignee)
            .map_err(|_| write_error("invalid_value", "负责人无法安全序列化"))?
    };
    let relations = serde_json::to_string(&preview.draft.related_ids)
        .map_err(|_| write_error("invalid_value", "关系列表无法安全序列化"))?;
    let tags = serde_json::to_string(&preview.draft.tags)
        .map_err(|_| write_error("invalid_value", "标签无法安全序列化"))?;
    let blocked_by = serde_json::to_string(&preview.draft.blocked_by_ids)
        .map_err(|_| write_error("invalid_value", "阻塞关系无法安全序列化"))?;
    Ok(format!(
        "---\n\
id: {id}\n\
task_id: {id}\n\
schema_version: 1\n\
record_type: task\n\
record_status: current\n\
task_status: {status}\n\
workflow_status: 待推进\n\
title: {title}\n\
domain: {domain}\n\
owner_scope: personal\n\
priority: {priority}\n\
assignee:{assignee_prefix}{assignee}\n\
deadline:{deadline_prefix}{deadline}\n\
tags: {tags}\n\
parent_id:{parent_prefix}{parent_id}\n\
blocked_by_ids: {blocked_by}\n\
next_action:\n\
project_id:\n\
source_refs: [\"task-center-ui\"]\n\
privacy: general\n\
codex_access: proposal_only\n\
verification_status: human_confirmed\n\
ai_status: human_confirmed\n\
approval_status: accepted\n\
created_at: {created}\n\
updated_at: {created}\n\
completed_at:\n\
related_ids: {relations}\n\
---\n\
# {body_title}\n\n\
## 预期结果\n\n\n\
## 当前情况\n\n\n\
## 下一步\n\n\n\
## 补充说明\n",
        id = preview.task_id,
        status = preview.draft.task_status,
        title = title,
        domain = domain,
        priority = preview.draft.priority,
        assignee_prefix = if assignee.is_empty() { "" } else { " " },
        assignee = assignee,
        deadline_prefix = if preview.draft.deadline.is_empty() {
            ""
        } else {
            " "
        },
        deadline = preview.draft.deadline,
        tags = tags,
        parent_prefix = if preview.draft.parent_id.is_empty() {
            ""
        } else {
            " "
        },
        parent_id = preview.draft.parent_id,
        blocked_by = blocked_by,
        created = preview.created_at,
        relations = relations,
        body_title = preview.draft.title,
    ))
}

fn preview_create_task_at(
    draft: NewTaskDraft,
    occurred_at: &str,
) -> Result<CreateTaskPreview, WriteError> {
    validate_new_task_draft(&draft)?;
    let created_at = occurred_at
        .get(..10)
        .ok_or_else(|| write_error("clock_error", "无法生成任务日期"))?
        .to_string();
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| write_error("clock_error", "系统时间不可用"))?
        .as_nanos();
    let seed = format!(
        "{}\n{}\n{}\n{nonce}",
        draft.title, draft.domain, occurred_at
    );
    let digest = sha256_hex(seed.as_bytes());
    let task_id = format!("tsk_{}", &digest[..16]);
    let slug = safe_title_slug(&draft.title);
    let file_token = if slug.is_empty() {
        format!("{task_id}.md")
    } else {
        format!("{task_id}-{slug}.md")
    };
    let mut preview = CreateTaskPreview {
        draft,
        task_id,
        file_token,
        created_at,
        occurred_at: occurred_at.to_string(),
        expected_hash: String::new(),
    };
    preview.expected_hash = sha256_hex(render_new_task(&preview)?.as_bytes());
    Ok(preview)
}

fn apply_create_task_at(
    root: &Path,
    events_root: &Path,
    request: CreateTaskRequest,
    event_month: &str,
) -> Result<CreateTaskReceipt, WriteError> {
    apply_create_task_at_with_fault(root, events_root, request, event_month, CommitFault::None)
}

fn apply_create_task_at_with_fault(
    root: &Path,
    events_root: &Path,
    request: CreateTaskRequest,
    event_month: &str,
    fault: CommitFault,
) -> Result<CreateTaskReceipt, WriteError> {
    if !request.confirmed {
        return Err(write_error(
            "confirmation_required",
            "用户未确认，未执行写入",
        ));
    }
    let content = render_new_task(&request.preview)?;
    let content_hash = sha256_hex(content.as_bytes());
    if content_hash != request.preview.expected_hash {
        return Err(write_error(
            "preview_mismatch",
            "新建预览已变化，请重新确认",
        ));
    }
    let root = root
        .canonicalize()
        .map_err(|_| write_error("invalid_file", "任务根目录不可用"))?;
    let token = Path::new(&request.preview.file_token);
    if token.components().count() != 1
        || token.extension().and_then(|value| value.to_str()) != Some("md")
    {
        return Err(write_error("invalid_file", "新任务文件名无效"));
    }
    let task_path = root.join(token);
    if task_path.exists() {
        return Err(write_error("conflict", "同名任务已经存在，请重新生成预览"));
    }
    if scan_metadata(&root)
        .map_err(|_| write_error("read_failed", "无法核对现有任务编号"))?
        .iter()
        .filter(|source| source.error.is_none())
        .any(|source| {
            frontmatter_scalar(&source.frontmatter, "task_id") == Some(&request.preview.task_id)
        })
    {
        return Err(write_error("conflict", "任务编号已经存在，请重新生成预览"));
    }

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
    let event_id = format!("evt_task_center_{nonce}_{}", &content_hash[..12]);
    let event = serde_json::json!({
        "id": event_id,
        "task_id": request.preview.task_id,
        "event_type": "created",
        "occurred_at": request.preview.occurred_at,
        "source_refs": ["task-center-ui"],
        "confirmed_by": "user_ui_confirmation",
        "new_task_status": request.preview.draft.task_status,
        "privacy": "general"
    });
    let event_line = serde_json::to_string(&event)
        .map_err(|_| write_error("event_prepare_failed", "事件序列化失败，未执行写入"))?;
    let mut next_events = existing_events.clone();
    next_events.extend_from_slice(event_line.as_bytes());
    next_events.push(b'\n');

    let task_temp = unique_sidecar_path(&task_path, "tmp")?;
    let event_temp = unique_sidecar_path(&event_path, "tmp")?;
    let event_backup = if event_path.exists() {
        Some(unique_sidecar_path(&event_path, "bak")?)
    } else {
        None
    };
    write_prepared_file(&task_temp, content.as_bytes(), None)?;
    if let Err(error) = write_prepared_file(
        &event_temp,
        &next_events,
        fs::metadata(&event_path)
            .ok()
            .map(|value| value.permissions()),
    ) {
        let _ = fs::remove_file(&task_temp);
        return Err(error);
    }
    if let Some(backup) = event_backup.as_deref() {
        if fs::copy(&event_path, backup).is_err() {
            let _ = fs::remove_file(&task_temp);
            let _ = fs::remove_file(&event_temp);
            return Err(write_error(
                "backup_failed",
                "无法创建事件恢复副本，未执行写入",
            ));
        }
    }

    let mut task_committed = false;
    let mut event_committed = false;
    let commit_result = (|| -> Result<(), WriteError> {
        fs::hard_link(&task_temp, &task_path)
            .map_err(|_| write_error("task_commit_failed", "新任务原子创建失败"))?;
        task_committed = true;
        fs::remove_file(&task_temp)
            .map_err(|_| write_error("task_commit_failed", "新任务临时文件清理失败"))?;
        if fault == CommitFault::AfterTaskCommit {
            return Err(write_error("event_commit_failed", "创建事件追加失败"));
        }
        #[cfg(test)]
        if fault == CommitFault::BeforeEventCommitExternalChange {
            let mut external_events = existing_events.clone();
            external_events.extend_from_slice(b"{\"id\":\"external-concurrent-event\"}\n");
            fs::write(&event_path, external_events)
                .map_err(|_| write_error("test_fault_failed", "无法注入测试冲突"))?;
        }
        let event_is_unchanged = if event_path.exists() {
            fs::read(&event_path)
                .map_err(|_| write_error("event_conflict", "事件提交前无法重新核对"))?
                == existing_events
        } else {
            existing_events.is_empty()
        };
        if !event_is_unchanged {
            return Err(write_error(
                "event_conflict",
                "事件文件已被其他操作修改，请重新读取后确认",
            ));
        }
        fs::rename(&event_temp, &event_path)
            .map_err(|_| write_error("event_commit_failed", "创建事件追加失败"))?;
        event_committed = true;
        if fault == CommitFault::AfterEventCommit {
            return Err(write_error("readback_mismatch", "新任务写后核对不一致"));
        }
        if sha256_hex(
            &fs::read(&task_path).map_err(|_| write_error("readback_failed", "新任务回读失败"))?,
        ) != content_hash
        {
            return Err(write_error("readback_mismatch", "新任务写后核对不一致"));
        }
        let written_events = fs::read(&event_path)
            .map_err(|_| write_error("readback_failed", "创建事件回读失败"))?;
        if !written_events.ends_with(format!("{event_line}\n").as_bytes()) {
            return Err(write_error(
                "event_readback_mismatch",
                "创建事件写后核对不一致",
            ));
        }
        Ok(())
    })();
    if let Err(error) = commit_result {
        let _ = fs::remove_file(&task_temp);
        let task_rollback = if task_committed && task_path.exists() {
            fs::remove_file(&task_path).map_err(|_| ())
        } else {
            Ok(())
        };
        let event_rollback = if event_committed {
            rollback_file(&event_path, event_backup.as_deref())
        } else {
            if let Some(backup) = event_backup.as_deref() {
                let _ = fs::remove_file(backup);
            }
            Ok(())
        };
        let _ = fs::remove_file(&event_temp);
        if task_rollback.is_err() || event_rollback.is_err() {
            return Err(write_error(
                "rollback_failed",
                "新建失败且自动恢复未完成，请停止继续写入",
            ));
        }
        return Err(error);
    }
    if let Some(backup) = event_backup.as_deref() {
        let _ = fs::remove_file(backup);
    }
    Ok(CreateTaskReceipt {
        task_id: request.preview.task_id,
        file_token: request.preview.file_token,
        file_hash: content_hash,
        event_id,
        event_file: event_file_name,
        verified: true,
    })
}

#[tauri::command]
fn preview_create_task(draft: NewTaskDraft) -> Result<CreateTaskPreview, WriteError> {
    let (occurred_at, _) = current_event_time()?;
    preview_create_task_at(draft, &occurred_at)
}

#[tauri::command]
fn apply_create_task(request: CreateTaskRequest) -> Result<CreateTaskReceipt, WriteError> {
    let month = request
        .preview
        .occurred_at
        .get(..7)
        .ok_or_else(|| write_error("clock_error", "创建事件月份无效"))?
        .to_string();
    let root = default_task_root();
    apply_create_task_at(&root, &events_root(&root), request, &month)
}

#[tauri::command]
fn preview_task_field_edit(
    file_token: String,
    field: String,
    new_value: Value,
) -> Result<TaskFieldEditPreview, WriteError> {
    preview_task_field_edit_at(&default_task_root(), &file_token, &field, &new_value)
}

#[tauri::command]
fn apply_task_field_edit(
    request: TaskFieldEditRequest,
) -> Result<TaskFieldEditReceipt, WriteError> {
    let (occurred_at, month) = current_event_time()?;
    let root = default_task_root();
    apply_task_field_edit_at(&root, &events_root(&root), request, &occurred_at, &month)
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

#[tauri::command]
async fn load_codex_thread_page(
    thread_id: String,
    cursor: Option<String>,
) -> Result<codex_history::CodexThreadPage, codex_history::CodexHistoryError> {
    tauri::async_runtime::spawn_blocking(move || {
        codex_history::load_thread_page(&thread_id, cursor.as_deref())
    })
    .await
    .map_err(|_| {
        codex_history::CodexHistoryError::new("worker_failed", "Codex 历史读取任务异常结束")
    })?
}

#[tauri::command]
async fn load_codex_thread_list(
    cursor: Option<String>,
) -> Result<codex_history::CodexThreadListPage, codex_history::CodexHistoryError> {
    tauri::async_runtime::spawn_blocking(move || codex_history::load_thread_list(cursor.as_deref()))
        .await
        .map_err(|_| {
            codex_history::CodexHistoryError::new("worker_failed", "Codex 任务列表读取异常结束")
        })?
}

#[tauri::command]
fn initialize_local_task_library() -> Result<(), String> {
    initialize_task_library_at(&default_task_root())
}

fn saved_filter_path(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    app.path()
        .app_config_dir()
        .map(|path| path.join("saved-task-filters.json"))
        .map_err(|_| "保存筛选目录不可用".to_string())
}

#[tauri::command]
fn load_saved_task_filters(app: tauri::AppHandle) -> Result<Vec<SavedTaskFilter>, String> {
    read_saved_filters_at(&saved_filter_path(&app)?)
}

#[tauri::command]
fn save_task_filter(
    app: tauri::AppHandle,
    draft: SavedTaskFilterDraft,
) -> Result<SavedTaskFilter, String> {
    save_task_filter_at(&saved_filter_path(&app)?, draft)
}

#[tauri::command]
fn delete_task_filter(app: tauri::AppHandle, id: String) -> Result<(), String> {
    delete_task_filter_at(&saved_filter_path(&app)?, &id)
}

#[tauri::command]
fn preview_task_note(
    file_token: String,
    kind: String,
    text: String,
    author: String,
) -> Result<TaskNotePreview, WriteError> {
    let (occurred_at, _) = current_event_time()?;
    let root = default_task_root();
    preview_task_note_at(
        &root,
        &events_root(&root),
        &file_token,
        &kind,
        &text,
        &author,
        &occurred_at,
    )
}

#[tauri::command]
fn apply_task_note(request: TaskNoteRequest) -> Result<TaskNoteReceipt, WriteError> {
    let root = default_task_root();
    apply_task_note_at(&root, &events_root(&root), request)
}

fn validate_expected_update_version(expected: &str, actual: &str) -> Result<(), String> {
    let valid = |value: &str| {
        !value.is_empty()
            && value.len() <= 48
            && value.chars().all(|character| {
                character.is_ascii_alphanumeric() || matches!(character, '.' | '-' | '+')
            })
    };
    if !valid(expected) || !valid(actual) || expected != actual {
        return Err("可安装版本已变化，请重新检查更新".to_string());
    }
    Ok(())
}

fn parse_macos_https_proxy(settings: &str) -> Option<url::Url> {
    fn value<'a>(settings: &'a str, key: &str) -> Option<&'a str> {
        settings.lines().find_map(|line| {
            let (candidate, value) = line.trim().split_once(':')?;
            (candidate.trim() == key).then_some(value.trim())
        })
    }

    if value(settings, "HTTPSEnable")? != "1" {
        return None;
    }
    let host = value(settings, "HTTPSProxy")?;
    let port = value(settings, "HTTPSPort")?.parse::<u16>().ok()?;
    if host.is_empty()
        || host.len() > 255
        || host.chars().any(|character| {
            character.is_whitespace() || matches!(character, '/' | '@' | '?' | '#')
        })
    {
        return None;
    }
    let authority = if host.contains(':') {
        format!("[{host}]:{port}")
    } else {
        format!("{host}:{port}")
    };
    let proxy = url::Url::parse(&format!("http://{authority}")).ok()?;
    (proxy.username().is_empty() && proxy.password().is_none()).then_some(proxy)
}

#[cfg(target_os = "macos")]
fn system_https_proxy() -> Option<url::Url> {
    let output = std::process::Command::new("/usr/sbin/scutil")
        .arg("--proxy")
        .output()
        .ok()?;
    if !output.status.success() || output.stdout.len() > 64 * 1024 {
        return None;
    }
    parse_macos_https_proxy(std::str::from_utf8(&output.stdout).ok()?)
}

#[cfg(not(target_os = "macos"))]
fn system_https_proxy() -> Option<url::Url> {
    None
}

fn update_download_error(error: &tauri_plugin_updater::Error) -> String {
    match error {
        tauri_plugin_updater::Error::Minisign(_)
        | tauri_plugin_updater::Error::Base64(_)
        | tauri_plugin_updater::Error::SignatureUtf8(_) => {
            "更新包签名验证失败；当前版本未改变，请等待修正版".to_string()
        }
        tauri_plugin_updater::Error::Reqwest(_) | tauri_plugin_updater::Error::Network(_) => {
            "更新包下载中断；当前版本未改变，请检查网络后重试".to_string()
        }
        _ => "更新包下载或安全验证失败；当前版本未改变".to_string(),
    }
}

fn update_install_error(_: &tauri_plugin_updater::Error) -> String {
    "更新包已通过验证，但无法替换当前应用；当前版本未改变".to_string()
}

#[tauri::command]
async fn check_task_center_update(app: tauri::AppHandle) -> Result<TaskCenterUpdateInfo, String> {
    let current_version = app.package_info().version.to_string();
    let mut builder = app.updater_builder().timeout(Duration::from_secs(15));
    if let Some(proxy) = system_https_proxy() {
        builder = builder.proxy(proxy);
    }
    let update = builder
        .build()
        .map_err(|_| "更新组件不可用".to_string())?
        .check()
        .await
        .map_err(|_| "暂时无法读取任务中心更新".to_string())?;
    Ok(match update {
        Some(update) => TaskCenterUpdateInfo {
            current_version,
            available: true,
            version: Some(update.version.to_string()),
        },
        None => TaskCenterUpdateInfo {
            current_version,
            available: false,
            version: None,
        },
    })
}

#[tauri::command]
async fn install_task_center_update(
    app: tauri::AppHandle,
    expected_version: String,
) -> Result<(), String> {
    let mut builder = app.updater_builder().timeout(Duration::from_secs(15));
    if let Some(proxy) = system_https_proxy() {
        builder = builder.proxy(proxy);
    }
    let update = builder
        .build()
        .map_err(|_| "更新组件不可用".to_string())?
        .check()
        .await
        .map_err(|_| "暂时无法重新核对任务中心更新".to_string())?
        .ok_or_else(|| "当前已经是最新版".to_string())?;
    validate_expected_update_version(&expected_version, &update.version.to_string())?;
    let bytes = update
        .download(|_, _| {}, || {})
        .await
        .map_err(|error| update_download_error(&error))?;
    update
        .install(&bytes)
        .map_err(|error| update_install_error(&error))?;
    app.restart();
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
        .plugin(tauri_plugin_updater::Builder::new().build())
        .invoke_handler(tauri::generate_handler![
            load_task_metadata,
            load_task_body,
            load_task_events,
            load_project_mappings,
            load_codex_thread_list,
            load_codex_thread_page,
            initialize_local_task_library,
            load_saved_task_filters,
            save_task_filter,
            delete_task_filter,
            preview_priority_edit,
            apply_priority_edit,
            preview_task_field_edit,
            apply_task_field_edit,
            preview_create_task,
            apply_create_task,
            preview_task_note,
            apply_task_note,
            check_task_center_update,
            install_task_center_update
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
record_status: current\n\
task_status: doing\n\
title: Write test\n\
domain: test\n\
priority: {priority}\n\
unknown_extension: keep-me\n\
privacy: general\n\
codex_access: proposal_only\n\
source_refs: []\n\
related_ids:\n\
  - tsk_existing_relation\n\
verification_status: human_confirmed\n\
---\n\
# Body\n\nKeep --- body formatting.\n"
        )
    }

    #[test]
    fn local_task_library_is_created_only_at_explicit_target() {
        let temp = tempdir().unwrap();
        let root = temp.path().join("CodexMonitorTaskCenter").join("任务");
        assert!(!root.exists());
        initialize_task_library_at(&root).unwrap();
        assert!(root.is_dir());
        assert!(events_root(&root).is_dir());
        initialize_task_library_at(&root).unwrap();
        assert!(initialize_task_library_at(&temp.path().join("ambiguous")).is_err());
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
    fn saved_filters_round_trip_update_delete_and_reject_unknown_version() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("config").join("saved-task-filters.json");
        let draft = SavedTaskFilterDraft {
            id: Some("filter_release".to_string()),
            name: "发布任务".to_string(),
            project_id: "prj_monitor".to_string(),
            status: "doing".to_string(),
            tag: "发布".to_string(),
            show_archived: false,
            view: "board".to_string(),
        };
        let saved = save_task_filter_at(&path, draft.clone()).unwrap();
        assert_eq!(saved.id, "filter_release");
        assert_eq!(read_saved_filters_at(&path).unwrap(), vec![saved.clone()]);
        let serialized: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        assert!(serialized["filters"][0].get("query").is_none());

        let updated = save_task_filter_at(
            &path,
            SavedTaskFilterDraft {
                name: "发布任务（列表）".to_string(),
                view: "list".to_string(),
                ..draft
            },
        )
        .unwrap();
        assert_eq!(read_saved_filters_at(&path).unwrap(), vec![updated]);
        delete_task_filter_at(&path, "filter_release").unwrap();
        assert!(read_saved_filters_at(&path).unwrap().is_empty());

        let unsupported = r#"{"version":2,"filters":[]}"#;
        fs::write(&path, unsupported).unwrap();
        let error = save_task_filter_at(
            &path,
            SavedTaskFilterDraft {
                id: Some("filter_future".to_string()),
                name: "未来配置".to_string(),
                project_id: String::new(),
                status: "all".to_string(),
                tag: String::new(),
                show_archived: false,
                view: "board".to_string(),
            },
        )
        .unwrap_err();
        assert!(error.contains("版本"));
        assert_eq!(fs::read_to_string(path).unwrap(), unsupported);
    }

    fn note_request(preview: TaskNotePreview, confirmed: bool) -> TaskNoteRequest {
        TaskNoteRequest {
            file_token: preview.file_token,
            task_id: preview.task_id,
            kind: preview.kind,
            text: preview.text,
            author: preview.author,
            occurred_at: preview.occurred_at,
            expected_task_hash: preview.expected_task_hash,
            expected_event_hash: preview.expected_event_hash,
            confirmed,
        }
    }

    #[test]
    fn note_preview_cancel_and_confirm_are_append_only_with_readback() {
        let dir = tempdir().unwrap();
        let task_root = dir.path().join("任务");
        let events = dir.path().join("事件");
        fs::create_dir_all(&task_root).unwrap();
        let task_path = task_root.join("tsk_write_test.md");
        let original = writable_task("medium");
        fs::write(&task_path, &original).unwrap();
        let preview = preview_task_note_at(
            &task_root,
            &events,
            "tsk_write_test.md",
            "comment",
            "  已核验评论  ",
            " 本人 ",
            "2026-08-24T08:00:00Z",
        )
        .unwrap();
        assert_eq!(preview.text, "已核验评论");
        assert_eq!(preview.author, "本人");
        assert!(!events.exists());
        assert_eq!(fs::read_to_string(&task_path).unwrap(), original);

        let cancelled =
            apply_task_note_at(&task_root, &events, note_request(preview.clone(), false))
                .unwrap_err();
        assert_eq!(cancelled.code, "confirmation_required");
        assert!(!events.exists());

        let receipt = apply_task_note_at(&task_root, &events, note_request(preview, true)).unwrap();
        assert!(receipt.verified);
        assert_eq!(fs::read_to_string(&task_path).unwrap(), original);
        let event_text = fs::read_to_string(events.join("2026-08.jsonl")).unwrap();
        let event: Value = serde_json::from_str(event_text.trim()).unwrap();
        assert_eq!(event["event_type"], "comment_added");
        assert_eq!(event["message"], "已核验评论");
        assert_eq!(event["author"], "本人");
        let loaded = read_events(&events, "tsk_write_test").unwrap();
        assert_eq!(loaded.len(), 1);
        assert_eq!(loaded[0].message.as_deref(), Some("已核验评论"));
        assert_eq!(loaded[0].author.as_deref(), Some("本人"));
    }

    #[test]
    fn note_conflicts_and_invalid_content_never_append() {
        let dir = tempdir().unwrap();
        let task_root = dir.path().join("任务");
        let events = dir.path().join("事件");
        fs::create_dir_all(&task_root).unwrap();
        let task_path = task_root.join("tsk_write_test.md");
        fs::write(&task_path, writable_task("medium")).unwrap();
        let preview = preview_task_note_at(
            &task_root,
            &events,
            "tsk_write_test.md",
            "activity",
            "完成本机验收",
            "本人",
            "2026-08-24T08:00:00Z",
        )
        .unwrap();
        fs::write(
            &task_path,
            writable_task("medium").replace("keep-me", "external-change"),
        )
        .unwrap();
        let task_conflict =
            apply_task_note_at(&task_root, &events, note_request(preview, true)).unwrap_err();
        assert_eq!(task_conflict.code, "conflict");
        assert!(!events.exists());

        fs::write(&task_path, writable_task("medium")).unwrap();
        let preview = preview_task_note_at(
            &task_root,
            &events,
            "tsk_write_test.md",
            "activity",
            "完成本机验收",
            "本人",
            "2026-08-24T08:00:00Z",
        )
        .unwrap();
        fs::create_dir_all(&events).unwrap();
        let external = "{\"id\":\"external\",\"task_id\":\"tsk_write_test\"}\n";
        fs::write(events.join("2026-08.jsonl"), external).unwrap();
        let event_conflict =
            apply_task_note_at(&task_root, &events, note_request(preview, true)).unwrap_err();
        assert_eq!(event_conflict.code, "event_conflict");
        assert_eq!(
            fs::read_to_string(events.join("2026-08.jsonl")).unwrap(),
            external
        );

        let too_long = "字".repeat(MAX_NOTE_CHARS + 1);
        assert_eq!(
            preview_task_note_at(
                &task_root,
                &events,
                "tsk_write_test.md",
                "comment",
                &too_long,
                "本人",
                "2026-08-24T08:00:00Z",
            )
            .unwrap_err()
            .code,
            "invalid_value"
        );
        assert_eq!(
            fs::read_to_string(events.join("2026-08.jsonl")).unwrap(),
            external
        );
    }

    #[test]
    fn event_reader_exposes_note_text_only_for_bounded_note_events() {
        let dir = tempdir().unwrap();
        let long_message = "字".repeat(MAX_NOTE_CHARS + 1);
        let rows = [
            serde_json::json!({"id":"comment","task_id":"tsk_one","event_type":"comment_added","occurred_at":"2026-08-24T08:00:00Z","privacy":"general","message":"可见评论","author":"本人"}),
            serde_json::json!({"id":"status","task_id":"tsk_one","event_type":"status_changed","occurred_at":"2026-08-24T07:00:00Z","privacy":"general","message":"不得透传"}),
            serde_json::json!({"id":"too-long","task_id":"tsk_one","event_type":"manual_activity_added","occurred_at":"2026-08-24T06:00:00Z","privacy":"general","message":long_message}),
        ];
        let content = rows
            .iter()
            .map(|row| serde_json::to_string(row).unwrap())
            .collect::<Vec<_>>()
            .join("\n")
            + "\n";
        fs::write(dir.path().join("2026-08.jsonl"), content).unwrap();
        let loaded = read_events(dir.path(), "tsk_one").unwrap();
        assert_eq!(loaded.len(), 3);
        assert_eq!(loaded[0].message.as_deref(), Some("可见评论"));
        assert!(loaded[1].message.is_none());
        assert!(loaded[2].message.is_none());
    }

    #[test]
    fn project_mapping_requires_absolute_workdirs() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("projects.json");
        fs::write(&path, r#"[{"id":"p","name":"P","workdirs":["relative"]}]"#).unwrap();
        assert!(read_project_mappings(Some(&path)).is_err());
    }

    #[test]
    fn updater_requires_the_exact_rechecked_version() {
        assert!(validate_expected_update_version("1.2.0", "1.2.0").is_ok());
        assert!(validate_expected_update_version("1.2.0", "1.2.1").is_err());
        assert!(validate_expected_update_version("1.2.0\n", "1.2.0").is_err());
        assert!(validate_expected_update_version("", "1.2.0").is_err());
    }

    #[test]
    fn macos_https_proxy_parser_accepts_only_enabled_bounded_settings() {
        let enabled =
            "<dictionary> {\n  HTTPSEnable : 1\n  HTTPSPort : 6134\n  HTTPSProxy : 127.0.0.1\n}";
        assert_eq!(
            parse_macos_https_proxy(enabled).unwrap().as_str(),
            "http://127.0.0.1:6134/"
        );
        assert!(parse_macos_https_proxy(
            "HTTPSEnable : 0\nHTTPSPort : 6134\nHTTPSProxy : 127.0.0.1"
        )
        .is_none());
        assert!(parse_macos_https_proxy(
            "HTTPSEnable : 1\nHTTPSPort : 6134\nHTTPSProxy : proxy.invalid/path"
        )
        .is_none());
        assert!(parse_macos_https_proxy(
            "HTTPSEnable : 1\nHTTPSPort : 70000\nHTTPSProxy : 127.0.0.1"
        )
        .is_none());
    }

    #[test]
    fn updater_errors_keep_network_and_install_stages_distinct() {
        let network = tauri_plugin_updater::Error::Network("synthetic interruption".to_string());
        assert!(update_download_error(&network).contains("下载中断"));
        let install = tauri_plugin_updater::Error::Io(std::io::Error::new(
            std::io::ErrorKind::PermissionDenied,
            "synthetic permission failure",
        ));
        assert!(update_install_error(&install).contains("无法替换"));
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
    fn post_commit_failures_restore_task_and_event_exactly() {
        for fault in [CommitFault::AfterTaskCommit, CommitFault::AfterEventCommit] {
            let dir = tempdir().unwrap();
            let task_root = dir.path().join("任务");
            let events = dir.path().join("事件");
            fs::create_dir_all(&task_root).unwrap();
            fs::create_dir_all(&events).unwrap();
            let task_path = task_root.join("tsk_write_test.md");
            let event_path = events.join("2026-08.jsonl");
            let original = writable_task("medium");
            let original_events = "{\"id\":\"existing\",\"task_id\":\"tsk_write_test\"}\n";
            fs::write(&task_path, &original).unwrap();
            fs::write(&event_path, original_events).unwrap();
            let updated = original.replacen("priority: medium", "priority: high", 1);

            let error = commit_existing_task_change(
                &task_path,
                &original,
                &updated,
                &events,
                "tsk_write_test",
                "priority_changed",
                serde_json::json!({
                    "previous_priority": "medium",
                    "new_priority": "high"
                }),
                "2026-08-24T16:00:00+08:00",
                "2026-08",
                fault,
            )
            .unwrap_err();

            assert!(matches!(
                error.code,
                "event_commit_failed" | "readback_mismatch"
            ));
            assert_eq!(fs::read_to_string(&task_path).unwrap(), original);
            assert_eq!(fs::read_to_string(&event_path).unwrap(), original_events);
            assert!(!task_root.read_dir().unwrap().any(|entry| entry
                .unwrap()
                .file_name()
                .to_string_lossy()
                .contains("task-center")));
            assert!(!events.read_dir().unwrap().any(|entry| entry
                .unwrap()
                .file_name()
                .to_string_lossy()
                .contains("task-center")));
        }
    }

    #[test]
    fn late_task_and_event_conflicts_preserve_external_changes() {
        for fault in [
            CommitFault::BeforeTaskCommitExternalChange,
            CommitFault::BeforeEventCommitExternalChange,
        ] {
            let dir = tempdir().unwrap();
            let task_root = dir.path().join("任务");
            let events = dir.path().join("事件");
            fs::create_dir_all(&task_root).unwrap();
            fs::create_dir_all(&events).unwrap();
            let task_path = task_root.join("tsk_write_test.md");
            let event_path = events.join("2026-08.jsonl");
            let original = writable_task("medium");
            let original_events = "{\"id\":\"existing\",\"task_id\":\"tsk_write_test\"}\n";
            fs::write(&task_path, &original).unwrap();
            fs::write(&event_path, original_events).unwrap();
            let updated = original.replacen("priority: medium", "priority: high", 1);

            let error = commit_existing_task_change(
                &task_path,
                &original,
                &updated,
                &events,
                "tsk_write_test",
                "priority_changed",
                serde_json::json!({}),
                "2026-08-24T16:00:00+08:00",
                "2026-08",
                fault,
            )
            .unwrap_err();

            if fault == CommitFault::BeforeTaskCommitExternalChange {
                assert_eq!(error.code, "conflict");
                assert!(fs::read_to_string(&task_path)
                    .unwrap()
                    .contains("unknown_extension: external-change"));
                assert_eq!(fs::read_to_string(&event_path).unwrap(), original_events);
            } else {
                assert_eq!(error.code, "event_conflict");
                assert_eq!(fs::read_to_string(&task_path).unwrap(), original);
                assert!(fs::read_to_string(&event_path)
                    .unwrap()
                    .ends_with("{\"id\":\"external-concurrent-event\"}\n"));
            }
        }
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

    fn apply_field_fixture(
        task_root: &Path,
        events: &Path,
        field: &str,
        new_value: Value,
    ) -> TaskFieldEditReceipt {
        let preview = preview_task_field_edit_at(task_root, "tsk_write_test.md", field, &new_value)
            .unwrap_or_else(|error| panic!("preview failed for {field}: {error:?}"));
        apply_task_field_edit_at(
            task_root,
            events,
            TaskFieldEditRequest {
                file_token: preview.file_token,
                field: preview.field,
                new_value,
                expected_hash: preview.expected_hash,
                confirmed: true,
            },
            "2026-08-24T08:00:00Z",
            "2026-08",
        )
        .unwrap()
    }

    #[test]
    fn approved_fields_status_relations_archive_and_restore_round_trip() {
        let dir = tempdir().unwrap();
        let task_root = dir.path().join("任务");
        let events = dir.path().join("事件");
        fs::create_dir_all(&task_root).unwrap();
        let path = task_root.join("tsk_write_test.md");
        fs::write(&path, writable_task("medium")).unwrap();

        let cases = [
            ("title", serde_json::json!("Updated title")),
            ("task_status", serde_json::json!("done")),
            ("priority", serde_json::json!("low")),
            ("deadline", serde_json::json!("2026-09-30")),
            ("assignee", serde_json::json!("用户本人")),
            ("tags", serde_json::json!(["发布", "P1"])),
            ("parent_id", serde_json::json!("tsk_parent_task")),
            (
                "blocked_by_ids",
                serde_json::json!(["tsk_blocker_one", "tsk_blocker_two"]),
            ),
            (
                "related_ids",
                serde_json::json!(["tsk_relation_one", "tsk_relation_two"]),
            ),
            ("record_status", serde_json::json!("archived")),
            ("record_status", serde_json::json!("current")),
        ];
        let expected_event_count = cases.len();
        for (field, value) in cases {
            let receipt = apply_field_fixture(&task_root, &events, field, value.clone());
            assert_eq!(receipt.field, field);
            assert_eq!(receipt.new_value, value);
            assert!(receipt.verified);
        }

        let written = fs::read_to_string(path).unwrap();
        assert!(written.contains("title: \"Updated title\""));
        assert!(written.contains("task_status: done"));
        assert!(written.contains("priority: low"));
        assert!(written.contains("deadline: 2026-09-30"));
        assert!(written.contains("assignee: \"用户本人\""));
        assert!(written.contains("tags: [\"发布\",\"P1\"]"));
        assert!(written.contains("parent_id: tsk_parent_task"));
        assert!(written.contains("blocked_by_ids: [\"tsk_blocker_one\",\"tsk_blocker_two\"]"));
        assert!(written.contains("related_ids: [\"tsk_relation_one\",\"tsk_relation_two\"]"));
        assert!(written.contains("record_status: current"));
        assert!(written.contains("unknown_extension: keep-me"));
        assert!(written.ends_with("# Body\n\nKeep --- body formatting.\n"));
        let event_rows: Vec<Value> = fs::read_to_string(events.join("2026-08.jsonl"))
            .unwrap()
            .lines()
            .map(|line| serde_json::from_str(line).unwrap())
            .collect();
        assert_eq!(event_rows.len(), expected_event_count);
        assert_eq!(event_rows[1]["previous_task_status"], "doing");
        assert_eq!(event_rows[1]["new_task_status"], "done");
        assert_eq!(event_rows[9]["event_type"], "archived");
        assert_eq!(event_rows[10]["event_type"], "restored");
    }

    #[test]
    fn invalid_unknown_and_self_relation_fields_are_rejected_before_write() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("tsk_write_test.md");
        let original = writable_task("medium");
        fs::write(&path, &original).unwrap();
        let cases = [
            (
                "task_status",
                serde_json::json!("blocked"),
                "unsupported_value",
            ),
            ("deadline", serde_json::json!("2026-02-30"), "invalid_value"),
            (
                "related_ids",
                serde_json::json!(["tsk_write_test"]),
                "invalid_value",
            ),
            ("tags", serde_json::json!(["重复", "重复"]), "invalid_value"),
            (
                "parent_id",
                serde_json::json!("tsk_write_test"),
                "invalid_value",
            ),
            (
                "blocked_by_ids",
                serde_json::json!(["tsk_write_test"]),
                "invalid_value",
            ),
            ("comments", serde_json::json!([]), "unsupported_field"),
        ];
        for (field, value, expected_code) in cases {
            let error = preview_task_field_edit_at(dir.path(), "tsk_write_test.md", field, &value)
                .unwrap_err();
            assert_eq!(error.code, expected_code);
            assert_eq!(fs::read_to_string(&path).unwrap(), original);
        }
    }

    #[test]
    fn malformed_yaml_is_rejected_without_event_or_rewrite() {
        let dir = tempdir().unwrap();
        let task_root = dir.path().join("任务");
        let events = dir.path().join("事件");
        fs::create_dir_all(&task_root).unwrap();
        let path = task_root.join("tsk_broken.md");
        let original = "---\ntask_id: tsk_broken\npriority: [not closed\nprivacy: general\ncodex_access: proposal_only\n---\nBODY\n";
        fs::write(&path, original).unwrap();

        let error = preview_task_field_edit_at(
            &task_root,
            "tsk_broken.md",
            "priority",
            &serde_json::json!("high"),
        )
        .unwrap_err();

        assert_eq!(error.code, "invalid_frontmatter");
        assert_eq!(fs::read_to_string(path).unwrap(), original);
        assert!(!events.exists());
    }

    #[test]
    #[cfg_attr(windows, allow(clippy::permissions_set_readonly_false))]
    fn read_only_task_is_rejected_before_preview() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("tsk_write_test.md");
        fs::write(&path, writable_task("medium")).unwrap();
        let mut permissions = fs::metadata(&path).unwrap().permissions();
        permissions.set_readonly(true);
        fs::set_permissions(&path, permissions).unwrap();
        let error = preview_task_field_edit_at(
            dir.path(),
            "tsk_write_test.md",
            "priority",
            &serde_json::json!("high"),
        )
        .unwrap_err();
        assert_eq!(error.code, "read_only");

        #[cfg(windows)]
        {
            // On Windows this toggles a file attribute; it does not broaden Unix mode bits.
            let mut permissions = fs::metadata(&path).unwrap().permissions();
            permissions.set_readonly(false);
            fs::set_permissions(&path, permissions).unwrap();
        }
    }

    fn new_task_draft() -> NewTaskDraft {
        NewTaskDraft {
            title: "新建安全任务".to_string(),
            domain: "task_hub".to_string(),
            task_status: "todo".to_string(),
            priority: "medium".to_string(),
            assignee: "用户本人".to_string(),
            deadline: "2026-09-30".to_string(),
            tags: vec!["发布".to_string(), "安全".to_string()],
            parent_id: "tsk_parent_task".to_string(),
            blocked_by_ids: vec!["tsk_blocker".to_string()],
            related_ids: vec!["tsk_existing_relation".to_string()],
        }
    }

    #[test]
    fn create_preview_and_cancel_write_nothing() {
        let dir = tempdir().unwrap();
        let task_root = dir.path().join("任务");
        let events = dir.path().join("事件");
        fs::create_dir_all(&task_root).unwrap();
        let preview = preview_create_task_at(new_task_draft(), "2026-08-24T08:00:00Z").unwrap();
        assert!(preview.task_id.starts_with("tsk_"));
        assert!(preview.file_token.ends_with(".md"));
        assert!(task_root.read_dir().unwrap().next().is_none());
        let error = apply_create_task_at(
            &task_root,
            &events,
            CreateTaskRequest {
                preview,
                confirmed: false,
            },
            "2026-08",
        )
        .unwrap_err();
        assert_eq!(error.code, "confirmation_required");
        assert!(task_root.read_dir().unwrap().next().is_none());
        assert!(!events.exists());
    }

    #[test]
    fn create_task_writes_minimum_fields_event_and_readback() {
        let dir = tempdir().unwrap();
        let task_root = dir.path().join("任务");
        let events = dir.path().join("事件");
        fs::create_dir_all(&task_root).unwrap();
        let preview = preview_create_task_at(new_task_draft(), "2026-08-24T08:00:00Z").unwrap();
        let receipt = apply_create_task_at(
            &task_root,
            &events,
            CreateTaskRequest {
                preview: preview.clone(),
                confirmed: true,
            },
            "2026-08",
        )
        .unwrap();
        assert!(receipt.verified);
        assert_eq!(receipt.task_id, preview.task_id);
        let content = fs::read_to_string(task_root.join(&preview.file_token)).unwrap();
        assert_eq!(sha256_hex(content.as_bytes()), receipt.file_hash);
        for expected in [
            "record_type: task",
            "record_status: current",
            "task_status: todo",
            "privacy: general",
            "codex_access: proposal_only",
            "verification_status: human_confirmed",
            "ai_status: human_confirmed",
            "approval_status: accepted",
            "source_refs: [\"task-center-ui\"]",
            "tags: [\"发布\",\"安全\"]",
            "parent_id: tsk_parent_task",
            "blocked_by_ids: [\"tsk_blocker\"]",
        ] {
            assert!(content.contains(expected), "missing {expected}");
        }
        let event: Value = serde_json::from_str(
            fs::read_to_string(events.join("2026-08.jsonl"))
                .unwrap()
                .trim(),
        )
        .unwrap();
        assert_eq!(event["task_id"], preview.task_id);
        assert_eq!(event["event_type"], "created");
        assert_eq!(event["confirmed_by"], "user_ui_confirmation");
    }

    #[test]
    fn create_conflict_tamper_and_event_failure_leave_no_partial_task() {
        let dir = tempdir().unwrap();
        let task_root = dir.path().join("任务");
        let events = dir.path().join("事件");
        fs::create_dir_all(&task_root).unwrap();
        let preview = preview_create_task_at(new_task_draft(), "2026-08-24T08:00:00Z").unwrap();

        let mut tampered = preview.clone();
        tampered.draft.title = "预览后被篡改".to_string();
        let error = apply_create_task_at(
            &task_root,
            &events,
            CreateTaskRequest {
                preview: tampered,
                confirmed: true,
            },
            "2026-08",
        )
        .unwrap_err();
        assert_eq!(error.code, "preview_mismatch");
        assert!(task_root.read_dir().unwrap().next().is_none());

        fs::write(task_root.join(&preview.file_token), "existing").unwrap();
        let error = apply_create_task_at(
            &task_root,
            &events,
            CreateTaskRequest {
                preview: preview.clone(),
                confirmed: true,
            },
            "2026-08",
        )
        .unwrap_err();
        assert_eq!(error.code, "conflict");
        fs::remove_file(task_root.join(&preview.file_token)).unwrap();

        fs::create_dir_all(events.join("2026-08.jsonl")).unwrap();
        let error = apply_create_task_at(
            &task_root,
            &events,
            CreateTaskRequest {
                preview: preview.clone(),
                confirmed: true,
            },
            "2026-08",
        )
        .unwrap_err();
        assert_eq!(error.code, "event_prepare_failed");
        assert!(!task_root.join(preview.file_token).exists());
    }

    #[test]
    fn create_post_commit_failures_remove_task_and_restore_event() {
        for fault in [CommitFault::AfterTaskCommit, CommitFault::AfterEventCommit] {
            let dir = tempdir().unwrap();
            let task_root = dir.path().join("任务");
            let events = dir.path().join("事件");
            fs::create_dir_all(&task_root).unwrap();
            fs::create_dir_all(&events).unwrap();
            let event_path = events.join("2026-08.jsonl");
            let original_events = "{\"id\":\"existing\",\"task_id\":\"tsk_existing\"}\n";
            fs::write(&event_path, original_events).unwrap();
            let preview =
                preview_create_task_at(new_task_draft(), "2026-08-24T16:00:00+08:00").unwrap();

            let error = apply_create_task_at_with_fault(
                &task_root,
                &events,
                CreateTaskRequest {
                    preview: preview.clone(),
                    confirmed: true,
                },
                "2026-08",
                fault,
            )
            .unwrap_err();

            assert!(matches!(
                error.code,
                "event_commit_failed" | "readback_mismatch"
            ));
            assert!(!task_root.join(&preview.file_token).exists());
            assert_eq!(fs::read_to_string(&event_path).unwrap(), original_events);
            assert!(!task_root.read_dir().unwrap().any(|entry| entry
                .unwrap()
                .file_name()
                .to_string_lossy()
                .contains("task-center")));
            assert!(!events.read_dir().unwrap().any(|entry| entry
                .unwrap()
                .file_name()
                .to_string_lossy()
                .contains("task-center")));
        }
    }

    #[test]
    fn create_late_event_conflict_removes_new_task_but_preserves_external_event() {
        let dir = tempdir().unwrap();
        let task_root = dir.path().join("任务");
        let events = dir.path().join("事件");
        fs::create_dir_all(&task_root).unwrap();
        fs::create_dir_all(&events).unwrap();
        let event_path = events.join("2026-08.jsonl");
        fs::write(
            &event_path,
            "{\"id\":\"existing\",\"task_id\":\"tsk_existing\"}\n",
        )
        .unwrap();
        let preview =
            preview_create_task_at(new_task_draft(), "2026-08-24T16:00:00+08:00").unwrap();

        let error = apply_create_task_at_with_fault(
            &task_root,
            &events,
            CreateTaskRequest {
                preview: preview.clone(),
                confirmed: true,
            },
            "2026-08",
            CommitFault::BeforeEventCommitExternalChange,
        )
        .unwrap_err();

        assert_eq!(error.code, "event_conflict");
        assert!(!task_root.join(preview.file_token).exists());
        assert!(fs::read_to_string(event_path)
            .unwrap()
            .ends_with("{\"id\":\"external-concurrent-event\"}\n"));
    }
}
