# Mount support: every mount, one code path

Design spec, 2026-10-22.

**Status, 2026-10-27: the built-in drivers are built and released (v0.1.1)**, not yet tried on a real mount, and
there's no iOptron driver yet (`src/mounts/`, tests in `tests/test_mounts.cpp` against
`tests/fakes/fake_mounts.*`). Dan asked for "as much compatibility for various mounts (skywatcher, synscan, onstep,
lx-200 protocol) into as small of an executable as possible", which settles two of the §9 questions: serial is our
own Win32 / termios class, not Qt SerialPort (9.2), and Sky-Watcher direct is in (9.3). Built:

- `ITransport` (serial, TCP, UDP), `CommandChannel`, `IMountDriver`, `MountWorker` (thread, queue with Stop first,
  polling, emulated pulses, 10 s move deadman, J2000 / JNow, reconnect every 10 s), specs and `--mount` /
  `--list-mounts`, Settings → Mount, the Mount panel's Connect.
- `lx200` with the Meade Classic, Meade (Autostar / II / LX200GPS) and OnStep dialects, from Meade's 2007.10 protocol
  and OnStepX's command reference; `nexstar` / `synscan` from Celestron's and Sky-Watcher's (v3.3) documents;
  `skywatcher` (motor controller, equatorial only, home = pole with counterweights down, sync as an offset) from
  Sky-Watcher's 2021 command set; and `synscan-app`, Sky-Watcher's SynScan App Protocol (UDP 11881, ITelescopeV3 as
  text), which wasn't in the table below and covers any mount the app drives; `sim`.
- The driver interface is simpler than §3.2's: `move(Dir, rateIndex)` / `stop(Dir)` over named rates instead of
  `moveAxis(Axis, degPerSec)`, which fits the LX200 and hand-controller rates.

OnStep has its own driver since 2026-11-07, tried on an OnStepX (§3.7).

