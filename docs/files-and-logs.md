# Files, settings and logs

## Recordings

Recordings go to the folder chosen with **Folder…** on the Capture panel. Out of the box that's:

| | |
|---|---|
| Windows | `C:\Users\<you>\Videos\SlopCap` |
| Linux | `~/Videos/SlopCap` |
| macOS | `~/Movies/SlopCap` |

Each recording is two files:

- **`2026-10-17-0315_4-Saturn.ser`**: the video, in the SER format that AutoStakkert!, PIPP, PSS, SER Player and
  WinJUPOS read. The name is WinJUPOS-style: the date, then the time at the *middle* of the recording to a tenth of a
  minute, then the filter (if set) and the target. Every frame has its own timestamp.
- **`2026-10-17-0315_4-Saturn.txt`**: what was used: camera, gain, exposure and every other control, ROI, bit depth,
  readout mode, filter, start time, duration, frame count, the focuser position, and the mount's RA / Dec when a mount
  is connected.

A recording that was cut short (a crash, the power going) is still readable: SlopCap keeps the file's header up to date
while it writes.

Focus sweeps save a CSV of every frame's scores in the same folder.

## Session logs

Every time SlopCap runs, it writes a log of the session, the place to look when you want to know what happened during
a night:

| | |
|---|---|
| Windows | `%LOCALAPPDATA%\SlopCap\SlopCap\logs\` |
| Linux | `~/.local/share/SlopCap/SlopCap/logs/` |
| macOS | `~/Library/Application Support/SlopCap/SlopCap/logs/` |

One file per run, named by the start time in UTC (`slopcap-2026-10-17-020512.log`); SlopCap keeps the newest 100 and
deletes older ones. It records:

- the SlopCap version, the computer, and the settings at start;
- the camera and its controls, and every change to them;
- every recording's start and stop, with frames dropped and how fast the disk kept up;
- a line of statistics every 10 seconds (frame rate, drops, disk speed, free space, sharpness, target);
- INDI connections and everything the focuser's and mount's drivers say, including errors such as a serial port that
  won't open;
- focuser moves and focus sweeps, mount guide pulses;
- the automatic target's decisions and the measurements behind them;
- remote viewing: who connected, and every change in picture quality with its reason.

## Settings

SlopCap remembers the last camera, the recording folder, the target and filter, the focuser and mount connection,
optics and site, and each camera's settings. They're kept in the usual place for each system:

| | |
|---|---|
| Windows | the registry, `HKEY_CURRENT_USER\Software\SlopCap\SlopCap` |
| Linux | `~/.config/SlopCap/SlopCap.conf` |
| macOS | `~/Library/Preferences/com.slopcap.SlopCap.plist` |

To start from scratch, quit SlopCap and delete that entry or file.
