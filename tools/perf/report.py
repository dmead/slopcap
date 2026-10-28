#!/usr/bin/env python3
"""report.py: turn a capture-program comparison session into results (tools/perf/README.md).

    python3 report.py --runs runs.csv --serinfo windows=serinfo-windows.csv linux=serinfo-linux.csv \
                      --perfmon windows=perfmon-windows.csv linux=perfmon-linux.csv --out results/

runs.csv     one line per recording: machine,app,camera,repeat,file,app_reported,notes (app = slopcap, sharpcap or
             firecapture; file = the SER file's name; app_reported = what the program itself said, e.g. "3 dropped")
serinfo      `slopcap-serinfo --csv` over the session's recordings, one file per machine
perfmon      perfmon.ps1 / perfmon.py output, one file per machine

The recording itself is the yardstick: frame rate and lost frames come from each SER file's timestamps, read the same
way whichever program wrote it, and the format (width, height, bits) from its header. CPU and memory are the
program's samples inside the recording's time window.
Writes results.csv (one line per recording) and summary.md (median of the repeats, programs side by side).
"""
import argparse
import csv
import datetime
import os
import re
import statistics
from collections import defaultdict

APPS = ["slopcap", "sharpcap", "firecapture"]
APP_NAMES = {"slopcap": "SlopCap", "sharpcap": "SharpCap", "firecapture": "FireCapture"}
PROCESS = {"slopcap": r"^slopcap$", "sharpcap": r"^SharpCap$", "firecapture": r"^(FireCapture|java|javaw)$"}


def when(s):
    return datetime.datetime.fromisoformat(s.replace("Z", "+00:00")) if s else None


def labelled(items):
    out = {}
    for item in items:
        label, _, path = item.partition("=")
        if not path:
            raise SystemExit(f"expected LABEL=FILE, got {item}")
        out[label] = path
    return out


def load_serinfo(files):
    rows = {}
    for machine, path in files.items():
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                rows[(machine, os.path.basename(r["file"].replace("\\", "/")))] = r
    return rows


def load_perfmon(files):
    samples = defaultdict(list)   # machine -> [(utc, name, cpu_pct, cores, rss_mb, write_mb, sys_pct, pid)]
    for machine, path in files.items():
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                samples[machine].append((when(r["utc"]), r["name"], float(r["cpu_pct"]), float(r["cpu_cores"]),
                                         float(r["rss_mb"]), float(r["write_mb"]), float(r["sys_cpu_pct"]), r["pid"]))
    for s in samples.values():
        s.sort(key=lambda x: x[0])
    return samples


def window(s):
    """The recording's time span: its own timestamps if they agree with the file's modified time (some programs
    have written local time there), else the span ending at the modified time."""
    mod = when(s["modified_utc"])
    first, last = when(s["first_utc"]), when(s["last_utc"])
    if first and last and abs((last - mod).total_seconds()) < 120:
        return first, last
    return mod - datetime.timedelta(seconds=float(s["span_s"] or 0)), mod


def usage(samples, app, start, end):
    pat = re.compile(PROCESS[app])
    by_time = defaultdict(lambda: [0.0, 0.0, 0.0, 0.0])   # utc -> cpu_pct, cores, rss, sys
    writes = defaultdict(list)                             # pid -> write_mb samples
    for utc, name, cpu, cores, rss, wr, sys_pct, pid in samples:
        if start <= utc <= end and pat.search(name):
            t = by_time[utc]
            t[0] += cpu
            t[1] += cores
            t[2] += rss
            t[3] = sys_pct
            writes[pid].append(wr)
    if not by_time:
        return {}
    cpu = sorted(v[0] for v in by_time.values())
    secs = max((end - start).total_seconds(), 1e-3)
    return {
        "samples": len(cpu),
        "cpu_avg": statistics.fmean(cpu),
        "cpu_p95": cpu[min(len(cpu) - 1, int(0.95 * len(cpu)))],
        "cores_avg": statistics.fmean(v[1] for v in by_time.values()),
        "rss_peak_mb": max(v[2] for v in by_time.values()),
        "sys_cpu_avg": statistics.fmean(v[3] for v in by_time.values()),
        "os_write_mb_s": sum(max(w) - min(w) for w in writes.values()) / secs,
    }


