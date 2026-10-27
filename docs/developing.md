# Building and testing

SlopCap is C++20 with Qt 6 (Widgets, OpenGL, Network), built with CMake presets and Ninja, tested with Catch2. The
camera backends use the ZWO, QHY, Player One, ToupTek and MallinCam SDKs when the build finds them; without them
SlopCap still builds and can replay recordings and do remote viewing.

## The quickest way: Docker

`docker/build.sh` builds and tests inside a pinned image (`docker/Dockerfile`: Ubuntu 26.04, GCC 15, Qt 6, Catch2, the
camera SDKs and INDI from the INDI PPA, MinGW-w64, Xvfb + Mesa so the OpenGL tests run without a screen). It works
the same on any machine with Docker; on Windows run it from Git Bash with `MSYS_NO_PATHCONV=1`.

```bash
docker/build.sh                          # linux-debug: build + tests
docker/build.sh linux-asan linux-tsan    # sanitizer builds + tests
docker/build.sh windows-static           # Windows: one self-contained build/windows-static/slopcap.exe
docker/build.sh --shell                  # a shell in the build environment
```

Output goes to `build/<preset>/` in your checkout. The image is tagged with a hash of its Dockerfile and only rebuilt
when the Dockerfile changes; with a registry in `docker/registry`, machines pull it instead of building it.

