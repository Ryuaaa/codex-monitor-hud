# 隐私说明

Codex Monitor HUD 默认只在本机处理数据。

- 电脑性能数据来自 macOS 系统接口。
- Codex数据来自本机安装的 Codex App Server，请求 `account/rateLimits/read`、`account/read` 和 `account/usage/read`，分别读取额度窗口、订阅类型和按日Token用量。
- 当前版本不请求任务列表，不读取任务标题、预览、对话、提示词、文件名或命令内容。
- 不读取浏览器 Cookie、API 密钥、登录令牌或完整磁盘内容。
- 不上传监控数据。
- 可选历史记录只保存性能汇总，不保存对话内容。
- 额度、订阅和Token用量只用于当前界面显示，不写入性能历史文件。

Codex数据查询会按设定周期短暂启动本机Codex接口进程，读取完成后退出。本工具不直接向第三方服务器发起请求，但Codex本机接口可能按OpenAI客户端自身机制联网。
