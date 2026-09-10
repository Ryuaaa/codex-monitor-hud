#!/bin/sh
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/hud-maintenance-tests.XXXXXX")
trap 'test -n "$fixture_dir" && /bin/rm -rf "$fixture_dir"' EXIT HUP INT TERM
xcrun clang -O2 -fobjc-arc -Wall -Wextra -Werror -mmacosx-version-min=15.0 \
  -framework Cocoa -I "$source_dir/overlay" \
  "$source_dir/tests/maintenance-tests.m" "$source_dir/overlay/CodexProtocolCompatibility.m" \
  "$source_dir/overlay/HUDLocalization.m" \
  "$source_dir/overlay/CodexStatusProvider.m" "$source_dir/overlay/CodexCostHistory.m" \
  "$source_dir/overlay/UpdateManager.m" -o "$fixture_dir/maintenance-tests"
"$fixture_dir/maintenance-tests" "$fixture_dir"
