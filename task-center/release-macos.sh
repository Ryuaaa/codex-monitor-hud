#!/bin/zsh

set -euo pipefail

source_dir="${0:A:h}"
version="$(/usr/bin/sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$source_dir/src-tauri/tauri.conf.json" | /usr/bin/head -1)"
expected_tag="task-center-v${version}"
release_tag="${TASK_CENTER_RELEASE_TAG:-$expected_tag}"
if [[ "$release_tag" != "$expected_tag" ]]; then
  /usr/bin/printf '任务中心标签必须是 %s；当前是 %s。\n' "$expected_tag" "$release_tag" >&2
  exit 1
fi

identity_text="$(/usr/bin/security find-identity -v -p codesigning | /usr/bin/sed -n 's/.*"\(Developer ID Application:.*\)"/\1/p')"
identities=()
if [[ -n "$identity_text" ]]; then identities=("${(@f)identity_text}"); fi
if [[ -n "${APPLE_SIGNING_IDENTITY:-}" ]]; then
  sign_identity="$APPLE_SIGNING_IDENTITY"
elif (( ${#identities[@]} == 1 )); then
  sign_identity="$identities[1]"
else
  /usr/bin/printf '正式发布需要且只能自动识别一个 Developer ID Application 证书。\n' >&2
  exit 1
fi
if [[ "$sign_identity" != Developer\ ID\ Application:* ]]; then
  /usr/bin/printf '正式发布必须使用 Developer ID Application 证书。\n' >&2
  exit 1
fi

if [[ -z "${TAURI_SIGNING_PRIVATE_KEY:-}" ]]; then
  export TAURI_SIGNING_PRIVATE_KEY="$(/usr/bin/security find-generic-password -a "$USER" -s "Codex Monitor Task Center Updater Private Key" -w)"
fi
if [[ -z "${TAURI_SIGNING_PRIVATE_KEY_PASSWORD:-}" ]]; then
  export TAURI_SIGNING_PRIVATE_KEY_PASSWORD="$(/usr/bin/security find-generic-password -a "$USER" -s "Codex Monitor Task Center Updater Key Password" -w)"
fi
test -n "$TAURI_SIGNING_PRIVATE_KEY"
test -n "$TAURI_SIGNING_PRIVATE_KEY_PASSWORD"

export APPLE_SIGNING_IDENTITY="$sign_identity"
cd "$source_dir"
npm run tauri build -- --target universal-apple-darwin --bundles app --ci

bundle_dir="$source_dir/src-tauri/target/universal-apple-darwin/release/bundle/macos"
app_name="Codex Monitor Task Center.app"
app_path="$bundle_dir/$app_name"
updater_archive="$bundle_dir/$app_name.tar.gz"
updater_signature="$updater_archive.sig"
submission_archive="/private/tmp/Codex-Monitor-Task-Center-${version}-notarization.zip"
artifacts_dir="$source_dir/release-artifacts"
download_archive="$artifacts_dir/Codex-Monitor-Task-Center-macOS-universal-${version}.app.zip"
download_checksum="$download_archive.sha256"
published_updater="$artifacts_dir/Codex-Monitor-Task-Center-macOS-universal-${version}.app.tar.gz"
published_signature="$published_updater.sig"

/usr/bin/codesign --verify --deep --strict --verbose=2 "$app_path"
/usr/bin/lipo "$app_path/Contents/MacOS/codex-monitor-task-center" -verify_arch arm64 x86_64
if [[ -e "$submission_archive" ]]; then /bin/unlink "$submission_archive"; fi
/usr/bin/ditto -c -k --norsrc --noextattr --noqtn --noacl --keepParent "$app_path" "$submission_archive"

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

/usr/bin/xcrun stapler staple "$app_path"
/usr/bin/xcrun stapler validate "$app_path"
/usr/sbin/spctl --assess --type execute --verbose=4 "$app_path"
/usr/bin/codesign --verify --deep --strict --verbose=2 "$app_path"

if [[ -e "$updater_archive" ]]; then /bin/unlink "$updater_archive"; fi
if [[ -e "$updater_signature" ]]; then /bin/unlink "$updater_signature"; fi
(
  cd "$bundle_dir"
  COPYFILE_DISABLE=1 /usr/bin/tar --no-xattrs --exclude '._*' -czf "$updater_archive" "$app_name"
)
npm run tauri signer sign -- "$updater_archive"

/bin/mkdir -p "$artifacts_dir"
for existing in "$download_archive" "$download_checksum" "$published_updater" "$published_signature"; do
  if [[ -e "$existing" ]]; then /bin/unlink "$existing"; fi
done
/usr/bin/ditto -c -k --norsrc --noextattr --noqtn --noacl --keepParent "$app_path" "$download_archive"
(
  cd "$artifacts_dir"
  /usr/bin/shasum -a 256 "${download_archive:t}" > "${download_checksum:t}"
)
/bin/cp "$updater_archive" "$published_updater"
/bin/cp "$updater_signature" "$published_signature"

/usr/bin/printf '任务中心正式 macOS 产物已完成：\n'
/usr/bin/printf '  版本：%s\n' "$version"
/usr/bin/printf '  标签：%s\n' "$release_tag"
/usr/bin/printf '  应用：%s\n' "$app_path"
/usr/bin/printf '  下载包：%s\n' "$download_archive"
/usr/bin/printf '  更新包：%s\n' "$published_updater"
