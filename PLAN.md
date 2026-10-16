# SlopCap — plan

A planetary / lunar / solar capture program for Linux, written from scratch: fast lossless video capture from
astronomy cameras, a live preview that keeps up at full frame rate, the analysis tools that matter at the telescope
(focus, drift, target tracking), and a REST API so the whole thing can be scripted or driven by an assistant.

Status: plan only (2026-10-15). Nothing built yet.

---

## 1. Goals

1. **Never drop a frame.** Record at whatever rate the camera and USB deliver, sustained for long captures.
2. **A preview that doesn't hold capture back.** Debayered, stretched preview at ≥ 30 fps even at full frame, without
   stealing CPU from capture.
3. **The workflow tools built in, not bolted on:** focus sweep, drift / polar-alignment measurement, a guide overlay
   showing where the target should go, ROI tracking that follows the planet, capture sequences for WinJUPOS.
4. **API-first.** Everything the GUI can do, the REST API can do, with an event stream. The GUI is one client.
5. **Correct, interoperable files:** SER that AutoStakkert!, PSS, SER Player and WinJUPOS read, with per-capture
   metadata.

6. **Linux and Windows, both first-class** (decided 2026-10-15). The observatory machine is Windows 11 with ASCOM;
   development and the original FireCapture work are on Linux. Same code, CMake presets per platform, CI on both.

## 2. Non-goals (for now)

- Deep-sky / long-exposure imaging, plate solving, guiding (PHD2, Ekos and NINA cover these).
- macOS (nothing designed to block it later). Raspberry Pi (aarch64) after the two desktop platforms.
- Every camera brand on day one. Two SDKs first, ZWO and QHY (we have both to test with), behind one interface
  that later brands plug into.

## 3. First target rig

| Item | Detail |
|---|---|
| Camera | ZWO ASI585MC: 3840×2160, 2.9 µm, colour (RGGB), USB3. Measured: 46.9 fps full frame RAW8, ~168 fps at 1040×560 |
| Focuser | Gemini EAF via INDI (`indi_myfocuserpro2_focus`, localhost:7624) |
| Mount | LX200 on a wedge, via INDI (`indi_lx200…`), for guide pulses / axis moves |
| Scope | 3048 mm + 2.5× Barlow → 7620 mm, 0.0785″/px |
| Host | Intel i5-1135G7 (4C/8T, AVX2 + AVX-512BW), Linux, X11/XWayland |

These numbers are the performance bar: SlopCap must match the SDK's own ceiling (46.9 / 168 fps) with zero drops.

## 4. Stack

| Concern | Choice | Why |
|---|---|---|
| Language | **C++20** | INDI's client library and the camera SDKs are C/C++; Qt; mature tooling |
| Build | CMake + Ninja; presets for GCC/Clang on Linux and MSVC on Windows, debug / release / asan / tsan | |
| GUI, threads, networking | **Qt 6** (Widgets for panels, `QOpenGLWidget` for the preview) | one framework; signals across threads |
| REST API | `QHttpServer` (Qt ≥ 6.4) + WebSocket or SSE for events | no extra dependency |
| Devices | INDI (`libindiclient`) on Linux; ASCOM on Windows, via Alpaca (HTTP, works on both) or COM | one `IFocuser` / `IMount` / `IFilterWheel` interface over both |
| Camera | ZWO ASI SDK (`libASICamera2`) and QHY SDK (`libqhyccd`) behind an `ICamera` interface | two SDKs from the start keep the interface honest |
| SIMD | **Google Highway**: runtime dispatch AVX-512 / AVX2 / NEON | one binary, fast everywhere |
| Preview | OpenGL shaders: debayer + stretch + overlay on the GPU | takes preview load off the CPU |
| Tests | Catch2; ASan/UBSan and TSan builds in CI | stands in for Rust's guarantees |
| Logging | spdlog | |
| Config | TOML (toml++) for settings and profiles | human-editable |

**C++ only, no Python.** No Python at runtime, in the build, in tests or for prototyping: analysis is written
directly as fast C++ (Highway SIMD + threads), and test fixtures and benchmarks are C++ tools too.

