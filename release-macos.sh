#!/bin/zsh

set -euo pipefail

source_dir="${0:A:h}"
app_dir="/private/tmp/codex-monitor-hud-build/Codex Monitor HUD.app"
archive="$source_dir/Codex-Monitor-HUD.app.zip"
checksum="$archive.sha256"
submission_archive="/private/tmp/Codex-Monitor-HUD-notarization.zip"
version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$source_dir/overlay/Info.plist")"
build_number="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$source_dir/overlay/Info.plist")"

if [[ -n "${CODE_SIGN_IDENTITY:-}" ]]; then
  sign_identity="$CODE_SIGN_IDENTITY"
else
  identity_text="$(/usr/bin/security find-identity -v -p codesigning | /usr/bin/sed -n 's/.*"\(Developer ID Application:.*\)"/\1/p')"
  identities=()
  if [[ -n "$identity_text" ]]; then identities=("${(@f)identity_text}"); fi
  if (( ${#identities[@]} != 1 )); then
    /usr/bin/printf '需要且只能自动识别一个 Developer ID Application 证书；当前找到 %d 个。\n' "${#identities[@]}" >&2
    /usr/bin/printf '如有多个证书，请设置 CODE_SIGN_IDENTITY 后重试。\n' >&2
    exit 1
  fi
  sign_identity="$identities[1]"
fi

if [[ "$sign_identity" != Developer\ ID\ Application:* ]]; then
  /usr/bin/printf '正式发布必须使用 Developer ID Application 证书。\n' >&2
  exit 1
fi

export CODE_SIGN_IDENTITY="$sign_identity"
"$source_dir/build-overlay.sh"

/usr/bin/codesign --verify --deep --strict --verbose=2 "$app_dir"
/usr/bin/lipo "$app_dir/Contents/MacOS/CodexMonitorHUD" -verify_arch arm64 x86_64

/bin/rm -f "$submission_archive" "$archive" "$checksum"
/usr/bin/ditto -c -k --norsrc --noextattr --noqtn --noacl --keepParent "$app_dir" "$submission_archive"

if [[ -n "${APPLE_ID:-}" && -n "${APPLE_TEAM_ID:-}" && -n "${APPLE_APP_PASSWORD:-}" ]]; then
  /usr/bin/xcrun notarytool submit "$submission_archive" \
    --apple-id "$APPLE_ID" \
    --team-id "$APPLE_TEAM_ID" \
    --password "$APPLE_APP_PASSWORD" \
    --wait
else
  notary_profile="${NOTARY_KEYCHAIN_PROFILE:-codex-monitor-hud-notary}"
  /usr/bin/xcrun notarytool submit "$submission_archive" --keychain-profile "$notary_profile" --wait
fi

/usr/bin/xcrun stapler staple "$app_dir"
/usr/bin/xcrun stapler validate "$app_dir"
/usr/sbin/spctl --assess --type execute --verbose=4 "$app_dir"
/usr/bin/codesign --verify --deep --strict --verbose=2 "$app_dir"

/usr/bin/ditto -c -k --norsrc --noextattr --noqtn --noacl --keepParent "$app_dir" "$archive"
(
  cd "$source_dir"
  /usr/bin/shasum -a 256 "${archive:t}" > "${checksum:t}"
)

/usr/bin/printf '正式安装包已完成签名、公证和系统验证：\n'
/usr/bin/printf '  版本：%s (%s)\n' "$version" "$build_number"
/usr/bin/printf '  应用：%s\n' "$app_dir"
/usr/bin/printf '  压缩包：%s\n' "$archive"
/usr/bin/printf '  校验文件：%s\n' "$checksum"
