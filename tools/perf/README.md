# Comparing SlopCap with SharpCap and FireCapture

How fast each capture program records, how many frames it loses, and what it costs the computer, on the same
machine, with the same camera, at the same formats. The point is a like-for-like comparison: every program is run
the way an observer runs it, through its own window, with the preview showing and the recording going to the
machine's own disk. Nothing is scripted inside the programs, and SlopCap gets no shortcuts (no `slopcap-bench`, no
command-line recording).

What's measured:

| What | How |
|---|---|
| Frame rate, lost frames | From each recording's own per-frame timestamps (SER trailer), read the same way whichever program wrote it: `slopcap-serinfo --csv`. Lost frames: what the recording's time span should hold at the camera's frame period, less what it holds (the period is the median over 100-frame windows, so frames stamped in bursts don't count as lost). |
| What the program says | Its own frame and dropped counts at the end of each recording, written down from its window. |
| CPU, memory, disk writes | `perfmon.ps1` (Windows) / `perfmon.py` (Linux) sample the program once a second; `report.py` takes the samples inside each recording's time span. |

SharpCap runs only on Windows, so a Linux machine compares SlopCap with FireCapture.

## Formats

Each camera's formats, chosen to be on every program's list. Where a program has no exact match, use its nearest
centred size and write it down; the report takes the real size from the file.

| Camera | Formats |
|---|---|
| ZWO ASI585MC (3840×2160) | full 8-bit, full 16-bit, 1920×1080 8-bit, 1280×720 8-bit, 640×480 8-bit |
| QHY533M (3008×3008) | full 16-bit, full 8-bit, 1024×1024 8-bit, 512×512 8-bit |
| QHY5III715C | full 8-bit, full 16-bit, 1920×1080 8-bit, 640×480 8-bit |

## Settings that must match in every program

- Exposure **1 ms**, so the camera and the computer set the pace, not the exposure. Gain: ZWO 200, QHY 30.
- Raw frames (RAW8 / RAW16, no debayering into the file), **SER** output, no frame rate limit.
- ZWO: USB bandwidth **100** (SharpCap "Turbo USB", FireCapture "USBTraffic", SlopCap "BandWidth"), high-speed mode
  **off**. QHY: USB traffic and speed at the same values in every program (note them; start from SlopCap's).
- Recording limit **30 s** (SlopCap's shortest), started from the program's own record button.
- Window maximised, preview at its default fit-to-window zoom, debayered colour preview where the program offers it,
  no extra tools (histogram stretch, focus aids, overlays) beyond the defaults.
- The program's own buffer settings left at their defaults (write them down).
- The same capture folder location on the same disk for every program; nothing else running.

## Procedure

On each machine, for each camera:

1. Start the monitor in a terminal and leave it running for the whole camera:
   `powershell -NoProfile -ExecutionPolicy Bypass -File perfmon.ps1 -Out C:\perf\perfmon.csv` or
   `python3 perfmon.py --out ~/perf/perfmon.csv`.
2. Three rounds. Each round runs every program once, in a rotated order (round 1: SlopCap, SharpCap, FireCapture;
   round 2: SharpCap, FireCapture, SlopCap; round 3: FireCapture, SlopCap, SharpCap), so warm-up, heat and disk state
   don't favour one program.
3. In each program: open it, connect the camera, set the fixed settings above, then for each format:
   set the format, let the preview run 10 s, start a recording, wait for the 30 s limit, and write a line in
   `runs.csv` with what the program says (frames, dropped). Close the program before the next one opens the camera.
4. Rest the disk before full-frame recordings where its fast cache runs out (see below).
5. After the three rounds, stop the monitor and summarise the recordings:
   `slopcap-serinfo --csv <capture folders>/*.ser > serinfo.csv` (recurse as needed).

`runs.csv` has one line per recording:

```
machine,app,camera,repeat,file,app_reported,notes
linux,slopcap,ASI585MC,1,2026-11-02-0412_3-capture.ser,"0 dropped",
linux,firecapture,ASI585MC,1,2026-11-02-0415_1-capture.ser,"2 dropped",FireCapture ROI list has no 1280x720: used 1280x768
```

`app` is `slopcap`, `sharpcap` or `firecapture`; `file` is the recording's file name.

Then, anywhere with Python 3:

```
python3 report.py --runs runs.csv --serinfo linux=serinfo-linux.csv windows=serinfo-windows.csv \
                  --perfmon linux=perfmon-linux.csv windows=perfmon-windows.csv --out results/
```

It writes `results/results.csv` (one line per recording) and `results/summary.md`: per machine and camera, a row per
format with each program's median frame rate, lost frames, CPU share and peak memory.

## Things that skew the numbers

- **Disk fast cache.** Cheap SSDs write at full speed only until a cache fills (one of our test machines: about
  12-20 GB, then ~150-190 MB/s). A full-frame ASI585MC recording is ~380 MB/s, so 30 s fills most of it. On such a
  machine, rest the disk a few minutes (3 on ours) before each full-frame recording. A disk that fell behind shows as
  lost frames in every program at that format, at a write rate (`mb_s`) above what the disk keeps up; a program that
  fell behind, in that one only.
- **Remote desktop.** If the programs are driven over a remote desktop, the machine also encodes the screen. That
  costs every program, but more for a preview that changes more often; note it in the results. On Linux use the
  desktop's own screen sharing (GNOME Desktop Sharing), not a separate session such as xrdp: a separate session has
  no GPU, which cripples a GPU preview.
- **Timestamps.** The programs may stamp a frame at slightly different moments (on arrival from the driver, or on its
  way to disk), and their clocks' resolution differs. FireCapture's are whole milliseconds and come in bursts (at
  168 fps: up to 20 ms apart, sometimes out of order). That changes the jitter between frames (`p99_ms`, `backwards`),
  not the frame rate over 30 s. The lost-frame estimate is good to a few frames in 30 s; trust it from about five up,
  and set it beside what the program itself reported.
- **Disk space.** Thirty seconds of full frame is ~11 GB, and one round of the ASI585MC across three programs is
  ~100 GB: summarise the recordings and clear the test folder after each round.