Numeric / astronomy libraries (all permissive, so SlopCap's own licence stays open):

| Need | Library |
|---|---|
| Linear algebra, least-squares fits (focus parabolas, drift lines) | Eigen (header-only) |
| SIMD kernels | Google Highway |
| FFT (registration, seeing metrics) | pocketfft (header-only) |
| Nonlinear fits, if Eigen isn't enough | Ceres Solver |
| FITS files | CFITSIO |
| Time scales, precession, ephemerides | ERFA + Astronomy Engine |
| Units in the type system (µm, steps, arcsec) | mp-units |

Avoided for licensing: FFTW and GSL (GPL). OpenCV only if its registration/filtering is actually needed.

Code style: modern C++ only (RAII, `std::span`, `std::unique_ptr`, no raw `new`/`delete`), `-Wall -Wextra -Werror`,
clang-tidy.

## 5. Architecture

```
            ┌──────────────┐  frames (zero-copy slots)  ┌──────────────┐
 camera ──► │ CaptureThread│ ─────────────────────────► │  RingBuffer  │──┐
 SDK        └──────────────┘                            │  (pooled,    │  │
                                                       │  pre-alloc)  │  │
                                                       └──────┬───────┘  │
                     ┌────────────────────┬──────────────────┼──────────┘
                     ▼                    ▼                  ▼
              ┌─────────────┐     ┌──────────────┐    ┌──────────────┐
              │ WriterThread│     │ AnalysisPool │    │ PreviewFeed  │
              │ SER/FITS    │     │ histo, target│    │ latest frame │
              │ O_DIRECT /  │     │ sharpness,   │    │ → GPU upload │
              │ io_uring    │     │ drift (SIMD) │    │ (drops OK)   │
              └─────────────┘     └──────┬───────┘    └──────┬───────┘
                                         │ results            │
                                         ▼                    ▼
                     ┌───────────────────────────────────────────────┐
                     │ Core (state, profiles, sequencer, devices)    │◄── INDI (focuser, mount, wheel)
                     └───────────────┬───────────────────┬───────────┘
                                     ▼                   ▼
                               Qt GUI client       REST API + events
```

Rules:
- **Capture is the priority.** The capture thread only moves frames into the ring; it never waits on preview, analysis
  or disk. If the writer falls behind, the ring absorbs it and the status says so. Frames are only dropped at the
  camera (counted from the SDK) or when the ring is full (counted and shown).
- **Preview and analysis are allowed to skip frames.** They take the latest frame, not every frame.
- **Writing every frame is mandatory** while recording. The writer works on big sequential writes and never touches
  pixels.
- **One state owner (Core).** The GUI and API send commands to Core and subscribe to its events; they never touch
  devices directly.

### Modules

| Module | Responsibility |
|---|---|
| `camera/` | `ICamera` (open, controls with ranges, ROI, binning, bit depth, start/stop stream, get frame, dropped count). readout modes, gain landmarks (§5a). `AsiCamera`, `QhyCamera`, `SimCamera` (replays SER files, see §8) |
| `pipeline/` | frame pool, lock-free SPSC ring, frame metadata (timestamp, sequence #, ROI offset) |
| `io/` | SER writer (+ timestamp trailer), reader (for SimCamera and tools), metadata sidecar, WinJUPOS naming; FITS later |
| `analysis/` | histogram/clipping, target detection (centroid, radius, bbox, edge contact), sharpness score, drift fit — SIMD via Highway |
| `preview/` | GL upload, debayer shader (RGGB/BGGR/…), stretch (gamma / auto-levels / software gain), overlays |
| `devices/` | INDI client: focuser (absolute/relative, backlash handling), mount (pulse, axis move, stop, park), filter wheel |
| `core/` | state machine (idle / previewing / recording / paused), profiles & per-filter settings, sequencer, ROI tracking |
| `api/` | REST + events (design carried over from our FireCapture Remote API, §7) |
| `gui/` | Qt Widgets: camera panel, capture panel, histogram, focuser, devices, overlay drawing |
| `tools/` | CLI: `slopcap-camprobe` (capabilities, exists: `tools/camprobe/`), `slopcap-bench` (fps/drops), `slopcap-serinfo`, `slopcap-focus` |

## 5a. Readout modes and gain landmarks

Drivers describe "modes" very differently, so `ICamera` normalises them into two things the UI and API can show the
same way for every brand:

- **Readout modes**: a list of named modes the camera supports, with the current one marked and the resolution if a
  mode changes it. Switching may require stopping the stream (and, for QHY, re-initialising the camera).
  - *QHY*: the camera's own named read modes (`GetQHYCCDNumberOfReadModes` / `GetQHYCCDReadModeName` /
    `SetQHYCCDReadMode`), e.g. high-gain or extended-full-well modes, plus the "light performance mode" where present.
  - *ZWO*: no named modes in the SDK; SlopCap presents the `ASI_HIGH_SPEED_MODE` switch as two modes (normal / high
    speed). ZWO "camera modes" are trigger modes and are listed separately.
  - *Later brands*: PlayerOne sensor modes, ToupTek conversion gain (LCG / HCG / HDR) map onto the same list.
- **Gain landmarks**: named gain values with an optional matching offset and where the value came from.
  - *ZWO*: `ASIGetGainOffset` (offset for highest dynamic range, offset at unity gain, gain + offset for lowest read
    noise, which is where HCG is on) and `ASIGetLMHGainOffset` (preset low / medium / high gain). Unity gain itself
    is estimated from `ElecPerADU` (gain unit = 0.1 dB). HCG switching is automatic inside the driver; there's no
    control for it.
  - *QHY*: `DefaultGain` / `DefaultOffset`, and, where the camera provides the curves (`QHYCCD_curveReadoutNoise`,
    `QHYCCD_curveSystemGain`, `QHYCCD_curveFullWell`), landmarks computed from them: the HCG switch (the step in
    read noise), unity gain (e-/ADU crosses 1), lowest read noise, and highest dynamic range (max full well / read
    noise).
  - UI: tick marks on the gain slider and one-click presets ("Unity", "HCG / lowest noise", "Max dynamic range"),
    each setting the matching offset too.
  - API: `GET /v1/camera/modes`, `PUT /v1/camera/mode {index}`, `GET /v1/camera/gain-landmarks`.

**What the drivers actually report** (probed 2026-10-15 on the observatory machine, fixtures in
`tools/camprobe/fixtures/`):
- *ASI585MC* (SDK 1.41): gain 0–600; lowest read noise at gain 450 / offset 15; offsets 3 for highest dynamic range
  and unity; presets 0 / 200 / 450; high-speed readout. **The HCG switch gain is not reported**, and the lowest-read-noise
  gain is not the switch point. `ElecPerADU` (0.053) is in model-dependent units, so unity gain can't be derived from it.
- *QHY533M* and *QHY5III715C* (SDK 25.6.16): one read mode each ("STANDARD MODE"), no noise curves, no default gain /
  offset, ADC depth not reported. The 715C supports gain↔dB conversion.

So the drivers alone can't place the landmarks SlopCap wants to show. Two sources fill the gap:
1. **A per-model sensor table** (HCG switch gain, unity gain, ADC bits, read noise / full well at key gains) taken
   from the manufacturers' published charts, shipped as data and marked as "published".
2. **Measure it**: a guided calibration that takes bias and flat pairs at a sweep of gains (lens cap / flat panel)
   and computes e-/ADU, read noise and full well per gain (photon transfer). The HCG switch shows up as the step in
   read noise. Results are saved per camera serial and marked "measured", and they override the table.

`tools/camprobe/` (`slopcap-camprobe`, read-only, `--json` available) reports all of this per connected camera.
Run it on every camera we have before writing `AsiCamera` / `QhyCamera`, and keep its output as test fixtures.

## 5b. Adding camera brands (assessed 2026-10-16)

Every SDK below is already on the laptop: they come in the same INDI `indi-3rdparty-libs` package as the ZWO and QHY
ones, so Linux builds need nothing new. For Windows, each vendor's DLL is taken from the vendor's own software.

**What one backend is** (from ZWO: 271 lines, QHY: 317, plus a ~150-line `camprobe` probe):
- an `ICamera` implementation: enumerate, open, controls, ROI/bin/bit depth, readout modes, gain landmarks, a
  streaming `readFrame` into our buffer with a timeout, and a dropped-frame count;
- a `slopcap-camprobe` probe and a fixture recorded from a real camera;
- CMake detection;
- acceptance as in M1: `slopcap-bench` at the SDK's own frame-rate ceiling, 10 minutes, 0 dropped.

| Brand | SDK (version on disk) | Fit with `ICamera` | Effort | Risk |
|---|---|---|---|---|
| **Player One** (Mars, Neptune, Saturn, Uranus, Ares, Apollo) | `PlayerOneCamera` 3.10.1, C, 37 calls, 0.4 MB | Very close to ZWO's flow. `POAGetGainsAndOffsets` returns highest DR / HCG / unity / lowest read noise directly → `GainLandmark`, source Driver. `POAGetSensorModeInfo` → readout modes. `POAGetDroppedImagesCount`, USB bandwidth limit. | ~1 day + ½ day with a camera | Low |
| **ToupTek and its rebrands**: Altair, Ogma, Omegon, Orion StarShoot G, SVBony (newer models, `svbonycam`), Mallincam, Meade, Bresser, TS-Optics (`tscam`), `nncam` | `toupcam` 60.32226; every rebrand is the same 256-call API under a different prefix (Altair's header differs from ToupTek's in 4 lines after renaming) | Different delivery model: pull mode with an event callback (`StartPullModeWithCallback` → `PullImageV4` into our slot), bridged to `readFrame(timeout)` with a condition variable. Conversion gain LCG / HCG / HDR (`OPTION_CG`) → readout modes. Gain is a percentage (100 = ×1). RAW / bit depth / frame-rate limit via `put_Option`. **One source file serves all eleven brands:** load whichever library is present at run time and resolve `<Prefix>_Function` by name. | 3–4 days for the backend + loader, ~½ day per brand with a camera | Medium (callback threading, timestamps, which library recognises which camera) |
| **SVBony** (SV305, SV705C, SV905C…; newer models use `svbonycam`, covered above) | `SVBCameraSDK` 1.13.4, C, ~36 calls, 5.5 MB | Derived from ZWO's API: video capture mode, `SVBGetDroppedFrames`, RAW8–16 output, sensor pixel size. No gain-landmark call → published values (`GainLandmark` source Published). | ~1 day + ½ day with a camera | Medium (SDK quirks unknown until tested) |

**Shared work first (~1 day):**
- **A run-time SDK loader** (`dlopen` / `LoadLibrary` plus a symbol table), generalising the Windows delay-loading
  added for ZWO/QHY:
  - a missing SDK never stops SlopCap starting, on either OS;
  - the large SDKs needn't be linked at all. ToupTek-family libraries are 21–67 MB each on Linux, too big to embed in
    the Windows single exe, so on Windows they load from beside `slopcap.exe` or from the vendor's install folder.
  - Player One's 0.4 MB library can be embedded like ZWO's and QHY's.
- **ROI alignment from the camera:** `CameraInfo` gains ROI step sizes. `MainWindow::applyFormat` currently hardcodes
  ZWO-safe values (x multiple of 8, y even), which other brands may not need or may not accept.
- **Published gain landmarks** for brands without an API, as §5a already plans.

**Testing is the real constraint.** We only own ZWO and QHY cameras, and none of these SDKs has a simulated camera. A
backend can be written, compiled and unit-tested without hardware, but it stays marked *untested* until a `camprobe`
fixture and a zero-drop bench run come from a real camera of that brand (borrowed, or run by someone who owns one).

**Order:**
1. Player One: popular for planetary, the cheapest to add, and its API maps best.
2. The ToupTek family: one backend covers eleven brands.
3. SVBony's own SDK.

About 7–8 days of code for all three plus the shared work, and ½–1 day per brand once a camera is available.

**Broader fallbacks, if wanted later** (each slower than a native SDK for high-frame-rate planetary work):
- **ASCOM / Alpaca camera** on Windows: any brand with a driver. Per-frame image transfer limits fps, so it suits
  focusing and slow cameras. 3–5 days; Alpaca over Qt Network is easier than COM from MinGW.
- **INDI CCD** on Linux or remote: via the existing `IndiClient` with BLOB streaming. 2–3 days.
- **The Imaging Source** (DMK/DFK): `tiscamera` on Linux and IC Imaging Control on Windows are two separate backends,
  4–6 days.
- **Teledyne FLIR / USB3 Vision** (Spinnaker): 3–5 days.

## 6. Performance plan

Budget at the Saturn ROI (1040×560 RAW8, 168 fps): 98 MB/s to disk, ~6 ms per frame across all CPU work.
At full frame (3840×2160, 46.9 fps): ~390 MB/s, ~21 ms per frame.

1. **Zero-copy capture:** the SDK writes straight into pooled ring slots; the writer writes those same slots out.
   No `memcpy` on the hot path.
2. **Disk:** large aligned sequential writes (`O_DIRECT` or `io_uring`), pre-allocated file (`fallocate`). Measure
   sustained throughput on the target SSD at startup; refuse a recording whose rate can't be sustained, or warn.
3. **Preview on the GPU:** upload the raw Bayer frame as a texture and debayer + stretch in a shader. CPU cost ≈ one
   upload per displayed frame.
4. **Analysis with SIMD + threads:** histogram, target detection and sharpness on every Nth frame (every frame for
   focus/quality modes), vectorised with Highway, spread over the cores. Target: < 2 ms per 1040×560 frame.
5. **Measure, don't guess:** `slopcap-bench` reports fps, camera drops, ring high-water mark and writer latency.
   `perf` before any hand-optimisation; compile hot code with `-O3 -march=x86-64-v3` and let auto-vectorisation do
   the easy part first.

Acceptance: a 10-minute recording at 1040×560 and at full frame with **0 dropped frames**, preview ≥ 30 fps, on the
host above.

**Vectorization log** (what was vectorized, why, and what it bought; laptop, -O3 x86-64-v3):

| Code | Why it was a candidate | Result |
|---|---|---|
| `analysis/focus.cpp`: Bayer plane extraction, levels/clip count, signal + centroid, gradient, Laplacian, variance | Runs on every displayed frame, over every pixel of the scoring window, in each of the three channels | Written as plain strided/contiguous loops with branch-free selects. Float sums marked with `SLOPCAP_SIMD_SUM` (`src/util/simd.hpp`: OpenMP simd reductions via `-fopenmp-simd`, no runtime). Each one confirmed with `-fopt-info-vec`. In an A/B with vectorization off: 2.65 → 2.04 ms per 4K frame |
| Same scorer, algorithm | Profile first: scoring the whole frame just to find the planet dominated | Window-only extraction (23 → 2.0 ms). Histogram quantile instead of `nth_element` (perf: 41% of time) → 1.1 ms. Vectorizing alone would not have got there |
| FFT butterfly (fourier metric) | Hand-written complex multiply, assuming `std::complex` calls `__mulsc3` | **Rejected:** 4× slower (GCC already handled it). Kept as a reminder to measure every change |
| GPU preview | Demosaic + stretch per pixel per frame | Done in the fragment shader instead of on the CPU |

## 7. REST API (v1)

Same shape as the FireCapture Remote API we built (that design is ours), served by SlopCap itself:

- `GET /v1`, `GET /v1/status`, events at `/v1/events` (capture started/stopped, frame stats, device changes)
- Camera: `GET/PUT /v1/camera`, `GET/PUT /v1/camera/controls`, `POST /v1/camera/roi/center`, `GET /v1/camera/frame`
  (PNG) and `GET /v1/camera/frame/raw` (8/16-bit, for analysis clients)
- Capture: `GET/PUT /v1/capture`, `POST /v1/capture/start|stop|pause|resume`, profiles, filters
- Devices: `/v1/focuser`, `/v1/mount` (pulse, move, stop, park), `/v1/filterwheel`
- Analysis: `GET /v1/target`, `POST /v1/focus/sweep`, `POST /v1/drift/measure` (returns RA/Dec drift in ″/min)
- Overlay: `GET/POST/DELETE /v1/overlay` (planet / saturn / sun / moon / cross; edge arrows when off view)

Loopback-only by default; optional token for LAN use.

## 7a. Feature: per-channel (RGB) focus

Other capture tools give one focus number for a colour camera. That hides **longitudinal chromatic aberration**:
through a Barlow, ADC or imperfect optics, red, green and blue come to focus at different focuser positions, and a
single blended score either picks one channel's best focus by accident or splits the difference without saying so.
SlopCap scores each colour channel separately and shows all three.

**How it's measured**
- Split the raw Bayer frame into its colour planes before any debayering: R, B, and G (the two green sites
  averaged). Each plane is a quarter-resolution image with no interpolation, so no channel's sharpness leaks into
  another's.
- Run the same focus metric on each plane: peak brightness / edge contrast for planets, the Moon and the Sun;
  half-flux diameter for stars. Each channel is normalised to its own best value, since blue is usually dimmer and
  noisier than green.
- Score many frames per focuser position (median plus best quarter) so seeing doesn't pick the winner.
- Fit a parabola (or a V for half-flux diameter) per channel; each vertex is that channel's best focus, with an
  uncertainty from the fit.

**What the user sees**
- **Live, while focusing by hand:** three sharpness bars (R, G, B) with peak-hold markers, so you can watch one
  channel peak while another is still improving.
- **After a sweep:** three curves on one chart, each channel's best position, and the **spread** (largest minus
  smallest) in steps and in µm when the step size is known.
