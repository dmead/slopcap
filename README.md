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
| `tests/` | Catch2 unit tests; `slopcap-gl-tests` checks the GPU preview against the CPU reference |

## Build in Docker (any machine)

`docker/build.sh` builds and tests inside a pinned Ubuntu 26.04 image (GCC 15, CMake, Ninja, Catch2, MinGW-w64,
ZWO / QHY SDKs, Qt 6). Tests run under Xvfb (a virtual X display) with Mesa's software OpenGL, so the GPU preview
tests work without a screen. The repo is mounted at the same path as on the host, so `build/<preset>/` works from either side.

```bash
docker/build.sh                          # linux-debug build + tests
docker/build.sh linux-asan linux-tsan    # any presets
docker/build.sh windows-cross            # Windows .exe files (SLOPCAP_WIN_SDK_DLL_DIR=... to include camprobe)
docker/build.sh --shell                  # shell in the build environment
```

The image is tagged with the Dockerfile's hash. With a registry in `docker/registry`, a machine pulls the image
instead of building it, and any changed environment is pushed when first built (`docker/build.sh --push` to retry).
`.devcontainer/` opens the same environment in VS Code.

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
