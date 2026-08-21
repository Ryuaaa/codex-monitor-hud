# Codex 运行状态能力矩阵（P0）

OpenAI 官方 App Server 文档说明：`thread/read` 可在不恢复任务的前提下读取；线程状态为 `notLoaded`、`idle`、`systemError` 或带活动标记的 `active`；`thread/status/changed` 只针对已加载线程；`thread/loaded/list` 返回当前服务器内存中的线程。因此状态只证明所连接实例，不能证明其他 Codex 桌面实例。

| 固定样例 | Provider 证据 | UI 语义 | 禁止表达 |
|---|---|---|---|
| 所连接实例中的运行任务 | 新鲜 `active`，可带 `waitingOnApproval` 等标记 | `已证明正在运行`；细分等待批准/等待用户 | “所有 Codex 中唯一运行任务” |
| 已知但 `notLoaded` | 新鲜 `thread/list/read` 返回 `notLoaded` | `已知任务，当前连接实例未加载` | “未运行” |
| 其他 Codex 桌面实例任务 | 正式任务有绑定，但当前 Provider 不拥有该实例 | `外部 Codex 任务，运行状态未知` | “空闲”“未运行” |
| 接口失败或缓存过期 | 连接错误，或最后成功时间超过 TTL | `接口不可用` 或 `数据已过期` | 用旧状态冒充实时状态 |

P1 提供 `CodexRuntimeProvider` 抽象、四类固定夹具和纯映射测试。默认发行版不尝试连接桌面版内部服务器；在没有已审核官方连接配置时，绑定任务显示“外部任务，状态未知”。不启用 CDP、注入、会话文件写入或第二个 Codex。

“打开对应任务”仅在官方稳定深链或应用内受控导航已被单独验证后启用。P1 先展示任务编号和禁用按钮，并解释当前无已验证的第三方安全打开方式。