- **The spread against the depth of focus.** Critical focus zone ≈ ±2.44 λ N²; about ±0.84 mm at f/25 for green,
  converted to steps once the focuser's µm per step is calibrated. If all three bests fall inside it, one focus
  position is fine for all channels; if not, the user has to choose, and SlopCap says which channel it is giving up.
- **Choose where to focus:**
  - *Green*, which carries most of the luminance detail in an RGB image
  - *Luminance-weighted*: about 0.30 R + 0.59 G + 0.11 B
  - *Balanced*: the position that minimises the worst channel's defocus
  - *Red* or *Blue*, when processing that channel (e.g. IR/R-pass on Mars, or a methane band)

  The move to the chosen position goes through the normal backlash-aware approach.
- Remembered per profile and filter as **focus offsets** (e.g. R +70, B −80 relative to G), so a later sweep or a
  filter change can apply them. With a mono camera and a filter wheel, the same chart shows one curve per filter.

**Related, but a different problem:** atmospheric dispersion shifts the colour channels sideways, not in focus. SlopCap
reports per-channel centroid offsets as a separate readout (useful with an ADC); it isn't part of the focus curves.

**API:** `POST /v1/focus/sweep` returns `{channels: {R: {best, err, curve[]}, G: …, B: …}, spread, recommended: {mode,
position}}`. `GET /v1/focus/live` returns per-channel live scores. `PUT /v1/focus/target {mode}` picks the strategy.

