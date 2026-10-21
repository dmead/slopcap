# Building and testing

SlopCap is C++20 with Qt 6 (Widgets, OpenGL, Network), built with CMake presets and Ninja, tested with Catch2. The
camera backends use the ZWO and QHY SDKs when the build finds them; without them SlopCap still builds and can replay
recordings and do remote viewing.

## The quickest way: Docker

`docker/build.sh` builds and tests inside a pinned image (`docker/Dockerfile`: Ubuntu 26.04, GCC 15, Qt 6, Catch2, the
ZWO / QHY SDKs and INDI from the INDI PPA, MinGW-w64, Xvfb + Mesa so the OpenGL tests run without a screen). It works
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
`ASICamera2.dll` and `qhyccd.dll` (copied from SharpCap's folder, for example; they aren't in this repository):

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
tools/macos-sdks.sh        # once: ZWO + QHY SDKs from INDI's indi-3rdparty, hash-checked, into build/mac-sdk
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

`SLOPCAP_TEST_SER=/path/to/recording.ser ctest …` also replays a real recording. `tools/remote-test.sh FILE.ser` tests
remote viewing end to end with three SlopCaps in containers (an observatory, a desktop, a projector in full view).

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request (changes to Markdown files only are skipped):

| Job | What it does |
|---|---|
| **linux** (4 jobs) | `docker/build.sh` with `linux-debug`, `linux-asan`, `linux-tsan` (build + tests) and `linux-release`, in the same image as above. |
| **windows-exe** | builds `slopcap.exe` in the MinGW image (no camera drivers: they aren't in the repository) and keeps it as an artifact. |
| **windows-smoke** | starts that exe on a real Windows runner, replays a small generated recording, records it, and checks the session log and the file. |
| **macos** | Apple silicon: Homebrew Qt, the SDKs (`tools/macos-sdks.sh`) and INDI (`tools/macos-indi.sh`, cached), `macos-debug` with every test, then `SlopCap.app` and `SlopCap.dmg` as an artifact. |

The Docker images are cached in GitHub's Actions cache, so the slow parts (Qt for Windows, about an hour) happen
once. The `.dmg` and the Windows exe can be downloaded from each run's page under *Artifacts*.

GitHub counts macOS minutes ten times and Windows minutes twice against a private repository's monthly allowance.
To save minutes, limit the `macos` job to `main` and pull requests in `ci.yml`.

## Where things are

| Path | What |
|---|---|
| `src/io/` | SER reader and writer (unbuffered, aligned writes that keep the file readable if cut off) |
| `src/camera/` | `ICamera` and its backends: ZWO, QHY, and the simulated camera that replays a SER |
| `src/pipeline/` | the capture session: capture thread, lock-free frame ring, writer thread |
| `src/preview/` | the live view: GPU debayer and stretch, the CPU reference, histogram |
| `src/analysis/` | focus scoring and sweeps, collimation, ephemerides, target identification |
| `src/devices/` | a small INDI client, the focuser (backlash-aware) and the mount |
| `src/net/` | remote viewing: protocol (described at the top of `protocol.hpp`), frame codec, discovery, rate control |
| `src/app/` | the GUI |
| `tools/` | `slopcap-serinfo`, `slopcap-diag`, `slopcap-bench`, `slopcap-camprobe`, `slopcap-focusbench`, scripts |
| `tests/` | Catch2 tests |

The design, its reasoning and the to-do list are in [PLAN.md](../PLAN.md).