**The Windows exe** is built with MinGW and a statically built Qt (`docker/Dockerfile.mingw`; building that Qt takes
a while, once). To build the camera drivers in, point `SLOPCAP_WIN_SDK_DLL_DIR` at a folder holding the Windows
`ASICamera2.dll`, `qhyccd.dll`, `PlayerOneCamera.dll`, `toupcam.dll` and `mallincam.dll` (copied from SharpCap's folder,
for example, where the last two are `ToupCam.dll` and `MallinCam.dll`; they aren't in this repository). Each one found
is built in; the build checks every SDK function SlopCap calls against the DLL's exports:

```bash
SLOPCAP_WIN_SDK_DLL_DIR=tools/camprobe/build-win/sdk docker/build.sh windows-static
```

## Natively

**Linux (Ubuntu 26.04):**

```bash
sudo apt install build-essential cmake ninja-build catch2 qt6-base-dev fonts-ibm-plex
sudo add-apt-repository ppa:mutlaqja/ppa && sudo apt install indi-3rdparty-libs indi-bin   # SDKs, INDI tests
cmake --preset linux-debug && cmake --build --preset linux-debug && ctest --preset linux-debug
cmake --preset linux-release && cmake --build --preset linux-release                        # -O3 -march=x86-64-v3
```

**macOS (Apple silicon or Intel):** Xcode's command-line tools, then:

```bash
brew install qtbase cmake ninja catch2 libusb
tools/macos-sdks.sh        # once: the camera SDKs from INDI's indi-3rdparty, hash-checked, into build/mac-sdk
tools/macos-indi.sh        # once: INDI built into ~/indi (Homebrew has none), for the focuser, mount and INDI tests
cmake --preset macos-debug && cmake --build --preset macos-debug && PATH=~/indi/bin:$PATH ctest --preset macos-debug
tools/macos-app.sh --dmg   # build/macos-release/SlopCap.app (Qt, SDKs, libusb inside; signed ad hoc) + SlopCap.dmg
```

**Windows (MSVC):** the `windows-msvc` preset exists (Visual Studio Build Tools, Catch2 from vcpkg) but isn't
maintained; the MinGW static build above is the one that's used.

| Preset | |
|---|---|
| `linux-debug`, `linux-release` | GCC or Clang |
| `linux-asan`, `linux-tsan` | AddressSanitizer + UBSan; ThreadSanitizer |
| `macos-debug`, `macos-release`, `macos-asan` | Apple clang, Homebrew Qt |
| `windows-static` | the self-contained Windows exe (MinGW cross-build in Docker) |
| `windows-cross` | Windows command-line tools (MinGW cross-build) |
| `windows-msvc` | native MSVC |

## Tests

`ctest --preset <preset>` runs:

- **slopcap-tests:** the core: SER reading and writing (including damaged files), the capture pipeline and its drop
  counting, the simulated camera, the CPU preview, focus scoring and sweeps, collimation, ephemerides and target
  identification.
- **gl-render:** the GPU preview against the CPU reference. Needs a screen (or Xvfb); skipped where there's none,
  such as over SSH on a Mac.
- **indi-devices:** the INDI client, focuser and mount against INDI's simulators on a private `indiserver`. Skipped
  when INDI isn't installed.
- **remote:** remote viewing: protocol, lossless and JPEG frame coding, discovery, rate control, a server and a
  client on loopback.
- **app:** the application, driven the way a user drives it (`tests/test_app_*.cpp`, needs a screen or Xvfb): the
  window's dialogs and menus (answered by `tests/app_harness.cpp`), a replayed recording as the camera, recording and
  its file names, saved settings, full view, focus sweeps and the mount against INDI's simulators, collimation, two
  windows in one process sharing and watching a camera, and the `slopcap` program itself with its command-line
  options. The same executable tests the camera backends against fakes of their SDKs (`tests/fakes/`: the fakes' C
  functions take the real libraries' place there), so every SDK call and error path runs without a camera. ToupTek's
  fake is written once (`fake_toup.inc`) and compiled under MallinCam's names too, as the backend is.

**Screenshots for the docs:** the README's Focus tab picture comes from a hidden test that sweeps INDI's focuser
simulator with a test camera whose colours focus apart: `SLOPCAP_DOCS_DIR=DIR slopcap-app-tests "[docs]"` (under
Xvfb in the build image) writes `DIR/focus.png` (the README's `docs/images/focus-sweep.png`), and `DIR/capture-sweep.png`,
the Capture tab halfway through with the curves over the preview.

**Coverage:** `tools/coverage.sh [MIN]` builds the `linux-coverage` preset (gcov) in the Docker image, runs every
test and prints line coverage of `src/` per file and folder (`build/linux-coverage/coverage.txt`; it exits 1 below
`MIN` percent). CI requires 90 %.

`SLOPCAP_TEST_SER=/path/to/recording.ser ctest …` also replays a real recording. `tools/remote-test.sh FILE.ser` tests
remote viewing end to end with three SlopCaps in containers (an observatory, a desktop, a projector in full view).

## Continuous integration

**Locally:** `tools/ci-local.sh [REF]` (on Windows: `MSYS_NO_PATHCONV=1 tools/ci-local.sh` from Git Bash) runs the
Linux and Windows jobs below on this machine, in Docker, for a commit (default `HEAD`; uncommitted changes aren't
tested). It checks the commit out into its own worktree, `build/ci/src`, and keeps that worktree's build folders, so
only the first run builds everything. Each job is logged under `build/ci/runs/<time>-<commit>/` with a `summary.txt`;
the script exits 1 if any job failed. `--jobs "linux-debug coverage"` runs a subset; `--windows-smoke` also starts the
Windows exe on this desktop as the windows-smoke job does (a window opens for about ten seconds). `--upload DEST` copies
the zips and the summary to an [rclone](https://rclone.org) destination (for example `drive:slopcap-builds`) after a run
in which every job passed; settings for one machine, such as a default destination, go in `build/ci/settings.sh`, which
git ignores (see the script's header). The Linux zip's start
in a bare Ubuntu container needs an `ubuntu:26.04` image (`docker pull ubuntu:26.04`); without one it is skipped. The
macos job needs a Mac: follow [Natively](#natively) there and run `ctest --preset macos-debug`.

**On GitHub:** `.github/workflows/ci.yml` runs the same jobs, plus macOS, only when started by hand (*Actions → ci →
Run workflow*), because each run costs a private repository's minutes:

| Job | What it does |
|---|---|
| **linux** (4 jobs) | `docker/build.sh` with `linux-debug`, `linux-asan`, `linux-tsan` (build + tests) and `linux-release`, in the same image as above; the release build is packed into `slopcap-linux-x64.zip` and started in a bare Ubuntu container. |
| **coverage** | `tools/coverage.sh 90`: every test on a gcov build; fails below 90 % of `src/`; the report is on the run's summary page. |
| **windows-exe** | builds `slopcap.exe` in the MinGW image (no camera drivers: they aren't in the repository) and keeps it, and `slopcap-windows-x64.zip`, as artifacts. |
| **windows-smoke** | starts that exe on a real Windows runner, replays a small generated recording, records it, and checks the session log and the file. |
| **macos** | Apple silicon: Homebrew Qt, the SDKs (`tools/macos-sdks.sh`) and INDI (`tools/macos-indi.sh`, cached), `macos-debug` with every test, then `SlopCap.app`, `SlopCap.dmg` and `slopcap-macos-arm64.zip` as artifacts, and a run of the packed app. |

The Docker images are cached in GitHub's Actions cache, so the slow parts (Qt for Windows, about an hour) happen
once. The zips, the `.dmg` and the Windows exe can be downloaded from each run's page under *Artifacts*.

## Packages

`tools/package.sh linux|windows|macos` makes `build/package/slopcap-<system>-<arch>.zip`: the program, a README for
that system (`tools/package/README-*.md`), `LICENSE`, the third-party licenses in `licenses/` and the observer pages of
`docs/`. The Linux zip carries the Qt libraries and plugins and the camera SDKs with a launcher, so it runs without Qt
installed (it needs glibc 2.43, Ubuntu 26.04 or newer); build `linux-release` first. The Windows zip takes
`build/windows-static/slopcap.exe` (with the camera drivers inside when it was built with `SLOPCAP_WIN_SDK_DLL_DIR`),
the Mac zip `SlopCap.app` from `tools/macos-app.sh`.

`licenses/` starts from `tools/package/licenses/` (its README says what each part is; the camera SDKs' texts are
INDI's). Linux adds the Debian copyright file of every package the bundled files come from, found with `dpkg -S`.
Windows adds what `docker/build.sh windows-static` leaves in `build/windows-static/licenses`: Qt's license texts and
SBOM, the MinGW runtime's licenses and the IBM Plex fonts' (the image keeps them in `/opt/licenses`). The Mac zip
doesn't carry Qt's yet.

**Releases** are zips only, never a bare exe. Tag the commit, run `tools/ci-local.sh --windows-smoke TAG` with
`SLOPCAP_WIN_SDK_DLL_DIR` set (so the exe has the camera drivers), push the tag, then
`tools/release.sh TAG NOTES.md`: it publishes that run's zips, unchanged, on GitHub, and only when every job passed
and each zip holds the README, `LICENSE` and `licenses/`.

GitHub counts macOS minutes ten times and Windows minutes twice against a private repository's monthly allowance,
which is why `ci.yml` only runs when started by hand and `tools/ci-local.sh` is the everyday check.

## Command-line tools

Built next to `slopcap` (`build/<preset>/`):

| Tool | Does |
|---|---|
| `slopcap-bench --camera SPEC …` / `--sim FILE.ser` / `--list` | records through the full capture pipeline and reports frames per second, drops, the frame ring's peak and MB/s |
| `slopcap-serinfo FILE.ser…` | a SER file's header, frames on disk against the header, timestamps, the real frame rate and the longest gap |
| `slopcap-camprobe [--json]` | what every connected ZWO and QHY camera reports (its controls, readout modes, gain landmarks); the fixtures from real cameras are in `tools/camprobe/fixtures` |
| `slopcap-diag [--png OUT/] FILE.ser…` | optical diagnostics from planetary recordings: stacks the best frames and reports dispersion, limb sharpness per colour and side, halo and jitter |
| `slopcap-focusbench FILE.ser [frames] [metric]` | times the focus scorer on a recording |

`slopcap` itself takes `--camera SPEC` (`asi[:N]`, `qhy[:N]`, `playerone[:N]`, `touptek[:N]`, `mallincam[:N]`,
`sim:FILE.ser`, `sim-loop:FILE.ser`), `--out`, `--record`,
`--tab`, `--serve` / `--listen ADDRESS` / `--connect HOST`, `--fullscreen`, `--set NAME=VALUE[@SECONDS]`, `--snapshot
SECONDS:FILE`, `--screenshot FILE --quit-after SECONDS` and `--settings DIR` (an INI file instead of the user's
settings): handy for scripted screenshots and smoke tests. `slopcap --help` lists them all.

## Platform notes

**Docker.** The image is tagged with the Dockerfile's hash. With a registry in `docker/registry`, a machine pulls the
image instead of building it, and an environment that changed is pushed when it's first built (`docker/build.sh
--push` to retry). `.devcontainer/` opens the same environment in VS Code. The checkout is mounted at the same path as
on the host, so `build/<preset>/` and compile errors read the same from either side.

**Windows.** `windows-static` builds in its own image (`docker/Dockerfile.mingw`: Fedora's MinGW-w64 GCC and a statically
built Qt 6). With `SLOPCAP_WIN_SDK_DLL_DIR`, the camera SDK DLLs found there are embedded in the exe; on first use
they're written to `%LOCALAPPDATA%\SlopCap\SlopCap\sdk\<hash>\` and loaded from there (a DLL placed next to the exe
wins, to try a newer SDK). The SDKs are delay-loaded, so without them SlopCap still starts and the camera menu says
which is missing. Settings live in the registry (`HKCU\Software\SlopCap`). The preview needs OpenGL 3.3; without it
(Windows' own OpenGL 1.1, some Remote Desktop sessions) capture still works and the preview says why it's blank. The
focuser is INDI-only for now; the mount has SlopCap's own drivers as well (no ASCOM yet).

**macOS.** `tools/macos-sdks.sh` takes the ZWO (1.41), QHY (26.7.21), Player One (3.10.1), ToupTek and MallinCam
(60.32226) SDKs from INDI's indi-3rdparty repository, the same source as the Linux packages, every file hash-checked; the build copies them and Homebrew's libusb into
SlopCap.app. QHY cameras start without firmware on a Mac: SlopCap uploads it from the SDK in the background at launch
and the camera reappears on USB a few seconds later. Homebrew has no INDI, so `tools/macos-indi.sh` builds indiserver
and the drivers into `~/indi`. Recording uses `F_NOCACHE` where Linux uses `O_DIRECT`; App Nap is off for SlopCap so
capture and sharing keep full speed in the background. `-DSLOPCAP_FONT_DIR=<folder>` embeds IBM Plex from wherever its
TTFs are. The app is signed ad hoc, not notarized, and runs on the macOS version Homebrew's Qt was built for and newer.

**Remote viewing.** The protocol is described at the top of `src/net/protocol.hpp`. `tools/remote-test.sh FILE.ser`
runs it end to end in three containers on one Docker network: an observatory server, a desktop viewer and a projector
viewer in full view.

## Where things are

| Path | What |
|---|---|
| `src/io/ser.*` | SER reader and writer: timestamp trailer, truncated-file recovery, real-world quirks |
| `src/io/ser_direct.*`, `direct_file.*` | the unbuffered, aligned SER writer (O_DIRECT / F_NOCACHE / unbuffered Windows I/O) that keeps a cut-off file readable |
| `src/camera/camera.hpp` | `ICamera`: the interface every camera backend implements |
| `src/camera/asi_camera.*`, `qhy_camera.*`, `playerone_camera.*` | the ZWO, QHY and Player One backends (built when their SDKs are found) |
| `src/camera/touptek_camera.hpp`, `touptek_family.inc` | ToupTek and MallinCam: one backend, a template over the brand, since MallinCam's SDK is ToupTek's under other names (`touptek_camera.cpp`, `mallincam_camera.cpp` bind each brand's symbols) |
| `src/camera/sim_camera.*` | `SimCamera`: replays a SER recording as a camera (real-time pacing, ROI, drop counting) |
| `src/camera/open_camera.*` | opens a camera from a spec: `asi[:N]`, `qhy[:N]`, `playerone[:N]`, `touptek[:N]`, `mallincam[:N]`, `sim:FILE.ser`, `sim-loop:FILE.ser` |
| `src/pipeline/frame_ring.*` | `FrameRing`: pre-allocated, page-aligned frame slots passed from capture to writer without locks |
| `src/pipeline/capture_session.*` | `CaptureSession`: the capture and writer threads; counts camera drops apart from ring drops |
| `src/preview/` | the live view: `LatestFrame` tap, the GPU debayer and stretch (`GlRenderer`), the CPU reference, histograms |
| `src/analysis/` | per-channel focus scoring (four metrics) and sweep fits, collimation, atmospheric dispersion, low-precision ephemerides (the Moon, Mars to Neptune), target identification |
| `src/devices/` | a small INDI client (Qt Network), the backlash-aware focuser and the mount (guide pulses, moves, goto, park, tracking) |
| `src/net/` | remote viewing: protocol, frame codec (lossless and JPEG), discovery, rate control, server and client |
| `src/app/` | the GUI: main window, focus, collimation and ADC panels, mount, settings, full view, session log |
| `tools/` | the command-line tools above, packaging (`package.sh`), coverage, macOS and remote-test scripts |
| `tests/` | Catch2 tests; `tests/fakes/` the camera SDK fakes; `tests/app_harness.*` the application tests' helpers |

The design, its reasoning and the to-do list are in [PLAN.md](../PLAN.md).
