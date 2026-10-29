#!/bin/bash
# Run the Windows test programs (docker/build.sh windows-tests) on Windows, from Git Bash:
#   tools/windows-tests.sh [BUILD_DIR]        default build/windows-tests; logs in BUILD_DIR/test-logs
# The GL and application tests open windows for a few minutes and need this desktop session's GPU. The INDI tests run
# INDI's simulators in WSL: SLOPCAP_INDI_WSL names a distro with indi-bin installed (there is no indiserver for
# Windows). A program that skips everything it holds counts as a failure: that is how a missing screen, OpenGL or INDI
# shows; SLOPCAP_ALLOW_SKIP lists programs exempt from that (CI runners with no GPU or WSL). Exits 1 if anything failed.
set -uo pipefail
case $(uname -s) in MINGW* | MSYS* | CYGWIN*) ;; *) echo "windows-tests.sh: run it on Windows (Git Bash)" >&2; exit 2 ;; esac
dir=${1:-build/windows-tests}
logs=$dir/test-logs
mkdir -p "$logs"
failed=0
for t in slopcap-tests slopcap-gl-tests slopcap-net-tests slopcap-mount-tests slopcap-qt-tests slopcap-app-tests; do
    if [ ! -f "$dir/$t.exe" ]; then
        printf '  %-20s MISSING\n' "$t"
        failed=1
        continue
    fi
    t0=$SECONDS
    "$dir/$t.exe" --colour-mode none > "$logs/$t.log" 2>&1
    rc=$?
    summary=$(grep -E '^(All tests passed|test cases:)' "$logs/$t.log" | tail -1)
    case $rc in
        0) result=pass ;;
        4) result="all skipped"
           case " ${SLOPCAP_ALLOW_SKIP:-} " in *" $t "*) result="all skipped (allowed)" ;; *) failed=1 ;; esac
           [ $t = slopcap-qt-tests ] && [ -z "${SLOPCAP_INDI_WSL:-}" ] && summary="(set SLOPCAP_INDI_WSL to a WSL distro with indi-bin)" ;;
        *) result="FAIL (exit $rc)"; failed=1 ;;
    esac
    printf '  %-20s %-16s %4d s  %s\n' "$t" "$result" $((SECONDS - t0)) "$summary"
done
[ $failed = 0 ] || echo "logs: $logs"
exit $failed
