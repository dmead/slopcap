#!/bin/bash
# macOS: fetch the ZWO, QHY, Player One, ToupTek and MallinCam camera SDKs as INDI ships them (indilib/indi-3rdparty, which also feeds the Linux
# packages), checking every file against the git hash GitHub lists for the tag. Then configure with
# -DSLOPCAP_MAC_SDK_DIR=DEST (the macos presets default to build/mac-sdk).
#   tools/macos-sdks.sh [DEST]          DEST defaults to build/mac-sdk
# QHY firmware isn't needed: on macOS libqhyccd carries it and SlopCap uploads it (OSXInitQHYCCDFirmwareArray).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
DEST=${1:-$REPO/build/mac-sdk}
TAG=v2.2.5   # ZWO ASI SDK 1.41, QHY SDK 26.7.21, Player One 3.10.1, ToupTek and MallinCam 60.32226
case "$(uname -m)" in
    arm64) ARCH=mac_arm64 ;;
    *) ARCH=mac_x64 ;;
esac

# GitHub's API allows few anonymous requests from a shared address (CI runners); use a token when there is one.
auth=()
[ -n "${GITHUB_TOKEN:-}" ] && auth=(-H "Authorization: Bearer $GITHUB_TOKEN")

fetch() {   # fetch PATH: download one file from the tag and check its git blob hash
    local path=$1 out=$DEST/$1 dir want got
    dir=$(dirname "$path")
    mkdir -p "$(dirname "$out")"
    curl -fsSL -o "$out" "https://raw.githubusercontent.com/indilib/indi-3rdparty/$TAG/$path"
    want=$(curl -fsSL ${auth[@]+"${auth[@]}"} "https://api.github.com/repos/indilib/indi-3rdparty/contents/$dir?ref=$TAG" |
           python3 -c "import json,sys; print(next(x['sha'] for x in json.load(sys.stdin) if x['name'] == sys.argv[1]))" "$(basename "$path")")
    got=$(git hash-object --no-filters "$out")
    if [ "$want" != "$got" ]; then
        echo "$path: hash mismatch (GitHub lists $want, got $got)" >&2
        rm -f "$out"
        exit 1
    fi
    echo "ok  $path ($(wc -c < "$out" | tr -d ' ') bytes)"
}

fetch libasi/ASICamera2.h
fetch libasi/license.txt
fetch libasi/$ARCH/libASICamera2.bin
for h in qhyccd.h qhyccdcamdef.h qhyccderr.h qhyccdstruct.h; do fetch libqhy/$h; done
fetch libqhy/$ARCH/libqhyccd.bin
# universal binaries, one per SDK (not yet tried on a Mac)
fetch libplayerone/PlayerOneCamera.h
fetch libplayerone/license.txt
fetch libplayerone/mac/libPlayerOneCamera.bin
fetch libtoupcam/toupcam.h
fetch libtoupcam/COPYING.LGPL
fetch libtoupcam/mac/libtoupcam.bin
fetch libmallincam/mallincam.h
fetch libmallincam/COPYING.LGPL
fetch libmallincam/mac/libmallincam.bin
echo "SDKs in $DEST (indi-3rdparty $TAG). ZWO's and QHY's load libusb: brew install libusb."
