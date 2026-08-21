# ADR-0001：任务中心运行边界

- 状态：P0 已接受为 P1 实现约束
- 日期：2026-08-22
- 基线：`ebadeb701fd246bb78fcc51d042d3a94621ffdd8`

## 决定

Codex Monitor 是同一产品家族、单一仓库、两个独立应用进程：

1. `overlay/` 与 `windows/` 继续构建原生 HUD。HUD 常驻，但不链接任务中心 UI、WebView、刷新器或任务解析器。
2. `task-center/` 构建独立的 Tauri 2 应用。前端为 React，生产包使用系统 WKWebView/WebView2；Node 只参与构建。
3. P1 不修改 HUD，也不创建 HUD 到任务中心的启动入口。
4. 任务中心不启动本地 HTTP 服务、第二个 Codex、CDP 端口、注入器、托盘、LaunchAgent、计划任务或后台服务。最后一个窗口关闭时应用退出。
5. 共享代码只允许位于 `task-center/src/domain/` 的纯数据契约、状态映射、权限规则和固定样例。未来若 HUD 需要摘要，应复制稳定的小型契约为独立原生实现，不得加载任务中心运行时。

## 包边界

| 目录 | 责任 | 允许运行时依赖 |
|---|---|---|
| `task-center/src/domain/` | 纯类型、解析、映射、派生提示、Codex 状态语义 | 无系统访问 |
| `task-center/src/data/` | Tauri 命令适配、合成夹具 | 只读命令 |
| `task-center/src/ui/` | React 展示 | domain/data |
| `task-center/src-tauri/` | 最小权限文件读取与应用生命周期 | Tauri、Rust 标准库、serde |
| HUD 现有目录 | 轻量监控 | 不依赖 task-center |

## 未来允许的 HUD 交互

- 只允许用户动作触发操作系统启动已安装的任务中心应用。
- 可选启动参数只能是非敏感定位键，例如任务编号或项目编号；任务中心仍须独立读取权威资料并校验权限。
- 不允许共享内存、嵌入 WebView、常驻 IPC 服务、共享可写数据库或由 HUD 代持任务中心缓存。
- 任务中心故障、升级或不存在时，HUD 只显示非阻断提示并继续运行。

## 技术版本与依据

P1 锁定：Tauri CLI `2.11.4`、Tauri Rust crate `2.11.5`、Tauri JS API `2.11.1`、React/ReactDOM `19.2.8`、Vite `8.2.2`、TypeScript `7.0.2`、Vitest `4.1.11`。版本于 2026-08-22 从 npm/crates.io 查询并由锁文件冻结。

官方依据：[Tauri 2 prerequisites](https://v2.tauri.app/start/prerequisites/) 确认 macOS/Windows 系统依赖与 WebView2，[Tauri Vite guide](https://v2.tauri.app/start/frontend/vite/) 给出静态 `frontendDist` 构建方式；[OpenAI Codex App Server](https://developers.openai.com/codex/app-server) 确认 `thread/read` 不会恢复任务，`thread/list` 返回的状态属于所连接实例，`notLoaded` 不能解释为“未运行于所有 Codex 实例”。

## 被拒绝的替代方案

- 把 Dashi 的 Node/SQLite/本地服务/注入器搬入 HUD：破坏常驻负担和单一事实源。
- 在任务中心建立任务业务数据库：形成第二套状态。
- 连接或注入桌面版内部 App Server：官方文档未证明稳定公共接入能力。
- P1 直接写 Markdown 或事件 JSONL：尚未通过字段与冲突策略验收。
