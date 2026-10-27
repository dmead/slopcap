#!/bin/bash
# Publish a GitHub release of a tag from its local CI run (tools/ci-local.sh --windows-smoke TAG, every job passed):
#   tools/release.sh TAG NOTES.md
# Only that run's zips go up, unchanged (their hashes are in the run's summary), and each must hold the README, the
# LICENSE and licenses/: no bare exe, nothing else. The SHA-256 of each is added to the notes. The tag must already be
# on GitHub (the `github` remote), and the release must not exist yet. With .git/leak-patterns present (the pre-push
# guard's), the zips' contents are searched for those too. On Windows run it from Git Bash with MSYS_NO_PATHCONV=1.
set -euo pipefail

REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
TAG=${1:?usage: tools/release.sh TAG NOTES.md}
NOTES=${2:?usage: tools/release.sh TAG NOTES.md}
native() { if command -v cygpath >/dev/null; then cygpath -m "$1"; else echo "$1"; fi; }
git() { command git -C "$(native "$REPO")" "$@"; }
py=$(command -v python || command -v python3)   # (on Windows, python3 can be the Store's stand-in)
GH_REPO=$(git remote get-url github | sed -E 's#^.*github\.com[:/]##; s#\.git$##')

sha=$(git rev-parse --verify --quiet "refs/tags/$TAG^{commit}") || { echo "no tag $TAG" >&2; exit 2; }
git ls-remote --tags github | grep -E "^$sha[[:space:]]+refs/tags/$TAG(\^\{\})?\$" >/dev/null ||
    { echo "$TAG isn't on GitHub at ${sha:0:7}: push it first" >&2; exit 2; }
if gh release view "$TAG" --repo "$GH_REPO" >/dev/null 2>&1; then echo "release $TAG exists already" >&2; exit 2; fi

# the newest run of that commit in which every job passed, the Windows smoke test included
run=
for d in $(ls -d "$REPO"/build/ci/runs/*-"${sha:0:7}" 2>/dev/null | sort -r); do
    s=$d/summary.txt
    if grep -q '^all jobs passed' "$s" && grep -qE '^  windows-smoke +pass' "$s"; then run=$d; break; fi
done
[ -n "$run" ] || { echo "no passing local CI run of $TAG (${sha:0:7}) with --windows-smoke in build/ci/runs" >&2; exit 1; }
for j in linux-debug linux-asan linux-tsan linux-release coverage windows-exe; do
    grep -qE "^  $j +pass" "$run/summary.txt" || { echo "$run: job $j didn't run or didn't pass" >&2; exit 1; }
done
echo "CI run: $run"

zips=()
while read -r zip hash; do
    [ -f "$zip" ] || { echo "$zip is gone (a later CI run replaced it): run CI on $TAG again" >&2; exit 1; }
    [ "$(sha256sum < "$zip" | cut -c1-64)" = "$hash" ] || { echo "$zip isn't the one $run made: run CI on $TAG again" >&2; exit 1; }
    "$py" - "$(native "$zip")" <<'CHECK' || exit 1
import sys, zipfile
path = sys.argv[1]
names = set(zipfile.ZipFile(path).namelist())
top = path.replace("\\", "/").rsplit("/", 1)[-1][:-4]
missing = [f for f in ("README.md", "LICENSE", "licenses/README.md") if f"{top}/{f}" not in names]
if missing:
    sys.exit(f"{path}: no {', '.join(missing)} in {top}/")
CHECK
    zips+=("$zip")
done < <(sed -nE 's/^  package: (.*\.zip)  sha256 ([0-9a-f]{64})$/\1 \2/p' "$run/summary.txt")
[ ${#zips[@]} -ge 2 ] || { echo "$run: expected the Linux and Windows zips, found ${#zips[@]}" >&2; exit 1; }

patterns=$(git rev-parse --git-common-dir)/leak-patterns
case $patterns in /* | ?:/*) ;; *) patterns=$REPO/$patterns ;; esac
if [ -f "$patterns" ]; then
    tmp=$(mktemp -d)
    grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$patterns" > "$tmp/rules"   # as the pre-push hook reads them
    for z in "${zips[@]}"; do "$py" -m zipfile -e "$(native "$z")" "$(native "$tmp/files")"; done
    if grep -rlaiEf "$tmp/rules" "$tmp/files"; then echo "the files above match .git/leak-patterns" >&2; rm -rf "$tmp"; exit 1; fi
    rm -rf "$tmp"
    echo "leak patterns: none in the zips"
fi

body=$(mktemp)
{ cat "$NOTES"; printf '\n## SHA-256\n\n```\n'
  for z in "${zips[@]}"; do echo "$(sha256sum < "$z" | cut -c1-64)  $(basename "$z")"; done
  printf '```\n'; } > "$body"
files=(); for z in "${zips[@]}"; do files+=("$(native "$z")"); done
gh release create "$TAG" "${files[@]}" --repo "$GH_REPO" --verify-tag --title "SlopCap ${TAG#v}" --notes-file "$(native "$body")"
rm -f "$body"
