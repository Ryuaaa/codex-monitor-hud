#!/bin/zsh

set -eu

label="com.codexmonitorhud.collector"
source_dir="${0:A:h}"
install_dir="$HOME/Library/Application Support/CodexSystemMonitor"
launch_agent="$HOME/Library/LaunchAgents/$label.plist"
uid=$(/usr/bin/id -u)

/bin/mkdir -p "$install_dir/data" "$install_dir/logs" "$HOME/Library/LaunchAgents"
/usr/bin/install -m 700 "$source_dir/collect.sh" "$install_dir/collect.sh"
/usr/bin/install -m 700 "$source_dir/report.py" "$install_dir/report.py"

/usr/bin/printf '%s\n' "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>Label</key>
  <string>$label</string>
  <key>ProgramArguments</key>
  <array>
    <string>$install_dir/collect.sh</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>StartInterval</key>
  <integer>60</integer>
  <key>ProcessType</key>
  <string>Background</string>
  <key>LowPriorityIO</key>
  <true/>
  <key>Nice</key>
  <integer>10</integer>
  <key>StandardOutPath</key>
  <string>$install_dir/logs/stdout.log</string>
  <key>StandardErrorPath</key>
  <string>$install_dir/logs/stderr.log</string>
</dict>
</plist>" > "$launch_agent"

/usr/bin/plutil -lint "$launch_agent"
/bin/launchctl bootout "gui/$uid/$label" 2>/dev/null || true
/bin/launchctl bootstrap "gui/$uid" "$launch_agent"
# RunAtLoad normally creates the first sample. A non-forcing kickstart is only
# a best-effort nudge and must not make a successful installation look failed.
/bin/launchctl kickstart "gui/$uid/$label" 2>/dev/null || true

/usr/bin/printf '已安装并启动：%s\n' "$label"
/usr/bin/printf '数据目录：%s/data\n' "$install_dir"
/usr/bin/printf '24 小时报表：%s/report.py --hours 24\n' "$install_dir"
