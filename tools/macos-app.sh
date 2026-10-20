#!/bin/bash
# macOS: build SlopCap.app and make it self-contained (the Qt frameworks and plugins copied inside, so it runs on a Mac
# without Homebrew), signed ad hoc so Apple silicon will run it.
#   tools/macos-app.sh          # build/macos-release/SlopCap.app
#   tools/macos-app.sh --dmg    # ... and build/macos-release/SlopCap.dmg to copy to other Macs
# Needs: brew install qtbase cmake ninja (Xcode's command-line tools for the compiler).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"
APP=build/macos-release/SlopCap.app

cmake --preset macos-release >/dev/null
cmake --build --preset macos-release --target slopcap

DEPLOY=${MACDEPLOYQT:-$(command -v macdeployqt || echo "$(brew --prefix qtbase 2>/dev/null)/bin/macdeployqt")}
[ -x "$DEPLOY" ] || { echo "macdeployqt not found (brew install qtbase, or set MACDEPLOYQT)" >&2; exit 2; }
"$DEPLOY" "$APP" -always-overwrite
# macdeployqt rewrites the binaries' load paths, which breaks their signatures; Apple silicon won't run unsigned code.
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"

# Self-contained means nothing may still load from the build machine's Homebrew.
leaks=$(find "$APP/Contents" -type f -perm -u+x -exec otool -L {} + 2>/dev/null | grep -E '^\s+(/opt/homebrew|/usr/local)/' || true)
if [ -n "$leaks" ]; then
    echo "SlopCap.app still loads libraries from outside the bundle:" >&2
    echo "$leaks" | sort -u >&2
    exit 1
fi
echo "$APP: $(du -sh "$APP" | cut -f1), self-contained"

if [ "${1:-}" = "--dmg" ]; then
    # The image holds SlopCap.app and a link to /Applications to drag it onto.
    DMG=build/macos-release/SlopCap.dmg
    STAGE=build/macos-release/dmg
    rm -rf "$STAGE" "$DMG"
    mkdir -p "$STAGE"
    ditto "$APP" "$STAGE/SlopCap.app"
    ln -s /Applications "$STAGE/Applications"
    if diskutil image create from --help >/dev/null 2>&1; then   # macOS 26+, where hdiutil create is deprecated
        diskutil image create from --format UDZO --volumeName SlopCap "$STAGE" "$DMG" >/dev/null 2>&1 ||
            { echo "diskutil image create failed" >&2; exit 1; }
    else
        hdiutil create -volname SlopCap -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
    fi
    rm -rf "$STAGE"
    echo "$DMG: $(du -sh "$DMG" | cut -f1)"
fi