def fmt_cell(g, best_fps):
    fps = g["fps"]
    s = f"**{fps:.1f}**" if best_fps and fps >= 0.99 * best_fps else f"{fps:.1f}"
    s += f" fps · {g['missing']} lost"
    if g.get("cpu") is not None:
        s += f" · CPU {g['cpu']:.0f} % · {g['rss']:.0f} MB"
    if g["runs"] > 1:
        s += f" ({g['runs']} runs, {g['fps_min']:.1f}–{g['fps_max']:.1f})"
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--runs", required=True)
    ap.add_argument("--serinfo", nargs="+", required=True, metavar="MACHINE=FILE")
    ap.add_argument("--perfmon", nargs="*", default=[], metavar="MACHINE=FILE")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    ser = load_serinfo(labelled(a.serinfo))
    perf = load_perfmon(labelled(a.perfmon))
    os.makedirs(a.out, exist_ok=True)

    results = []
    with open(a.runs, newline="") as f:
        for r in csv.DictReader(f):
            if not r.get("file"):
                continue
            app = r["app"].strip().lower()
            if app not in APPS:
                raise SystemExit(f"unknown app {r['app']!r} (use {', '.join(APPS)})")
            key = (r["machine"], os.path.basename(r["file"].replace("\\", "/")))
            s = ser.get(key)
            if not s:
                print(f"warning: no serinfo line for {key[1]} on {key[0]}")
                continue
            start, end = window(s)
            u = usage(perf.get(r["machine"], []), app, start, end)
            results.append({
                "machine": r["machine"], "app": app, "camera": r.get("camera") or s["instrument"], "repeat": r.get("repeat", ""),
                # SER's bit depth is what the program says the ADC gives (12, 14 or 16 for "16-bit"), so programs are
                # compared by sample size
                "width": int(s["width"]), "height": int(s["height"]), "bits": 8 if int(s["bits"]) <= 8 else 16,
                "ser_bits": int(s["bits"]), "file": key[1],
                "frames": int(s["frames"]), "span_s": float(s["span_s"]), "fps": float(s["fps"]), "period_ms": float(s["period_ms"]),
                "missing": int(s["missing"]), "max_gap_ms": float(s["max_gap_ms"]), "p99_ms": float(s["p99_ms"]), "mb_s": float(s["mb_s"]),
                "backwards": int(s["backwards"]), "truncated": s["truncated"], "app_reported": r.get("app_reported", ""), "notes": r.get("notes", ""),
                "start_utc": start.isoformat(), "end_utc": end.isoformat(), **u,
            })

    fields = ["machine", "app", "camera", "width", "height", "bits", "ser_bits", "repeat", "file", "frames", "span_s", "fps", "period_ms",
              "missing", "max_gap_ms", "p99_ms", "backwards", "mb_s", "truncated", "samples", "cpu_avg", "cpu_p95", "cores_avg", "rss_peak_mb",
              "sys_cpu_avg", "os_write_mb_s", "app_reported", "notes", "start_utc", "end_utc"]
    with open(os.path.join(a.out, "results.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        for r in results:
            w.writerow({k: (f"{v:.3f}" if isinstance(v, float) else v) for k, v in r.items()})

    groups = defaultdict(list)
    for r in results:
        groups[(r["machine"], r["camera"], r["width"], r["height"], r["bits"], r["app"])].append(r)
    summary = {}
    for k, rs in groups.items():
        fps = [r["fps"] for r in rs]
        cpu = [r["cpu_avg"] for r in rs if "cpu_avg" in r]
        summary[k] = {
            "runs": len(rs), "fps": statistics.median(fps), "fps_min": min(fps), "fps_max": max(fps),
            "missing": sum(r["missing"] for r in rs),
            "cpu": statistics.median(cpu) if cpu else None,
            "rss": max((r["rss_peak_mb"] for r in rs if "rss_peak_mb" in r), default=0),
        }

    lines = ["# Capture program comparison", "",
             "Median of the repeats. fps and lost frames come from each recording's own timestamps; CPU is the program's",
             "average share of the whole machine during the recording, memory its peak working set. **Bold**: within 1 % of",
             "the fastest program for that format.", ""]
    for machine, camera in sorted({(k[0], k[1]) for k in summary}):
        apps = [a_ for a_ in APPS if any(k[0] == machine and k[1] == camera and k[5] == a_ for k in summary)]
        lines += [f"## {machine} · {camera}", "", "| Format | " + " | ".join(APP_NAMES[x] for x in apps) + " |",
                  "|---|" + "---|" * len(apps)]
        formats = sorted({(k[2], k[3], k[4]) for k in summary if k[0] == machine and k[1] == camera}, key=lambda f: (-f[0] * f[1], f[2]))
        for w_, h_, b_ in formats:
            cells = {x: summary.get((machine, camera, w_, h_, b_, x)) for x in apps}
            best = max((c["fps"] for c in cells.values() if c), default=0)
            row = [fmt_cell(cells[x], best) if cells[x] else "—" for x in apps]
            lines.append(f"| {w_}×{h_} {b_}-bit | " + " | ".join(row) + " |")
        lines.append("")
    with open(os.path.join(a.out, "summary.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"{len(results)} recordings -> {a.out}/results.csv, {a.out}/summary.md")


if __name__ == "__main__":
    main()
