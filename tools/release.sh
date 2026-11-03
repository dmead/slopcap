#!/bin/bash
# Publish a GitHub release of a tag from its local CI run (tools/ci-local.sh --windows-smoke TAG, every job passed):
#   tools/release.sh [--update] [--dry-run] TAG NOTES.md
# Only that run's zips go up, unchanged (their hashes are in the run's summary), and each must hold the README, the
# LICENSE and licenses/: no bare exe, nothing else. A Mac zip made from the same commit after its tests passed on a Mac
# goes up too when it was added to the run (a "  macos  pass" line and its "  package:" line in summary.txt; see
# docs/developing.md). The tag must already be on GitHub (the `github` remote). With .git/leak-patterns present (the
# pre-push guard's), the zips' contents are searched for those too. On Windows run it from Git Bash with
# MSYS_NO_PATHCONV=1.
#
# GitHub lists a release's files below its notes, so the notes must start with a "## Downloads" table that names each
# zip in its first column, in backticks (`slopcap-linux-x64.zip`). Each zip that goes up gets its download link and
# size there, and their SHA-256s follow the table. A row for a zip that isn't up yet is left as written.
#   --update   the release exists already: upload the run's zips it doesn't have yet (a Mac zip added later), check
#              that the ones it has are the run's (by GitHub's SHA-256), and replace its notes
#   --dry-run  do every check and print the notes, but change nothing on GitHub
set -euo pipefail

update=; dry=
while [ $# -gt 0 ]; do
    case $1 in
        --update) update=1; shift ;;
        --dry-run) dry=1; shift ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
usage="usage: tools/release.sh [--update] [--dry-run] TAG NOTES.md"
REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
TAG=${1:?$usage}
NOTES=${2:?$usage}
native() { if command -v cygpath >/dev/null; then cygpath -m "$1"; else echo "$1"; fi; }
git() { command git -C "$(native "$REPO")" "$@"; }
py=$(command -v python || command -v python3)   # (on Windows, python3 can be the Store's stand-in)
GH_REPO=$(git remote get-url github | sed -E 's#^.*github\.com[:/]##; s#\.git$##')

sha=$(git rev-parse --verify --quiet "refs/tags/$TAG^{commit}") || { echo "no tag $TAG" >&2; exit 2; }
git ls-remote --tags github | grep -E "^$sha[[:space:]]+refs/tags/$TAG(\^\{\})?\$" >/dev/null ||
    { echo "$TAG isn't on GitHub at ${sha:0:7}: push it first" >&2; exit 2; }
if gh release view "$TAG" --repo "$GH_REPO" >/dev/null 2>&1; then
    [ -n "$update" ] || { echo "release $TAG exists already (--update adds files to it and replaces its notes)" >&2; exit 2; }
else
    [ -z "$update" ] || { echo "no release $TAG to update" >&2; exit 2; }
fi

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

# --update: the release's files by name, with GitHub's SHA-256 and size
declare -A have=() haveSize=()
if [ -n "$update" ]; then
    while read -r name digest size; do have[$name]=${digest#sha256:}; haveSize[$name]=$size; done \
        < <(gh api "repos/$GH_REPO/releases/tags/$TAG" --jq '.assets[] | "\(.name) \(.digest) \(.size)"')
fi

names=(); sizes=(); hashes=(); up=()
while read -r zip hash; do
    name=$(basename "$zip")
    if [ -n "${have[$name]+x}" ]; then   # up already, checked when it went up: only the same file may stay
        [ "${have[$name]}" = "$hash" ] ||
            { echo "$name on the release (SHA-256 ${have[$name]:0:12}) isn't the one $run made (${hash:0:12})" >&2; exit 1; }
        names+=("$name"); sizes+=("${haveSize[$name]}"); hashes+=("$hash")
        continue
    fi
    [ -f "$zip" ] || { echo "$zip is gone (a later CI run replaced it): run CI on $TAG again" >&2; exit 1; }
    [ "$(sha256sum < "$zip" | cut -c1-64)" = "$hash" ] || { echo "$zip isn't the one $run made: run CI on $TAG again" >&2; exit 1; }
    case $(basename "$zip") in
        slopcap-macos-*) grep -qE '^  macos +pass' "$run/summary.txt" || { echo "$zip: no passing macos line in the run" >&2; exit 1; } ;;
    esac
    "$py" - "$(native "$zip")" <<'CHECK' || exit 1
