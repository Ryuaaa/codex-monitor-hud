# Windows 原生 CI 证据（2026-08-23）

## 可复查对象

- 分支：`codex/task-center-p1`
- 提交：`ed4c6b7f22fd787e9b9a1ec632a325d25a6ff3d0`
- GitHub Actions run：[`32609138427`](https://github.com/Ryuaaa/codex-monitor-hud/actions/runs/32609138427)
- Windows job：`97119107838`，结论 `success`
- macOS job：`97119107922`，结论 `success`
- Windows artifact：`task-center-windows-x64-1`，artifact ID `9485095146`（CI 临时证据，保留 14 天；长期结论以本文件和 Actions 日志为准）

## Windows runner 已验证

Runner 为 GitHub 托管 Windows `10.0.26100.0`、AMD64，镜像 `win25-vs2026`、版本 `20260818.207.1`。

- Node `22.23.0` 安装与锁文件依赖安装成功；生产依赖审计报告 0 个已知漏洞。
- 前端领域/组件测试 10/10 通过，Rust 只读边界测试 8/8 通过。
- Tauri release `.exe` 原生构建成功；文件版本和产品版本均为 `1.1.0`。
- PE 子系统值为 `2`（`Windows GUI`），不是会附带终端窗的控制台程序。
- 产物大小 `9,148,416` 字节，SHA-256：`908c9bbd965471616afe7e619919189a775f7138a5b7fa621cc17b901df85c51`。
- 下载后在 macOS 独立执行 `shasum -a 256 -c SHA256SUMS.txt`，结果为 `OK`；`file` 识别为 `PE32+ executable (GUI) x86-64, for MS Windows`。
- 20/20 次启动均枚举到唯一的可见顶层窗口，标题为 `Codex Monitor 任务中心`，窗口类为 `Tauri Window`；向该精确 HWND 发送 `WM_CLOSE` 后，主进程均以退出码 0 在 10 秒内退出。
- 每轮均观察到新 WebView2 进程，证明 runner 上已安装运行时且应用确实使用它启动；每轮关闭后本轮 WebView2 PID 均归零。
- 任务中心及本轮 WebView2 的 TCP 监听和 UDP 绑定端点均为 0；结束后任务中心 PID、新 WebView2 PID 均为 0，前后 Node PID 均为空。
- 匹配 `codex-monitor-task-center` 路径的 Windows 服务在测试前后均为空，服务清单未改变。
- 强制终止任务中心后，其 WebView2 PID 归零，独立 PowerShell 哨兵进程继续存活。
- 同一提交的 macOS runner 同时通过前端、Rust 和 Tauri 应用二进制构建，未发现本轮 Windows 修复导致的共享代码回归。

## 证据边界

- WebView2 的注册表版本键在该 runner 未取到；“WebView2 可执行”已由每轮真实进程证明，但安装版本和来源未验证。
- GitHub runner 能创建真实 HWND 并处理 `WM_CLOSE`，但没有人工观察桌面画面；Windows 200% 系统缩放、最小窗口视觉布局、键盘/读屏体验仍需真实桌面 Windows 验收。
- 未在 Windows 上测 CPU、内存、唤醒次数和能耗三轮基线。
- 强制终止只证明与独立哨兵进程隔离，不等于 Windows HUD 真实运行隔离；Windows HUD 共存、WebView2 定向崩溃后的 HUD 状态仍未验证。
- 产物是未签名、未打包的 CI `.exe` 证据，不是发布包、安装包或桌面体验通过证明；未创建 Release，也未进入发布流程。
