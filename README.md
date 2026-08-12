# Codex 电脑轻量监测器

> macOS 正式版：1.0.0，已完成Developer ID签名和Apple公证。Windows x64公开预览版：0.3.0，已经通过GitHub Windows虚拟机的构建、窗口、安装、升级和安全测试，暂未配置可信商业代码签名。

这是一个常驻在电脑屏幕角落的原生三页面悬浮窗：主页可自由组合Codex和电脑模块，Codex页显示账户额度、订阅与Token用量，电脑性能页用来快速判断电脑是否吃紧、是否主要由 Codex 引起、瓶颈在 CPU 还是内存。macOS版使用AppKit，Windows版使用Win32 + C++17；两者都不嵌入网页浏览器引擎。

界面使用原生 AppKit 深色半透明材质、静态高光和卡片层级实现，不嵌入网页浏览器引擎；视觉接近HTML原型，同时避免额外的浏览器进程和持续动画负担。

应用图标使用“小烈刀的AI宇宙”原创人物IP：低饱和灰蓝线稿、透明橙色发夹，以及对应性能监控的仪表环和柱形图；在小尺寸下优先保留人物、发夹和工具属性的辨识度。

## Codex页面

- 可选显示“任务活动（本机推测）”：显示最近活跃任务数量、最长活动时长和最多3个任务标题；活跃时每5秒、空闲时每20秒判断一次。
- 新增“最近任务（历史）”：通过官方任务列表显示最近3个任务标题和更新时间，明确不把历史列表冒充为正在运行的任务。
- Codex桌面版正在运行的 App Server 只通过桌面版自身的标准输入输出连接，外部悬浮窗无法直接取得它的内存中运行状态。因此本工具把官方任务列表中的标题、任务开始时间和文件路径，与本机会话文件最近写入情况组合判断；结果用于快速观察，不冒充官方精确的“运行中”状态。
- 显示5小时和每周额度的剩余百分比与恢复倒计时；两项拥有独立开关，默认同时显示。
- 接口暂时没有返回某个额度窗口时明确显示“当前未返回”，不会当成0%。
- “Token用量与费用”优先采用官方每日汇总作为Token总量，再用本机会话记录中的模型和输入/输出结构估算30天API等价费用及本月费用；官方当天尚未结算时，今日值临时使用本机趋势。费用在卡片中只标一次“估算”。该模块默认显示，主页和Codex页可分别关闭。
- “额度趋势预测”根据同一额度窗口内的剩余百分比变化判断按当前速度能否撑到重置，至少积累15分钟后显示；5小时与每周窗口分别计算。
- “OpenAI服务状态”直接读取官方公开状态页中的“Codex in ChatGPT Desktop”和OpenAI整体状态，用来辅助区分官方故障、本机网络和账号问题。Codex页默认显示，主页默认关闭并可自行加入。
- 官方“账户Token统计”保留为可选模块；接口尚未返回今天的数据时显示“最新日期 + 对应用量”，不会把缺失写成0。升级到本地统计的版本后，该模块默认关闭，仍可重新勾选。
- 模型专属额度属于高级显示，默认关闭；只有接口确实返回独立额度时才出现。
- 最长单次任务时长和历史最长连续天数可分别加入主页或Codex页，默认关闭。
- Codex账户数据和任务元数据采用智能刷新：正常或空闲时每5分钟，额度低于15%且仍有任务活动、或读取失败时每1分钟短暂读取一次，完成后立即退出接口进程；额度、订阅、用量和任务历史分别显示正常、过期、失败及上次有效时间。

## 常驻显示

- 总体判断：正常、CPU 较高、CPU 吃紧、内存吃紧或系统热压力限制性能。
- CPU：整机占用与 Codex 占整机总算力的比例。
- 内存：整机已用内存 / 总内存 / 百分比，以及Codex原生常驻内存和它占总内存的百分比；两组数据分开标注。
- 内存排行：电脑性能页直接显示内存占用最高的5个软件、占用GB及总内存百分比；每20秒更新一次。
- 诊断卡片：显示当前瓶颈、Codex影响程度和启发式判断置信度，不把相关性说成已证明的因果。
- 趋势：复用现有采样在内存中绘制CPU曲线；未采满10分钟时显示实际采样时长，采满后显示近10分钟，不增加新的采集进程。
- 健康状态：macOS 内存压力、交换空间及十分钟变化、系统热压力等级。这里不是摄氏温度。

点击顶部标签切换主页、Codex和电脑性能页面。齿轮会打开集中设置窗口，主页、Codex页和电脑性能页的全部勾选状态可同时查看；关闭某项后，它的卡片、状态文字和详细文字会一起隐藏。设置中还可拖动调整主页Codex模块和电脑模块的上下顺序，并直接查看每类数据的来源与所需权限。顶部减号会把窗口真正最小化到macOS程序栏（Dock），点击程序栏图标即可恢复；图钉按钮可切换置顶，锁形按钮可锁定位置和大小，避免误拖动。悬浮窗可直接拖动任意边角连续等比调整整体大小，最小为75%，最大尺寸按照当前页面内容和屏幕可用空间自动决定；拖拽过程中卡片、文字和图形不会被单向拉伸，设置和右键菜单可恢复标准大小。主页高度会随已选模块增长，不再因固定上限裁掉底部内容。右键菜单继续提供颜色、透明度、刷新频率、四角位置、窄栏模式、趋势记录、登录后自动启动和检查更新。

