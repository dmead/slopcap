# A night at the telescope

This guide walks through a planetary night with SlopCap: connecting the camera, getting a good image, collimating,
focusing, and recording. It assumes a colour or mono astronomy camera (ZWO, QHY, Player One, ToupTek or MallinCam)
and, optionally, an electronic focuser that INDI can drive and a mount (through INDI, or one SlopCap drives itself:
section 8).

## The screen

![SlopCap's main window, replaying a recording of Jupiter](images/jupiter-capture.png)

- **Top bar:** the mode tabs (**Capture**, **Focus**, **Collimate**, **ADC**; *Align* and *Sequence* aren't finished yet), the
  **Target** and **Filter** for naming recordings, **⇄ Remote** for [remote viewing](remote-viewing.md), the
  **camera pill** (which camera is connected; click it to connect or disconnect), and the gear for Settings.
- **Left: the camera.** Gain and exposure, region of interest (ROI), 8 or 16 bit, readout mode, and the capture
  options with the big **Record** button at the bottom.
- **Middle: the live view.** The toolbar above it sets the zoom (Fit, 100 %, 200 %), the stretch (Linear or Auto),
  colour (Debayer, Auto WB) and overlays (Target box, Reticle), and has **Full screen**. Under it: the histogram, live
  sharpness and dispersion numbers, and the frame size.
- **Right: what the telescope is doing.** The target SlopCap thinks it sees, the focuser, the mount, and a raw
  histogram (the camera's real numbers, per colour channel, before any stretch).
- **Bottom bar:** frame rate at the camera, frames dropped (should stay at 0), recording speed and free disk space.

## 1. Connect the camera

Click the camera pill at the top right. The list shows every camera that's plugged in, by maker (`asi:0 ASI585MC`,
`qhy:0 QHY533M …`, `playerone:0 Uranus-C`, `touptek:0 …`, `mallincam:0 …`). Pick yours. SlopCap remembers the camera
and its settings for next time.

No camera to hand? **Replay a recording…** in the same menu plays an existing `.ser` file as if it were a camera,
in real time. Everything below works on a replay, which is a good way to learn SlopCap indoors.

## 2. Get a good image

- **Exposure** is in milliseconds; short exposures (a few ms) freeze the seeing. The camera's maximum frame rate is
  shown under the slider.
- **Gain:** the buttons under the gain slider are the camera's own landmarks: **Unity** (one electron per step),
  **Low noise** (lowest read noise), **Max DR** (most dynamic range), **HCG** (where the high-conversion-gain mode
  switches on). They're a good place to start.
- **ROI:** a planet only fills a small part of the sensor. A smaller ROI (the size list, **Center**) means a faster
  frame rate and smaller files. Right-click the planet in the live view and choose **Centre ROI here** to move the ROI
  onto it.
- **8-bit or 16-bit:** 8-bit is faster and is fine for planets; 16-bit keeps the camera's full range (the Moon, the
  Sun, faint moons).
- **All controls…** has the rest: white balance, USB traffic, offset, and whatever else the camera offers.

Watch the histogram: the **clipped %** figure under it should be zero or close to it on the bright parts of the planet.
The live view's stretch doesn't change the recording; it's only for your eyes.

In the live view: the mouse wheel zooms, dragging pans, **F** fits the image to the window, **1** shows it pixel for
pixel, **C** toggles a crosshair.

## 3. Collimate (Collimate tab)

Do this on a bright star, before focusing, whenever the telescope has been moved or handled.

1. Put a bright star in the middle of the view and defocus until it's a ring 40–100 pixels across.
2. Tick **Analyse the live view.** SlopCap measures how far the shadow of the secondary mirror sits from the centre of
   the ring, as a percentage of the ring's radius, and draws an arrow.
3. Turn the secondary screw toward the arrow (the ring's thin side), a little at a time. Re-centre the star after
   each turn.
4. Stop when the offset is under 2 % (the dashed line on the chart), which is good for most Schmidt-Cassegrains.

## 4. Focus (Focus tab)

With an electronic focuser, SlopCap finds the best focus for each colour separately: on a colour camera the red,
green and blue images don't come to focus at exactly the same place, and SlopCap tells you by how much.

1. **Connect the focuser:** the Focuser panel's **Connect** uses the INDI server in Settings (normally
   `localhost:7624`). In Settings, set the focuser's **backlash** (about 600 steps for a Gemini EAF) and the direction
   every move should finish in, so that the final approach always takes up the slack the same way. The first move
   after connecting overshoots even toward that direction, since which way the gears sit isn't known yet. If the
   focuser's USB drops out, SlopCap reconnects it by itself once it's back.
2. **Watch the live sharpness bars** (R, G, B) while you nudge the focuser with the ± step buttons; the tick on each
   bar is the best seen so far. **STOP**, above them, halts the focuser at once (and ends a sweep where it is). The
   focuser's position, STOP and step buttons are on every tab (Capture and Focus in the side column, Collimate and
   ADC at the bottom of the right one), and all of them work the same focuser.
