#!/bin/zsh
set -eu
repo_dir="${0:A:h:h:h}"
render_dir=$(mktemp -d /private/tmp/hud-readme-render.XXXXXX)
render_app="$render_dir/ReadmeRenderer.app"
mkdir -p "$render_app/Contents/MacOS" "$render_app/Contents/Resources" "$repo_dir/docs/images"
cp "$repo_dir/overlay/HUDLocalizations.json" "$render_app/Contents/Resources/"
xcrun clang -O2 -fobjc-arc -mmacosx-version-min=15.0 -I "$repo_dir/overlay" \
  -framework Cocoa -framework ImageIO \
  "$repo_dir/docs/tools/render-readme.m" "$repo_dir/overlay/HUDView.m" \
  "$repo_dir/overlay/HUDLocalization.m" "$repo_dir/overlay/NativeSampler.m" \
  -o "$render_app/Contents/MacOS/ReadmeRenderer"
for language in zh-Hans zh-Hant en ja ko; do
  "$render_app/Contents/MacOS/ReadmeRenderer" "$language" "$repo_dir/docs/images"
done
