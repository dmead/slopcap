# SlopCap

A planetary / lunar / solar capture program for Linux and Windows: zero-drop video capture from astronomy cameras,
a live preview that keeps up, built-in focus / drift / guide-overlay tools, and a REST API. C++20, no Python.

Status: **M1 in progress**: capture pipeline + ZWO / QHY backends; ZWO verified at 168 fps with 0 drops on Linux and Windows. See [PLAN.md](PLAN.md) for the design and milestones.

## What's here

| Path | What |
|---|---|
| `src/io/ser.*` | SER video reader / writer (timestamp trailer, truncated-file recovery, real-world quirks) |
| `src/camera/camera.hpp` | `ICamera`: the interface every camera backend implements |
| `src/camera/sim_camera.*` | `SimCamera`: replays a SER recording as a camera (real-time pacing, ROI, drop counting) |
| `src/pipeline/frame_ring.*` | `FrameRing`: pre-allocated, page-aligned frame slots passed capture → writer lock-free |
| `src/pipeline/capture_session.*` | `CaptureSession`: capture thread + SER writer thread; counts camera drops vs ring drops |
| `src/camera/asi_camera.*`, `qhy_camera.*` | ZWO and QHY backends (built when their SDKs are found) |
| `src/camera/open_camera.*` | open a camera from a spec: `asi[:N]`, `qhy[:N]`, `sim:FILE.ser`, `sim-loop:FILE.ser` (replays forever) |
| `src/preview/` | `LatestFrame` tap, CPU reference debayer/stretch/histogram, `GlRenderer` (GPU), preview + histogram widgets |
| `src/app/` | `slopcap`: the GUI. `slopcap --camera SPEC [--out DIR] [--record] [--serve \| --connect HOST] [--fullscreen] [--quit-after S --screenshot F.png]` |
| `tools/bench` | `slopcap-bench --camera SPEC …` / `--sim FILE.ser` / `--list`: record through the full pipeline and report fps / drops / ring peak / MB/s |
| `tools/serinfo` | `slopcap-serinfo FILE.ser…`: header, frames vs header, timestamps, real frame rate, longest gap |
| `tools/camprobe` | `slopcap-camprobe [--json]`: what each connected ZWO / QHY camera reports (fixtures from real cameras inside) |
| `src/analysis/` | per-channel focus scoring (4 metrics), focus sweep fits, defocused-star collimation, low-precision ephemerides (Moon, Mars to Neptune) and target identification |
| `src/devices/` | minimal INDI client (Qt Network), a backlash-aware focuser, and a mount (guide pulses, axis moves, goto, park, tracking) |
| `src/net/` | remote viewing: one SlopCap shares its camera, others watch and steer it (protocol, JPEG frame codec, discovery, rate control) |
| `tools/diag` | `slopcap-diag FILE.ser…`: optical diagnostics from planetary recordings |
| `tools/focusbench` | `slopcap-focusbench FILE.ser [frames] [metric]`: time the focus scorer |
| `tests/` | Catch2 unit tests; `slopcap-gl-tests` checks the GPU preview against the CPU reference |

## At the telescope

```bash
build/linux-release/slopcap                      # remembers the last camera, folder, target, focuser, per-camera settings
build/linux-release/slopcap --camera asi --focuser localhost:7624
```

