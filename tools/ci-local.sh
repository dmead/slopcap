#!/bin/bash
# The jobs of .github/workflows/ci.yml on this machine, in Docker, instead of on GitHub Actions (which costs minutes on
# a private repository). The commit is checked out into its own worktree, build/ci/src, whose build folders are kept
# between runs (later runs are incremental), and every job runs there in turn, also after one has failed.
#   tools/ci-local.sh [--jobs "JOB ..."] [--windows-smoke] [--upload DEST] [REF]
#     REF                  the commit to test (default HEAD; uncommitted changes are not tested)
#     --jobs               a subset of: linux-debug linux-asan linux-tsan linux-release coverage windows-exe
#     --windows-smoke      also start the Windows exe on this desktop (Windows only; a window opens for about 10 s)
#     --upload DEST        when every job passed, copy the zips and summary.txt to DEST/<time>-<commit>/ with rclone
#                          (DEST is an rclone remote and folder, e.g. drive:slopcap-builds; no share links are made)
# Machine-local settings, not in git, go in build/ci/settings.sh (read first): SLOPCAP_CI_UPLOAD (the default for
# --upload), RCLONE (the rclone program when it isn't on PATH) and an exported RCLONE_CONFIG (rclone's own setting).
# On Windows run it from Git Bash with MSYS_NO_PATHCONV=1. Logs and summary.txt: build/ci/runs/<time>-<commit>/.
# Exits 1 when a job or the upload failed. Not here: the macos job (it needs a Mac: the "Natively" steps in
# docs/developing.md). The Linux zip is started in a bare Ubuntu, as CI does, when an ubuntu:26.04 image is present
# (docker pull ubuntu:26.04).
set -uo pipefail

REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
CI=${SLOPCAP_CI_DIR:-$REPO/build/ci}
WT=$CI/src
# shellcheck source=/dev/null
[ -f "$CI/settings.sh" ] && . "$CI/settings.sh"
JOBS="linux-debug linux-asan linux-tsan linux-release coverage windows-exe"
SMOKE=0
UPLOAD=${SLOPCAP_CI_UPLOAD:-}
REF=HEAD
while [ $# -gt 0 ]; do
    case $1 in
        --jobs) JOBS=$2; shift 2 ;;
        --windows-smoke) SMOKE=1; shift ;;
        --upload) UPLOAD=$2; shift 2 ;;
        -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0 ;;
        *) REF=$1; shift ;;
    esac
done
# Git for Windows wants D:/… paths, and with MSYS_NO_PATHCONV=1 (which docker/build.sh needs) nothing converts them
native() { if command -v cygpath >/dev/null; then cygpath -m "$1"; else echo "$1"; fi; }
SHA=$(git -C "$(native "$REPO")" rev-parse --verify --quiet "$REF^{commit}") || { echo "not a commit: $REF" >&2; exit 2; }
docker info >/dev/null 2>&1 || { echo "Docker isn't answering (is Docker Desktop running?)" >&2; exit 2; }

# the worktree: made once, then moved to the commit; untracked files go, the build folders stay
if [ ! -e "$WT/.git" ]; then
    mkdir -p "$CI"
    git -C "$(native "$REPO")" worktree add -f --detach "$(native "$WT")" "$SHA" >/dev/null || exit 2
else
    git -C "$(native "$WT")" checkout -q --detach --force "$SHA" &&
        git -C "$(native "$WT")" clean -q -ffdx -e /build/ || exit 2
fi
rm -rf "$WT/build/package"   # only this run's zips

RUN=$CI/runs/$(date +%Y%m%d-%H%M%S)-${SHA:0:7}
mkdir -p "$RUN"
SUMMARY=$RUN/summary.txt
echo "SlopCap local CI: $(git -C "$(native "$WT")" log -1 --format='%h %s')  ($(date '+%F %T'))" | tee "$SUMMARY"
failed=0

job() {   # job NAME COMMAND...: run in the worktree, log to RUN/NAME.log, one line in the summary
    local name=$1 t0=$SECONDS rc result
    shift
    (cd "$WT" && "$@") > "$RUN/$name.log" 2>&1
    rc=$?
    result=pass
    [ $rc -eq 0 ] || { result="FAIL (exit $rc)"; failed=1; }
    [ "$name" = coverage ] && [ $rc -eq 0 ] && coverage_ok=1
    printf '  %-15s %-15s %5d s\n' "$name" "$result" $((SECONDS - t0)) | tee -a "$SUMMARY"
}
coverage_ok=0

