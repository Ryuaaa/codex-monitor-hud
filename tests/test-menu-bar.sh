#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d /private/tmp/hud-menubar-tests.XXXXXX)
test_app="$test_dir/MenuBarTests.app"
mkdir -p "$test_app/Contents/MacOS" "$test_app/Contents/Resources"
cp "$repo_dir/overlay/HUDLocalizations.json" "$test_app/Contents/Resources/"
xcrun clang -O2 -fobjc-arc -Wall -Wextra -Werror -mmacosx-version-min=15.0 \
  -framework Cocoa -framework UserNotifications -I "$repo_dir/overlay" \
  "$repo_dir/tests/menu-bar-tests.m" "$repo_dir/overlay/HUDView.m" \
  "$repo_dir/overlay/UpdateManager.m" "$repo_dir/overlay/CodexStatusProvider.m" \
  "$repo_dir/overlay/CodexProtocolCompatibility.m" "$repo_dir/overlay/CodexCostHistory.m" \
  "$repo_dir/overlay/HUDLocalization.m" "$repo_dir/overlay/OpenAIServiceStatus.m" \
  "$repo_dir/overlay/NativeSampler.m" -o "$test_app/Contents/MacOS/MenuBarTests"
for lang in zh-Hans zh-Hant en ja ko; do
  "$test_app/Contents/MacOS/MenuBarTests" "$test_dir/menu-$lang.png" "$lang"
done
printf 'Synthetic captures: %s\n' "$test_dir"
