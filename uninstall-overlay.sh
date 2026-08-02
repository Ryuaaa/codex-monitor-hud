#!/bin/zsh

set -eu

label="com.codexmonitorhud.app"
launch_agent="$HOME/Library/LaunchAgents/$label.plist"
install_app="$HOME/Applications/Codex Monitor HUD.app"
uid=$(/usr/bin/id -u)

/bin/launchctl bootout "gui/$uid/$label" 2>/dev/null || true
if [[ -f "$launch_agent" ]]; then
  /bin/rm "$launch_agent"
fi
if [[ -d "$install_app" ]]; then
  /bin/rm -rf "$install_app"
fi

/usr/bin/printf '已移除悬浮窗；后台数据采集器和历史数据保持不变。\n'
