#!/usr/bin/env python3
"""Line coverage of SlopCap's own sources (src/) from a --coverage build's .gcda files, without gcovr or lcov: gcov's
JSON output, merged per source line across every executable that ran the code.
   tools/coverage_report.py BUILD_DIR [--min PERCENT] [--json OUT.json]
Run it in the same environment that built BUILD_DIR (gcov must match the compiler); tools/coverage.sh does both.
Exits 1 when the total is below --min."""
import argparse
import collections
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def gcov_json(gcda, objdir):
    out = subprocess.run(["gcov", "--json-format", "--stdout", "--object-directory", objdir, gcda],
                         cwd=objdir, capture_output=True, text=True)
    for doc in out.stdout.splitlines():
        doc = doc.strip()
        if doc.startswith("{"):
            yield json.loads(doc)


def live_objects(build):
    """The object files the build makes now (from build.ninja), or None without one. A .gcno whose object no target
    builds any more is left over from an earlier layout of the build and has stale line numbers."""
    path = os.path.join(build, "build.ninja")
    if not os.path.exists(path):
        return None
    objs = set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("build "):
                out = line[6:].split(":", 1)[0].split(" ")[0]
                if out.endswith(".o"):
                    objs.add(os.path.normpath(os.path.join(build, out)))
    return objs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("--min", type=float, default=0.0)
    ap.add_argument("--json")
    a = ap.parse_args()
    build = os.path.abspath(a.build)
    counts = collections.defaultdict(dict)   # source (repo-relative) -> {line: hits}
    # Every compiled object has a .gcno; only the ones that ran have a .gcda. Going by the .gcno files counts code no
    # test ever touched as 0% instead of leaving it out.
    notes = [os.path.join(d, f) for d, _, fs in os.walk(build) for f in fs if f.endswith(".gcno")]
    live = live_objects(build)
    if live is not None:
        notes = [g for g in notes if os.path.normpath(g[:-len(".gcno")] + ".o") in live]
    if not any(f.endswith(".gcda") for _, _, fs in os.walk(build) for f in fs):
        sys.exit(f"no .gcda files under {build}: build with the linux-coverage preset and run the tests first")
    for g in notes:
        for doc in gcov_json(g, os.path.dirname(g)):
            cwd = doc.get("current_working_directory", os.path.dirname(g))
            for f in doc["files"]:
                path = os.path.normpath(os.path.join(cwd, f["file"]))
                rel = os.path.relpath(path, REPO)
                if not rel.startswith("src" + os.sep):
                    continue
                c = counts[rel]
                for ln in f["lines"]:
                    n = ln["line_number"]
                    c[n] = c.get(n, 0) + ln["count"]
    files = []
    for rel, c in counts.items():
        total = len(c)
        if total == 0:
            continue
        hit = sum(1 for v in c.values() if v > 0)
        missed = sorted(n for n, v in c.items() if v == 0)
        files.append((rel, hit, total, missed))
    files.sort(key=lambda f: (f[2] - f[1], f[0]), reverse=True)
    hit = sum(f[1] for f in files)
    total = sum(f[2] for f in files)
    dirs = collections.defaultdict(lambda: [0, 0])
    for rel, h, t, _ in files:
        d = os.path.dirname(rel)
        dirs[d][0] += h
        dirs[d][1] += t
    print(f"{'file':42} {'lines':>6} {'hit':>6} {'cover':>7}  missed")
    for rel, h, t, missed in files:
        ranges, start, prev = [], None, None
        for n in missed:
            if start is None:
                start = prev = n
            elif n == prev + 1:
                prev = n
            else:
                ranges.append(f"{start}" if start == prev else f"{start}-{prev}")
                start = prev = n
        if start is not None:
            ranges.append(f"{start}" if start == prev else f"{start}-{prev}")
        shown = ",".join(ranges)
        print(f"{rel:42} {t:6} {h:6} {100 * h / t:6.1f}%  {shown[:90] + ('…' if len(shown) > 90 else '')}")
    print()
    for d, (h, t) in sorted(dirs.items()):
        print(f"{d + '/':42} {t:6} {h:6} {100 * h / t:6.1f}%")
    pct = 100 * hit / total if total else 0
    print(f"{'TOTAL':42} {total:6} {hit:6} {pct:6.1f}%")
    if a.json:
        json.dump({"total": {"lines": total, "hit": hit, "percent": pct},
                   "files": {rel: {"lines": t, "hit": h, "missed": m} for rel, h, t, m in files}},
                  open(a.json, "w"), indent=1)
    if pct < a.min:
        print(f"coverage {pct:.1f}% is below the {a.min:.0f}% minimum", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
