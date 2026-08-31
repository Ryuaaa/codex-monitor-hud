use crate::codex_history::{
    bounded_text, checked_result, codex_executable, observed_at, validate_thread_id, AppServer,
    CodexHistoryError,
};
use serde::Serialize;
use serde_json::{json, Value};
use std::{
    path::Path,
    sync::{
        atomic::{AtomicU64, Ordering},
        mpsc::{self, Receiver, Sender, TryRecvError},
        Arc, Mutex,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

const STARTUP_TIMEOUT: Duration = Duration::from_secs(15);
const MAX_TURN_RUNTIME: Duration = Duration::from_secs(2 * 60 * 60);
const STREAM_POLL_INTERVAL: Duration = Duration::from_millis(100);
const MAX_INPUT_CHARS: usize = 16_000;
const MAX_INPUT_BYTES: usize = 64 * 1024;
const MAX_ERROR_CHARS: usize = 320;
const APPROVAL_TIMEOUT: Duration = Duration::from_secs(10 * 60);
const INTERRUPT_GRACE: Duration = Duration::from_secs(5);

static NEXT_SESSION_ID: AtomicU64 = AtomicU64::new(1);

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexApprovalPrompt {
    request_id: String,
    kind: String,
    label: String,
    summary: Option<String>,
    reason: Option<String>,
    available_decisions: Vec<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CodexTurnSnapshot {
    session_id: String,
    thread_id: String,
    turn_id: Option<String>,
    state: String,
    started_at: i64,
    finished_at: Option<i64>,
    error_message: Option<String>,
    pending_approval: Option<CodexApprovalPrompt>,
}

impl CodexTurnSnapshot {
    fn starting(session_id: String, thread_id: String) -> Self {
        Self {
            session_id,
            thread_id,
            turn_id: None,
            state: "starting".to_string(),
            started_at: observed_at(),
            finished_at: None,
            error_message: None,
            pending_approval: None,
        }
    }

    fn is_terminal(&self) -> bool {
        matches!(
            self.state.as_str(),
            "completed" | "failed" | "interrupted" | "timedOut"
        )
    }
}

enum TurnControl {
    RespondApproval {
        request_id: String,
        decision: String,
    },
    Interrupt,
    Shutdown,
}

#[derive(Debug)]
struct PendingApproval {
    protocol_id: Value,
    public_id: String,
    available_decisions: Vec<String>,
    started: Instant,
}

struct TurnSession {
    snapshot: Arc<Mutex<CodexTurnSnapshot>>,
    control: Sender<TurnControl>,
    worker: Option<JoinHandle<()>>,
}

impl TurnSession {
    fn finish(mut self) {
        let _ = self.control.send(TurnControl::Shutdown);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

#[derive(Default)]
pub(crate) struct CodexTurnManager {
    current: Mutex<Option<TurnSession>>,
}

impl CodexTurnManager {
    pub(crate) fn start(
        &self,
        thread_id: String,
        input: String,
    ) -> Result<CodexTurnSnapshot, CodexHistoryError> {
        let executable = codex_executable()?;
        self.start_with_executable(executable.as_path(), thread_id, input)
    }

    fn start_with_executable(
        &self,
        executable: &Path,
        thread_id: String,
        input: String,
    ) -> Result<CodexTurnSnapshot, CodexHistoryError> {
        validate_thread_id(&thread_id)?;
        validate_turn_input(&input)?;

        let mut current = self.current.lock().map_err(|_| {
            CodexHistoryError::new("runtime_unavailable", "Codex 继续任务状态不可用")
        })?;
        if let Some(session) = current.as_ref() {
            refresh_finished_worker(session);
        }
        if current.as_ref().is_some_and(|session| {
            session
                .snapshot
                .lock()
                .map(|snapshot| !snapshot.is_terminal())
                .unwrap_or(true)
        }) {
            return Err(CodexHistoryError::new(
                "turn_already_running",
                "任务中心已有一个 Codex 任务在运行",
            ));
        }
        if let Some(previous) = current.take() {
            previous.finish();
        }

        let sequence = NEXT_SESSION_ID.fetch_add(1, Ordering::Relaxed);
        let session_id = format!("turn-{}-{sequence}", observed_at());
        let initial = CodexTurnSnapshot::starting(session_id.clone(), thread_id.clone());
        let snapshot = Arc::new(Mutex::new(initial.clone()));
        let (control_tx, control_rx) = mpsc::channel();
        let worker_snapshot = Arc::clone(&snapshot);
        let executable = executable.to_path_buf();
        let worker = thread::spawn(move || {
            run_turn_worker(
                executable.as_path(),
                &thread_id,
                &input,
                &worker_snapshot,
                control_rx,
            );
        });
        *current = Some(TurnSession {
            snapshot,
            control: control_tx,
            worker: Some(worker),
        });
        Ok(initial)
    }

    pub(crate) fn status(&self, session_id: &str) -> Result<CodexTurnSnapshot, CodexHistoryError> {
        let current = self.current.lock().map_err(|_| {
            CodexHistoryError::new("runtime_unavailable", "Codex 继续任务状态不可用")
        })?;
        let session = current.as_ref().ok_or_else(|| {
            CodexHistoryError::new("turn_not_found", "没有找到该次 Codex 继续任务")
        })?;
        refresh_finished_worker(session);
        let snapshot = session.snapshot.lock().map_err(|_| {
            CodexHistoryError::new("runtime_unavailable", "Codex 继续任务状态不可用")
        })?;
        if snapshot.session_id != session_id {
            return Err(CodexHistoryError::new(
                "turn_not_found",
                "没有找到该次 Codex 继续任务",
            ));
        }
        Ok(snapshot.clone())
    }

    pub(crate) fn respond_approval(
        &self,
        session_id: &str,
        request_id: String,
        decision: String,
    ) -> Result<CodexTurnSnapshot, CodexHistoryError> {
        if request_id.is_empty() || request_id.len() > 160 {
            return Err(CodexHistoryError::new(
                "invalid_approval",
                "Codex 授权请求编号无效",
            ));
        }
        if !matches!(
            decision.as_str(),
            "accept" | "acceptForSession" | "decline" | "cancel"
        ) {
            return Err(CodexHistoryError::new(
                "invalid_approval",
                "Codex 授权选项无效",
            ));
        }
        let current = self.current.lock().map_err(|_| {
            CodexHistoryError::new("runtime_unavailable", "Codex 继续任务状态不可用")
        })?;
        let session = session_for_id(current.as_ref(), session_id)?;
        let value = session.snapshot.lock().map_err(|_| {
            CodexHistoryError::new("runtime_unavailable", "Codex 继续任务状态不可用")
        })?;
        let approval_matches = value.pending_approval.as_ref().is_some_and(|prompt| {
            prompt.request_id == request_id
                && prompt
                    .available_decisions
                    .iter()
                    .any(|item| item == &decision)
        });
        if !approval_matches {
            return Err(CodexHistoryError::new(
                "approval_changed",
                "Codex 授权请求已变化，没有执行旧选择",
            ));
        }
        session
            .control
            .send(TurnControl::RespondApproval {
                request_id,
                decision,
            })
            .map_err(|_| CodexHistoryError::new("provider_exited", "Codex 继续任务连接已关闭"))?;
        Ok(value.clone())
    }

    pub(crate) fn interrupt(
        &self,
        session_id: &str,
    ) -> Result<CodexTurnSnapshot, CodexHistoryError> {
        let current = self.current.lock().map_err(|_| {
            CodexHistoryError::new("runtime_unavailable", "Codex 继续任务状态不可用")
        })?;
        let session = session_for_id(current.as_ref(), session_id)?;
        let mut value = session.snapshot.lock().map_err(|_| {
            CodexHistoryError::new("runtime_unavailable", "Codex 继续任务状态不可用")
        })?;
        if value.is_terminal() {
            return Ok(value.clone());
        }
        session
            .control
            .send(TurnControl::Interrupt)
            .map_err(|_| CodexHistoryError::new("provider_exited", "Codex 继续任务连接已关闭"))?;
        value.state = "interrupting".to_string();
        value.pending_approval = None;
        Ok(value.clone())
    }

    pub(crate) fn shutdown_all(&self) {
        if let Ok(mut current) = self.current.lock() {
            if let Some(session) = current.take() {
                session.finish();
            }
        }
    }
}

fn refresh_finished_worker(session: &TurnSession) {
    let finished = session.worker.as_ref().is_some_and(JoinHandle::is_finished);
    if !finished {
        return;
    }
    if let Ok(mut snapshot) = session.snapshot.lock() {
        if !snapshot.is_terminal() {
            snapshot.state = "failed".to_string();
            snapshot.finished_at = Some(observed_at());
            snapshot.error_message = Some("Codex 继续任务异常结束".to_string());
            snapshot.pending_approval = None;
        }
    }
}

fn session_for_id<'a>(
    session: Option<&'a TurnSession>,
    session_id: &str,
) -> Result<&'a TurnSession, CodexHistoryError> {
    let session = session
        .ok_or_else(|| CodexHistoryError::new("turn_not_found", "没有找到该次 Codex 继续任务"))?;
    let matches = session
        .snapshot
        .lock()
        .map(|snapshot| snapshot.session_id == session_id)
        .unwrap_or(false);
    if !matches {
        return Err(CodexHistoryError::new(
            "turn_not_found",
            "没有找到该次 Codex 继续任务",
        ));
    }
    Ok(session)
}

impl Drop for CodexTurnManager {
    fn drop(&mut self) {
        if let Ok(current) = self.current.get_mut() {
            if let Some(session) = current.take() {
                session.finish();
            }
        }
    }
}

fn validate_turn_input(input: &str) -> Result<(), CodexHistoryError> {
    let trimmed = input.trim();
    if trimmed.is_empty() {
        return Err(CodexHistoryError::new(
            "invalid_turn_input",
            "请先输入要继续交给 Codex 的内容",
        ));
    }
    if input.len() > MAX_INPUT_BYTES || input.chars().count() > MAX_INPUT_CHARS {
        return Err(CodexHistoryError::new(
            "turn_input_too_large",
            "本次输入过长，请缩短后再继续",
        ));
    }
    if input
        .chars()
        .any(|character| character.is_control() && !matches!(character, '\n' | '\r' | '\t'))
    {
        return Err(CodexHistoryError::new(
            "invalid_turn_input",
            "本次输入包含不可用字符",
        ));
    }
    Ok(())
}

fn initialize_request() -> Value {
    json!({
        "id": 1,
        "method": "initialize",
        "params": {
            "clientInfo": {
                "name": "codex-monitor-task-center",
                "title": "Codex Monitor Task Center",
                "version": env!("CARGO_PKG_VERSION")
            },
            "capabilities": {
                // `thread/resume.excludeTurns` is deliberately used so resuming a task does not
                // load its full conversation into the always-lightweight task center process.
                "experimentalApi": true,
                "optOutNotificationMethods": [
                    "item/agentMessage/delta",
                    "item/started",
                    "item/completed",
                    "item/reasoning/summaryTextDelta",
                    "item/reasoning/summaryPartAdded",
                    "item/reasoning/textDelta",
                    "item/commandExecution/outputDelta",
                    "command/exec/outputDelta",
                    "process/outputDelta",
                    "rawResponseItem/completed",
                    "rawResponse/completed",
                    "remoteControl/status/changed"
                ]
            }
        }
    })
}

fn resume_request(thread_id: &str) -> Value {
    json!({
        "id": 2,
        "method": "thread/resume",
        "params": {
            "threadId": thread_id,
            "excludeTurns": true
        }
    })
}

fn start_request(thread_id: &str, input: &str) -> Value {
    json!({
        "id": 3,
        "method": "turn/start",
        "params": {
            "threadId": thread_id,
            "input": [{
                "type": "text",
                "text": input,
                "text_elements": []
            }]
        }
    })
}

fn contains_sensitive_marker(value: &str) -> bool {
    let lower = value.to_ascii_lowercase();
    [
        "authorization",
        "cookie",
        "password",
        "passwd",
        "api_key",
        "api-key",
        "secret",
        "token",
    ]
    .iter()
    .any(|marker| lower.contains(marker))
}

fn simple_decisions(value: Option<&Value>) -> Vec<String> {
    let mut decisions: Vec<String> = value
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .filter(|decision| {
            matches!(
                *decision,
                "accept" | "acceptForSession" | "decline" | "cancel"
            )
        })
        .map(str::to_string)
        .collect();
    if decisions.is_empty() {
        decisions = ["accept", "acceptForSession", "decline", "cancel"]
            .into_iter()
            .map(str::to_string)
            .collect();
    }
    decisions
}

fn parse_approval_request(
    message: &Value,
    thread_id: &str,
    turn_id: &str,
    approval_sequence: u64,
) -> Result<(CodexApprovalPrompt, PendingApproval), CodexHistoryError> {
    let method = message
        .get("method")
        .and_then(Value::as_str)
        .ok_or_else(|| protocol_error("Codex 授权请求缺少方法"))?;
    let params = message
        .get("params")
        .ok_or_else(|| protocol_error("Codex 授权请求缺少参数"))?;
    if params.get("threadId").and_then(Value::as_str) != Some(thread_id)
        || params.get("turnId").and_then(Value::as_str) != Some(turn_id)
    {
        return Err(protocol_error("Codex 授权请求与当前任务不一致"));
    }
    let protocol_id = message
        .get("id")
        .filter(|id| id.is_string() || id.is_number())
        .cloned()
        .ok_or_else(|| protocol_error("Codex 授权请求缺少编号"))?;
    let public_id = format!("approval-{approval_sequence}");
    let reason = params
        .get("reason")
        .and_then(Value::as_str)
        .filter(|value| !contains_sensitive_marker(value))
        .and_then(|value| bounded_text(value, 240));

    let (kind, label, summary, mut available_decisions) = match method {
        "item/commandExecution/requestApproval" => {
            let command = params.get("command").and_then(Value::as_str);
            let sensitive = command.is_some_and(contains_sensitive_marker);
            let summary = if sensitive {
                Some("命令可能包含敏感信息，请改在 Codex 中审核".to_string())
            } else {
                command.and_then(|value| bounded_text(value, 360))
            };
            let decisions = simple_decisions(params.get("availableDecisions"));
            (
                "command".to_string(),
                "Codex 请求执行命令".to_string(),
                summary,
                if sensitive {
                    decisions
                        .into_iter()
                        .filter(|decision| matches!(decision.as_str(), "decline" | "cancel"))
                        .collect()
                } else {
                    decisions
                },
            )
        }
        "item/fileChange/requestApproval" => {
            let summary = params
                .get("grantRoot")
                .and_then(Value::as_str)
                .and_then(|root| Path::new(root).file_name())
                .and_then(|name| name.to_str())
                .and_then(|name| bounded_text(name, 120))
                .map(|name| format!("请求写入目录：{name}"));
            (
                "fileChange".to_string(),
                "Codex 请求修改文件".to_string(),
                summary,
                ["accept", "acceptForSession", "decline", "cancel"]
                    .into_iter()
                    .map(str::to_string)
                    .collect(),
            )
        }
        _ => {
            return Err(CodexHistoryError::new(
                "unsupported_approval",
                "Codex 请求了任务中心尚未安全支持的交互",
            ))
        }
    };
    if available_decisions.is_empty() {
        available_decisions = vec!["cancel".to_string()];
    }
    let prompt = CodexApprovalPrompt {
        request_id: public_id.clone(),
        kind,
        label,
        summary,
        reason,
        available_decisions: available_decisions.clone(),
    };
    let pending = PendingApproval {
        protocol_id,
        public_id,
        available_decisions,
        started: Instant::now(),
    };
    Ok((prompt, pending))
}

fn send_interrupt(
    server: &mut AppServer,
    thread_id: &str,
    turn_id: &str,
) -> Result<(), CodexHistoryError> {
    server.send(&json!({
        "id": 9000001,
        "method": "turn/interrupt",
        "params": {"threadId": thread_id, "turnId": turn_id}
    }))
}

fn set_failed(snapshot: &Arc<Mutex<CodexTurnSnapshot>>, error: CodexHistoryError) {
    if let Ok(mut value) = snapshot.lock() {
        value.state = if error.code == "interrupted" {
            "interrupted".to_string()
        } else {
            "failed".to_string()
        };
        value.finished_at = Some(observed_at());
        value.error_message = bounded_text(&error.message, MAX_ERROR_CHARS);
        value.pending_approval = None;
    }
}

fn set_terminal_from_turn(snapshot: &Arc<Mutex<CodexTurnSnapshot>>, turn: &Value) {
    let status = turn
        .get("status")
        .and_then(Value::as_str)
        .unwrap_or("failed");
    let state = match status {
        "completed" => "completed",
        "interrupted" => "interrupted",
        "failed" => "failed",
        _ => "failed",
    };
    let error_message = turn
        .get("error")
        .and_then(|error| error.get("message"))
        .and_then(Value::as_str)
        .and_then(|message| bounded_text(message, MAX_ERROR_CHARS));
    if let Ok(mut value) = snapshot.lock() {
        value.state = state.to_string();
        value.finished_at = Some(observed_at());
        value.error_message = error_message;
        value.pending_approval = None;
    }
}

fn wait_response(
    server: &AppServer,
    expected_id: i64,
    deadline: Instant,
    control: &Receiver<TurnControl>,
) -> Result<Value, CodexHistoryError> {
    loop {
        match control.try_recv() {
            Ok(TurnControl::Interrupt | TurnControl::Shutdown)
            | Err(TryRecvError::Disconnected) => {
                return Err(CodexHistoryError::new(
                    "interrupted",
                    "Codex 继续任务已在启动阶段中断",
                ))
            }
            Ok(TurnControl::RespondApproval { .. }) | Err(TryRecvError::Empty) => {}
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(CodexHistoryError::new("timeout", "Codex 官方接口启动超时"));
        }
        if let Some(value) = server.receive(remaining.min(STREAM_POLL_INTERVAL))? {
            if value.get("id").and_then(Value::as_i64) == Some(expected_id) {
                return Ok(value);
            }
        }
    }
}

fn protocol_error(message: &str) -> CodexHistoryError {
    CodexHistoryError::new("protocol_changed", message)
}

fn run_turn_worker(
    executable: &Path,
    thread_id: &str,
    input: &str,
    snapshot: &Arc<Mutex<CodexTurnSnapshot>>,
    control: Receiver<TurnControl>,
) {
    let result = (|| -> Result<(), CodexHistoryError> {
        let mut server = AppServer::start_streaming(executable)?;
        let startup_deadline = Instant::now() + STARTUP_TIMEOUT;

        server.send(&initialize_request())?;
        checked_result(&wait_response(&server, 1, startup_deadline, &control)?)?;
        server.send(&json!({"method": "initialized", "params": {}}))?;

        server.send(&resume_request(thread_id))?;
        let resumed = wait_response(&server, 2, startup_deadline, &control)?;
        let resumed = checked_result(&resumed)?;
        if resumed
            .get("thread")
            .and_then(|thread| thread.get("id"))
            .and_then(Value::as_str)
            != Some(thread_id)
        {
            return Err(protocol_error("Codex 恢复的任务编号不一致"));
        }

        server.send(&start_request(thread_id, input))?;
        let started = wait_response(&server, 3, startup_deadline, &control)?;
        let started = checked_result(&started)?;
        let turn_id = started
            .get("turn")
            .and_then(|turn| turn.get("id"))
            .and_then(Value::as_str)
            .ok_or_else(|| protocol_error("Codex 未返回新一轮任务编号"))?
            .to_string();
        if let Ok(mut value) = snapshot.lock() {
            value.turn_id = Some(turn_id.clone());
            value.state = "running".to_string();
        }

        let turn_deadline = Instant::now() + MAX_TURN_RUNTIME;
        let mut pending_approval: Option<PendingApproval> = None;
        let mut approval_sequence = 1u64;
        let mut interrupt_deadline: Option<(Instant, &'static str)> = None;
        loop {
            match control.try_recv() {
                Ok(TurnControl::RespondApproval {
                    request_id,
                    decision,
                }) => {
                    let Some(pending) = pending_approval.as_ref() else {
                        continue;
                    };
                    if pending.public_id != request_id
                        || !pending
                            .available_decisions
                            .iter()
                            .any(|item| item == &decision)
                    {
                        continue;
                    }
                    server.send(&json!({
                        "id": pending.protocol_id,
                        "result": {"decision": decision}
                    }))?;
                    pending_approval = None;
                    if let Ok(mut value) = snapshot.lock() {
                        value.state = "running".to_string();
                        value.pending_approval = None;
                        value.error_message = None;
                    }
                }
                Ok(TurnControl::Interrupt) => {
                    if interrupt_deadline.is_none() {
                        send_interrupt(&mut server, thread_id, &turn_id)?;
                        pending_approval = None;
                        interrupt_deadline =
                            Some((Instant::now() + INTERRUPT_GRACE, "interrupted"));
                        if let Ok(mut value) = snapshot.lock() {
                            value.state = "interrupting".to_string();
                            value.pending_approval = None;
                        }
                    }
                }
                Ok(TurnControl::Shutdown) | Err(TryRecvError::Disconnected) => {
                    let _ = send_interrupt(&mut server, thread_id, &turn_id);
                    if let Ok(mut value) = snapshot.lock() {
                        value.state = "interrupted".to_string();
                        value.finished_at = Some(observed_at());
                        value.error_message =
                            Some("任务中心已关闭，它启动的本轮任务已要求中断".to_string());
                        value.pending_approval = None;
                    }
                    server.stop();
                    return Ok(());
                }
                Err(TryRecvError::Empty) => {}
            }

            if interrupt_deadline.is_none() && Instant::now() >= turn_deadline {
                send_interrupt(&mut server, thread_id, &turn_id)?;
                pending_approval = None;
                interrupt_deadline = Some((Instant::now() + INTERRUPT_GRACE, "timedOut"));
                if let Ok(mut value) = snapshot.lock() {
                    value.state = "interrupting".to_string();
                    value.pending_approval = None;
                    value.error_message = Some("本轮已运行2小时，正在请求 Codex 中断".to_string());
                }
            }

            if pending_approval
                .as_ref()
                .is_some_and(|pending| pending.started.elapsed() >= APPROVAL_TIMEOUT)
            {
                if let Some(pending) = pending_approval.take() {
                    server.send(&json!({
                        "id": pending.protocol_id,
                        "result": {"decision": "cancel"}
                    }))?;
                    if let Ok(mut value) = snapshot.lock() {
                        value.state = "running".to_string();
                        value.pending_approval = None;
                        value.error_message =
                            Some("授权等待超过10分钟，任务中心已取消该操作".to_string());
                    }
                }
            }

            if let Some((deadline, outcome)) = interrupt_deadline {
                if Instant::now() >= deadline {
                    if let Ok(mut value) = snapshot.lock() {
                        value.state = outcome.to_string();
                        value.finished_at = Some(observed_at());
                        value.pending_approval = None;
                        value.error_message = Some(if outcome == "timedOut" {
                            "本次连续运行超过2小时，已发送中断请求并停止连接".to_string()
                        } else {
                            "已发送中断请求并停止连接".to_string()
                        });
                    }
                    server.stop();
                    return Ok(());
                }
            }

            let Some(message) = server.receive(STREAM_POLL_INTERVAL)? else {
                continue;
            };
            let method = message.get("method").and_then(Value::as_str);
            if method == Some("turn/completed") {
                let params = message
                    .get("params")
                    .ok_or_else(|| protocol_error("Codex 任务完成通知缺少参数"))?;
                if params.get("threadId").and_then(Value::as_str) != Some(thread_id) {
                    continue;
                }
                let turn = params
                    .get("turn")
                    .ok_or_else(|| protocol_error("Codex 任务完成通知缺少轮次"))?;
                if turn.get("id").and_then(Value::as_str) != Some(turn_id.as_str()) {
                    continue;
                }
                set_terminal_from_turn(snapshot, turn);
                server.stop();
                return Ok(());
            }

            if message.get("id").is_some() && method.is_some() {
                match parse_approval_request(&message, thread_id, &turn_id, approval_sequence) {
                    Ok((prompt, pending)) => {
                        approval_sequence += 1;
                        pending_approval = Some(pending);
                        if let Ok(mut value) = snapshot.lock() {
                            value.state = "waitingApproval".to_string();
                            value.pending_approval = Some(prompt);
                            value.error_message = None;
                        }
                    }
                    Err(error) => {
                        send_interrupt(&mut server, thread_id, &turn_id)?;
                        if let Ok(mut value) = snapshot.lock() {
                            value.state = "interrupting".to_string();
                            value.pending_approval = None;
                            value.error_message = bounded_text(&error.message, MAX_ERROR_CHARS);
                        }
                        interrupt_deadline = Some((Instant::now() + INTERRUPT_GRACE, "failed"));
                    }
                }
            }
        }
    })();

    if let Err(error) = result {
        set_failed(snapshot, error);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn continuation_requests_follow_the_official_protocol() {
        let initialize = initialize_request();
        assert_eq!(
            initialize["params"]["capabilities"]["experimentalApi"],
            true
        );

        let resume = resume_request("thread_123");
        assert_eq!(resume["method"], "thread/resume");
        assert_eq!(resume["params"]["threadId"], "thread_123");
        assert_eq!(resume["params"]["excludeTurns"], true);

        let start = start_request("thread_123", "继续完成当前任务");
        assert_eq!(start["method"], "turn/start");
        assert_eq!(start["params"]["threadId"], "thread_123");
        assert_eq!(start["params"]["input"][0]["type"], "text");
        assert_eq!(start["params"]["input"][0]["text"], "继续完成当前任务");
        assert_eq!(start["params"]["input"][0]["text_elements"], json!([]));

        let serialized = start.to_string();
        assert!(!serialized.contains("approvalPolicy"));
        assert!(!serialized.contains("sandboxPolicy"));
        assert!(!serialized.contains("cwd"));
    }

    #[test]
    fn completion_event_updates_only_safe_metadata() {
        let snapshot = Arc::new(Mutex::new(CodexTurnSnapshot::starting(
            "turn-test".to_string(),
            "thread_123".to_string(),
        )));
        set_terminal_from_turn(
            &snapshot,
            &json!({
                "id": "turn_456",
                "status": "failed",
                "error": {
                    "message": "网络连接失败",
                    "additionalDetails": "不应跨过边界的内部细节"
                },
                "items": [{"type": "agentMessage", "text": "不应返回的对话正文"}]
            }),
        );
        let value = snapshot.lock().unwrap().clone();
        assert_eq!(value.state, "failed");
        assert_eq!(value.error_message.as_deref(), Some("网络连接失败"));
        let serialized = serde_json::to_string(&value).unwrap();
        assert!(!serialized.contains("对话正文"));
        assert!(!serialized.contains("内部细节"));
    }

    #[test]
    fn input_validation_is_bounded_and_allows_normal_multiline_text() {
        assert!(validate_turn_input("继续\n并运行测试").is_ok());
        assert_eq!(
            validate_turn_input(" \n ").unwrap_err().code,
            "invalid_turn_input"
        );
        assert_eq!(
            validate_turn_input(&"x".repeat(MAX_INPUT_CHARS + 1))
                .unwrap_err()
                .code,
            "turn_input_too_large"
        );
        assert_eq!(
            validate_turn_input("unsafe\0text").unwrap_err().code,
            "invalid_turn_input"
        );
    }

    #[test]
    fn command_approval_keeps_only_simple_official_decisions() {
        let (prompt, pending) = parse_approval_request(
            &json!({
                "id": "request-1",
                "method": "item/commandExecution/requestApproval",
                "params": {
                    "threadId": "thread_123",
                    "turnId": "turn_456",
                    "command": "npm test",
                    "reason": "运行本地测试",
                    "availableDecisions": [
                        "accept",
                        {"acceptWithExecpolicyAmendment": {}},
                        "decline"
                    ]
                }
            }),
            "thread_123",
            "turn_456",
            1,
        )
        .unwrap();
        assert_eq!(prompt.request_id, "approval-1");
        assert_eq!(prompt.summary.as_deref(), Some("npm test"));
        assert_eq!(prompt.available_decisions, vec!["accept", "decline"]);
        assert_eq!(pending.protocol_id, json!("request-1"));
    }

    #[test]
    fn sensitive_command_is_never_exposed_or_approved_here() {
        let (prompt, _) = parse_approval_request(
            &json!({
                "id": 44,
                "method": "item/commandExecution/requestApproval",
                "params": {
                    "threadId": "thread_123",
                    "turnId": "turn_456",
                    "command": "curl -H 'Authorization: Bearer private-token' example.invalid",
                    "availableDecisions": ["accept", "decline", "cancel"]
                }
            }),
            "thread_123",
            "turn_456",
            2,
        )
        .unwrap();
        assert_eq!(prompt.available_decisions, vec!["decline", "cancel"]);
        let serialized = serde_json::to_string(&prompt).unwrap();
        assert!(!serialized.contains("private-token"));
        assert!(!serialized.contains("Bearer"));
    }

    #[test]
    fn unsupported_interaction_fails_closed() {
        let error = parse_approval_request(
            &json!({
                "id": 9,
                "method": "item/tool/requestUserInput",
                "params": {"threadId": "thread_123", "turnId": "turn_456"}
            }),
            "thread_123",
            "turn_456",
            1,
        )
        .unwrap_err();
        assert_eq!(error.code, "unsupported_approval");
    }

    #[cfg(unix)]
    #[test]
    fn shutting_down_manager_reaps_the_app_server_process() {
        use std::os::unix::fs::PermissionsExt;

        let directory = tempfile::tempdir().unwrap();
        let executable = directory.path().join("fake-codex");
        let pid_file = directory.path().join("provider.pid");
        let script = format!(
            "#!/bin/sh\n\
             echo $$ > '{}'\n\
             while IFS= read -r line; do\n\
               case \"$line\" in\n\
                 *'\"method\":\"initialize\"'*) echo '{{\"id\":1,\"result\":{{}}}}' ;;\n\
                 *'\"method\":\"thread/resume\"'*) echo '{{\"id\":2,\"result\":{{\"thread\":{{\"id\":\"thread_test\"}}}}}}' ;;\n\
                 *'\"method\":\"turn/start\"'*) echo '{{\"id\":3,\"result\":{{\"turn\":{{\"id\":\"turn_test\",\"status\":\"inProgress\"}}}}}}' ;;\n\
                 *'\"method\":\"turn/interrupt\"'*) echo '{{\"id\":9000001,\"result\":{{}}}}' ;;\n\
               esac\n\
             done\n",
            pid_file.display()
        );
        std::fs::write(&executable, script).unwrap();
        let mut permissions = std::fs::metadata(&executable).unwrap().permissions();
        permissions.set_mode(0o700);
        std::fs::set_permissions(&executable, permissions).unwrap();

        let manager = CodexTurnManager::default();
        let initial = manager
            .start_with_executable(&executable, "thread_test".to_string(), "继续".to_string())
            .unwrap();
        // The full test suite creates several temporary processes in parallel. Give the fake
        // provider enough startup time on a loaded release builder while still failing quickly.
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            if manager.status(&initial.session_id).unwrap().state == "running" {
                break;
            }
            assert!(
                Instant::now() < deadline,
                "fake provider did not reach running"
            );
            thread::sleep(Duration::from_millis(10));
        }
        manager.shutdown_all();

        let pid = std::fs::read_to_string(pid_file).unwrap();
        let running = std::process::Command::new("kill")
            .args(["-0", pid.trim()])
            .stderr(std::process::Stdio::null())
            .status()
            .map(|status| status.success())
            .unwrap_or(false);
        assert!(!running, "app-server child remained after shutdown");
    }

    #[cfg(unix)]
    #[test]
    fn scripted_app_server_completes_resume_and_turn_start_end_to_end() {
        use std::os::unix::fs::PermissionsExt;

        let directory = tempfile::tempdir().unwrap();
        let executable = directory.path().join("fake-codex-complete");
        let script = "#!/bin/sh\n\
            while IFS= read -r line; do\n\
              case \"$line\" in\n\
                *'\"method\":\"initialize\"'*) echo '{\"id\":1,\"result\":{}}' ;;\n\
                *'\"method\":\"thread/resume\"'*) echo '{\"id\":2,\"result\":{\"thread\":{\"id\":\"thread_test\"}}}' ;;\n\
                *'\"method\":\"turn/start\"'*)\n\
                  echo '{\"id\":3,\"result\":{\"turn\":{\"id\":\"turn_test\",\"status\":\"inProgress\"}}}'\n\
                  echo '{\"method\":\"turn/completed\",\"params\":{\"threadId\":\"thread_test\",\"turn\":{\"id\":\"turn_test\",\"status\":\"completed\",\"items\":[{\"type\":\"agentMessage\",\"text\":\"must-not-cross-boundary\"}]}}}' ;;\n\
              esac\n\
            done\n";
        std::fs::write(&executable, script).unwrap();
        let mut permissions = std::fs::metadata(&executable).unwrap().permissions();
        permissions.set_mode(0o700);
        std::fs::set_permissions(&executable, permissions).unwrap();

        let manager = CodexTurnManager::default();
        let initial = manager
            .start_with_executable(&executable, "thread_test".to_string(), "继续".to_string())
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(2);
        let completed = loop {
            let current = manager.status(&initial.session_id).unwrap();
            if current.is_terminal() {
                break current;
            }
            assert!(Instant::now() < deadline, "scripted turn did not finish");
            thread::sleep(Duration::from_millis(10));
        };
        assert_eq!(completed.state, "completed");
        assert_eq!(completed.turn_id.as_deref(), Some("turn_test"));
        let serialized = serde_json::to_string(&completed).unwrap();
        assert!(!serialized.contains("must-not-cross-boundary"));
        manager.shutdown_all();
    }

    #[cfg(unix)]
    #[test]
    fn provider_crash_is_isolated_as_a_failed_turn() {
        use std::os::unix::fs::PermissionsExt;

        let directory = tempfile::tempdir().unwrap();
        let executable = directory.path().join("fake-codex-crash");
        let script = "#!/bin/sh\n\
            while IFS= read -r line; do\n\
              case \"$line\" in\n\
                *'\"method\":\"initialize\"'*) echo '{\"id\":1,\"result\":{}}' ;;\n\
                *'\"method\":\"thread/resume\"'*) echo '{\"id\":2,\"result\":{\"thread\":{\"id\":\"thread_test\"}}}' ;;\n\
                *'\"method\":\"turn/start\"'*)\n\
                  echo '{\"id\":3,\"result\":{\"turn\":{\"id\":\"turn_test\",\"status\":\"inProgress\"}}}'\n\
                  exit 17 ;;\n\
              esac\n\
            done\n";
        std::fs::write(&executable, script).unwrap();
        let mut permissions = std::fs::metadata(&executable).unwrap().permissions();
        permissions.set_mode(0o700);
        std::fs::set_permissions(&executable, permissions).unwrap();

        let manager = CodexTurnManager::default();
        let initial = manager
            .start_with_executable(&executable, "thread_test".to_string(), "继续".to_string())
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(2);
        let failed = loop {
            let current = manager.status(&initial.session_id).unwrap();
            if current.is_terminal() {
                break current;
            }
            assert!(Instant::now() < deadline, "provider crash was not isolated");
            thread::sleep(Duration::from_millis(10));
        };
        assert_eq!(failed.state, "failed");
        assert!(failed.pending_approval.is_none());
        manager.shutdown_all();
    }

    #[cfg(target_os = "macos")]
    #[test]
    #[ignore = "creates and archives one real Codex test thread; run only for an explicit release acceptance"]
    fn live_official_continuation_smoke() {
        let executable = codex_executable().expect("installed Codex executable");
        let workdir = tempfile::tempdir().expect("temporary test workspace");
        let thread_id = create_live_test_thread(&executable, workdir.path())
            .expect("create official test thread");

        let result = (|| -> Result<(), CodexHistoryError> {
            let manager = CodexTurnManager::default();
            let initial = manager.start_with_executable(
                &executable,
                thread_id.clone(),
                "这是 Codex Monitor Task Center 的受控验收。请只回复 TEST_OK，不要调用任何工具。"
                    .to_string(),
            )?;
            let deadline = Instant::now() + Duration::from_secs(180);
            loop {
                let current = manager.status(&initial.session_id)?;
                if current.is_terminal() {
                    manager.shutdown_all();
                    return if current.state == "completed" {
                        Ok(())
                    } else {
                        Err(CodexHistoryError::new(
                            "live_turn_failed",
                            current
                                .error_message
                                .as_deref()
                                .unwrap_or("真实 Codex 继续任务未完成"),
                        ))
                    };
                }
                if Instant::now() >= deadline {
                    let _ = manager.interrupt(&initial.session_id);
                    manager.shutdown_all();
                    return Err(CodexHistoryError::new(
                        "live_turn_timeout",
                        "真实 Codex 继续任务验收超时",
                    ));
                }
                thread::sleep(Duration::from_millis(250));
            }
        })();

        let archived = archive_live_test_thread(&executable, &thread_id);
        result.expect("official continuation completed");
        archived.expect("archive the release test thread");
    }

    #[cfg(target_os = "macos")]
    fn create_live_test_thread(
        executable: &Path,
        workdir: &Path,
    ) -> Result<String, CodexHistoryError> {
        let mut server = AppServer::start_streaming(executable)?;
        let (_control_tx, control) = mpsc::channel();
        let deadline = Instant::now() + Duration::from_secs(30);
        server.send(&initialize_request())?;
        checked_result(&wait_response(&server, 1, deadline, &control)?)?;
        server.send(&json!({"method": "initialized", "params": {}}))?;
        server.send(&json!({
            "id": 2,
            "method": "thread/start",
            "params": {
                "cwd": workdir,
                "approvalPolicy": "never",
                "sandbox": "read-only",
                "serviceName": "codex-monitor-task-center-live-test"
            }
        }))?;
        let started = wait_response(&server, 2, deadline, &control)?;
        let thread_id = checked_result(&started)?
            .get("thread")
            .and_then(|thread| thread.get("id"))
            .and_then(Value::as_str)
            .ok_or_else(|| protocol_error("真实验收未返回 Codex 任务编号"))?
            .to_string();
        server.send(&json!({
            "id": 3,
            "method": "thread/name/set",
            "params": {
                "threadId": thread_id,
                "name": "[测试] Codex Monitor Task Center 继续任务验收"
            }
        }))?;
        checked_result(&wait_response(&server, 3, deadline, &control)?)?;
        server.send(&json!({
            "id": 4,
            "method": "turn/start",
            "params": {
                "threadId": thread_id,
                "input": [{
                    "type": "text",
                    "text": "这是受控验收的初始轮次。请只回复 SEED_OK，不要调用任何工具。",
                    "text_elements": []
                }]
            }
        }))?;
        let started = wait_response(
            &server,
            4,
            Instant::now() + Duration::from_secs(30),
            &control,
        )?;
        let turn_id = checked_result(&started)?
            .get("turn")
            .and_then(|turn| turn.get("id"))
            .and_then(Value::as_str)
            .ok_or_else(|| protocol_error("真实验收初始轮次缺少编号"))?
            .to_string();
        let completion_deadline = Instant::now() + Duration::from_secs(180);
        loop {
            if Instant::now() >= completion_deadline {
                return Err(CodexHistoryError::new(
                    "live_seed_timeout",
                    "真实验收初始轮次超时",
                ));
            }
            let Some(message) = server.receive(Duration::from_millis(250))? else {
                continue;
            };
            if message.get("method").and_then(Value::as_str) != Some("turn/completed") {
                continue;
            }
            let params = message.get("params").unwrap_or(&Value::Null);
            let turn = params.get("turn").unwrap_or(&Value::Null);
            if params.get("threadId").and_then(Value::as_str) != Some(thread_id.as_str())
                || turn.get("id").and_then(Value::as_str) != Some(turn_id.as_str())
            {
                continue;
            }
            if turn.get("status").and_then(Value::as_str) != Some("completed") {
                return Err(CodexHistoryError::new(
                    "live_seed_failed",
                    "真实验收初始轮次未完成",
                ));
            }
            break;
        }
        server.stop();
        Ok(thread_id)
    }

    #[cfg(target_os = "macos")]
    fn archive_live_test_thread(
        executable: &Path,
        thread_id: &str,
    ) -> Result<(), CodexHistoryError> {
        let mut server = AppServer::start_streaming(executable)?;
        let (_control_tx, control) = mpsc::channel();
        let deadline = Instant::now() + Duration::from_secs(30);
        server.send(&initialize_request())?;
        checked_result(&wait_response(&server, 1, deadline, &control)?)?;
        server.send(&json!({"method": "initialized", "params": {}}))?;
        server.send(&json!({
            "id": 2,
            "method": "thread/archive",
            "params": {"threadId": thread_id}
        }))?;
        checked_result(&wait_response(&server, 2, deadline, &control)?)?;
        server.stop();
        Ok(())
    }
}
