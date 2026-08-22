# Codex Monitor 任务中心（P1 只读 MVP）

这是与 HUD 严格隔离的第二个应用。生产运行时不需要 Node，不监听端口，不创建托盘或后台服务；关闭主窗口即退出。

## 开发

```bash
npm ci
npm test
npm run build
npm run tauri build -- --no-bundle
```

macOS 默认只读正式任务目录。其他位置或 Windows 本地验证可在启动前设置 `CODEX_TASK_CENTER_TASK_ROOT`，它必须指向明确的 `任务/` 目录。项目/工作目录映射默认只读：

`~/.codex-monitor/task-center-projects.json`

格式见 `config/projects.example.json`。P1 不在应用内写配置或正式任务。

浏览器开发预览只使用合成夹具，避免真实任务正文进入开发服务器。Tauri 生产构建才调用 Rust 只读适配器。

架构、字段矩阵、隐私边界和验收方案位于 `../docs/task-center/`。

## 平台静态检查

在 macOS 上可以检查 Windows 条件编译，但这不等于 Windows 构建或运行验证：

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