## 8. Testing without the sky

- **SimCamera replays real SER recordings**, e.g. the Saturn captures in `~/firecapture/Sat/`, at the recorded rate
  or as fast as possible, with optional synthetic drift, seeing jitter and defocus (including a different defocus per
  colour channel, to test RGB focus against a known answer). Every pipeline, analysis and
  overlay feature can be developed and regression-tested indoors.
- Golden tests for the SER writer: byte-exact header, frame count, timestamps; files open in SER Player and PSS.
- Analysis tests on stored frames: known centroid / radius / sharpness ordering.
- CI: release + ASan/UBSan + TSan builds, unit tests, a SimCamera soak test (10 min at max rate, 0 drops).

### Device test rig: Linux on the observatory machine (set up 2026-10-15)

The observatory machine ("observatory", 192.168.1.20, Windows 11) runs Ubuntu 26.04 in WSL2, the same release as the
development laptop, with INDI 2.2.5 and the ZWO / QHY SDKs from the INDI PPA. `usbipd-win` 5.3 moves a camera from
Windows into Linux and back; SSH key login from the laptop drives all of it.

| Bus | Camera |
|---|---|
| 3-3 | ZWO ASI585MC |
| 3-2 | QHY533M (USB `1618:c534`) |
| 5-4 | QHY5III715C (USB `1618:0716`) |

