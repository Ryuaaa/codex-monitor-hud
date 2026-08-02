#!/bin/zsh

set -eu

label="com.codexmonitorhud.collector"
launch_agent="$HOME/Library/LaunchAgents/$label.plist"
uid=$(/usr/bin/id -u)

/bin/launchctl bootout "gui/$uid/$label" 2>/dev/null || true
if [[ -f "$launch_agent" ]]; then
  /bin/rm "$launch_agent"
fi

/usr/bin/printf '已停止并移除自动启动项。历史数据仍保留在：\n'
/usr/bin/printf '%s\n' "$HOME/Library/Application Support/CodexSystemMonitor/data"
