# Codex Monitor HUD Windows 1.1.0 未签名预览版

这是 Windows 10/11 x64 的 1.1.0 功能预览版，产品版本已与macOS 1.1.0统一，供信任本项目并愿意参与测试的用户提前使用。两个平台共享核心产品方向，平台专属界面和系统能力可能不同。

## 重要说明

- EXE 和 MSI 尚未获得 SignPath Foundation 可信签名，Windows 会显示“未知发布者”。
- 该 GitHub Release 标记为预发布，不会被 Codex Monitor HUD 的 Windows 自动更新通道选中。
- 可信签名完成后，将另行发布 `windows-v1.1.0` 正式版。
- 安装或运行前，可使用同名 `.sha256` 文件核对下载内容。

## 主要功能

- 原生 Win32/C++17 三页常驻悬浮窗，无内嵌浏览器引擎。
- 电脑 CPU、内存、进程、网络和磁盘负载及 Codex/ChatGPT 资源归因。
- Codex 5小时/每周额度、Token与估算费用、最近任务和本机活动趋势。
- 可自定义页面模块、颜色、透明度、置顶、最小化、位置锁定和自由缩放。
- 可选周额度消耗提醒、OpenAI服务状态和每日检查更新。

## 已验证

- GitHub Windows x64 编译与完整自动测试。
- 真实 MSI 安装、卸载和从旧版升级。
- 便携 ZIP/MSI 内容和 SHA-256 回验。

Windows ARM64 尚未支持。这是社区工具，不代表 OpenAI 官方产品。
