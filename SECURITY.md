# 安全说明

## 报告问题

如果问题可能暴露账号数据、Token、Cookie、文件内容或本机隐私，请不要公开发布完整日志。请使用GitHub仓库的私密安全报告功能，并提供最小复现步骤和脱敏后的诊断结果。

## 安全边界

- 工具不需要管理员权限、完整磁盘权限或浏览器Cookie。
- Codex账户数据只通过本机Codex App Server读取。
- 性能历史不保存额度、订阅、Token用量或对话内容。
- 安装脚本只写入用户的 `~/Applications`、`~/Library/LaunchAgents` 和 `~/Library/Application Support/CodexSystemMonitor`。
