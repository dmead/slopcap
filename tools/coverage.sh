#!/bin/bash
# Line coverage of src/ by the whole test suite: the linux-coverage preset (gcov) built and tested in the Docker build
# image (Xvfb + Mesa for the GUI and GPU tests), then tools/coverage_report.py.
#   MSYS_NO_PATHCONV=1 tools/coverage.sh [MIN_PERCENT]      (on Windows: from Git Bash)
# Report: build/linux-coverage/coverage.txt and coverage.json. Exits 1 below MIN_PERCENT (default 0).
set -euo pipefail

REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
MIN=${1:-0}
B=build/linux-coverage
# counts add up across runs: start from zero
[ -d "$REPO/$B" ] && find "$REPO/$B" -name '*.gcda' -delete
SLOPCAP_REGISTRY=${SLOPCAP_REGISTRY-} "$REPO/docker/build.sh" linux-coverage
TAG=$(sha256sum < "$REPO/docker/Dockerfile" | cut -c1-12)
docker run --rm --user "$(id -u):$(id -g)" -v "$REPO:$REPO" -w "$REPO" "slopcap-build:$TAG" \
    bash -c "python3 tools/coverage_report.py $B --min $MIN --json $B/coverage.json | tee $B/coverage.txt; exit \${PIPESTATUS[0]}"
