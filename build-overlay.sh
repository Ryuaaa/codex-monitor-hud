#!/bin/zsh

set -eu

source_dir="${0:A:h}"
build_dir="/private/tmp/codex-monitor-hud-build"
app_dir="$build_dir/Codex Monitor HUD.app"
contents_dir="$app_dir/Contents"
macos_dir="$contents_dir/MacOS"
resources_dir="$contents_dir/Resources"
helper_app="$contents_dir/Helpers/SubscriptionSync.app"
helper_contents="$helper_app/Contents"
helper_macos="$helper_contents/MacOS"
helper_resources="$helper_contents/Resources"
sign_identity="${CODE_SIGN_IDENTITY:--}"

/bin/rm -rf "$app_dir"
/bin/mkdir -p "$macos_dir" "$resources_dir" "$helper_macos" "$helper_resources"
/usr/bin/xcrun clang -O2 -fobjc-arc -Wall -Wextra -Werror \
  -mmacosx-version-min=15.0 \
  -arch arm64 -arch x86_64 \
  -framework Cocoa \
  -framework UserNotifications \
  "$source_dir/overlay/CodexMonitorHUD.m" \
  "$source_dir/overlay/HUDView.m" \
  "$source_dir/overlay/UpdateManager.m" \
  "$source_dir/overlay/CodexStatusProvider.m" \
  "$source_dir/overlay/CodexProtocolCompatibility.m" \
  "$source_dir/overlay/CodexCostHistory.m" \
  "$source_dir/overlay/HUDLocalization.m" \
  "$source_dir/overlay/OpenAIServiceStatus.m" \
  "$source_dir/overlay/NativeSampler.m" \
  -o "$macos_dir/CodexMonitorHUD"
/usr/bin/lipo "$macos_dir/CodexMonitorHUD" -verify_arch arm64 x86_64
/bin/cp "$source_dir/overlay/Info.plist" "$contents_dir/Info.plist"
/bin/cp "$source_dir/overlay/AppIcon.icns" "$resources_dir/AppIcon.icns"
/bin/cp "$source_dir/overlay/HUDLocalizations.json" "$resources_dir/HUDLocalizations.json"
/usr/bin/xcrun clang -O2 -fobjc-arc -Wall -Wextra -Werror \
  -mmacosx-version-min=15.0 \
  -arch arm64 -arch x86_64 \
  -framework Cocoa -framework WebKit \
  "$source_dir/overlay/SubscriptionSync.m" \
  -o "$helper_macos/SubscriptionSync"
/bin/cp "$source_dir/overlay/SubscriptionSync-Info.plist" "$helper_contents/Info.plist"
/bin/cp "$source_dir/overlay/SubscriptionBillingReader.js" "$helper_resources/SubscriptionBillingReader.js"
/usr/bin/plutil -lint "$contents_dir/Info.plist"
/usr/bin/plutil -lint "$helper_contents/Info.plist"
/usr/bin/xattr -cr "$app_dir"
if [[ "$sign_identity" == "-" ]]; then
  /usr/bin/codesign --force --sign - "$helper_app"
  /usr/bin/codesign --force --sign - "$app_dir"
else
  /usr/bin/codesign --force --sign "$sign_identity" --options runtime --timestamp "$helper_app"
  /usr/bin/codesign --force --sign "$sign_identity" --options runtime --timestamp "$app_dir"
fi

/usr/bin/printf '构建完成：%s\n' "$app_dir"
