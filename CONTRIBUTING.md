# 参与贡献

欢迎提交问题和改进建议。这个项目首先保证数据边界、低负担和长期常驻稳定性，其次才是增加功能。

## 本地验证

替换 `assets/AppIcon-master.png` 后，先重新生成应用图标：

```zsh
python3 generate-app-icon.py --input assets/AppIcon-master.png --output overlay/AppIcon.icns
```

```zsh
./build-overlay.sh
"/private/tmp/codex-monitor-hud-build/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --logic-diagnostic
"/private/tmp/codex-monitor-hud-build/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --diagnostic
"/private/tmp/codex-monitor-hud-build/Codex Monitor HUD.app/Contents/MacOS/CodexMonitorHUD" --ui-diagnostic
./report.py --self-test
```

涉及Codex账户接口的修改，还应在已登录桌面版的Mac上运行 `--codex-diagnostic`。提交截图或日志前，请删除账号、路径和其他私人信息。

## 正式发行

正式安装包必须使用 `Developer ID Application` 证书并通过Apple公证，不能把临时本地签名包标为正式版：

```zsh
NOTARY_KEYCHAIN_PROFILE=codex-monitor-hud-notary ./release-macos.sh
```

脚本会依次完成双架构构建、强化运行时签名、公证、票据装订、macOS安全评估、重新打包和SHA-256生成。GitHub标签工作流使用同一脚本；仓库需配置 `MACOS_CERTIFICATE_P12_BASE64`、`MACOS_CERTIFICATE_PASSWORD`、`APPLE_ID`、`APPLE_TEAM_ID` 和 `APPLE_APP_PASSWORD` 五个加密Secrets。任何一步失败都不会发布Release。

Windows正式包必须来自精确Git标签的GitHub托管运行器，并通过SignPath.io与SignPath Foundation代码签名；签名政策和角色见[`CODE_SIGNING_POLICY.md`](CODE_SIGNING_POLICY.md)。未通过签名、发布者固定、MSI身份、安装和卸载验证时，工作流不会创建正式Windows Release。

## 修改原则

- 不读取对话、提示词、Cookie、密钥或命令内容。
- 任务标题只能来自官方标题字段；不得用任务预览或正文补全缺失标题。
- 不把估算值描述成绝对精确值。
- 不加入持续动画、网页引擎或高频外部进程。
- 新增采集项必须说明来源、刷新频率和数据口径。
- 修复日期、额度和瓶颈判断时应补充可重复测试。
