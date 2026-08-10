# Windows 原生版：Codex 只读数据接入里程碑

当前 Windows 版使用 **Win32 + C++17** 构建常驻悬浮窗，已经具备主页、Codex、电脑性能三页、十一个可配置模块、真实性能采样，以及通过本机官方 `codex app-server` 读取 Codex 额度、订阅、官方 Token 日汇总和最近任务历史的第一版能力。

它不读取或保存提示词、回复、工具内容、账号邮箱、任务标识、工作目录、会话路径或原始接口行，也不保存性能与 Codex 采样历史。

## 已实现

### 窗口与模块

- 默认置顶，可随时切换置顶；支持标准标题栏拖动、任意边角连续缩放、真正最小化到任务栏、单实例和应用图标。
- 支持 `Home`、`Codex`、`Computer` 三页，重启后恢复当前页、窗口位置/大小和置顶状态。
- 十一个模块包括“电脑压力、主要瓶颈与 Codex/ChatGPT 影响”诊断卡、四个原始性能明细卡，以及 Codex 五小时额度、每周额度、订阅类型、账号 Token 用量、最近任务历史和 OpenAI 官方服务状态六个模块。
- 每个模块分别提供“显示在主页”和“显示在所属页面”两个开关；五小时额度与每周额度互不绑定。
- 电脑性能模块默认在主页和电脑页显示；Codex 模块默认不加入主页。新安装时，Codex 页默认显示五小时额度、每周额度、订阅、最近任务历史和 OpenAI 官方服务状态，Token 用量默认关闭。
- 设置使用版本化白名单格式保存到 `%LOCALAPPDATA%\CodexMonitorHUD\settings.ini`；当前格式为版本 4。旧版设置迁移时保留用户已有选择，不会自动把新增模块加入主页或改写已有页面选择。
- 卡片使用固定可读高度；内容超出窗口时出现纵向滚动条并支持鼠标滚轮，不再因缩小窗口而压扁卡片。布局和设置字号随每台显示器的 DPI 调整。
- 启动、显示器布局改变和 DPI 改变时，窗口会约束到可见工作区；损坏或越界的保存值会安全回退。

### 电脑性能

- 单一串行后台线程采样，窗口消息线程不执行系统指标读取。
- 每 5 秒读取整机 CPU、物理内存、进程列表及 Codex/ChatGPT 进程树；每 20 秒读取系统提交量、页面文件和全部进程的工作集排行。
- 汇总 Codex/ChatGPT 进程树的整机 CPU 份额和工作集内存，并显示工作集最高的五个进程。
- 使用同一批现有数据给出当前压力、CPU/内存瓶颈、Codex/ChatGPT 合计影响和置信度；它不增加采样或网络请求。数据不足时明确显示未知，不把单帧趋势写成持续故障。
- 两次慢采之间或慢采失败时保留上次有效的提交量、页面文件和内存排行，不闪成不可用。
- 后台采样不重入；工作线程忙时只合并必要请求，完整慢采优先。
- 窗口最小化、当前页不需要性能数据，或主页没有性能模块时，停止定时器、取消未执行请求并使在途旧结果失效；恢复需求后立即重建 CPU 基线并进行完整首采。
- 受保护进程、权限不足或采样间进程退出只让对应指标降级，不让整帧失败。
- Windows 没有适用于所有硬件的统一热压力接口，因此明确显示 `system not provided`，不伪造温度或热压力等级。

### Codex 官方只读数据

- 直接启动本机绝对路径的 `codex.exe app-server --stdio`，完成官方 `initialize` / `initialized` 握手后读取：
  - `account/rateLimits/read`：五小时与每周额度剩余比例、恢复时间；
  - `account/read`：订阅类型；
  - `account/usage/read`：官方每日 Token 汇总；
  - `thread/list`：最近任务历史。
- 五小时与每周额度独立显示；接口未返回某个窗口时明确显示“当前未返回”，不会用另一个窗口或 `0` 代替。
- Token 卡显示今日数据；若今日尚未结算，则显示官方最近一次有数据的日期，并同时显示近 7 日、近 30 日、本月累计和按当前月内日均线性外推的“月末约”趋势，不把缺失数据写成今日 `0`，数据不足时也不生成预测。
- `thread/list` 每次最多请求五条；界面最多显示三条经过清理和截断的名称及最近时间。任务状态只可能反映本次短生命周期 `app-server` 进程的范围，界面明确提示它不代表 Codex 桌面版的全局实时任务状态。
- 单个方法失败不会清空其他方法；已有成功值时继续显示上次数据并标明本次更新失败。
- 成功后每 5 分钟刷新；连续失败按 1、2、5、10、15 分钟退避并封顶，下一次成功后恢复 5 分钟正常刷新，避免故障期间持续创建进程。
- Codex 数据与性能数据分别按需运行。窗口最小化、当前页不需要 Codex 数据，或相关模块全部隐藏时，会取消在途读取并停止后续刷新；重新显示后立即刷新。

### OpenAI 官方服务状态

