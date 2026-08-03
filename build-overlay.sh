#!/bin/zsh

set -eu

source_dir="${0:A:h}"
build_dir="/private/tmp/codex-monitor-hud-build"
app_dir="$build_dir/Codex Monitor HUD.app"
contents_dir="$app_dir/Contents"
macos_dir="$contents_dir/MacOS"

/bin/mkdir -p "$macos_dir"
/usr/bin/xcrun clang -O2 -fobjc-arc -Wall -Wextra -Werror \
  -framework Cocoa \
  "$source_dir/overlay/CodexMonitorHUD.m" \
  "$source_dir/overlay/HUDView.m" \
  "$source_dir/overlay/CodexStatusProvider.m" \
  "$source_dir/overlay/NativeSampler.m" \
  -o "$macos_dir/CodexMonitorHUD"
/bin/cp "$source_dir/overlay/Info.plist" "$contents_dir/Info.plist"
/usr/bin/plutil -lint "$contents_dir/Info.plist"
/usr/bin/xattr -cr "$app_dir"
/usr/bin/codesign --force --sign - "$app_dir"

/usr/bin/printf '构建完成：%s\n' "$app_dir"
