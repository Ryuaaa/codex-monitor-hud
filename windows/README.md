# Windows 原生版：性能采集里程碑

当前里程碑在 Win32 + C++17 原生悬浮窗中接入了第一组真实 Windows 性能数据。它不读取对话、提示词、命令行或账号凭据，也不保存采样历史。

## 已实现

- 默认置顶，可切换置顶状态；支持标准标题栏拖动、连续缩放、最小化、DPI 重排和单实例。
- 每 5 秒采集整机 CPU、物理内存、进程列表及 Codex/ChatGPT 进程树；每 20 秒采集系统提交量、页面文件和全部进程的内存排行。
- 识别 `Codex.exe`、`ChatGPT.exe` 等已知根进程，并按父进程编号纳入其全部后代。
- 汇总 Codex/ChatGPT 进程树的整机 CPU 份额和工作集内存。
- 显示工作集内存最高的 5 个进程。
- 首次启动和从最小化恢复时立即执行一次完整采样；两次 20 秒慢采之间，或某次慢采失败时，保留上次有效的提交量、页面文件和内存排行，不闪成不可用。
- 窗口最小化时完全停止采样定时器；恢复时重建 CPU 基线，避免把整个最小化时段误算成一个 5 秒样本。
- 受保护进程、权限不足或采样间进程退出时保留其名称与进程关系；读不到的 CPU/工作集标为不可用或部分可用，不让整帧失败。
- Windows 没有可供所有硬件统一使用的系统热压力接口，因此明确显示 `system not provided`，不伪造温度或热压力等级。
- 修复首个骨架中 Windows 头文件可能用 `max` 宏干扰 `std::max` 的编译风险，构建时统一定义 `NOMINMAX`。

## 数据口径

| 指标 | Windows 接口 | 口径 |
| --- | --- | --- |
| 整机 CPU | `GetSystemTimes` | 对相邻两次累计时间做差；内核时间包含空闲时间，因此先扣除空闲时间，再限制到整机 0–100%。第一帧没有差值，明确不可用。 |
| Codex/ChatGPT CPU | `GetProcessTimes` | 对同一进程编号且创建时间一致的相邻累计值做差，再除以同一帧的整机 CPU 总时间差。100% 代表整台机器的逻辑处理能力接近占满。 |
| 物理内存 | `GlobalMemoryStatusEx` | 已用量为总物理内存减去当前可用物理内存。 |
| 系统提交量 | `GetPerformanceInfo` | 显示当前提交量、当前提交上限和开机后的提交峰值；不是单纯的页面文件大小。 |
| 页面文件 | `EnumPageFilesW` | 汇总所有已安装页面文件的总量、当前使用量和峰值。 |
| 进程列表 | `CreateToolhelp32Snapshot` | 一次快照取得进程编号、父进程编号和可执行文件名；不读取命令行。 |
| 进程内存 | `GetProcessMemoryInfo` | 使用工作集（当前映射到该进程的物理内存）做趋势和排名；共享页可能在多个进程中重复出现，不等于独占内存。 |

在超过 64 个逻辑处理器且划分为多个处理器组的机器上，`GetSystemTimes` 只覆盖调用线程所属的主要处理器组；当前通用桌面版尚未为这类工作站实现跨处理器组校准，界面读数不能当作整机精确值。

## 为什么继续使用 Win32

采用 **Win32 + C++17 + 系统原生控件/GDI**，构建层只使用 CMake：

- 运行时不嵌入网页引擎，不要求 Electron、WebView2、.NET 或 Windows App SDK；
- 系统指标全部来自 Windows 文档化接口，仅链接 `User32`、`Gdi32` 和 `Psapi`；
- x64 和 ARM64 都可由 Visual Studio 的对应工具链编译；
- 5 秒快采只打开 Codex/ChatGPT 进程树；打开全部进程并读取工作集、提交量和页面文件的慢采限制为每 20 秒一次，避免为轻量悬浮窗增加不必要负担。

## 在 Windows 上构建

需要 Windows 10/11、Visual Studio 2022 Community 或 Build Tools、Desktop development with C++、Windows 10/11 SDK、CMake。

在仓库根目录运行：

```powershell
cmake -S windows -B windows/out/build -A x64
cmake --build windows/out/build --config Release
ctest --test-dir windows/out/build -C Release --output-on-failure
./windows/out/build/Release/CodexMonitorHUD.exe
```

ARM64 构建把 `-A x64` 改为 `-A ARM64`。也可在 Visual Studio 中直接打开 `windows/CMakeLists.txt`。

## 不依赖 Windows 的验证

源码契约检查：

```bash
python3 windows/tests/static_checks.py
```

CPU 差值、整机归一化、进程树、内存前五名和溢出保护位于无 Windows 头文件依赖的固定 C++ 测试中。装有 CMake 和 C++17 编译器的平台可以运行：

```bash
cmake -S windows -B /tmp/codex-monitor-windows-tests -DBUILD_TESTING=ON
cmake --build /tmp/codex-monitor-windows-tests
ctest --test-dir /tmp/codex-monitor-windows-tests --output-on-failure
```

这些测试证明算法和源码契约，不替代 Windows 编译或真实交互验收。

## Windows 机器上的最小验收

1. MSVC x64 Release 编译成功，静态链接运行库的 EXE 能在未安装开发环境的同版本 Windows 上启动。
2. 启动后的第一帧 CPU 显示等待下一次采样，约 5 秒后出现 0–100% 数值。
3. 启动 ChatGPT/Codex 后，根进程与后代数量、CPU 和工作集出现；关闭进程时界面降级或归零，应用不崩溃。
4. 与任务管理器抽样对照整机 CPU、物理内存、提交量、页面文件和内存前五名；确认后三项约 20 秒更新一次，间隔中不闪成不可用。
5. 点击最小化后确认进入任务栏且完全停止采集；恢复时立即更新完整指标，CPU 先等待一个新基线，再显示差值。
6. 点击 `Unpin` 后普通窗口可以盖住它，再点击 `Pin on top` 恢复置顶。
7. 拖动标题栏可移动；拖动任意边或角可缩放；窄窗单列、宽窗双列，在不同 DPI 显示器上仍可读。
8. 再次启动第二个实例时只提示已有实例，不出现第二个悬浮窗。

## 当前明确未完成

- 尚未在真实 Windows x64、Windows ARM64 或多显示器环境完成编译和运行验收；仓库已有 Windows x64 CI，但本地未推送的提交不会触发远端结果。
- 尚未实现 Codex 账户额度、任务、Token/费用、网络/磁盘、历史曲线、设置持久化、登录启动、更新、安装包或签名。
- 热压力只有明确的“不提供”状态；不会通过不可靠的通用传感器值模拟 macOS 热压力。
- 根进程目前按已知可执行文件名识别；Windows 正式版若出现新的官方二进制名称，需要用真实安装样本补充固定名单。
- 当前使用标准 Windows 标题栏；视觉细节还不是 macOS 版的等价实现。

完整的跨平台复用审计和功能差异见 [PORTING_NOTES.md](PORTING_NOTES.md)。
