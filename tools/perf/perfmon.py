#!/usr/bin/env python3
"""perfmon.py: sample the capture programs' CPU, memory and disk I/O once a second for the capture-program comparison
(tools/perf/README.md). Linux (/proc); the Windows twin is perfmon.ps1, with the same columns. Stop with Ctrl+C.

    python3 perfmon.py --out ~/perf/perfmon.csv

Columns: utc, machine, pid, name, cpu_pct (of the whole machine), cpu_cores, rss_mb, private_mb (anonymous memory),
read_mb / write_mb (storage I/O since the process started), sys_cpu_pct (whole machine).
FireCapture runs in Java, so a java process whose command line names FireCapture is reported as FireCapture.
"""
import argparse
import datetime
import os
import re
import socket
import time

HZ = os.sysconf("SC_CLK_TCK")
CORES = os.cpu_count() or 1


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return None


def sys_times():
    f = [int(x) for x in read("/proc/stat").splitlines()[0].split()[1:]]
    idle = f[3] + f[4]   # idle + iowait
    return idle, sum(f[:8])


def process(pid):
    stat = read(f"/proc/{pid}/stat")
    status = read(f"/proc/{pid}/status")
    if not stat or not status:
        return None
    rest = stat[stat.rindex(")") + 2:].split()
    cpu = int(rest[11]) + int(rest[12])   # utime + stime, fields 14 and 15
    mem = {}
    for line in status.splitlines():
        k, _, v = line.partition(":")
        if k in ("VmRSS", "RssAnon"):
            mem[k] = int(v.split()[0]) / 1024
    io = {}
    for line in (read(f"/proc/{pid}/io") or "").splitlines():
        k, _, v = line.partition(":")
        io[k] = int(v) / (1024 * 1024)
    return cpu, mem.get("VmRSS", 0.0), mem.get("RssAnon", 0.0), io.get("read_bytes", 0.0), io.get("write_bytes", 0.0)


def name_of(pid, pattern):
    comm = (read(f"/proc/{pid}/comm") or "").strip()
    cmd = (read(f"/proc/{pid}/cmdline") or "").replace("\0", " ")
    if comm.startswith("java") and re.search(r"fire_?capture", cmd, re.I):
        return "FireCapture" if pattern.search("FireCapture") else None
    return comm if pattern.search(comm) else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0: until Ctrl+C)")
    ap.add_argument("--match", default=r"^slopcap$|FireCapture|Firecapture")
    ap.add_argument("--machine", default=socket.gethostname())
    a = ap.parse_args()
    pattern = re.compile(a.match)

    new = not os.path.exists(a.out)
    out = open(a.out, "a", buffering=1)
    if new:
        out.write("utc,machine,pid,name,cpu_pct,cpu_cores,rss_mb,private_mb,read_mb,write_mb,sys_cpu_pct\n")
    print(f"perfmon: {CORES} logical CPUs, sampling every {a.interval} s into {a.out} (Ctrl+C stops)")
    prev = {}   # pid -> cpu ticks
    names = {}  # pid -> reported name (None = not watched)
    idle0, total0 = sys_times()
    last = begin = time.monotonic()
    try:
        while a.seconds <= 0 or time.monotonic() - begin < a.seconds:
            time.sleep(a.interval)
            now = time.monotonic()
            dt, last = now - last, now
            utc = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"
            idle, total = sys_times()
            sys_pct = 100.0 * (1 - (idle - idle0) / (total - total0)) if total > total0 else 0.0
            idle0, total0 = idle, total
            seen = set()
            for d in os.listdir("/proc"):
                if not d.isdigit():
                    continue
                pid = int(d)
                if pid not in names:
                    names[pid] = name_of(pid, pattern)
                if not names[pid]:
                    continue
                p = process(pid)
                if not p:
                    continue
                seen.add(pid)
                cpu, rss, anon, rd, wr = p
                if pid in prev and dt > 0:
                    cores = (cpu - prev[pid]) / HZ / dt
                    out.write(f"{utc},{a.machine},{pid},{names[pid]},{100 * cores / CORES:.1f},{cores:.3f},{rss:.1f},{anon:.1f},"
                              f"{rd:.1f},{wr:.1f},{sys_pct:.1f}\n")
                prev[pid] = cpu
            for pid in list(names):
                if pid not in seen and not os.path.exists(f"/proc/{pid}"):
                    names.pop(pid)
                    prev.pop(pid, None)
    except KeyboardInterrupt:
        pass
    finally:
        out.close()


if __name__ == "__main__":
    main()
