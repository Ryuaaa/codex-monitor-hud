#!/bin/zsh
set -euo pipefail

helper="/private/tmp/codex-monitor-hud-build/Codex Monitor HUD.app/Contents/Helpers/SubscriptionSync.app/Contents/MacOS/SubscriptionSync"
if [[ ! -x "$helper" ]]; then
  print -u2 'Build the macOS app before testing subscription sync.'
  exit 1
fi

for case_name in zh zh-Hant en ja ko; do
  output="$("$helper" --fixture --silent --fixture-case "$case_name")"
  if [[ "$output" != *'fixture_read=2030-04-15:cancelled'* ]]; then
    print -u2 "Failed $case_name: $output"
    exit 1
  fi
done
saved_before="$(defaults read com.codexmonitorhud.subscription-data.fixture subscriptionLastVerified)"
for case_name in invoice ambiguous invalid appstore; do
  output="$("$helper" --fixture --silent --fixture-case "$case_name")"
  if [[ "$output" != *'fixture_error=parse-failed'* || "$output" == *'fixture_read='* ]]; then
    print -u2 "Invalid or ambiguous date was misread ($case_name): $output"
    exit 1
  fi
done
saved_after="$(defaults read com.codexmonitorhud.subscription-data.fixture subscriptionLastVerified)"
if [[ "$saved_before" != "$saved_after" || "$saved_after" != *'2030-04-15'* ]]; then
  print -u2 'A failed read overwrote the previous valid date.'
  exit 1
fi
defaults delete com.codexmonitorhud.subscription-data.fixture >/dev/null 2>&1 || true
print 'subscription_sync_fixtures=9/9 cache_fallback=pass'
