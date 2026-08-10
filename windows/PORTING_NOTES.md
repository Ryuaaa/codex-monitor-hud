# Windows 复用边界与功能对照

审计基线：macOS 1.0 候选提交 `864ddea`。本目录只新增 Windows 文件，没有改动 `overlay/`、macOS 构建/发布脚本或候选产物。

## 技术边界

现有实现把界面、平台采集和一部分产品逻辑都写在 Objective-C/Foundation/AppKit 中，因此没有可直接在 Windows 编译的源码模块。可复用的是已经验证过的**数据定义、协议方法、算法口径、刷新策略和测试样例**；Windows 需要为这些行为建立 C++ 模块，并用相同输入夹具做对照测试，不能简单复制 Objective-C 文件后声称完成移植。

| 现有部分 | 代表文件 | Windows 处理方式 |
| --- | --- | --- |
| 窗口、卡片、设置、模块排序 | `overlay/CodexMonitorHUD.m`、`overlay/HUDView.m` | AppKit 不可复用；按同一信息架构用 Win32 重建。置顶、最小化、拖动、缩放已有骨架。 |
| CPU、内存、进程树、磁盘、网络、热压力 | `overlay/NativeSampler.m` | macOS Mach、libproc、sysctl 接口不可复用；用 Windows 系统接口重写，并重新校准口径。 |
| Codex 账户、额度、用量、任务列表 | `overlay/CodexStatusProvider.m` | JSON-RPC 方法和容错语义可复用；进程启动、管道、可执行文件发现、目录和 JSON 实现需重写。 |
| Token 与费用历史 | `overlay/CodexCostHistory.m` | 计价、高水位去重、预测规则和隐私边界可复用；文件遍历、缓存位置和解析实现需移植。 |
| OpenAI 状态 | `overlay/OpenAIServiceStatus.m` | 状态映射规则可复用；网络传输改用 Windows 自带 WinHTTP，并保留隐藏后停采。 |
| 自动更新 | `overlay/UpdateManager.m` | 版本比较、SHA-256 先验和失败保留旧版原则可复用；下载、签名验证、替换运行中程序必须重写。 |
| macOS 签名、公证、安装 | `release-macos.sh`、`install-overlay.sh` | 完全不可复用；Windows 使用 Authenticode、时间戳和独立安装/更新流程，不存在 Apple 公证。 |

## 功能等价性

| macOS 功能 | Windows 最佳当前判断 | 关键限制或验证条件 |
| --- | --- | --- |
| 三页面、模块开关/排序、颜色透明度、角落位置、窄栏、锁定位置与大小 | 可完整等价实现 | UI 代码需重写；需真实 DPI、多显示器、键盘和屏幕阅读器测试。 |
| 始终置顶、最小化、拖动缩放、单实例 | 可完整等价实现 | 第一个骨架已实现源码；尚待 Windows 真实运行验证。 |
| 整机 CPU、内存、交换/提交量、进程排行 | 可实现同类能力 | Windows 指标口径与活动监视器不同，不能直接沿用 macOS 阈值和文案。 |
| Codex 进程树 CPU、内存和磁盘读写 | 可实现同类能力 | 需识别 Windows 桌面版/命令行版实际进程关系；共享内存仍只能作趋势指标。 |
| 网络速度、历史曲线、分钟聚合 | 可实现 | 用 Windows 网络表和本地文件持久化；需验证睡眠、重启和网卡切换。 |
| macOS“内存压力” | 只能做 Windows 等价判断 | Windows 可给内存负载、可用量、提交限制和分页趋势，但没有同一套 macOS pressure 指标，界面必须明确换口径。 |
| macOS“系统热压力” | 通用 Windows 版无法保证等价 | Windows 没有对所有硬件都可靠一致的用户态热压力接口；厂商 WMI/传感器会增加兼容成本。默认应显示“系统未提供”，不能伪造温度或等级。 |
| Codex 额度、订阅、官方 Token 汇总、任务列表 | 协议上可移植，当前待核验 | 前提是 Windows 本机存在可执行的 Codex `app-server --stdio`，且 `account/rateLimits/read`、`account/read`、`account/usage/read`、`thread/list` 返回兼容；必须在登录状态下只读验证。 |
| 本机会话活动、Token/费用估算 | 大部分可移植 | 需核验 Windows 会话目录、文件锁和换行行为；仍不得保存提示词、回复或工具内容。 |
| 登录后自动启动 | 可完整实现 | 推荐当前用户范围的启动任务或注册项，默认关闭且不要求管理员权限。 |
| GitHub 检查更新、一键更新、失败回滚 | 可完整实现 | 运行中 EXE 不能直接覆盖自身，需要一次性更新辅助进程；下载后同时验证 SHA-256 和 Authenticode。 |
| Developer ID 签名与 Apple 公证 | 不适用 | Windows 对应的是 Authenticode 代码签名和可信时间戳。签名可验证发布者与文件完整性，但 SmartScreen 信誉需要真实下载/运行积累，不能由签名测试代替。 |
| x64 与 ARM64 | 技术路线支持 | 必须分别在 Windows 工具链构建，并在真实或可信虚拟机上运行验证；当前没有此证据。 |

## 下一阶段建议顺序

1. 在真实 Windows x64 上完成构建、六项交互验收和空闲资源基线；再补 ARM64 编译验证。
2. 先实现独立的 Windows 系统快照结构与 CPU/内存/进程树采样，使用固定夹具测试算法，不接 UI。
3. 单独验证 Windows Codex 本机接口和会话路径；确认兼容后再接到账户/任务占位模块。
4. 最后做持久化、登录启动、签名、便携 ZIP/安装包和失败可回滚更新；签名与 SmartScreen 状态分别报告。
