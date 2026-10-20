#!/bin/bash
# End-to-end test of remote viewing: three SlopCaps in separate containers on one Docker network, standing in for the
# observatory NUC (server, a replayed recording as its camera), the desktop (client) and a projector laptop (client in
# full view). Checks discovery, the stream, control changes in both directions and recording on the server only.
#   MSYS_NO_PATHCONV=1 tools/remote-test.sh FILE.ser [BUILD_DIR]     (BUILD_DIR defaults to build/linux-debug)
# Logs and screenshots go to build/remote-test/.
set -euo pipefail

REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
SER=$(readlink -f "$1")
BUILD=${2:-build/linux-debug}
OUT=$REPO/build/remote-test
NET=slopcap-remote-test
IMAGE=slopcap-build:$(sha256sum < "$REPO/docker/Dockerfile" | cut -c1-12)
rm -rf "$OUT"
mkdir -p "$OUT/server-rec" "$OUT/client-rec"
docker network inspect $NET >/dev/null 2>&1 || docker network create $NET >/dev/null

xvfb='Xvfb :99 -screen 0 1600x1000x24 -nolisten tcp >/dev/null 2>&1 & export DISPLAY=:99; for i in $(seq 50); do [ -e /tmp/.X11-unix/X99 ] && break; sleep 0.1; done'
run() {   # run NAME ARGS...: one SlopCap in its own container, named and hostnamed NAME
    local name=$1
    shift
    docker rm -f "$name" >/dev/null 2>&1 || true
    docker run --rm --name "$name" --hostname "$name" --network $NET --user "$(id -u):$(id -g)" -v "$REPO:$REPO" -w "$REPO" \
        -v "$(dirname "$SER"):/data:ro" "$IMAGE" bash -c "$xvfb; exec $BUILD/slopcap $*" >"$OUT/$name.log" 2>&1
}

# observatory: shares the camera; changes the exposure itself at 16 s
run observatory --camera "sim:/data/$(basename "$SER")" --serve --out "$OUT/server-rec" --window 1500x950 \
    --set Exposure=5000@16 --snapshot "22:$OUT/observatory-live.png" --quit-after 34 --screenshot "$OUT/observatory.png" &
sleep 3
# desktop: finds it, sets the gain at 8 s, records on the server from 20 to 26 s
run desktop --connect auto --out "$OUT/client-rec" --window 1500x950 --set Gain=321@8 --set Record=1@20 --set Record=0@26 \
    --snapshot "22:$OUT/desktop-recording.png" --quit-after 28 --screenshot "$OUT/desktop.png" &
# projector: connects by name, full view
run projector --connect observatory --fullscreen --snapshot "2.5:$OUT/projector-bar.png" --quit-after 24 --screenshot "$OUT/projector.png" &
wait

pass=0 fail=0
check() {   # check DESCRIPTION FILE PATTERN
    if grep -qE -- "$3" "$OUT/$2"; then echo "PASS  $1"; pass=$((pass + 1)); else echo "FAIL  $1"; fail=$((fail + 1)); fi
}
summary() { grep -E "Previewing|Watching|Recording|Stopped|No camera|⇄" "$OUT/$1.log" | grep -v "^\[" | tail -1; }
echo "observatory: $(summary observatory)"
echo "desktop:     $(summary desktop)"
echo "projector:   $(summary projector)"
check "desktop found the observatory by discovery" desktop.log "remote: connected to observatory"
check "projector connected by name" projector.log "remote: connected to observatory"
check "the observatory saw two viewers" observatory.log "Watching: .*(desktop.*projector|projector.*desktop)"
check "desktop's gain reached the observatory" observatory.log "control: Gain = 321 \(from a viewer\)"
check "desktop's gain reached the projector" projector.log "control: Gain = 321 \(set on observatory\)"
check "the observatory's exposure reached the desktop" desktop.log "control: Exposure = 5000 \(set on observatory\)"
check "desktop ends with both values" desktop.log "Gain=321 · Exposure=5000"
check "observatory ends with both values" observatory.log "Gain=321 · Exposure=5000"
check "desktop shows the link rate" desktop.log "[0-9.]+ Mbps · [0-9]+ fps"
check "desktop shows the LIVE badge" desktop.log "badge: ⇄ LIVE FROM OBSERVATORY"
check "the observatory shows the shared badge" observatory.log "badge: ⇄ SHARED WITH (DESKTOP, PROJECTOR|PROJECTOR, DESKTOP)"
check "the observatory reports the link rate" observatory.log "⇄ [0-9.]+ Mbps to 2 viewers"
check "desktop shows the server's recording" desktop.log "● Recording on observatory"
check "recording started on the observatory when asked" observatory.log "record: start asked by a viewer"
check "the projector went full screen" projector.log "view: full screen"
if ls "$OUT"/server-rec/*.ser >/dev/null 2>&1; then echo "PASS  the raw file is on the observatory: $(ls "$OUT"/server-rec/*.ser | xargs -n1 basename)"; pass=$((pass + 1)); else echo "FAIL  no recording on the observatory"; fail=$((fail + 1)); fi
if ls "$OUT"/client-rec/*.ser >/dev/null 2>&1; then echo "FAIL  the desktop wrote a recording"; fail=$((fail + 1)); else echo "PASS  nothing recorded on the desktop"; pass=$((pass + 1)); fi
echo "$pass passed, $fail failed · logs and screenshots in $OUT"
[ $fail -eq 0 ]
