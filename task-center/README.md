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
