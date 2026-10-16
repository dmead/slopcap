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
| `src/camera/open_camera.*` | open a camera from a spec: `asi[:N]`, `qhy[:N]`, `sim:FILE.ser` |
| `src/preview/` | `LatestFrame` tap, CPU reference debayer/stretch/histogram, `GlRenderer` (GPU), preview + histogram widgets |
| `src/app/` | `slopcap`: the GUI. `slopcap --camera SPEC [--out DIR] [--record] [--quit-after S --screenshot F.png]` |
| `tools/bench` | `slopcap-bench --camera SPEC …` / `--sim FILE.ser` / `--list`: record through the full pipeline and report fps / drops / ring peak / MB/s |
| `tools/serinfo` | `slopcap-serinfo FILE.ser…`: header, frames vs header, timestamps, real frame rate, longest gap |
| `tools/camprobe` | `slopcap-camprobe [--json]`: what each connected ZWO / QHY camera reports (fixtures from real cameras inside) |
| `src/analysis/` | per-channel focus scoring (4 metrics), focus sweep fits, defocused-star collimation |
| `src/devices/` | minimal INDI client (Qt Network) and a backlash-aware focuser |
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
   C = crosshair, right-click "Centre ROI here" moves a planetary ROI onto the target.
2. **Collimate tab** (bright star): defocus to a ring 40–100 px across, tick "Analyse", turn the secondary screw toward
   the arrow (the ring's thin side) in small amounts, re-centre the star, repeat until the offset is < 2 %.
3. **Focus tab:** Connect (INDI server, default localhost:7624; the Gemini EAF is MyFocuserPro2). Set backlash
   (~600) and the final approach direction. Watch the R/G/B bars while nudging; then "Start sweep around current
   position". The table shows each channel's best focus ± error for every metric and the spread; pick a strategy
   (Green, Luminance-weighted, Balanced, Red, Blue) and "Go to". Each sweep's per-frame scores are saved as a CSV
   in the recording folder.
4. **Record** (Ctrl+R): files are named WinJUPOS-style from the mid-capture time and the Target field
   (`2026-10-17-0315_4-Saturn.ser`), with a `.txt` of the settings and focuser position next to each.

Afterwards: `slopcap-diag --png OUT/ *.ser` stacks the best frames of each recording and reports dispersion, limb
sharpness per channel and side, halo and jitter.

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
Remote Desktop sessions) capture still works and the preview says why it's blank. The focuser is INDI-only for now
(no ASCOM yet).

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

Optional test against a real recording: `SLOPCAP_TEST_SER=/path/to/file.ser ctest --preset linux-debug`.