- 只请求 `https://status.openai.com/api/v2/summary.json`，优先显示 `Codex in ChatGPT Desktop` 组件状态，同时保留 OpenAI 整体状态，帮助区分官方服务异常与本机问题。
- 请求不携带账号、令牌或 Cookie，关闭自动认证与重定向，响应上限为 1 MiB；解析失败或出现未来未知状态时不会显示为“正常”。
- 成功后 15 分钟再检查；连续失败按 1、2、5、10、15 分钟退避并封顶。失败时保留上次成功状态和时间，同时明确标注“更新失败”。
- 该模块默认显示在 Codex 页、主页默认关闭；只有它在当前页面实际可见且窗口没有隐藏或最小化时才启动请求。隐藏后不再安排新请求，在途同步请求会在约 8 秒的有界超时内结束且结果丢弃；15 分钟内切回会复用新鲜状态，不会因反复切页重复联网。

### `codex.exe` 查找与进程安全

- 优先接受绝对路径的 `CODEX_CLI_PATH`，其次检查绝对 `PATH` 目录，再检查已知的官方桌面版、打包应用、本机 standalone release 和 npm 用户级安装布局。
- 自动发现最多检查 64 个候选，只查看固定目录或一层版本目录；不递归扫描磁盘、不执行 `where`、脚本或命令行外壳，也不尝试受保护的 `WindowsApps` 目录。
- 候选必须是绝对路径、普通文件，且路径不经过重解析点；版本目录中选择最新的安全候选。
- 子进程通过 `CreateProcessW` 直接启动，只继承明确列出的管道句柄，并在恢复执行前加入“关闭即清理”的 Windows Job，暂停、超时或退出时不会遗留 `app-server` 子进程。
- 单行接口数据限制为 1 MiB、输出队列和总输出有固定上限、保留的标准错误最多 16 KiB、单次读取总时限 15 秒；原始接口内容不会传到界面或落盘。

## 数据口径

| 指标 | Windows 接口或来源 | 口径 |
| --- | --- | --- |
| 整机 CPU | `GetSystemTimes` | 对相邻累计时间做差，扣除空闲时间并限制为整机 0–100%；第一帧没有差值，明确不可用。 |
| Codex/ChatGPT CPU | `GetProcessTimes` | 对进程编号和创建时间一致的相邻累计值做差，再除以同一帧整机 CPU 总时间差；100% 表示整台机器的逻辑处理能力接近占满。 |
| 物理内存 | `GlobalMemoryStatusEx` | 已用量为总物理内存减去当前可用物理内存。 |
| 系统提交量 | `GetPerformanceInfo` | 显示当前提交量、提交上限和开机后峰值，不等于页面文件大小。 |
| 页面文件 | `EnumPageFilesW` | 汇总所有页面文件的总量、当前使用量和峰值。 |
| 进程列表 | `CreateToolhelp32Snapshot` | 只读取进程编号、父进程编号和可执行文件名，不读取命令行。 |
| 进程内存 | `GetProcessMemoryInfo` | 使用工作集做趋势和排名；共享页可能重复计入，不等于独占内存。 |
| 一眼诊断 | 上述 CPU、物理内存、提交量与 Codex/ChatGPT 进程树指标 | 保守的当前快照启发式判断；工作集归因置信度最高为中，不等于证明某一个应用是长期瓶颈。 |
| Codex 额度/订阅/Token/任务历史 | 本机官方 `codex app-server --stdio` | 只保留界面需要的最小字段；接口缺失与数值 `0` 严格区分。 |
| OpenAI 服务状态 | `status.openai.com/api/v2/summary.json` | 公共只读状态；Codex 组件优先，组件缺失时明确回退到 OpenAI 整体状态。 |

在超过 64 个逻辑处理器且划分为多个处理器组的机器上，`GetSystemTimes` 只覆盖调用线程所属的主要处理器组；当前桌面版尚未做跨处理器组校准，不能把这类工作站的读数当作整机精确值。

## 为什么继续使用 Win32

- 不嵌入网页引擎，不要求 Electron、WebView2、.NET 或 Windows App SDK；
- 性能指标、窗口和用户设置均使用 Windows 文档化接口；
- 高频工作保持在单一可停止的后台线程中，只有界面实际需要时才运行；
- 官方 Codex 读取也在独立串行线程中短时执行，不阻塞窗口消息。

## 在 Windows x64 上构建

需要 Windows 10/11、Visual Studio 2022 Community 或 Build Tools、`Desktop development with C++`、Windows 10/11 SDK 和 CMake。

在仓库根目录运行：

```powershell
cmake -S windows -B windows/out/build -A x64
cmake --build windows/out/build --config Release
ctest --test-dir windows/out/build -C Release --output-on-failure
./windows/out/build/Release/CodexMonitorHUD.exe
```

仓库的 Windows x64 CI 会执行源码契约检查、MSVC Release 编译、全部测试、EXE 存在性验证并上传临时构建产物。ARM64 尚未进入支持和发布范围。

CI 还会生成 `CodexMonitorHUD-windows-x64.zip`，其中包含 EXE、许可证和本说明，并同时生成 `.zip.sha256` 校验文件。这个便携包尚未进行 Windows 代码签名，不等同于正式安装包。

