# Codex 运行状态能力矩阵（P0）

OpenAI 官方 App Server 文档说明：`thread/read` 可在不恢复任务的前提下读取；线程状态为 `notLoaded`、`idle`、`systemError` 或带活动标记的 `active`；`thread/status/changed` 只针对已加载线程；`thread/loaded/list` 返回当前服务器内存中的线程。因此状态只证明所连接实例，不能证明其他 Codex 桌面实例。

| 固定样例 | Provider 证据 | UI 语义 | 禁止表达 |
|---|---|---|---|
| 所连接实例中的运行任务 | 新鲜 `active`，可带 `waitingOnApproval` 等标记 | `已证明正在运行`；细分等待批准/等待用户 | “所有 Codex 中唯一运行任务” |
| 已知但 `notLoaded` | 新鲜 `thread/list/read` 返回 `notLoaded` | `已知任务，当前连接实例未加载` | “未运行” |
| 其他 Codex 桌面实例任务 | 正式任务有绑定，但当前 Provider 不拥有该实例 | `外部 Codex 任务，运行状态未知` | “空闲”“未运行” |
| 接口失败或缓存过期 | 连接错误，或最后成功时间超过 TTL | `接口不可用` 或 `数据已过期` | 用旧状态冒充实时状态 |

P1 提供 `CodexRuntimeProvider` 抽象、四类固定夹具和纯映射测试。默认发行版不连接或注入桌面版内部服务器；绑定任务仍显示“外部任务，状态未知”。用户点击“读取历史”时，任务中心可以临时启动本机官方 `codex app-server --stdio`，用 `thread/read` 读取任务元数据，并在实验能力可用时用 `thread/turns/list`、`itemsView: "notLoaded"` 分页读取轮次元数据。该短期进程自身返回的状态不代表桌面版实时状态，请求结束后必须退出。分页不可用时不得回退为完整历史。全程不启用 CDP、注入或会话文件写入，也不启动第二个交互式 Codex。

默认“Codex 活动”使用稳定的 `thread/list` 主能力，每页 25 条、`recency_at` 倒序、非归档、`useStateDbOnly: true`；省略 `sourceKinds`，沿用官方默认的交互式 `cli` 与 `vscode` 来源。列表只用于历史与活动入口，返回状态仍不得解释为桌面版全部实例的实时状态。轮次分页继续单独视为实验能力并做降级。

“打开对应任务”仅在官方稳定深链或应用内受控导航已被单独验证后启用。P1 先展示任务编号和禁用按钮，并解释当前无已验证的第三方安全打开方式。