```
usbipd attach --wsl --busid 3-3      # camera to Linux (Windows apps lose it)
usbipd detach --busid 3-3            # back to Windows
```

Measured through the forwarding (ASI585MC, 0 dropped frames in every run): 1040×560 at **168 fps** in both RAW8 and
RAW16, the same as native; full frame at 43.0 fps RAW8 (native laptop: 46.9), limited to about 360 MB/s. So the rig
is good for performance work at planetary ROI sizes, not just functional tests. Full-frame benchmarks should still
be confirmed on native Linux. The WSL kernel has the CH340 / FTDI serial drivers, so INDI serial focusers can be
tested there too.

**QHY cameras: test natively, not through the WSL forwarding.** On 2026-10-15, QHY live streaming through usbip
hung inside the QHY SDK and knocked the QHY533M off USB until it was replugged; the ZWO streams through the same path
without trouble. Natively on Windows the QHY5III715C streamed cleanly (0 dropped) at 4.2 fps full-sensor, most likely
because it's on a slower USB port; not a SlopCap issue, so not pursued.

### Disk: the observatory machine's SSD is the full-frame bottleneck (measured 2026-10-16)

M1 go/no-go runs, Windows native, ZWO at 1 ms exposure:

| Run | Needs | Result |
|---|---|---|
| ASI585MC 1040x560, 10 min, buffered writer | 98 MB/s | 100,800 frames, **0 dropped** (writer lagged up to 197 frames) |
| same, 3 min, direct writer | 98 MB/s | 30,235 frames, **0 dropped**, lag at most 37 frames, worst gap 7.3 ms (vs 16.8 ms) |
| ASI585MC full frame, 3 min | ~380 MB/s | ~3,100 frames lost to a full ring, either writer |
| QHY533M full sensor, 10 min | ~241 MB/s | ~2,400 frames lost to a full ring, either writer |