展开详细显示后，还可以看到 Codex 进程构成、最大单进程、Codex 磁盘读写、整机网络速度，以及 CPU 和内存排名靠前的软件。

## 采集频率与负担

- CPU、Codex 内存和进程树：每 5 秒。
- 网络、交换空间：每 10 秒；全部软件排名：每 20 秒。
- 系统热压力等级：每 20 秒。
- Codex任务活动：有活跃任务时每5秒、空闲时每20秒只检查最近写入的本机会话文件；单个候选最多读取末尾1MB。官方账户和任务列表正常或空闲时每5分钟、低额度且有活动或失败重试时每1分钟读取一次，限制为最近64项并优先使用状态数据库。
- 官方任务列表中的工作目录优先采用状态数据库保存的最新值；若会话记录已迁移、压缩或暂不可读，工具不会启动持续解压任务，也不会把缺失记录显示成“0个活跃任务”，而会明确标注无法可靠判断或仅显示可确认部分。
- 本机模型与费用样本每5分钟在低优先级后台增量更新。首次补齐每轮最多读取64MB，普通正文流式跳过，单行最多保留512KB；补齐完成后只读取新增部分。关闭主页和Codex页的“Token用量与费用”后停止该扫描。
- 额度预测只在官方额度刷新时保存剩余百分比、重置时间和采样时间，不增加新的高频定时器。
- OpenAI官方服务状态正常时每10分钟检查一次，故障或读取失败时每2分钟重试；主页和Codex页都关闭该模块后停止请求。请求不带账号、Cookie或Token。
- 已隐藏且当前不需要的排行榜、网络、交换空间和热状态停止采集；只查看Codex页且关闭历史记录时，电脑采样定时器会暂停。
- 所有定时器允许macOS在小范围内合并唤醒；隐藏模块会停止对应采集，任务活动会在5秒与20秒之间自动切换。
- 历史：每 60 秒写一条这一分钟的平均值与峰值，不保存每个 5 秒原始样本。
- 连续性：另行记录启动、系统唤醒、正常退出和本次运行实例编号，用于区分睡眠、重启与无法解释的历史缺口。

悬浮窗直接调用 macOS 底层系统接口，不需要打开或读取“活动监视器”。Codex本机接口可能按OpenAI客户端自身机制联网；软件更新功能只向本项目的GitHub公开接口检查版本，不上传监控数据。性能历史位于：

`~/Library/Application Support/CodexSystemMonitor/native-history/`

Token增量缓存和额度预测样本位于：

`~/Library/Application Support/CodexSystemMonitor/codex-cost-cache.json`

`~/Library/Application Support/CodexSystemMonitor/quota-usage-history.json`

## 数据口径

- Codex 包括 ChatGPT/Codex 主程序以及由它们启动的渲染器、工具和辅助进程。
- Codex 内存使用 macOS 提供的原生常驻内存字段，和你此前看到的量级更接近，适合判断趋势和比例；它不等于记账级的绝对独占内存。
- 整机“已用内存”按macOS虚拟内存页统计估算，口径接近活动监视器的App内存、联动内存和压缩内存合计；同时保留内存压力作为是否真正吃紧的主要判断。
- CPU 已按整机总算力归一化，100% 表示整台电脑的逻辑处理器都接近占满。
- 进程CPU累计时间会先按当前Mac的系统计时比例换算，再按逻辑处理器数量归一化；固定公式测试用于防止再次明显低报。
- Token与费用统计参考CodexBar的本地会话统计思路，但使用本项目自己的混合口径：官方每日汇总负责7天、30天和月度Token总量，本机增量扫描负责模型与计价结构；官方数据不可用时才退回本机观察值。累计计数采用只升不降的高水位差值，重复快照和交错的较小累计值不会反复放大。缓存按日期和模型压缩保存Token计数、预计算费用、计价覆盖量，以及增量扫描所需的文件大小、修改时间、解析位置和累计高水位；会话文件路径只保存不可逆摘要，不保留会话原文。
- 费用是按OpenAI公开的标准API单价计算的API等价估算，不是Pro订阅账单，也不会产生扣款；不区分Priority、Flex、Batch、企业合同价等特殊计费档位。未知模型不会硬套价格，界面会降低“计价覆盖率”。当前内置价格核对日期为2026-08-08。
- 为显示任务活动，工具会请求官方 `thread/list`，使用任务编号、可选标题、最近活动时间和本机会话路径；有标题时会显示在悬浮窗中，没有标题时只显示“未命名任务”，不会改用任务预览。它还会短暂读取最近活跃会话文件的末尾，最多1MB，只解析记录类型、角色、阶段和时间戳，不提取、保存或显示对话正文、提示词、工具输出或命令内容。
- `thread/list` 返回的当前工作目录与不稳定的会话文件路径分开保存：工作目录用于项目归属，会话文件路径只用于本机活动趋势判断。迁移或压缩后的记录无法按原口径读取时，状态会降级为未知或部分可确认。
- Codex额度与最近任务通过随桌面版或Codex命令行工具安装的本机官方接口读取；工具不会读取浏览器Cookie或要求API密钥。由于无法连接ChatGPT桌面版正在使用的同一个App Server实例，单任务实时Token消耗速度当前不会显示。
- Codex App Server已有公开文档；本工具只调用公开文档中的账户和任务列表方法。任务路径属于官方标注的不稳定字段，因此路径缺失时会自动退回本机会话目录判断。读取仍可能因桌面版或命令行工具未安装、账号退出或认证方式不支持、网络或上游服务异常、接口超时、协议调整或可选字段暂未返回而失败；工具会保留上次数据并明确标出失败状态。

