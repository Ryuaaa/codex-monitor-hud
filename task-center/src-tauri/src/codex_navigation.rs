use crate::codex_history::{validate_thread_id, CodexHistoryError};
use serde::Serialize;
#[cfg(target_os = "macos")]
use std::{
    io::Write,
    path::Path,
    process::{Command, Stdio},
};

#[cfg(target_os = "macos")]
const CODEX_BUNDLE_ID: &str = "com.openai.codex";

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexOpenReceipt {
    mode: String,
    message: String,
}

fn thread_deep_link(thread_id: &str) -> Result<String, CodexHistoryError> {
    validate_thread_id(thread_id)?;
    Ok(format!("codex://threads/{thread_id}"))
}

#[cfg(target_os = "macos")]
pub(crate) fn open_codex_thread(thread_id: &str) -> Result<CodexOpenReceipt, CodexHistoryError> {
    open_codex_thread_with_tools(
        thread_id,
        Path::new("/usr/bin/open"),
        Path::new("/usr/bin/pbcopy"),
    )
}

#[cfg(not(target_os = "macos"))]
pub(crate) fn open_codex_thread(thread_id: &str) -> Result<CodexOpenReceipt, CodexHistoryError> {
    let _ = thread_deep_link(thread_id)?;
    Err(CodexHistoryError::new(
        "platform_unsupported",
        "当前版本只支持在 macOS 中打开 Codex 任务",
    ))
}

#[cfg(target_os = "macos")]
fn open_codex_thread_with_tools(
    thread_id: &str,
    open_tool: &Path,
    copy_tool: &Path,
) -> Result<CodexOpenReceipt, CodexHistoryError> {
    let deep_link = thread_deep_link(thread_id)?;
    if Command::new(open_tool)
        .arg(&deep_link)
        .status()
        .is_ok_and(|status| status.success())
    {
        return Ok(CodexOpenReceipt {
            mode: "deepLink".to_string(),
            message: "已在 Codex 中打开这个任务".to_string(),
        });
    }

    let opened = Command::new(open_tool)
        .args(["-b", CODEX_BUNDLE_ID])
        .status()
        .is_ok_and(|status| status.success());
    if !opened {
        return Err(CodexHistoryError::new(
            "codex_open_failed",
            "无法打开 Codex，请确认桌面应用已经安装",
        ));
    }

    let copied = copy_to_clipboard(copy_tool, thread_id);
    Ok(if copied {
        CodexOpenReceipt {
            mode: "openedAndCopied".to_string(),
            message: "已打开 Codex，并复制任务编号".to_string(),
        }
    } else {
        CodexOpenReceipt {
            mode: "openedOnly".to_string(),
            message: "已打开 Codex；任务编号复制失败，请使用任务详情中的编号查找".to_string(),
        }
    })
}

#[cfg(target_os = "macos")]
fn copy_to_clipboard(copy_tool: &Path, value: &str) -> bool {
    let Ok(mut child) = Command::new(copy_tool)
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
    else {
        return false;
    };
    let wrote = child
        .stdin
        .take()
        .is_some_and(|mut stdin| stdin.write_all(value.as_bytes()).is_ok());
    wrote && child.wait().is_ok_and(|status| status.success())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn deep_link_uses_only_a_validated_thread_id() {
        assert_eq!(
            thread_deep_link("019fc1c4-f575-7401-99a7-ec40fcf3135f").unwrap(),
            "codex://threads/019fc1c4-f575-7401-99a7-ec40fcf3135f"
        );
        assert_eq!(
            thread_deep_link("../../unsafe").unwrap_err().code,
            "invalid_thread_id"
        );
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn exact_deep_link_is_preferred_over_fallback() {
        let receipt = open_codex_thread_with_tools(
            "thread_test",
            Path::new("/usr/bin/true"),
            Path::new("/usr/bin/false"),
        )
        .unwrap();
        assert_eq!(receipt.mode, "deepLink");
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn fallback_opens_codex_and_copies_the_thread_id() {
        use std::{fs, os::unix::fs::PermissionsExt};

        let directory = tempfile::tempdir().unwrap();
        let open_tool = directory.path().join("fake-open");
        let copy_tool = directory.path().join("fake-copy");
        let copied = directory.path().join("copied.txt");
        fs::write(
            &open_tool,
            "#!/bin/sh\ncase \"$1\" in codex://*) exit 1 ;; esac\nexit 0\n",
        )
        .unwrap();
        fs::write(
            &copy_tool,
            format!("#!/bin/sh\n/bin/cat > '{}'\n", copied.display()),
        )
        .unwrap();
        for path in [&open_tool, &copy_tool] {
            let mut permissions = fs::metadata(path).unwrap().permissions();
            permissions.set_mode(0o755);
            fs::set_permissions(path, permissions).unwrap();
        }

        let receipt = open_codex_thread_with_tools("thread_test", &open_tool, &copy_tool).unwrap();
        assert_eq!(receipt.mode, "openedAndCopied");
        assert_eq!(fs::read_to_string(copied).unwrap(), "thread_test");
    }
}