The cameras and SlopCap delivered every frame. The disk, an unbranded "PCIe SSD" (firmware V0513A0), writes at
full speed for about 40-50 s (~12-15 GB, its fast cache), then falls to ~150-190 MB/s. It reached 83 °C in the small
chassis during back-to-back runs. BitLocker is off; Defender real-time scanning is on with no exclusions.

Rested drive (2026-10-16: 18 min idle first, 36 °C at the start, full-frame ASI585MC, direct writer, 3 min): it kept up
at ~388 MB/s for ~55 s (~20 GB), then dropped to 153-192 MB/s and stayed there. It peaked at only 66 °C, so **the cliff
is the fast cache running out, not heat**. Resting buys a slightly bigger cache (~20 GB vs 12-15 GB) but the same
outcome: 5,373 of 8,443 frames written (vs 5,229-5,399 back-to-back). Planetary ROI captures (≤ ~150 MB/s) are fine.
Full-frame or full-sensor captures longer than ~50 s need a better SSD (TLC with DRAM). A larger RAM ring only
stretches the burst: each extra GB adds ~5 s at 380 MB/s in, 170 MB/s out.

Consequences for SlopCap:
- The startup disk check (§6) must write past the drive's fast cache (tens of GB) or watch for the cliff; a short
  test reports the misleading fast-cache speed.