1. **Capture tab:** Connect the camera. Set gain/exposure (exposure in µs); the gain-landmark buttons set unity /
   lowest-read-noise gain. Watch "clipped %" under the histogram. Wheel zooms, drag pans, F fits, 1 = 1:1,
   C = crosshair, right-click "Centre ROI here" moves a planetary ROI onto the target. The Mount panel (the LX200's
   INDI driver, e.g. `indi_lx200classic`, on the focuser's INDI server) shows RA / Dec; N / E / W / S send guide pulses
   of the chosen length to nudge the target, Stop halts every motion.
2. **Collimate tab** (bright star): defocus to a ring 40–100 px across, tick "Analyse", turn the secondary screw toward
   the arrow (the ring's thin side) in small amounts, re-centre the star, repeat until the offset is < 2 %.
3. **Focus tab:** Connect (INDI server, default localhost:7624; the Gemini EAF is MyFocuserPro2). Set backlash
   (~600) and the final approach direction. Watch the R/G/B bars while nudging; then "Start sweep around current
   position". The table shows each channel's best focus ± error for every metric and the spread; pick a strategy
   (Green, Luminance-weighted, Balanced, Red, Blue) and "Go to". Each sweep's per-frame scores are saved as a CSV
   in the recording folder.
4. **Record** (Ctrl+R): files are named WinJUPOS-style from the mid-capture time and the Target field
   (`2026-10-17-0315_4-Saturn.ser`), with a `.txt` of the settings and focuser position next to each. With Target
   **Auto**, SlopCap identifies the Moon, Mars or an outer planet from the image and the clock: Saturn's rings, the
   Moon's shape at tonight's phase, a darkened limb, size and colour. It never asks the mount. Why it decided shows
   under the Target panel. When it can't decide, it says what's left ("Moon or Jupiter") and names the file "Target";
   Settings → Focal length (the image scale) settles most of those, and Settings → Site rules out what's below the
   horizon. The name is fixed while recording.

Every run writes a session log: `~/.local/share/SlopCap/SlopCap/logs/slopcap-<UTC start>.log` (Windows:
`%LOCALAPPDATA%\SlopCap\SlopCap\logs`). It records the version, settings, camera and controls; each recording's start
and stop, with drops and writer latency; a stats line every 10 s; INDI connections, driver messages and Alerts (e.g. a
focuser whose serial port won't open); focuser moves and sweeps; mount pulses; and automatic-target decisions with
their measurements.

Afterwards: `slopcap-diag --png OUT/ *.ser` stacks the best frames of each recording and reports dispersion, limb
sharpness per channel and side, halo and jitter.

## Remote viewing (outreach nights)

One SlopCap shares its camera (the **server**: the computer with the USB connection), others watch it (**clients**:
e.g. a laptop on a projector across the field). Any build (Windows, Linux) can be either.

- **Server:** "⇄ Remote" at the top → *Share this camera on the network* (or `slopcap --camera qhy --serve`).
- **Client:** "⇄ Remote" → pick the server from the list (same subnet), or *Watch an address…* with `192.168.1.20`
  (or `slopcap --connect 192.168.1.20`, `--connect auto` for the first one found).
- Either side can change gain, exposure and every other control; the other side's UI follows. ROI, bit depth and
  readout changes from a client are applied by the server. **Record** on a client records *on the server*: raw files
  never leave it, and the client shows the server's file, time and frame count.
- Both sides show a badge over the video while a link is up (LIVE FROM … / SHARED WITH …) and the stream's Mbps in
  the bottom bar. A dropped link reconnects by itself.
- **Full screen** (toolbar button or F11; Esc leaves): the preview over a whole screen; with several screens, pick
  which. Gain, exposure, stretch and record sit in a bar that hides itself (and the cursor) after 3 s; move the mouse
  to bring it back.
- The stream is the raw frame: **lossless** first (every bit, 16-bit included: each Bayer plane predicted pixel by
  pixel, the residuals deflated), else JPEG (each Bayer site its own plane, 16-bit through a square-root curve).
  **Detail first, frame rate second:** frames go at the best setting as many as the link carries (up to 30 fps); a
  slow link drops frames, not detail. Only when fewer than ~4 fps get through (5 fps is the floor) does a viewer step
  down: JPEG quality 85, 75, 65, then 1/2 and 1/4 size. Each change and its reason goes to the session log
  (`remote: … lossless → quality 85 …`). Measured on real recordings (desktop CPU): a 512×320 8-bit Saturn is
  127 kB lossless, a 1468×1188 16-bit Moon 1.6 MB (8 fps over 100 Mbps, so it stays lossless), a 3008×3008 16-bit
  Moon 5.6 MB (2 fps: it goes as JPEG 85, 333 kB, at 30 fps).

Ports: TCP 45720 (the stream; the next free one up to 45729 if taken) and UDP 45721 (discovery). On Windows the server
needs both allowed in, e.g. as admin:

```powershell
New-NetFirewallRule -DisplayName "SlopCap remote" -Direction Inbound -Program "C:\Users\<you>\slopcap\slopcap.exe" -Action Allow -Profile Private,Domain
```

The protocol is described at the top of `src/net/protocol.hpp`. `tools/remote-test.sh FILE.ser` tests it end to end in
three containers on one Docker network (an observatory server, a desktop client, a projector client in full view).

## Build in Docker (any machine)

`docker/build.sh` builds and tests inside a pinned Ubuntu 26.04 image (GCC 15, CMake, Ninja, Catch2, MinGW-w64,
ZWO / QHY SDKs, Qt 6). Tests run under Xvfb (a virtual X display) with Mesa's software OpenGL, so the GPU preview
tests work without a screen. The repo is mounted at the same path as on the host, so `build/<preset>/` works from either side.

```bash
docker/build.sh                          # linux-debug build + tests
docker/build.sh linux-asan linux-tsan    # any presets
docker/build.sh windows-cross            # Windows command-line tools (SLOPCAP_WIN_SDK_DLL_DIR=... to include camprobe)
docker/build.sh windows-static           # Windows GUI: one self-contained build/windows-static/slopcap.exe
docker/build.sh --shell                  # shell in the build environment
```

The image is tagged with the Dockerfile's hash. With a registry in `docker/registry`, a machine pulls the image
instead of building it, and any changed environment is pushed when first built (`docker/build.sh --push` to retry).
`.devcontainer/` opens the same environment in VS Code.

### Windows: one self-contained slopcap.exe

`windows-static` builds in its own image (`docker/Dockerfile.mingw`: Fedora's MinGW-w64 GCC plus Qt 6 compiled
statically, once, and cached in the registry). Point `SLOPCAP_WIN_SDK_DLL_DIR` at a folder holding the Windows
`ASICamera2.dll` and `qhyccd.dll` (e.g. copied from SharpCap's folder) and they are embedded too:

```bash
SLOPCAP_WIN_SDK_DLL_DIR=tools/camprobe/build-win/sdk docker/build.sh windows-static
```

Copy `build/windows-static/slopcap.exe` anywhere and run it: no installer, no DLLs. On first use it extracts the
camera SDKs to `%LOCALAPPDATA%\SlopCap\SlopCap\sdk\` (a DLL placed next to the exe takes precedence, to try a newer SDK).
The camera SDKs are delay-loaded, so without them SlopCap still starts and the camera menu says which one is
missing. Settings live in the registry (`HKCU\Software\SlopCap`). The preview needs OpenGL 3.3; without it (some
Remote Desktop sessions) capture still works and the preview says why it's blank. The focuser and mount are INDI-only
for now (no ASCOM yet).

## Build

Linux (Ubuntu 26.04): `sudo apt install build-essential cmake ninja-build catch2 qt6-base-dev`, plus `indi-3rdparty-libs` from the
INDI PPA for the camera SDKs (camprobe only).

```bash
cmake --preset linux-debug && cmake --build --preset linux-debug && ctest --preset linux-debug
cmake --preset linux-asan  && cmake --build --preset linux-asan  && ctest --preset linux-asan   # ASan + UBSan
cmake --preset linux-tsan  && cmake --build --preset linux-tsan  && ctest --preset linux-tsan   # ThreadSanitizer
cmake --preset linux-release && cmake --build --preset linux-release
```

Windows: `windows-msvc` preset (Visual Studio 2022 Build Tools, Catch2 via vcpkg), or cross-build from Linux with
`windows-cross` (MinGW-w64; set `SLOPCAP_WIN_SDK_DLL_DIR` to a folder with the target machine's `ASICamera2.dll` and
`qhyccd.dll`).

macOS (Apple silicon or Intel): Xcode's command-line tools (`xcode-select --install`), then `brew install qtbase cmake
ninja catch2`.

```bash
cmake --preset macos-debug && cmake --build --preset macos-debug && ctest --preset macos-debug
tools/macos-app.sh          # build/macos-release/SlopCap.app, self-contained (Qt inside) and signed ad hoc
tools/macos-app.sh --dmg    # ... plus SlopCap.dmg to copy to other Macs
```

No camera SDK is wired up on macOS yet, so a Mac build replays recordings (`--camera sim:FILE.ser`) and watches
other SlopCaps (remote viewing); the camera menu says ZWO and QHY support isn't built.
`-DSLOPCAP_FONT_DIR=<folder>` embeds IBM Plex from wherever its TTFs are (the default is the Linux path). The app keeps
settings in `~/Library/Preferences/com.slopcap.SlopCap.plist`, logs in `~/Library/Application Support/SlopCap/SlopCap/logs`
and records to `~/Movies/SlopCap`. Recording uses `F_NOCACHE` where Linux uses `O_DIRECT`. App Nap is off for SlopCap, so
capture and sharing keep full speed in the background. macOS asks once for Local Network access the first time
SlopCap shares, watches or reaches an INDI server on another machine. Full view is Ctrl+Cmd+F (F11 also works).
The app is signed ad hoc, not notarized: on another Mac, the first launch needs right-click → Open (or System Settings →
Privacy & Security → Open Anyway). It runs on the macOS version Homebrew's Qt was built for, and newer.

Optional test against a real recording: `SLOPCAP_TEST_SER=/path/to/file.ser ctest --preset linux-debug`.