在 Windows 本机完成 Release 编译后，也可运行：

```powershell
./windows/package-release.ps1 `
  -BinaryPath "windows/out/build/Release/CodexMonitorHUD.exe" `
  -OutputDirectory "windows/out/release"
```

Windows CI 还会使用 WiX 生成当前用户范围的 MSI，静默安装到临时目录，核对已安装 EXE 与测试产物一致，再执行卸载并确认程序文件已移除。开发分支产物会明确标为 `unsigned`，签名前不作为正式公开安装包。

本机装有 WiX Toolset 3 时可运行：

```powershell
./windows/build-installer.ps1 `
  -BinaryPath "windows/out/build/Release/CodexMonitorHUD.exe" `
  -OutputDirectory "windows/out/installer"
```

## 测试范围

不依赖 Windows 的源码契约与算法测试：

```bash
python3 windows/tests/static_checks.py
cmake -S windows -B /tmp/codex-monitor-windows-tests -DBUILD_TESTING=ON
cmake --build /tmp/codex-monitor-windows-tests
ctest --test-dir /tmp/codex-monitor-windows-tests --output-on-failure
```

Windows CI 还包含以下原生集成测试：

- CPU 差值、整机归一化、进程树、内存前五名和采样请求合并/失效；
- 十一模块排序、主页/所属页独立开关、设置版本迁移、损坏回退和窗口回屏；
- 诊断缺失数据、CPU/内存压力、Codex/ChatGPT 合计高/可能/低影响、部分读数和冲突读数的固定样例；
- `codex.exe` 安全发现、NDJSON 管道、超长行/输出上限、超时、取消和 Job 子进程清理；
- 官方 `app-server` 握手、乱序响应、单方法失败保留、异常消息隔离、响应洪水上限；
- Codex 工作线程的成功刷新、暂停取消、不遗留进程和恢复后立即刷新；
- OpenAI 状态映射、固定 JSON 解析、完整失败退避、旧值保留、暂停失效和停止回收；状态测试不访问公网；
- Token 日汇总的延迟结算、重复日期、闰年/月边界、无效/未来数据和整数溢出保护。
- MSI 的真实静默安装、安装文件一致性和静默卸载。

这些测试证明编译、算法和进程边界，不替代真实 Windows 上的视觉、交互、账号数据和任务管理器对照验收。

## Windows 机器上的最小验收

1. 在真实 Windows x64 上启动 CI 产物，确认应用图标、单实例、任务栏最小化、置顶切换和退出正常。
2. 在不同窗口大小和 DPI 下切换 Home、Codex、Computer；确认卡片不形变，超出时可以滚动，拖动边角连续缩放。
3. 在设置中分别切换每个模块的主页和所属页开关、调整主页顺序，重启后确认恢复；五小时与每周额度必须能独立选择。
4. 与任务管理器抽样对照整机 CPU、物理内存、提交量、页面文件、Codex/ChatGPT 进程树和内存前五名，并检查诊断卡没有在数据不足时误报“正常”或错误归因给某一个应用。
5. 在已登录 Codex 的机器上对照官方界面检查额度、订阅和 Token 日期/汇总；今日未返回时不得显示为 `0`。
6. 检查最近任务只显示名称与最近时间，且不把 `app-server` 进程范围的状态写成桌面版全局实时状态。
7. 对照 OpenAI 官方状态页检查 Codex 组件和整体状态；隐藏状态模块、隐藏窗口或最小化时确认不再发起状态请求，恢复后立即刷新。
8. 切换到不需要数据的页面、隐藏所有相关模块和最小化窗口，确认性能采样与 Codex 子进程停止；恢复后立即取得新数据。
9. 模拟接口失败，确认旧值保留、失败提示出现、没有残留 `codex.exe app-server`；恢复后能重新刷新。
10. 把窗口移到第二块显示器后退出，拔掉该显示器再启动，窗口应完整回到可见工作区。

## 当前明确未完成

- 尚未在真实 Windows x64 上完成视觉/交互验收，也未与真实任务管理器和真实 Codex 账号数据做逐项对照。
- 尚未实现网络速度、磁盘读写、历史曲线与持久化、Token 费用与长期预测历史和额度通知；月末 Token 线性趋势与 OpenAI 官方服务状态已显示。
- 尚未移植颜色、透明度、角落预设，以及锁定窗口位置/大小。
- 已有 CI 便携 ZIP、SHA-256 和经过安装/卸载测试的未签名 MSI；尚未实现登录后自动启动、检查更新/一键更新、Windows 代码签名和正式发布流程。
- 尚未支持或验证 Windows ARM64。
- 热压力仍只显示 Windows 没有统一接口，不会通过不可靠的通用传感器模拟 macOS 热压力。
- 当前仍使用标准 Windows 标题栏；视觉细节尚未达到 macOS 版的完整等价。

跨平台复用边界和剩余功能差异见 [PORTING_NOTES.md](PORTING_NOTES.md)。