- Better: measure the write rate live and, when it falls below the incoming rate, show "ring full in N s" before
  frames are lost.
- The direct writer is the default for high data rates (steadier at ROI sizes, up to 2.1 GB/s on the laptop).

## 9. Milestones

| # | Milestone | Done when |
|---|---|---|
| M0 | Skeleton | CMake project, CI with sanitizers, `ICamera` + `SimCamera` replaying a SER, `slopcap-serinfo` |
| M1 | Capture core | `AsiCamera` + `QhyCamera`, ring buffer, SER writer; `slopcap-bench` hits the SDK's own ceiling (ASI585MC: 46.9 / 168 fps) with 0 drops for 10 min on both brands |
| M2 | Preview | Qt window, GL debayer + stretch, histogram; ≥ 30 fps full frame while recording |
| M3 | Controls & profiles | gain/exposure/ROI/all controls, profiles + per-filter settings, limits, WinJUPOS naming, metadata sidecar |
| M4 | API | REST + events covering M1–M3; parity with the FireCapture Remote API's camera/capture endpoints |
| M5 | Devices | INDI focuser (with backlash-aware moves), mount pulses/moves, filter wheel |
| M6 | Workflow tools | target detection, guide overlay, focus sweep with per-channel RGB curves (§7a), drift measurement, ROI tracking during capture, sequences |
| M7 | Polish & port | settings UI, crash-safe recording (recoverable SER), aarch64/NEON build for Raspberry Pi |

M1 is the go/no-go: if the zero-drop capture core doesn't hold up, nothing else matters.

M2 status (2026-10-16):
- **Done:** `LatestFrame` tap (triple buffer, copied only when the preview asks); live-view sessions with no output;
  camera settings applied mid-stream through `CaptureSession::post`. The GL renderer demosaics and stretches into a
  mipmapped RGBA8 image, then draws it aspect-fitted. It matches the CPU reference to ±1 for every Bayer pattern,
  depth and gamma (`slopcap-gl-tests`; Iris Xe, and Mesa llvmpipe under Xvfb in Docker). Histogram with auto or manual
  levels, gain-landmark buttons, and record with live MB/s, buffer and drop counters.
- **Measured:** laptop, release build, 3840×2160 8-bit RGGB replay at 39 fps: the display runs at 35 fps while
  recording at 322 MB/s, 0 dropped.
- **Added since (2026-10-16, for the first live night, Saturn 2026-10-17, GUI on the laptop with the cameras and focuser
  plugged into it):**
  - zoom/pan/1:1, crosshair, right-click "Centre ROI here", overlays, display-only auto white balance;
  - Focus tab, pulled forward from M5/M6: INDI focuser with backlash-aware moves; live R/G/B sharpness; and the §7a
    sweep, scoring all four metrics on the same frames so the night's data shows which metric is tightest;
  - Collimate tab: defocused-star ring/shadow fit;
  - WinJUPOS file names (mid-capture), `.txt` sidecar, persistent settings (per camera serial);
  - `slopcap-diag`: diagnostics from old recordings.
- **Findings from the Oct 7–8 Saturn recordings (`slopcap-diag`):**
  - Dispersion is ~5 px vertical (atmospheric).
  - No dew trend and no one-sided limb blur.
  - Chromatic focus shift: blue softest at focuser ~64,420 and red softest at ~64,950.
  - The globe's size implies ~0.10″/px, not FireCapture's 0.14″/px setting.
