use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    env,
    fs::{self, File},
    io::{BufRead, BufReader, Read},
    path::{Path, PathBuf},
};
#[cfg(not(target_os = "windows"))]
use tauri::Manager;

const MAX_FRONTMATTER_BYTES: usize = 128 * 1024;
const MAX_BODY_BYTES: u64 = 2 * 1024 * 1024;

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
            load_project_mappings
        ])
        .on_window_event(|window, event| {
            if matches!(event, tauri::WindowEvent::CloseRequested { .. }) {
                // Windows can keep the event loop alive when AppHandle::exit is
                // requested from inside WM_CLOSE handling. This app has no tray,
                // sidecars, services, or pending writes, so terminate the isolated
                // process deterministically when its only window is closed.
                #[cfg(target_os = "windows")]
                std::process::exit(0);
                #[cfg(not(target_os = "windows"))]
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
}
