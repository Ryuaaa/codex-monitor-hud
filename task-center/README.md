# Codex Monitor 任务中心（Codex 活动 + 可选管理任务库）

这是与 HUD 严格隔离的第二个应用。生产运行时不需要 Node，不监听端口，不创建托盘或后台服务；关闭主窗口即退出。

## 下载

[macOS 1.3.0 正式版](https://github.com/Ryuaaa/codex-monitor-hud/releases/tag/task-center-v1.3.0)提供 Apple Silicon 与 Intel 通用的 `.app.zip`。应用已使用 Developer ID 签名并通过 Apple 公证；解压后移到“应用程序”即可使用。1.2.1 及后续版本也可在应用内检查并一键升级。

默认首页是“Codex 活动”：应用启动后通过本机官方 `thread/list` 自动读取最近 25 个交互式 Codex 任务，优先只读状态数据库并按最近活动排序。它只把任务编号、用户设置的名称、来源、时间、置顶状态和工作目录末级名称传给前端；不使用或保存 `preview` 对话预览，也不传递完整本机路径。请求结束后短期 `codex app-server` 子进程立即退出。

“管理任务”是可选入口。用户没有进入该页时，应用不会检测或读取正式任务目录；没有现成目录时，只有点击“创建本地任务库”才会在系统应用数据目录创建透明的 Markdown 任务与 JSONL 事件目录。现有个人正式任务目录仍自动兼容，也可通过 `CODEX_TASK_CENTER_TASK_ROOT` 指向其他明确的 `任务/` 目录。

## 开发

```bash
npm ci
npm test
npm run build
npm run tauri build -- --no-bundle
```

进入“管理任务”后，写入只允许由界面直接触发，并且必须先生成修改前/修改后预览、再次明确确认、通过文件哈希并发检查；随后原子写任务、追加正式 JSONL 事件并回读核对，失败时恢复原文件。开发和自动测试只使用合成夹具或临时目录，不写正式任务资料。

其他位置或 Windows 本地验证可在启动前设置 `CODEX_TASK_CENTER_TASK_ROOT`，它必须指向明确的 `任务/` 目录。项目/工作目录映射仍只读：

`~/.codex-monitor/task-center-projects.json`

格式见 `config/projects.example.json`。应用不在后台、定时器或页面加载时写任务，也不在应用内写项目映射配置。

已保存筛选只记录项目、正式状态、标签、归档开关和看板/列表视图，保存在应用自己的配置目录；搜索文字、任务标题、正文和其他任务内容不会写入筛选配置。筛选配置可删除、可重建，不是任务事实源。

浏览器开发预览只使用合成夹具，避免真实任务正文进入开发服务器。Tauri 生产构建才调用 Rust 文件适配器。

正式绑定的 Codex 对话历史只在用户点击“读取历史”后读取。任务中心按每页 20 条请求官方轮次元数据，分页请求使用 `itemsView: "notLoaded"`，不加载轮次内容；应用不展示、记录或持久化对话正文。请求结束后短期 `codex app-server` 子进程立即退出，不创建常驻连接或缓存。若当前 Codex 版本不支持分页，应用保留任务元数据并明确降级，不会一次性加载完整历史，也不会把短期接口状态当作桌面版实时状态。

选中官方任务后，可以把一段新内容交给原任务继续执行。界面会先展示确认卡，只有再次确认才调用官方 `thread/resume` 与 `turn/start`；恢复时不加载完整历史，输入内容也不写入任务中心缓存。运行中只显示开始、等待简单审批、完成、失败或中断等必要状态；命令正文、工具输出和对话正文不会进入任务中心。关闭任务中心或主动停止时，短期接口进程会退出。

“在 Codex 中打开”使用已安装 Codex 桌面版公开注册的任务链接，直接定位到选中的任务；若当前安装无法处理该链接，则只打开 Codex 并把任务编号复制到剪贴板，不会猜测或打开其他任务。

任务中心会从已安装的 Codex/ChatGPT 应用及 `PATH` 自动寻找官方命令；特殊安装位置可用 `CODEX_TASK_CENTER_CODEX_EXECUTABLE` 明确指定可执行文件。

当前允许写入：标题、正式任务状态、优先级、日期型截止日期、负责人、标签、父任务、阻塞任务、相关任务、归档/恢复，以及按正式最小字段新建任务。父任务和阻塞关系只写在当前任务中，子任务和“当前任务正在阻塞谁”由内存反向推导，避免一次操作改写多个任务文件。

评论和人工活动只以追加事件写入 `事件/YYYY-MM.jsonl`，不改变任务 Markdown；必须经过预览、明确确认、任务版本和事件版本双重检查及写后回读。附件、重复任务、甘特图、AI 总结、自动派发、工作流、Jira 或云协作仍不支持，也不会进入缓存或第二套业务数据库。

架构、字段矩阵、隐私边界和验收方案位于 `../docs/task-center/`。

## 平台验证

GitHub Actions 在 `macos-latest` 与 `windows-latest` 分别运行前端测试、Rust 测试和 Tauri 原生构建。Windows job 还验证 GUI 子系统、精确可见主窗口的 20 次 `WM_CLOSE`、WebView2 回收、监听端口、Node/进程残留和强制终止隔离；详细证据见 `../docs/task-center/windows-ci-evidence-2026-08-23.md`。

以下 macOS 交叉检查只用于提前发现条件编译问题，不等于 Windows 原生 CI 或桌面验收：

```bash
rustup target add --toolchain 1.88.0 x86_64-pc-windows-msvc
cargo +1.88.0 check --locked --manifest-path src-tauri/Cargo.toml \
  --target x86_64-pc-windows-msvc --all-targets
```

macOS 资源基线探针使用系统 `proc_pid_rusage`，直接读取目标 PID 的 CPU、常驻内存、物理占用、唤醒和累计能耗差值：

```bash
xcrun clang -Wall -Wextra -Werror -O2 tools/process_resource_probe.c \
  -o /private/tmp/task-center-process-resource-probe
/private/tmp/task-center-process-resource-probe 10 61 HUD_PID [TASK_CENTER_PID WEBKIT_PID ...]
```

`10 61` 表示每 10 秒采样一次、覆盖 10 分钟。首行是预热样本，不参与速率统计。
