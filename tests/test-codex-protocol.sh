#!/bin/sh
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/hud-protocol-tests.XXXXXX")
trap 'test -n "$fixture_dir" && /bin/rm -rf "$fixture_dir"' EXIT HUP INT TERM
xcrun clang -O2 -fobjc-arc -Wall -Wextra -Werror -mmacosx-version-min=15.0 \
  -framework Foundation -I "$source_dir/overlay" \
  "$source_dir/tests/codex-protocol-tests.m" \
  "$source_dir/overlay/CodexProtocolCompatibility.m" \
  "$source_dir/overlay/CodexStatusProvider.m" "$source_dir/overlay/CodexCostHistory.m" \
  -o "$fixture_dir/codex-protocol-tests"
cp "$source_dir/tests/fixtures/codex-app-server.py" "$fixture_dir/server.py"
chmod 700 "$fixture_dir/server.py"
for mode in legacy modern eof timeout; do ln -s "$fixture_dir/server.py" "$fixture_dir/$mode"; done
"$fixture_dir/codex-protocol-tests" "$fixture_dir"
