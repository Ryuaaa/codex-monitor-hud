# Windows 原生版：第一个可运行骨架

当前里程碑只交付 Windows 原生悬浮窗外壳，不采集、读取或伪造任何监控数据。它用于在真实 Windows 机器上独立验证以下基础交互：

- 默认置顶，并可通过 `Unpin / Pin on top` 来回切换；
- 标准标题栏和窗口内按钮都可最小化；
- 使用 Windows 标准标题栏拖动，使用边框和四角连续缩放；
- 窄窗单列、宽窗双列的四个功能模块占位；
- 跟随显示器缩放比例（DPI）重新排版，并限制最小可用尺寸；
- 单实例运行。

## 为什么选这条路线

采用 **Win32 + C++17 + 系统原生控件/GDI**，构建层只使用 CMake：

- 运行时不嵌入网页引擎，不要求 Electron、WebView2、.NET 或 Windows App SDK；
- 当前骨架没有后台定时器，空闲时只保留标准窗口消息循环；
- x64 和 ARM64 都可由 Visual Studio 的对应工具链编译；
- 先用成熟的系统标题栏验证拖动、缩放、最小化与键盘可达性，后续再决定是否值得自绘外观。

WinUI 3、WPF 和 Electron 都能更快做复杂界面，但它们会引入额外框架、运行时或进程负担，不符合这个项目“低常驻负担、面向普通用户开源”的首要约束。

## 在 Windows 上构建

需要：Windows 10/11、Visual Studio 2022 Community 或 Build Tools、Desktop development with C++、Windows 10/11 SDK、CMake。

在仓库根目录运行：

```powershell
cmake -S windows -B windows/out/build -A x64
cmake --build windows/out/build --config Release
./windows/out/build/Release/CodexMonitorHUD.exe
```

ARM64 构建把 `-A x64` 改为 `-A ARM64`。也可在 Visual Studio 中直接打开 `windows/CMakeLists.txt`。

## 不依赖 Windows 的静态检查

```bash
python3 windows/tests/static_checks.py
```

这项检查只证明关键窗口样式、消息处理、占位模块和低负担约束仍存在，不能替代 Windows 编译或真实交互测试。

## Windows 机器上的最小验收

1. 启动后窗口位于工作区左下方附近，默认盖在普通窗口上方。
2. 点击 `Unpin` 后普通窗口可以盖住它，再点击 `Pin on top` 恢复置顶。
3. 点击 `Minimize` 后进入任务栏，从任务栏可恢复；标题栏的最小化按钮行为相同。
4. 拖动标题栏可移动；拖动任意边或角可缩放，且不能缩到模块无法辨认。
5. 把窗口拉宽后四张占位卡从单列变为双列；拖到不同 DPI 的显示器后文字和间距保持可读。
6. 再次启动第二个实例时只提示已有实例，不出现第二个悬浮窗。

## 当前明确未完成

- 未实现 Windows 系统采集、Codex 本机接口、历史记录、设置持久化、登录启动、更新、安装包或签名。
- 尚未在 Windows x64、Windows ARM64 或真实多显示器环境编译和运行验证。
- 目前使用标准 Windows 标题栏；视觉细节还不是 macOS 版的等价实现。

完整的跨平台复用审计和功能差异见 [PORTING_NOTES.md](PORTING_NOTES.md)。
