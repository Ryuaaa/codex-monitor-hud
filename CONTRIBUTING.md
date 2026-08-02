# 参与贡献

欢迎提交问题和改进建议。这个项目首先保证数据边界、低负担和长期常驻稳定性，其次才是增加功能。

## 本地验证

```zsh
./build-overlay.sh
"/private/tmp/codex-monitor-hud-build/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --logic-diagnostic
"/private/tmp/codex-monitor-hud-build/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --diagnostic
```

涉及Codex账户接口的修改，还应在已登录桌面版的Mac上运行 `--codex-diagnostic`。提交截图或日志前，请删除账号、路径和其他私人信息。

## 修改原则

- 不读取对话、提示词、Cookie、密钥或命令内容。
- 不把估算值描述成绝对精确值。
- 不加入持续动画、网页引擎或高频外部进程。
- 新增采集项必须说明来源、刷新频率和数据口径。
- 修复日期、额度和瓶颈判断时应补充可重复测试。