3. **Run a sweep:** **Run RGB focus sweep →** (or the Focus tab) opens the sweep. **Coarse ±1500** or **Fine ±350**
   sets the range around the focuser, and the range then follows the focuser wherever it goes, so it's always centred
   on where you are now. Set From or To yourself and the range stays put until you pick a preset again. Then **Run
   sweep**. SlopCap moves the focuser through the range, scores every frame and fits a curve per colour. If the
   sharpest position is still at one end when the range is done, the sweep **carries on past that end**, 3 positions
   at a time (up to twice the range again), until the curve comes down on both sides of its peak. Then it **goes to
   the best focus** for the colour chosen under Focus on (both are check boxes under the settings; without them the
   sweep covers exactly the range and stops at its last position).
4. **Read the result:** the chart shows sharpness against position for each colour; the table gives each colour's
   best position and how far it is from green. Choose what to focus on (**Green**, **Luminance-weighted**,
   **Balanced**, **Red** or **Blue**) and press **Go to**. Several sharpness measures are computed on the same frames;
   **Which metric is tightest** shows which one agreed with itself best on this sweep.

The camera stays in view on the Focus tab: the top of its left column shows the target close up, or the whole frame
with the box focus is scored in (**Target** / **Frame**); double-click it for the full preview. The other way round,
while a sweep runs (and after it), its curves show small in a corner of the preview on the other tabs: click them for
the Focus tab, or × to hide them until the next sweep.

With the focal ratio and the focuser's microns per step entered in Settings → Optics, the chart also shows the
critical focus zone: how far you can be from perfect focus before it shows. Each sweep's numbers are saved as a CSV
file in the recording folder.

## 5. Line up the colours (ADC tab)

Low in the sky the air acts like a weak prism: it lifts a planet's blue image a little above its red one, so the
planet has a blue fringe on top and a red one underneath, and the detail smears. An **ADC** (atmospheric dispersion
corrector: two prisms with levers, between the telescope and the camera) pushes the colours back together. The ADC
tab measures how far the red image sits from the blue one, live, with a colour camera.

![The ADC tab on Jupiter before correcting: red and blue 2.2 px apart](images/jupiter-adc.png)

1. Centre the planet and set both ADC levers to **zero**.
2. Press **Remember uncorrected.** The dashed line on the plot is now the direction the ADC has to correct along.
3. Rotate the whole ADC in its holder so the levers are **level with the horizon**.
4. Spread the levers evenly, a little at a time, until the arrow over the planet (red minus blue, drawn ten times
   longer than it is) shrinks to nothing and the reading turns green.

The status line says what to do next. **Under-corrected** means spread the levers further. **Over-corrected** means
red and blue have crossed, so close them a little. **The ADC is turned** means what's left points across the dashed
line, so rotate the ADC itself, not the levers. Correct again every half hour or so as the planet climbs or sinks.

With **Settings → Site** and a planet in **Target** (or a connected mount), the tab also shows how much dispersion the
air causes at the planet's altitude right now. With **Settings → Optics → Focal length**, it shows the readings in
arcseconds as well as pixels. Uncorrected, the measured dispersion should come close to that figure; well over it
usually means the focal length in Settings is wrong.

## 6. Name the target

The **Target** list names your recordings. Leave it on **Auto** and SlopCap works out what it's looking at from the
image and the clock: Saturn by its rings, the Moon by its shape at tonight's phase, Mars, Jupiter, Uranus and Neptune
by size, colour and how dark the edge of the disc is. It never asks the mount. The Target panel says what it decided
and why; when it can't decide, it says what's left ("Moon or Jupiter") and recordings are named "Target" until it can.