**Direction, 2026-11-08 (Dan):** SlopCap talks to the mounts connected to it by USB itself ("I don't care about
ascom or alpaca. connected usb devices only"), so ASCOM and Alpaca are out of this spec (§3.4). The long goal: "find
the intersection between all this mounts and write a minimal, fast module that talks to all of them". What every
driver must do is what the mounts' protocols share; the panel is built on that and looks the same whatever mount
is connected; what only some mounts have (OnStep's own settings) shows only where it's there. INDI stays as it
is for now.

Not yet: INDI behind `IMountDriver` (`devices::Mount` keeps its INDI code and switches to the worker for a built-in
spec), the 10Micron / Gemini / ZWO / iOptron dialects, alt-az `AxisMount`, Celestron AUX, the horizon check before
a goto, planet-rate tracking. Only the OnStep driver has been on a real mount; directions marked "to check" in the
headers (Sky-Watcher's count direction, the hand controllers' RA motor sign) need one.

Today SlopCap drives a mount only through INDI (`src/devices/mount.cpp`, the standard telescope properties). That's
fine on Linux and macOS but leaves Windows needing an INDI server on another computer, and Windows is where most
mounts' ASCOM drivers are, the observatory machine among them. This spec extends that to every mount a planetary
imager is likely to have, inside the one `slopcap` executable, with everything above the wire shared.

## 1. Goals

- **One `Mount`:** **built-in drivers** for the mounts' own protocols over their USB (serial) cables, and INDI
  (exists). The GUI, the recordings, the ADC panel and the future REST API see one `devices::Mount` and never know
  which one is underneath.
- **Nothing extra to install where the protocol allows it.** A Meade, OnStep, iOptron, ZWO, Celestron or Sky-Watcher
  mount on a USB cable works with SlopCap alone.
- **Shared code above the driver:** threading, polling, pulses (native or emulated), slew rates, tracking rates,
  epoch conversion, safety, logging, reconnects and the test suite are written once. A driver only translates.
- **Small drivers:** what SlopCap needs from a mount is narrow (below), so most drivers are a few hundred lines.

What SlopCap needs from a mount: nudge the planet back (guide pulses), move and stop, go to (a planet), tracking on /
off and its rate (sidereal, lunar, solar, the planet's own), park / unpark, and where it points (recordings' `.txt`,
the ADC panel's altitude). The non-goals in PLAN.md §2 stand: no guiding, plate solving, or multi-star pointing models.

## 2. Coverage

| Mount family | Protocol (published by) | Wire | Built-in driver | Also through |
|---|---|---|---|---|
| Meade LX200 Classic | LX200 (Meade) | RS-232 | `lx200` (meade-classic) | INDI, ASCOM |
| Meade Autostar #494 / #497, Audiostar, Autostar II (LX200GPS, ACF, LX600, LX90) | LX200 + `:Mg` pulses (Meade) | RS-232, USB | `lx200` (meade) | INDI, ASCOM |
| OnStep / OnStepX | LX200 + extensions (open source) | USB, Bluetooth, Wi-Fi (TCP) | `lx200` (onstep) | INDI, ASCOM |
| 10Micron GM series | LX200 + extensions (10Micron) | RS-232, Ethernet (TCP) | `lx200` (10micron) | INDI, ASCOM |
| Losmandy Gemini 1 / 2 | LX200 + Gemini commands (Losmandy) | RS-232, USB, Ethernet | `lx200` (gemini) | INDI, ASCOM |
| ZWO AM3 / AM5 / AM7 | ZWO's LX200-based protocol (ZWO) | USB, Wi-Fi | `lx200` (zwo) | INDI, ASCOM |
| Pegasus NYX, Rainbow RST, Avalon StarGo | LX200 dialects (vendors) | USB, Wi-Fi | `lx200` (generic, own dialect if needed) | INDI, ASCOM |
| Astro-Physics GTOCP3 / 4 | LX200-like AP commands (AP) | RS-232, USB, Ethernet | `lx200` (astrophysics), later | INDI, ASCOM (APCC) |
| iOptron CEM / GEM / HEM / HAE, SkyHunter | iOptron command language (iOptron) | USB serial, Wi-Fi | `ioptron` (on the LX200 engine) | INDI, ASCOM |
| Celestron NexStar, CPC, CGEM, CGX, AVX, Evolution **through the hand controller** | NexStar HC (Celestron) | HC's USB / RS-232, SkyFi (TCP) | `nexstar` | INDI, ASCOM |
| Sky-Watcher **through a SynScan hand controller** | SynScan HC, NexStar-compatible (Sky-Watcher) | HC's serial / USB | `nexstar` (synscan) | INDI, ASCOM |
| Sky-Watcher **direct**: EQ6-R, HEQ5, EQ8, AZ-EQ5 / 6, AZ-GTi, Star Adventurer GTi | motor controller command set (Sky-Watcher) | USB on the mount, EQDIR cable, Wi-Fi (UDP 11880) | `skywatcher` (phase 5) | INDI (EQMod, AZ-GTi drivers), ASCOM (EQMOD, GSS) |
| Celestron **direct**: Evolution / CGX Wi-Fi, USB on the mount | AUX bus (reverse-engineered) | TCP, USB | `celestron-aux` (later) | INDI (Celestron AUX), ASCOM (CPWI) |
| Paramount, PlaneWave, Takahashi Temma, Vixen StarBook, Explore Scientific PMC-Eight, anything else | vendor | | none planned | ASCOM, Alpaca, INDI |

The LX200 engine carries most of the table: the protocols share `:…#` framing and differ in a few commands, so they
are dialect tables, not separate drivers. Celestron's and Sky-Watcher's hand controllers speak nearly the same
protocol, so they are one driver too.

**Per platform:**

| Backend | Windows | Linux | macOS |
|---|---|---|---|
| `indi` | to a server on another computer | yes | yes (INDI from `tools/macos-indi.sh`) |
| built-in (`lx200`, `ioptron`, `nexstar`, `skywatcher`) | yes | yes (`dialout` group) | yes (`/dev/cu.*`) |
| `sim` | yes | yes | yes |

## 3. Architecture

```
  MountSection · REST /v1/mount · recordings (.txt) · ADC altitude · session log
                    │  GUI thread: signals and a cached state; never blocks
           devices::Mount      façade: capabilities, pulses, moves, rates, epoch, safety, logging
                    │  queued commands ▼      ▲ MountStatus snapshots
           MountWorker         one thread per mount: poll loop, command queue (Stop jumps it)
                    │
           IMountDriver        small, synchronous, may block
           ├─ indi             IndiClient (exists)
           ├─ lx200, ioptron   LX200 engine + dialect tables ─ CommandChannel ─ Transport
           ├─ nexstar          hand-controller engine (Celestron, SynScan) ─ CommandChannel ─ Transport
           ├─ skywatcher       AxisMount ─ CommandChannel ─ Transport
           ├─ celestron-aux    AxisMount ─ CommandChannel ─ Transport
           └─ sim              AxisMount ─ simulated motors
                               Transport: serial · TCP · UDP
```

### 3.1 `devices::Mount`, the façade (GUI thread)

Keeps today's public API (`pulse`, `startMove` / `stopMove`, `goTo`, `abort`, `park` / `unpark`, `setTracking`,
`slewRates` / `setSlewRate`, `ra` / `dec` in JNow, the same signals), so `MountSection`, `main_window` and the tests
keep working. New: `open(spec)` / `close()`, `capabilities()`, `backend()` (for the panel title), `trackRate()` /
`setTrackRate()`, `sync()`, `pierSide()`, `altitude()`.

Written once here, for every backend:

- **Pulses.** Today's N/S and E/W bookkeeping (one pulse per axis, overlapping axes allowed, a timeout for drivers
  that never say a pulse ended). A driver with native pulse guiding gets the pulse; one without gets an **emulated**
  pulse: a move at the guide rate for that many milliseconds, timed on the worker thread with a steady clock, ended
  by a stop. That's how the LX200 Classic, the NexStar / SynScan hand controllers and the simulator get the N / E / W /
  S pad. On an alt-az mount the façade turns N / S / E / W into an alt / az move using the parallactic angle.
- **Slew rates:** a list of `{name, degrees per second if known}`, slowest first (the current contract). INDI gives
  names, LX200 has Guide / Centre / Find / Max, NexStar 1 to 9, OnStep and Sky-Watcher multiples of sidereal
  (0.25× to Max, 0.5× to 800×).
- **Epoch.** The façade speaks JNow, as `ephemeris` does. Drivers declare what they speak (the SynScan hand
  controller's J2000, 10Micron's setting, INDI's `EQUATORIAL_EOD_COORD`) and the façade converts with
  `ephemeris`'s `precess()` (made public).
- **Safety** (section 5), polling cadence, change-only signals, the 10 s retry after a failed connect (today's INDI
  behaviour, now for all), and logging: every command and reply at debug level (`mount[lx200]> :GR#  < 12:34:56#`),
  moves and pulses at info level as now, failures at warning.

### 3.2 `MountWorker` and `IMountDriver` (worker thread)

Serial calls block: an LX200 answers in tens to hundreds of milliseconds. So every mount runs on its own thread, and the capture thread and the GUI never wait
on one. INDI is asynchronous already but runs the same way so there is one path.

The worker polls every second when idle and every 250 ms while slewing, moving or pulsing. Drivers that hear about
changes (INDI, the simulator) can also wake it. Commands are queued in order, except Stop / abort, which goes to the
front and clears queued moves and gotos.

```cpp
// A mount driver. Runs on the mount's worker thread; calls may block. Throws MountError.
class IMountDriver {
public:
    virtual ~IMountDriver() = default;

    virtual MountInfo connect() = 0;                       ///< identify the mount, read its capabilities
    virtual void disconnect() = 0;                         ///< stops any motion first where the protocol allows
    virtual MountStatus poll() = 0;                        ///< coordinates, slewing, tracking, parked, pier side

    virtual void slewTo(const EqCoord& c) = 0;             ///< in info().epoch; tracks on arrival
    virtual void abort() = 0;                              ///< stop every motion; must work in any state
    virtual void moveAxis(Axis, double degPerSec) {}       ///< 0 stops that axis (caps.moveAxis)
    virtual void pulseGuide(GuideDir, int ms) {}           ///< caps.pulseGuide
    virtual void setTracking(bool on) {}
    virtual void setTrackRate(TrackRate) {}                ///< sidereal, lunar, solar, King (caps.trackRates)
    virtual void setRateOffsets(double raArcsecPerSec, double decArcsecPerSec) {}   ///< caps.rateOffsets
    virtual void park() {}
    virtual void unpark() {}
    virtual void sync(const EqCoord&) {}                   ///< caps.sync
    virtual std::optional<Site> site() { return {}; }
    virtual void setSite(const Site&) {}                   ///< caps.writeSite
    virtual void setTime(std::chrono::system_clock::time_point utc) {}   ///< caps.writeTime
};
```

- `MountInfo`: backend, vendor, model, firmware, epoch, alignment (German equatorial, fork / wedge, alt-az), guide
  rate, slew rates, capabilities.
- Capabilities follow ASCOM's `CanXxx` names (`pulseGuide`, `moveAxis`, `sync`, `park`, `unpark`, `setTracking`,
  `trackRates`, `rateOffsets`, `pierSide`, `readSite`, `writeSite`, `writeTime`). The panel and the API offer only
  what the mount can do.
- Units: hours and degrees, degrees per second for axis rates, arcseconds per SI second for rate offsets,
  milliseconds for pulses. Each driver converts, which is where the traps are: OnStep's rate offsets are arcseconds
  per sidereal second; Sky-Watcher rates are step periods.

### 3.3 Specs: `openMount()`, `listMounts()`

The same pattern as `openCamera(spec)`. One string names a mount, in Settings, on the command line (`--mount SPEC`,
`--list-mounts`) and in the API.

| Spec | Backend | Example |
|---|---|---|
| `indi[://HOST[:PORT]][/DEVICE]` | INDI; default `localhost:7624`, first telescope device | `indi://192.168.1.20:7624/LX200 Classic` |
| `lx200[-DIALECT]:PORT[@BAUD]` | built-in LX200 family | `lx200:COM3`, `lx200-onstep:/dev/ttyACM0`, `lx200:tcp://192.168.1.20:9999` |
| `ioptron:PORT` | built-in iOptron | `ioptron:/dev/ttyUSB0` |
| `nexstar:PORT`, `synscan:PORT` | built-in hand-controller driver | `nexstar:COM4`, `nexstar:tcp://192.168.1.20:4030` (SkyFi) |
| `skywatcher:PORT` | built-in motor controller | `skywatcher:COM5`, `skywatcher:udp://192.168.4.1` (the mount's own Wi-Fi) |
| `sim`, `sim-gem`, `sim-altaz` | simulated fork / German / alt-az mount | `sim-gem` |

`PORT` is a serial port (`COM3`, `/dev/ttyUSB0`, `/dev/cu.usbserial-…`) or `tcp://` / `udp://` `HOST:PORT`. Baud rates
default per protocol (9600 for LX200 and the hand controllers; Sky-Watcher tries 115200, then 9600). `lx200:PORT`
without a dialect asks `:GVP#` for the product name and picks the dialect from it; no answer means Meade Classic.

`listMounts()` (Settings → Mount → **Find mounts**) lists, without moving anything: serial ports with their USB
descriptions and the telescope devices on the configured INDI server. Probing a serial port (sending identify commands) happens only for the ports the user
picks (section 5).

### 3.4 ASCOM and Alpaca: not done

Dropped on 2026-11-08 (Dan: connected USB devices only). A built-in driver owns its serial port, so a mount that
NINA or PHD2 holds through its ASCOM driver is disconnected there first; OnStep's Wi-Fi takes a connection of its
own.

### 3.5 Built-in drivers: transport, command channel, dialects

**Transport:** `open` / `close` / `write` / `readUntil(terminator)` / `readCount(n)` / `drain`, blocking on the worker
thread. Three implementations: serial (Qt SerialPort; 8N1; DTR / RTS left low, because Arduino-based controllers
such as OnStep reset when DTR rises), TCP (`QTcpSocket`) and UDP (`QUdpSocket`, one datagram per command, for
Sky-Watcher Wi-Fi).

**`CommandChannel`:** one command at a time. Each command declares its reply: none, one character, up to a
terminator, a fixed length, or LX200's "`0`, or `1`/`2` + message + `#`". It drains stray input before each command,
times out, and **retries only reads**: a resent goto is a second goto. Every exchange goes to the session log at
debug level.

**LX200 engine.** A dialect is a table: how to recognise it, the command for each operation, reply formats and
quirks, each citing the vendor document and version it came from. The engine does the rest. Known differences:

| | Meade Classic | Meade Autostar / II | OnStep | 10Micron | Gemini | ZWO AM | iOptron |
|---|---|---|---|---|---|---|---|
| Pulse guide | none: emulated (`:RG#`, then `:Mn#` … `:Qn#`) | `:Mgd####` | `:Mgd####` | `:Mgd####` | | `:Mgd####` | `:MnXXXXX#` (5-digit ms) |
| Precision | `:U#` toggles it: read `:GR#` first, toggle only if short | as Classic | `:U#` as Meade | extended precision | | | own formats |
| Tracking off / on | | `:AL#` / `:AP#` (land / polar) | `:Td#` / `:Te#` | | | | |
| Park | none | `:hP#` | `:hP#` / `:hR#` | | | | |
| Identify | no `:GVP#` reply | `:GVP#` | `:GVP#`, `:GU#` status | `:GVP#` | | `:GVP#` | `:MountInfo#` |

Blank cells come from each vendor's command set when the dialect is written; nothing here is final until checked
against that document. Every dialect gets a fake (section 6) built from the same document.

**NexStar / SynScan hand-controller driver.** `e` (precise RA / Dec as 32-bit fractions of a turn), `r` (goto), `M`
(cancel), `L` (goto in progress), `t` / `T` (tracking mode: off, alt-az, EQ north, EQ south), `P` (pass-through to a
motor: fixed or variable-rate moves, then stop), `w` / `W` (location), `h` / `H` (time), `V` (version), `m` (model).
Celestron and SynScan can probably be told apart by the `V` reply (Celestron: two bytes; SynScan: six hex digits;
to confirm on both). There is no pulse command that works across hand-controller models, so pulses are emulated as
timed moves (ASIAIR does the same); check each model on hardware. Wi-Fi-to-serial bridges plugged into the hand
controller (SkyFi) pass this same protocol over TCP. Celestron's own Wi-Fi (the SkyPortal module, the Evolution)
speaks AUX instead: that's `celestron-aux`.

**Sky-Watcher motor controller (phase 5).** `:` + command letter + axis (`1` RA / az, `2` Dec / alt) + 24-bit hex in
little-endian byte order; replies `=` + data + CR, or `!` + error code + CR. Counts per revolution (`:a`), timer
frequency (`:b`), position (`:j`), status (`:f`), motion mode (`:G`), step period (`:I`), start / stop / instant stop
(`:J` / `:K` / `:L`), goto target (`:S`). The controller knows steps, not the sky, so this driver sits on
`AxisMount`.

### 3.6 `AxisMount`: the pointing core for direct drivers

For drivers that see only motor axes (Sky-Watcher direct, Celestron AUX) and for the simulator:
- axis angles ↔ hour angle / Dec for German (pier side, meridian flip), fork / wedge and alt-az mounts, from the site
  and sidereal time (`ephemeris` has GMST; made public as local sidereal time);
- tracking: equatorial mounts run the RA axis at the chosen rate; alt-az mounts recompute both axis rates every
  second (no field derotation: planetary captures are short);
- goto: both axes, then a slow final approach from a fixed direction;
- home and park positions, limits (horizon, meridian, counterweight-up);
- one-star sync as an offset; no multi-star pointing model (non-goal).

### 3.7 OnStep's own driver (`onstep`)

Added 2026-11-07. OnStep was an LX200 dialect (still there for `lx200:` when the mount says it's OnStep); `onstep:`
and `lx200-onstep:` now get `OnStepDriver` (`src/mounts/onstep.*`), written from OnStepX's command handlers
(`src/telescope/mount/*/*.command.cpp`, `src/libApp/commands/ProcessCmds.cpp`, main at 2026-11-02) and checked
against an OnStepX 10.25a (JTW Trident P75, ESP32, CP2102 at 230400 baud). What a good OnStep connector has to do,
and where each is:

| Need | Why | How |
|---|---|---|
| Find the speed | OnStepX defaults to 9600 baud, ESP32 builds often run at 115200 or 230400 (the P75: 230400) | `onstep:PORT` tries 9600, 230400, 115200, 57600, 38400, 19200 (`:GVP#`, noise is skipped); `@BAUD` or `tcp://` don't |
| Don't reset the controller | DTR / RTS edges reset an Arduino or put an ESP32 in its boot loader | the serial class keeps both low (unchanged) |
| Read replies the way OnStepX frames them | set commands answer a bare `0` / `1`, unknown commands a bare `0`, `:MS#` one digit, all without `#` | every command reads its own reply shape; stray bytes are drained before each command. (The ASCOM OnStep driver's `CommandString(raw)` reads to `#`: a `:Sr…#` through it left its replies one behind until NINA reconnected the mount.) |
| Say who's on the port | the ASCOM driver holds the port while NINA or PHD2 is connected | "Access is denied" / "busy" adds: disconnect there, or use the Wi-Fi |
| Precision | `:GRH#` / `:GDH#` give HH:MM:SS.SSSS where the firmware has them (10.25a does) | asked once at connect; else `:U#` as for Meade |
| One status read per poll | `:GU#` has tracking, goto, park state, at home, homing, guiding, mount type, pier side, the pulse-guide and guide rate indices and the general error | `parseOnStepStatus()`; three reads per poll (`:GRH#`, `:GDH#`, `:GU#`) instead of five |
| Faults in words | the last `:GU#` character is `Limits::errorCode()`: motor fault, horizon / overhead / meridian / under-pole limits, NV and site init | `MountStatus::fault`, said once by the worker when it appears, shown in the panel |
| Refusals with reasons | `:MS#` codes, `:CM#` `E<n>`, and `:GE#` (last command error) after a `0` from `:Te#`, `:hP#`, `:hR#` | "the mount couldn't start tracking: it is parked" |
| OnStep's ten speeds | `:R0#`..`:R9#` (0.25×, 0.5×, 1×, 2×, 4×, 8×, 20×, 48×, half max, max: named as the other drivers name speeds) | `MountInfo::rates` |
| Leave PHD2's guide rate alone | the rate a move sets (`:Rn#`) is what guide pulses go at, and with GUIDE_SEPARATE_PULSE_RATE `:R0#`..`:R2#` are also stored as the pulse-guide rate | after the moves end the driver sends back the rates `:GU#` gave before them (the pulse-guide rate, then the guide rate unless it was custom) |
| A goto right after a move | the mount is still slowing down and answers `8` (already moving) | asked again every 0.5 s for 6 s |
| Tracking that really tracks | after `:ERESET#` the tracking rate is 0 and `:Te#` still says `1` | `:GT#` checked after `:Te#`; 0 → the chosen rate (`:TQ#`, `:TL#`, …) and `:Te#` again, else an error saying so |
| A planet's own motion | `:SXTR,n#` / `:SXTD,n#`: rate offsets in arcseconds per sidereal second (±1800) | `IMountDriver::setRateOffsets()` (arcsec per SI second), `Caps::rateOffsets` from `:GXTR#`; `Mount::setRateOffsets()`. The panel doesn't offer it yet (§4, planet-rate tracking) |
| Clock and site | gotos land 15″ off per second of clock error; OnStep's `:GG#` offset doesn't follow daylight saving | at connect, `:GC#` + `:GL#` + `:GG#` against the computer's UTC (an hour out: "daylight saving?"), `:Gt#` / `:Gg#` (west positive) against Settings → Site; `MountInfo::notes`, said once. Read only: the mount's own app sets them (§5.5) |
| Limits and setup for the log | horizon `:Gh#`, overhead `:Go#`, meridian `:GXE9#` / `:GXEA#`, mount type, pulse-guide rate | `MountInfo::details`, on the "mount: … is …" log line |
| Home | the P75 starts at home (pole, counterweights down) | `OnStepDriver::home()` (`:hC#`), `MountStatus::atHome` |
| Its settings, without the web server | site, elevation, UTC offset, date and time; tracking rate and compensation (`:Tn#` / `:Tr#` / `:To#`, `:T1#` / `:T2#`); horizon, overhead and meridian limits (`:Sh`, `:So`, `:SXE9`, `:SXEA`); automatic flips, preferred pier side, straight across or through home (`:SX95`, `:SX96`, `:SX94`; before OnStepX 10.28 only waiting at home or not, `:SX98`, whose 0 means through home on the newer firmware); guide pulse and goto speeds (`:R0#`..`:R2#`, `:SX93`); backlash (`:$BR`, `:$BD`); buzzer (`:SX97`); go home, park here (`:hQ#`) | `IMountDriver::settings()` / `changeSetting()` (`MountSetting`: number, choice, toggle, text, info, action), `Caps::settings`; Mount → **Settings and log…** reads them when it opens and writes only the ones changed, on **Apply**, then reads them back. "Set date, time and UTC offset from this computer" fixes the daylight-saving hour. Left to the web server on purpose: axis wiring (steps, reverse, drivers), mount type, PEC, restarts: a wrong one there runs a motor the wrong way |
| A log of everything | errors were in the footer for a moment, commands only at debug level | `mounts::MountLog`: every command and reply (`CommandChannel`) and every error, fault and note (`MountWorker`), the last 5000, whatever the log level; the Log tab (status polls hidden, errors only, copy); the panel shows the latest error in red until the log is opened |

Not done: Send site and time (a button, §4), park position set (`:hQ#`), the rotator / focuser / auxiliary features
OnStepX also carries, `:Gu#` (the bit-packed status), sending commands through the ASCOM driver (it frames commands
its own way, see above), and OnStep's checksum framing.

**On the P75 (2026-11-07, `slopcap-mountprobe onstep:/dev/ttyUSB0 --move`, the CP2102 passed to WSL with
usbipd):** found at 230400 baud on the second speed; high precision and rate offsets present; clock and site agreed.
Tracking on, pulses each way, 8x moves south and north (215.9″ and 215.5″), half-max moves east and west, a goto
10° south (to Dec +79°00′00″, after the driver waited out the deceleration), home (`:hC#`), tracking off; `:GU#`
rates afterwards as before. The pulse distances weren't consistent: at the pole a 1 s south pulse moved 72″ with the
guide rate at 8x, and about 2″ a second at 0.25x and at 2x. That doesn't fit either reading of
GUIDE_SEPARATE_PULSE_RATE, and it isn't backlash: `:%BD#` says 0″. Still to explain.

`tools/mountprobe` (`slopcap-mountprobe SPEC [--move] [--log]`, or `PORT --send CMD …` for raw commands) is the
§6 tool, for any built-in driver.

## 4. Above the driver: what changes in the app

- **Mount panel:** same layout and pad. The title says the backend ("MOUNT · LX200 CLASSIC"); offline it
  shows the spec and its own **Connect** (today's button connects the focuser's INDI server). It adds a tracking
  rate picker (Sidereal / Lunar / Solar / *the planet*) where the mount supports it, and pier side in the state line.
- **Settings:** "Focuser and mount (INDI)" becomes **Focuser (INDI)** and **Mount**. Mount has the connection (found
  mounts plus free text), Find mounts…, connect at start, and **Send site and time
  to the mount** (a button, never automatic). Migration: with no `mount/spec` saved, the spec is
  `indi://` + `focuser/server`, which is today's behaviour.
- **INDI:** the mount opens its own client connection on its thread instead of sharing the focuser's (INDI servers
  take many clients). The focuser's "skip mounts" rule stays.
- **Recordings' `.txt`:** RA / Dec (JNow) as now, plus pier side, altitude and the backend when known.
- **Planet-rate tracking**, new and for every backend at once: rate offsets from the change in
  `ephemeris::bodyPosition` over a minute, sent through `setRateOffsets` (INDI
  `TELESCOPE_TRACK_RATE`, OnStep's custom rate, `AxisMount` directly). Mars near opposition
  moves about 1″ a minute against the stars, about 13 px a minute at 0.0785″/px.
- **Later, same façade:** a `remote` driver that carries mount commands over the remote-viewing protocol (one more
  message type), so a watching SlopCap can nudge the mount of the one that shares the camera; the REST API's
  `/v1/mount` (PLAN.md §7); and the focuser on the same layers (the ZWO EAF through ZWO's SDK, the Gemini EAF's
  MyFocuserPro2 serial protocol on `CommandChannel`), so no focuser needs INDI.

## 5. Safety (every backend, enforced in the façade)

1. **Stop always reaches the mount first.** Stop / abort jumps the worker's queue and clears queued moves and gotos.
   Each driver's `abort()` works in any state: LX200 `:Q#`,
   NexStar `M` plus a stop to both motors, Sky-Watcher `:K` on both axes.
2. **Moves have a deadman.** A continuous move stops after 10 s unless the caller renews it (a future press-and-hold
   pad renews while held). A frozen GUI or a dropped network link can't leave the mount slewing into the pier.
3. **A lost link mid-motion:** on reconnect, if a move or pulse was in flight, the first command is a stop. The
   built-in drivers send a stop before closing the port.
4. **Goto checks:** refused while parked; refused below the horizon limit (Settings → Site, default 10°) when the
   site is known; refused on `AxisMount` drivers without a site, since they can't know where anything is. The
   mount's own limits stay in force.
5. **No silent writes** of site, time, park position or alignment: buttons only, each logged.
6. **No serial probing on its own.** Find mounts sends bytes only to the ports the user picks, never to the
   focuser's port. The Gemini EAF, OnStep and many focusers are Arduinos that reset when a port is opened, and a
   stranger's protocol can move a motor.

## 6. Tests: one suite, every backend

- **`SimMount`** (`AxisMount` over a simulated motor controller, with a clock that can run fast) is the `sim`
  backend, and the device model behind every fake.
- **Fakes speak the real wire protocols** over an in-memory transport pair, on top of `SimMount`: `FakeLx200` (one
  per dialect), `FakeIoptron`, `FakeNexStar` (Celestron and SynScan), `FakeSkyWatcherMc` (serial and UDP framing) and
  `FakeOnStep` (OnStepX's replies). A driver's test runs its real wire code end to end, as
  `tests/fakes` does for the camera SDKs.
- **`mountConformance(spec)`**, the same test body for every backend: connects and identifies; coordinates sane;
  tracking on / off; N / S and E / W pulses move the right way by the right amount (±30 %); a move then a stop;
  goto, then tracking; abort mid-goto stops within 1 s; park / unpark; a transport dropped mid-move leaves the mount
  stopped; and the capabilities match what the fake declares. A mount without pulse guiding gets emulated pulses
  that must pass the same checks.
- **Run against:** `sim`, `sim-gem`, `sim-altaz`, every fake, INDI's `indi_simulator_telescope` (as today, skipped
  without INDI).
- **`slopcap-mountprobe`** (`tools/`, like `camprobe`): opens a spec, runs a read-only command list (identify,
  coordinates, site, time, rates, park state) and writes a fixture; `--move` adds a 1 s pulse each way. Fixtures from
  real mounts check the fakes' reply formats against hardware.
- **Hardware acceptance**, a table per family (date, mount, firmware, spec, result), as for cameras. First the
  LX200 on the wedge: built-in `lx200` from the laptop with no INDI running, INDI as today. Families without a mount to try are "built to the published protocol, tested against
  fakes, not yet on hardware" (as the Mac build is "not tested with hardware").
- The 90 % coverage gate covers the new code like the rest of `src/`.

## 7. Build: still one executable

- **Qt SerialPort** for the serial transport and for port listing with USB ids: `qt6-serialport-dev` in the Linux
  image and install docs, already in Homebrew's Qt on macOS, and `qtserialport-everywhere-src` added to
  `docker/Dockerfile.mingw`'s static Qt build, so Windows still gets one self-contained `slopcap.exe`. (The
  alternative is our own Win32 / termios serial class, about 250 lines, without the USB descriptions.)
- **TCP, UDP:** Qt Network, already linked.
- Every backend is always built (there's no vendor SDK to find, unlike the cameras).

## 8. Phases

Each phase leaves the app working; INDI users see no change until they pick another backend.

| Phase | What | Estimate | Done when |
|---|---|---|---|
| 1 | `IMountDriver`, `MountWorker`, the façade; INDI moved behind it; specs, `--mount`, Settings split; `SimMount` / `AxisMount` basics; the conformance suite on `sim` and INDI | 3–4 days | today's mount tests pass unchanged, plus the suite on `sim` |
| 2 | (ASCOM over COM and Alpaca: dropped 2026-11-08, connected USB devices only) | | |
| 3 | Transports, `CommandChannel`, the LX200 engine and dialects (Meade Classic / Autostar / II, OnStep, 10Micron, Gemini, ZWO, generic) and iOptron; serial in Find mounts; `mountprobe`; Qt SerialPort in all three builds | 5–7 days | the LX200 from the laptop with no INDI server; every dialect passes against its fake |
| 4 | NexStar / SynScan hand-controller driver, emulated pulses, SkyFi over TCP | 2–3 days | passes against `FakeNexStar` in both dialects |
| 5 | Sky-Watcher motor controller on `AxisMount`: serial, USB, Wi-Fi; equatorial and alt-az; tracking, goto, park, flip limits | 6–9 days | passes against `FakeSkyWatcherMc`, then on a borrowed mount |
| 6 | Celestron AUX; planet-rate tracking; go to a planet; sync on the centred planet; mount control from a remote viewer; `/v1/mount` | as wanted | |

## 9. Open decisions

1. **Which mounts can we test on?** Only the LX200 here so far. Do untested families ship visible and marked
   "untested", or behind a setting until someone tries one?
2. **Qt SerialPort or our own serial class** (section 7). Recommended: Qt SerialPort.
3. **Is phase 5 worth 6–9 days?** Sky-Watcher is the most common brand, and their hand-controller and INDI routes
   already work through phase 4, but the direct route is the only one without extra hardware or
   software for AZ-GTi-class mounts. Recommended: yes, after phase 4.
4. **Defaults:** horizon limit 10°, move deadman 10 s?
5. **Sync and Send site / time in the first release**, or later?
