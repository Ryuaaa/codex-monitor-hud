#!/bin/sh
# Explicit opt-in local check: requires an already authenticated Codex installation.
# Prints capability/error categories only, never task names, account data or balances.
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/hud-live-check.XXXXXX")
trap 'test -n "$fixture_dir" && /bin/rm -rf "$fixture_dir"' EXIT HUP INT TERM
xcrun clang -O2 -fobjc-arc -Wall -Wextra -Werror -mmacosx-version-min=15.0 \
  -framework Foundation -I "$source_dir/overlay" \
  "$source_dir/tests/live-codex-protocol-check.m" \
  "$source_dir/overlay/CodexProtocolCompatibility.m" \
  "$source_dir/overlay/CodexStatusProvider.m" "$source_dir/overlay/CodexCostHistory.m" \
  -o "$fixture_dir/check"
"$fixture_dir/check"