linux_release() {   # build, pack, and start the zip in a bare Ubuntu (no Qt, only what a desktop has)
    docker/build.sh linux-release && tools/package.sh linux || return 1
    if ! docker image inspect ubuntu:26.04 >/dev/null 2>&1; then
        echo "no ubuntu:26.04 image: the start in a bare Ubuntu was skipped (docker pull ubuntu:26.04 to include it)"
        return 0
    fi
    docker run --rm -v "$PWD:/src:ro" ubuntu:26.04 bash -euc '
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends python3 libgl1 libegl1 libopengl0 libfontconfig1 libharfbuzz0b \
            libdbus-1-3 libglib2.0-0t64 libx11-xcb1 libxcb1 libudev1 >/dev/null
        cd /tmp && python3 -m zipfile -e /src/build/package/slopcap-linux-x64.zip . &&
            chmod +x slopcap-linux-x64/slopcap slopcap-linux-x64/bin/slopcap
        python3 /src/tools/package/make-test-ser.py test.ser
        QT_QPA_PLATFORM=offscreen slopcap-linux-x64/slopcap --settings /tmp/settings --camera sim:test.ser --record \
            --out /tmp/rec --quit-after 4 --screenshot /tmp/shot.png
        ls -l /tmp/rec/*.ser /tmp/shot.png'
}

windows_exe() {
    docker/build.sh windows-static && tools/package.sh windows
}

windows_smoke() {   # as CI's windows-smoke: replay a generated recording, record it, quit; check the log and the file
    case $(uname -s) in MINGW* | MSYS* | CYGWIN*) ;; *) echo "not on Windows: skipped"; return 0 ;; esac
    local tmp log py
    py=$(command -v python || command -v python3)
    tmp=$(mktemp -d)
    "$py" tools/package/make-test-ser.py "$tmp/test.ser" || return 1
    QT_LOGGING_RULES="qt.qpa.gl=true" build/windows-static/slopcap.exe --settings "$(cygpath -w "$tmp/settings")" \
        --camera "sim:$(cygpath -w "$tmp/test.ser")" --record --out "$(cygpath -w "$tmp/rec")" --quit-after 8 ||
        { echo "slopcap.exe exited with $?"; return 1; }
    log=$(ls -t "$(cygpath "$LOCALAPPDATA")"/SlopCap/SlopCap/logs/*.log | head -1)
    cat "$log"
    grep -q "record: start" "$log" || { echo "the recording didn't start"; return 1; }
    ls -l "$tmp"/rec/*.ser || { echo "no recording written"; return 1; }
    rm -rf "$tmp"
}

for j in $JOBS; do
    case $j in
        linux-debug | linux-asan | linux-tsan) job "$j" docker/build.sh "$j" ;;
        linux-release) job "$j" linux_release ;;
        coverage) job "$j" tools/coverage.sh 90 ;;
        windows-exe) job "$j" windows_exe ;;
        *) echo "  unknown job: $j" | tee -a "$SUMMARY"; failed=1 ;;
    esac
done
[ $SMOKE = 1 ] && job windows-smoke windows_smoke

[ $coverage_ok = 1 ] &&
    echo "  coverage: $(grep '^TOTAL' "$WT/build/linux-coverage/coverage.txt" | awk '{print $NF}') of src/ lines" | tee -a "$SUMMARY"
for z in "$WT"/build/package/*.zip; do [ -f "$z" ] && echo "  package: $z" | tee -a "$SUMMARY"; done
echo "  logs: $RUN" | tee -a "$SUMMARY"
[ $failed = 0 ] && echo "all jobs passed" | tee -a "$SUMMARY" || echo "SOME JOBS FAILED" | tee -a "$SUMMARY"

if [ -n "$UPLOAD" ]; then
    if [ $failed != 0 ]; then
        echo "upload: skipped (a job failed)" | tee -a "$SUMMARY"
    else
        dest=$UPLOAD/$(basename "$RUN")
        up=done
        for f in "$WT"/build/package/*.zip "$SUMMARY"; do
            [ -f "$f" ] || continue
            "${RCLONE:-rclone}" copyto -q --no-traverse "$(native "$f")" "$dest/$(basename "$f")" || up=FAILED
        done
        echo "upload to $dest: $up" | tee -a "$SUMMARY"
        [ $up = done ] || failed=1
    fi
fi
exit $failed