## 系统要求

- macOS正式版：macOS 15或更高版本，支持Apple芯片和Intel处理器；从源码构建需要Xcode Command Line Tools。
- Windows公开预览版：Windows 10/11 x64；暂不支持ARM64。
- 电脑性能监控可独立工作；Codex额度、订阅和用量需要本机安装并登录ChatGPT桌面版、Codex桌面版或Codex命令行工具。

## 安装或更新

不想自行构建时，可从[GitHub发布页](https://github.com/Ryuaaa/codex-monitor-hud/releases/tag/v1.0.0)下载：

- macOS：`Codex-Monitor-HUD.app.zip`。解压后把应用移到“应用程序”文件夹并打开；文件已经Developer ID签名并通过Apple公证。
- Windows安装版：`CodexMonitorHUD-windows-x64-0.3.0-unsigned.msi`。
- Windows便携版：`CodexMonitorHUD-windows-x64-0.3.0-preview-unsigned.zip`，解压后运行其中的 `CodexMonitorHUD.exe`。

Windows当前是公开预览版，EXE和MSI暂未配置可信商业代码签名，Windows可能显示“未知发布者”或SmartScreen提示。安装包来自已全绿的公开GitHub Windows流水线；签名前，一键安装更新按设计保持关闭。完整Windows说明见[`windows/README.md`](windows/README.md)。

```zsh
./install-overlay.sh
```

默认在左下角并覆盖普通窗口。可拖动并记住位置；拖动任意边角可调整整体大小；齿轮集中显示所有内容开关，右键菜单可调整屏幕位置、透明度、颜色、刷新频率，或打开趋势数据文件夹。新安装默认不随登录自动启动；如有需要，可在设置或右键菜单中手动开启。升级会保留旧版已有的选择。

应用启动4秒后会判断距离上次自动检查是否已满24小时，满足时才检查；长期运行时每天自动检查一次。也可随时在设置、应用菜单或右键菜单点击“检查更新”。发现新版后可一键下载、校验安全摘要、替换旧版并重启，失败会保留旧版。更新来源固定为本仓库的GitHub Release。

发布页提供Developer ID签名并通过Apple公证的应用压缩包和SHA-256校验文件。使用0.8.0及后续版本的用户可在应用内收到并一键安装未来更新；只下载源码的用户仍可运行上面的安装脚本更新。

## 验证

```zsh
"$HOME/Applications/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --diagnostic
"$HOME/Applications/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --codex-diagnostic
"$HOME/Applications/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --cost-diagnostic
"$HOME/Applications/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --logic-diagnostic
"$HOME/Applications/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --ui-diagnostic
"$HOME/Applications/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --update-diagnostic
"$HOME/Applications/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --update-handoff-diagnostic
"$HOME/Applications/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --service-status-diagnostic
"$HOME/Applications/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --singleton-diagnostic
```

第二条必须返回 `quota_available=true`、`account_available=true` 和 `usage_available=true`，并给出官方7天、30天和月度Token汇总。第三条验证本机模型样本、费用和计价覆盖率；首次运行可能显示 `local_cost_scan_incomplete=true`，界面会标为“模型样本更新中”，但Token总量仍优先采用官方汇总。第四条包含Token解析、累计高水位、价格、长上下文、重复记录去重、增量缓存、额度预测和服务状态解析固定测试；第五条检查集中设置、位置和大小锁定、隐藏停采及等比缩放；倒数第二条实际访问OpenAI官方状态页，其余命令分别检查更新、安全交接和单实例保护。

## 开源

项目采用 MIT 许可证。隐私边界见 `PRIVACY.md`。Codex是OpenAI的产品名称，本项目是社区工具，不代表OpenAI官方产品。

首次公开与发行前的核验状态见 `RELEASE_CHECKLIST.md`。

## 查看趋势报告

```zsh
"$HOME/Library/Application Support/CodexSystemMonitor/report.py" --hours 24
```

小时数可改为 `1`、`72` 或 `168`。

## 停止悬浮窗

```zsh
./uninstall-overlay.sh
```

旧的每分钟脚本采集器已停用，但脚本仍保留作可选的独立采集方式。需要启用时先运行 `./install.sh`；它与原生悬浮窗历史目录不同。