import sys, zipfile
path = sys.argv[1]
names = set(zipfile.ZipFile(path).namelist())
top = path.replace("\\", "/").rsplit("/", 1)[-1][:-4]
missing = [f for f in ("README.md", "LICENSE", "licenses/README.md") if f"{top}/{f}" not in names]
if missing:
    sys.exit(f"{path}: no {', '.join(missing)} in {top}/")
CHECK
    names+=("$name"); sizes+=("$(wc -c < "$zip" | tr -d ' ')"); hashes+=("$hash"); up+=("$zip")
done < <(sed -nE 's/^  package: (.*\.zip)  sha256 ([0-9a-f]{64})$/\1 \2/p' "$run/summary.txt")
[ ${#names[@]} -ge 2 ] || { echo "$run: expected the Linux and Windows zips, found ${#names[@]}" >&2; exit 1; }
for name in "${!have[@]}"; do
    [[ " ${names[*]} " == *" $name "* ]] || { echo "$name is on the release but not in $run" >&2; exit 1; }
done

patterns=$(git rev-parse --git-common-dir)/leak-patterns
case $patterns in /* | ?:/*) ;; *) patterns=$REPO/$patterns ;; esac
if [ -f "$patterns" ] && [ ${#up[@]} -gt 0 ]; then
    tmp=$(mktemp -d)
    grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$patterns" > "$tmp/rules"   # as the pre-push hook reads them
    for z in "${up[@]}"; do "$py" -m zipfile -e "$(native "$z")" "$(native "$tmp/files")"; done
    if grep -rlaiEf "$tmp/rules" "$tmp/files"; then echo "the files above match .git/leak-patterns" >&2; rm -rf "$tmp"; exit 1; fi
    rm -rf "$tmp"
    echo "leak patterns: none in the zips"
fi

body=$(mktemp)
args=(); for i in "${!names[@]}"; do args+=("${names[$i]}" "${sizes[$i]}" "${hashes[$i]}"); done
"$py" - "$(native "$NOTES")" "$(native "$body")" "https://github.com/$GH_REPO/releases/download/$TAG" "${args[@]}" \
    <<'RENDER' || exit 1
import sys
notes, out, base = sys.argv[1:4]
rest = sys.argv[4:]
zips = [(rest[i], int(rest[i + 1]), rest[i + 2]) for i in range(0, len(rest), 3)]
lines = open(notes, encoding="utf-8-sig").read().splitlines()
start = next((i for i, l in enumerate(lines) if l.strip()), None)
if start is None or lines[start].strip() != "## Downloads":
    sys.exit(f"{notes}: the notes must start with a '## Downloads' table (GitHub lists the files below the notes)")
i = start + 1
while i < len(lines) and not lines[i].lstrip().startswith(("|", "#")):
    i += 1
rows = []
while i < len(lines) and lines[i].lstrip().startswith("|"):
    rows.append(i)
    i += 1
if len(rows) < 3:
    sys.exit(f"{notes}: no table under '## Downloads'")
for name, size, _ in zips:
    cell = f"`{name}`"
    hit = [r for r in rows[2:] if lines[r].strip().split("|")[1].strip() == cell]
    if not hit:
        sys.exit(f"{notes}: no row for {cell} in the Downloads table")
    for r in hit:
        lines[r] = lines[r].replace(cell, f"[{name}]({base}/{name}) ({size / 2**20:.1f}&nbsp;MB)", 1)   # (no line break in the size)
sums = ["", "<details><summary>SHA-256</summary>", "", "```", *(f"{h}  {name}" for name, _, h in zips), "```", "",
        "</details>"]
lines[rows[-1] + 1:rows[-1] + 1] = sums
with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines).rstrip("\n") + "\n")
RENDER

if [ -n "$dry" ]; then
    cat "$body"; rm -f "$body"; echo
    if [ -n "$update" ]; then
        echo "dry run: would upload ${#up[@]} file(s)$(for z in "${up[@]}"; do printf ' %s' "$(basename "$z")"; done) and replace the notes of $TAG"
    else
        echo "dry run: would create $TAG with$(for z in "${up[@]}"; do printf ' %s' "$(basename "$z")"; done)"
    fi
    exit 0
fi
files=(); for z in "${up[@]}"; do files+=("$(native "$z")"); done
if [ -n "$update" ]; then
    [ ${#files[@]} -eq 0 ] || gh release upload "$TAG" "${files[@]}" --repo "$GH_REPO"
    gh release edit "$TAG" --repo "$GH_REPO" --notes-file "$(native "$body")"
else
    gh release create "$TAG" "${files[@]}" --repo "$GH_REPO" --verify-tag --title "SlopCap ${TAG#v}" --notes-file "$(native "$body")"
fi
rm -f "$body"
