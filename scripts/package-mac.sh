#!/bin/sh
# Packages a built Sermon Polish.app as a dmg and a zip in release/.
#   scripts/package-mac.sh [target-triple]      (default: this Mac's, i.e. the plain `tauri build` output)
set -e
cd "$(dirname "$0")/.."
TRIPLE="$1"
if [ -n "$TRIPLE" ]; then APP="src-tauri/target/$TRIPLE/release/bundle/macos/Sermon Polish.app"; else APP="src-tauri/target/release/bundle/macos/Sermon Polish.app"; fi
case "$TRIPLE" in x86_64*) LABEL="Intel";; "") LABEL="$(uname -m | sed 's/arm64/Apple silicon/; s/x86_64/Intel/')";; *) LABEL="Apple silicon";; esac
VERSION="$(sed -n 's/.*"version": "\(.*\)".*/\1/p' src-tauri/tauri.conf.json | head -1)"
NAME="Sermon Polish $VERSION ($LABEL)"
STAGE="$(mktemp -d)"
mkdir -p release
cp -R "$APP" "$STAGE/" && ln -s /Applications "$STAGE/Applications"
rm -f "release/$NAME.dmg" "release/$NAME.zip"
hdiutil create -quiet -volname "Sermon Polish" -srcfolder "$STAGE" -ov -format UDZO "release/$NAME.dmg"
ditto -c -k --keepParent "$APP" "release/$NAME.zip"
rm -rf "$STAGE"
echo "release/$NAME.dmg and .zip"
