#!/bin/zsh

set -eu

label="com.codexmonitorhud.app"
source_dir="${0:A:h}"
source_app="/private/tmp/codex-monitor-hud-build/Codex Monitor HUD.app"
install_app="$HOME/Applications/Codex Monitor HUD.app"
executable="$install_app/Contents/MacOS/CodexMonitorHUD"
launch_agent="$HOME/Library/LaunchAgents/$label.plist"
log_dir="$HOME/Library/Application Support/CodexSystemMonitor/logs"
uid=$(/usr/bin/id -u)

"$source_dir/build-overlay.sh"
/bin/mkdir -p "$HOME/Applications" "$HOME/Library/LaunchAgents" "$log_dir"
/usr/bin/ditto "$source_app" "$install_app"
/bin/cp "$source_dir/report.py" "$HOME/Library/Application Support/CodexSystemMonitor/report.py"
/bin/chmod 755 "$HOME/Library/Application Support/CodexSystemMonitor/report.py"

/usr/bin/printf '%s\n' "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>Label</key>
  <string>$label</string>
  <key>ProgramArguments</key>
  <array>
    <string>$executable</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>LimitLoadToSessionType</key>
  <string>Aqua</string>
  <key>ProcessType</key>
  <string>Interactive</string>
  <key>StandardOutPath</key>
  <string>$log_dir/hud.stdout.log</string>
  <key>StandardErrorPath</key>
  <string>$log_dir/hud.stderr.log</string>
</dict>
</plist>" > "$launch_agent"

/usr/bin/plutil -lint "$launch_agent"
/bin/launchctl bootout "gui/$uid/$label" 2>/dev/null || true
loaded=false
for attempt in 1 2 3; do
  if /bin/launchctl bootstrap "gui/$uid" "$launch_agent" 2>/dev/null; then
    loaded=true
    break
  fi
  /bin/sleep 1
done
if [[ "$loaded" != true ]]; then
  /bin/launchctl bootstrap "gui/$uid" "$launch_agent"
fi
/bin/launchctl kickstart "gui/$uid/$label" 2>/dev/null || true

/usr/bin/printf '已安装并启动悬浮窗：%s\n' "$install_app"