Two settings help it a lot: **Settings → Optics → Focal length** (with the camera's pixel size this gives the image
scale, so a planet's size in pixels means something) and **Settings → Site** (latitude and longitude, so it knows
what's above the horizon). South and west are negative.

Pick the target yourself when you know better; the name can't change during a recording either way. **Filter** adds
the filter to the file name (L, R, G, B, IR, CH4, UV): `2026-10-17-0315_4-L-Saturn.ser`.

## 7. Record

Press **Record** (Ctrl+R, Cmd+R on a Mac). Press again to stop, or set a **Limit** (30 s to 10 min) and SlopCap stops
by itself. Use **Folder…** to choose where recordings go.

Each recording is a SER video named like WinJUPOS expects, from the time at the middle of the recording and the
target: `2026-10-17-0315_4-Saturn.ser`. Next to it, a `.txt` file lists the camera settings, the focuser position and
the mount's coordinates. Every frame carries its own timestamp.

Keep an eye on **dropped** in the bottom bar. Frames dropped by the camera usually mean the USB link is busy (lower USB
traffic, or a smaller ROI); frames dropped by the disk mean the disk can't keep up (a faster disk or a smaller ROI).

## 8. The mount (optional)

The **Mount** panel on the right shows where the telescope points. **N**, **E**, **W** and **S** send guide pulses
of the chosen length to nudge the planet back into the ROI; **Stop** halts every motion. Two ways to reach the mount:

- **Through INDI**, with a mount driver running on the same INDI server as the focuser (for a Meade LX200,
  `indi_lx200classic`). Leave Settings → Mount empty.
- **Directly**, with nothing else installed: SlopCap has its own drivers for the common protocols. Put the mount in
  Settings → Mount (and tick Connect at start), or start SlopCap with `--mount SPEC`:

  | Mount | Spec |
  |---|---|
  | Meade LX200, Autostar, Audiostar, LX200GPS, or OnStep, on a serial or USB cable | `lx200:COM3`, `lx200:/dev/ttyUSB0` (it asks which) |
  | OnStep over its Wi-Fi | `onstep:tcp://192.168.0.1` (port 9999 unless you add `:PORT`) |
  | Celestron, through the hand controller | `nexstar:COM4` |
  | Sky-Watcher, through the SynScan hand controller | `synscan:COM4` |
  | Sky-Watcher on its own motors: the mount's USB port, an EQDIR cable, or its Wi-Fi | `skywatcher:COM5`, `skywatcher:udp://192.168.4.1` |
  | Sky-Watcher through the SynScan app (phone, tablet or PC) | `synscan-app` (this computer) or `synscan-app:udp://HOST` |
  | A simulated mount, to try things out | `sim` |

  On a Mac the serial port is `/dev/cu.usbserial-…` (or similar). Serial ports take `@BAUD` when the mount isn't at
  its usual speed (`lx200:COM3@19200`). `lx200` asks the mount which kind it is; `lx200-classic`, `lx200-meade` or
  `lx200-onstep` skip the question. `slopcap --list-mounts` prints the specs and the computer's serial ports. The port
  belongs to SlopCap while it's connected, so close any other program using it (an INDI server, by contrast, can be
  shared). Mounts without guide pulses of their own (the LX200 Classic, the
  hand controllers) get timed slow moves instead. A Sky-Watcher on its own motors needs Settings → Site to know where
  it points, starts from its home position (pointing at the pole, counterweights down), and a sync on a known object
  puts it right. These drivers are written from the makers' published protocols and tested against simulations of
  them, but none has been tried on a real mount yet; through INDI, the LX200 has.

## Afterwards

- Stack the recordings as usual (AutoStakkert!, PSS…). The `.txt` files keep what you used.
- `slopcap-diag --png OUT/ *.ser` stacks the best frames of each recording and reports how good the optics and the
  night were: colour dispersion, edge sharpness per colour and side, halo, jitter.
- Every run of SlopCap writes a **session log** of the whole night; see [Files, settings and logs](files-and-logs.md).

## Keyboard and mouse

| Key | Does |
|---|---|
| Ctrl+R (Cmd+R) | start / stop recording |
| F11 (Mac: Ctrl+Cmd+F) | full view: the live view over the whole screen; Esc leaves |
| F | fit the image to the window |
| 1 | 100 %: one camera pixel per screen pixel |
| C | crosshair on / off |
| mouse wheel / drag | zoom / pan the live view |
| right-click | **Centre ROI here** |

## Starting SlopCap from a command line

Everything is in the window, but SlopCap can also be started ready to go, which is handy for scripts and shortcuts:

| Option | Does |
|---|---|
| `--camera SPEC` | connect at start: `asi`, `asi:1`, `qhy`, `playerone`, `touptek`, `mallincam`, `sim:FILE.ser` (replay), `sim-loop:FILE.ser` (replay forever) |
| `--out DIR` | recording folder |
| `--record` | start recording as soon as the camera is connected |
| `--focuser HOST:PORT` | connect the focuser through this INDI server |
| `--sweep` | with `--focuser`: run a focus sweep once the focuser is ready (`--sweep-range STEPS`, `--sweep-step STEPS`, `--sweep-frames N`) |
| `--mount SPEC` | drive the mount with SlopCap's own driver ([section 8](#8-the-mount-optional)) |
| `--list-mounts` | print the mount specs and this computer's serial ports, then quit |
| `--tab capture` / `focus` / `collimate` / `adc` | open on this tab |
| `--serve`, `--connect HOST` / `auto` | share the camera, or watch another SlopCap ([remote viewing](remote-viewing.md)) |
| `--fullscreen` | start in full view |
| `--window WxH` | window size |

`slopcap --help` lists every option, including the ones for scripts and tests.
