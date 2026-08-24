# Windows P2 原生 CI 证据（2026-08-24）

## 可复查对象

- 分支：`codex/task-center-p1`
- 提交：`515aa26f69d4fec72f4f63a053df18618060db60`
- GitHub Actions run：[`32694978011`](https://github.com/Ryuaaa/codex-monitor-hud/actions/runs/32694978011)，结论 `success`
- Windows job：`97335288306`；macOS 回归 job：`97335288262`
- Windows artifact：`task-center-windows-x64-1`，artifact ID `9508672930`，保留 14 天。它是 CI 证据，不是发布包或安装包。

## 共享 P2 功能与安全测试

Windows 和 macOS 使用同一前端、Rust 写入内核、字段规则、安全流程和 `1.1.0` 版本号，没有建立 Windows 私有任务模型或数据库。

| 项目 | Windows runner 结果 | 证据边界 |
|---|---|---|
| 前端领域与组件测试 | 18/18 通过 | 覆盖 P1 回归、预览、取消零写入、确认成功、冲突保留草稿和新建流程 |
| Rust P1/P2 安全测试 | 24/24 通过 | 覆盖新建、编辑、状态、优先级、截止日期、负责人、关系、归档/恢复；双重并发冲突、事件追加与回读、精确回滚、坏 YAML、Windows 只读文件、隐私过滤等负向路径 |
| 严格静态检查 | 通过 | `cargo clippy --locked --all-targets -- -D warnings` |
| Windows Tauri 原生构建 | 通过 | release `.exe`，PE32+ x86_64，Windows GUI 子系统 |
| macOS 共享代码回归 | 通过 | 同一提交前端 18/18、Rust 24/24、严格静态检查与 Tauri release 构建均通过 |

所有写入测试都在 runner 临时目录和合成夹具中执行。Windows 只读诊断读取 1 个合成元数据文件，报告 `write_operations: 0`、`body_bytes_read: 0`，且夹具运行前后 SHA-256 不变。Windows runner 上不存在用户的正式任务目录，因此这项证据证明的是 Windows 测试夹具零写入；macOS 正式任务与事件目录零写入证据仍以 `P2-macos-validation-report.md` 为准。

## Windows 生命周期与运行边界

Runner：Windows `10.0.26100.0`、AMD64，镜像 `win25-vs2026`、版本 `20260818.207.1`。

- 20/20 次启动都获得标题为 `Codex Monitor 任务中心`、窗口类为 `Tauri Window` 的真实可见顶层 HWND；向该 HWND 发送 `WM_CLOSE` 后主进程均以退出码 0 结束。
- 每轮都观察到 WebView2 子进程，关闭后本轮新增 WebView2 PID 均归零。注册表未返回 WebView2 版本，所以仅验证了运行时可实际执行，未验证其安装版本或来源。
- 任务中心及其 WebView2 的 TCP/UDP 绑定端点为 0；结束后任务中心、新 WebView2、Node 残留均为 0。
- 匹配任务中心的 Windows 服务在前后均为空，服务清单未改变。
- 强制终止任务中心后相关 WebView2 归零，独立 PowerShell 哨兵继续运行；这只证明进程隔离，不等于 Windows HUD 真实共存测试。

## 下载后独立产物复核

- 文件：`codex-monitor-task-center.exe`
- 文件版本、产品版本：`1.1.0`
- 大小：`9,500,672` 字节
- PE machine：`0x8664`（x86_64）；subsystem：`2`（Windows GUI）
- SHA-256：`4487e0a42e8340e1007c74e8078f9de055860c3fa9cc284fe07db1e1fcdfd212`
- 下载后重新计算值、`SHA256SUMS.txt` 和 `windows-build-identity.json` 三者完全一致。
- 产物目录包含 `.exe`、校验清单、构建身份、零写入诊断、生命周期报告和文件清单；存在产物不代表人工桌面体验已通过。

## 按用户决定暂缓的 Windows 桌面验收

- 人工视觉、文字截断、最小窗口与键盘/读屏体验。
- Windows 200% 系统缩放。
- CPU、内存、唤醒和能耗三轮资源基线。
- Windows HUD 真实共存与强制终止后的 HUD 状态。
- 定向 WebView2 崩溃注入及恢复表现。

这些项目未验证，不以 CI、macOS 或独立哨兵替代；根据用户决定，它们不阻塞本轮 Windows P2 代码同步与 CI 完成。
