# Installing SlopCap

SlopCap needs a graphics card that can do OpenGL 3.3 for its live preview (any computer from the last ten years). It
doesn't need one to record: without it the preview stays blank and says why, and capture still works.

## Windows

SlopCap for Windows is a single file, `slopcap.exe`. There is no installer and nothing else to copy: Qt and the ZWO
and QHY camera drivers are built into it.

1. Copy `slopcap.exe` anywhere, for example `C:\Users\<you>\slopcap\`.
2. Double-click it.

The first time a camera is used, SlopCap unpacks the camera drivers to `%LOCALAPPDATA%\SlopCap\SlopCap\sdk\`. To try a
newer ZWO or QHY driver, put its `ASICamera2.dll` or `qhyccd.dll` next to `slopcap.exe`; a file there wins.

The camera itself still needs the vendor's USB driver (the one SharpCap and FireCapture use too). If the camera works
in SharpCap, it works in SlopCap.

**Remote viewing** needs SlopCap allowed through the Windows firewall on the computer that shares its camera. Windows
asks the first time you share; choose *Private networks*. See [Remote viewing](remote-viewing.md) for the command that
does it by hand.

**The focuser and mount** go through INDI, which is Linux and macOS software. On Windows, SlopCap can use an INDI
server running on another computer (Settings → INDI server). ASCOM support is planned.

## Linux (Ubuntu)

SlopCap is built from source on Linux (see [Building and testing](developing.md) for the details):

```bash
sudo apt install build-essential cmake ninja-build catch2 qt6-base-dev fonts-ibm-plex
sudo add-apt-repository ppa:mutlaqja/ppa && sudo apt install indi-3rdparty-libs indi-bin   # cameras, focuser, mount
cmake --preset linux-release && cmake --build --preset linux-release
build/linux-release/slopcap
```

The INDI packages bring the ZWO and QHY camera drivers and their USB permission rules, so cameras work without root.
They also bring `indiserver` and the device drivers for the focuser and the mount. A typical setup runs `indiserver`
as a user service that starts with the computer, for example for a Gemini EAF focuser:

```bash
indiserver indi_myfocuserpro2_focus indi_lx200classic
```

Add yourself to the `dialout` group (`sudo usermod -aG dialout $USER`, then log in again) so INDI can open the
focuser's and mount's serial ports.

## macOS

SlopCap for Mac is `SlopCap.app`, usually inside `SlopCap.dmg`.

1. Open the `.dmg` and drag **SlopCap** onto **Applications**.
2. The first time, right-click SlopCap in Applications and choose **Open**, then **Open** again. (SlopCap isn't
   notarized by Apple, so a plain double-click is refused the first time. *System Settings → Privacy & Security →
   Open Anyway* does the same.)
3. The first time SlopCap shares a camera, watches one, or talks to an INDI server on another computer, macOS asks
   whether it may use the local network. Choose **Allow**.

The app needs the macOS version it was built on, or newer.

**Cameras:** ZWO and QHY drivers are inside the app. QHY cameras on a Mac start up without their firmware: SlopCap
sends it when it starts, and the camera then reconnects by itself, which takes a few seconds. Wait a moment after
launching before you open the camera list.

**Focuser and mount:** there's no ready-made INDI package for macOS, so INDI is built from source once with
`tools/macos-indi.sh` from the SlopCap source (it installs into `~/indi`). Then, for a Gemini EAF:

```bash
PATH=~/indi/bin:$PATH indiserver indi_myfocuserpro2_focus
```

macOS has the driver for the EAF's USB serial chip built in. The port appears as `/dev/cu.usbserial-…`; tell the
driver once which one it is:

```bash
ls /dev/cu.*                                                            # find the port
~/indi/bin/indi_setprop 'MyFocuserPro2.DEVICE_PORT.PORT=/dev/cu.usbserial-1420'
~/indi/bin/indi_setprop 'MyFocuserPro2.CONFIG_PROCESS.CONFIG_SAVE=On'   # remember it
```

To build the app yourself, see [Building and testing](developing.md).
