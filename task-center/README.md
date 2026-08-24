# Codex Monitor 任务中心（macOS P2 安全写入预览）

这是与 HUD 严格隔离的第二个应用。生产运行时不需要 Node，不监听端口，不创建托盘或后台服务；关闭主窗口即退出。

## 开发

```bash
npm ci
npm test
npm run build
npm run tauri build -- --no-bundle
```

macOS 默认读取正式任务目录。P2 写入只允许由界面直接触发，并且必须先生成修改前/修改后预览、再次明确确认、通过文件哈希并发检查；随后原子写任务、追加正式 JSONL 事件并回读核对，失败时恢复原文件。开发和自动测试只使用合成夹具或临时目录，不写正式任务资料。

其他位置或 Windows 本地验证可在启动前设置 `CODEX_TASK_CENTER_TASK_ROOT`，它必须指向明确的 `任务/` 目录。项目/工作目录映射仍只读：

`~/.codex-monitor/task-center-projects.json`

格式见 `config/projects.example.json`。应用不在后台、定时器或页面加载时写任务，也不在应用内写项目映射配置。

浏览器开发预览只使用合成夹具，避免真实任务正文进入开发服务器。Tauri 生产构建才调用 Rust 文件适配器。

P2 当前允许写入：标题、正式任务状态、优先级、日期型截止日期、负责人、关联任务编号、归档/恢复，以及按正式最小字段新建任务。不提供评论、附件、重复任务、甘特图、AI 总结、自动派发、工作流、Jira 或云协作；这些字段不会进入缓存或第二套数据库。

架构、字段矩阵、隐私边界和验收方案位于 `../docs/task-center/`。

## 平台验证

GitHub Actions 在 `macos-latest` 与 `windows-latest` 分别运行前端测试、Rust 测试和 Tauri 原生构建。Windows job 还验证 GUI 子系统、精确可见主窗口的 20 次 `WM_CLOSE`、WebView2 回收、监听端口、Node/进程残留和强制终止隔离；详细证据见 `../docs/task-center/windows-ci-evidence-2026-08-23.md`。

以下 macOS 交叉检查只用于提前发现条件编译问题，不等于 Windows 原生 CI 或桌面验收：

```bash
rustup target add --toolchain 1.88.0 x86_64-pc-windows-msvc
cargo +1.88.0 check --locked --manifest-path src-tauri/Cargo.toml \
  --target x86_64-pc-windows-msvc --all-targets
```

macOS 资源基线探针使用系统 `proc_pid_rusage`，直接读取目标 PID 的 CPU、常驻内存、物理占用、唤醒和累计能耗差值：

```bash
xcrun clang -Wall -Wextra -Werror -O2 tools/process_resource_probe.c \
  -o /private/tmp/task-center-process-resource-probe
/private/tmp/task-center-process-resource-probe 10 61 HUD_PID [TASK_CENTER_PID WEBKIT_PID ...]
```

`10 61` 表示每 10 秒采样一次、覆盖 10 分钟。首行是预热样本，不参与速率统计。