- **Also done 2026-10-16 (later):**
  - **GUI restyled and restructured** to the "SlopCap UI" design canvas
    (https://claude.ai/artifact/HDs3pRiwUQxpxZZRpmvayB). There's a header with modes, and Capture, Focus and Collimate
    are working modes. Align and Sequence show disabled until built.
  - **Windows:** one self-contained `slopcap.exe` (`docker/build.sh windows-static`: static Qt 6.11.2, embedded
    ZWO/QHY SDKs and fonts, SDKs delay-loaded). Tested on the NUC in Remote Desktop: ASI585MC full frame 46.8 fps,
    0 dropped, GPU preview working; QHY5III715C connected.
  - **Zips** in `build/windows-static/slopcap-windows.zip` and `build/linux-release/slopcap-linux.zip` (not in git).
    The Linux zip relies on system Qt and the INDI SDK packages.
  - **Mount (M5, first part):** `devices::Mount` over the standard INDI telescope properties: guide pulses
    (`TELESCOPE_TIMED_GUIDE_NS/WE`), axis moves at a slew rate, goto, abort, park / unpark, tracking, RA / Dec. Tested
    against `indi_simulator_telescope`. The Capture sidebar's Mount panel (as designed) has the N / E / W / S pulse pad
    and pulse length, and recordings' `.txt` files carry the mount's RA / Dec. It shares the focuser's INDI server.
  - **Focuser fix:** every LX200 driver reports TELESCOPE | GUIDER | FOCUSER, so with the mount driver on the same
    server the focuser could pick the LX200 instead of the EAF. Mounts are now skipped unless named (tested with
    `indi_lx200classic` started first).
- **Still to do:**
  - Real-camera check of the GUI **on the laptop** (cameras, EAF and LX200 plugged into it), before the Saturn night.
    Check that the LX200 driver takes guide pulses (the classic LX200 has no pulse command; INDI then times moves at
    the guide rate) and which way N / E move the target on screen.
  - Mount next: hold-to-move at a slew rate, `/v1/mount` in the REST API, and the Align mode's drift measurement.
  - Settings → optics: enter the focal ratio and the EAF's µm per step, so the sweep shows the critical focus zone.
  - Upload through a PBO if texture upload shows up in profiles.
  - ASCOM focuser on Windows (INDI only today).
  - Align (drift guide outline) and Sequence modes from the design.
  - More camera brands: specced in §5b, **parked**.

**Saturn night checklist (laptop):**
1. Plug the camera and the Gemini EAF into the laptop. `systemctl --user status indiserver` should show
   `indi_myfocuserpro2_focus`.
2. `build/linux-release/slopcap` → camera pill → `asi:0`; Focus tab → Connect (localhost:7624).
3. Collimate on a bright star first (Collimate mode).
4. Then Saturn: centre the ROI with right-click → "Centre ROI here", and run a Fine sweep. The table shows which
   metric gives the smallest error. Go to the chosen focus.
5. Record with Ctrl+R; each file gets a `.txt` of its settings. Afterwards, run `slopcap-diag` on the night's files.

**Where things live:**
- git: a private `origin` remote, and GitHub.
- Build images: `slopcap-build` (Linux) and `slopcap-build-mingw` (Windows, static Qt).
- The NUC: `C:\Users\<you>\slopcap\slopcap.exe`. To start it in the Remote Desktop session from SSH, use a
  one-off interactive scheduled task.

## 10. Ground rules

- **Clean room.** SlopCap is written from public sources only: camera SDK headers and docs, the SER format
  specification, INDI documentation, and our own measurements. Nothing derived from FireCapture's code or from
  disassembling it. (FireCapture's license forbids reverse engineering; keep this project clear of it.)
- No vendor SDK binaries committed to the repo; fetch or point to the installed SDK at build time.
- Every performance claim comes with a `slopcap-bench` number.

## 11. Open questions

0. ~~Platform~~: both Linux and Windows. Linux device testing on the observatory machine runs in WSL2 + usbipd-win (§8).
1. ~~GUI~~: Qt Widgets + QOpenGLWidget (M2).
2. SlopCap's own license (GPL-3.0 like INDI/KStars, or MIT/Apache)? Check that the ZWO SDK's license allows
   distributing builds that link it.
3. ~~Which camera SDK comes after ZWO?~~ QHY, alongside ZWO from M1. After those: Player One, then the ToupTek family, then SVBony (§5b).
4. File formats beyond SER: FITS cubes? AVI is probably not worth it.
5. Should the FireCapture skill and plugin be retired once SlopCap reaches M6, or kept as a fallback?
