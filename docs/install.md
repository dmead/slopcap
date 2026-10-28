# Installing SlopCap

SlopCap needs a graphics card that can do OpenGL 3.3 for its live preview (any computer from the last ten years). It
doesn't need one to record: without it the preview stays blank and says why, and capture still works.

## Windows

SlopCap for Windows (64-bit Windows 10 or 11) comes as `slopcap-windows-x64.zip`, and the program in it is a single
file, `slopcap.exe`. There is no installer and nothing else to install: Qt and the camera drivers (ZWO, QHY, Player
One, ToupTek, MallinCam) are built into it.

1. Unzip it anywhere, for example to `C:\Users\<you>\slopcap\`.
2. Double-click `slopcap.exe`. The first time, Windows may say it protected your PC from an unknown app (SlopCap isn't
   signed): choose **More info**, then **Run anyway**.

The first time a camera is used, SlopCap unpacks the camera drivers to `%LOCALAPPDATA%\SlopCap\SlopCap\sdk\`. To try a
newer driver, put its DLL next to `slopcap.exe` (`ASICamera2.dll`, `qhyccd.dll`, `PlayerOneCamera.dll`, `toupcam.dll`
or `mallincam.dll`); a file there wins. An exe you build yourself without the drivers looks for them there too:
SharpCap's folder has them all (it calls the last two `ToupCam.dll` and `MallinCam.dll`; either name works).

The camera itself still needs the vendor's USB driver (the one SharpCap and FireCapture use too). If the camera works
in SharpCap, it works in SlopCap.

**Remote viewing** needs SlopCap allowed through the Windows firewall on the computer that shares its camera. Windows
asks the first time you share; choose *Private networks*. See [Remote viewing](remote-viewing.md) for the command that
does it by hand.

**The focuser** goes through INDI, which is Linux and macOS software. On Windows, SlopCap can use an INDI server
running on another computer (Settings → INDI server). **The mount** can too, or SlopCap drives it itself over its COM
port or the network (see [the mount](user-guide.md#8-the-mount-optional) in the user guide).

## Linux (Ubuntu)

**From the zip:** `slopcap-linux-x64.zip` runs as it is, with Qt and the camera drivers inside, on Ubuntu 26.04 or
newer (or another Linux with glibc 2.43 or newer). Unzip it and run `slopcap-linux-x64/slopcap`. The INDI packages below
still bring the cameras' USB permission rules and QHY's firmware (the zip's `udev/` folder has the rules alone).

**From source** (see [Building and testing](developing.md) for the details):

```bash
sudo apt install build-essential cmake ninja-build catch2 qt6-base-dev fonts-ibm-plex
sudo add-apt-repository ppa:mutlaqja/ppa && sudo apt install indi-3rdparty-libs indi-bin   # cameras, focuser, mount
cmake --preset linux-release && cmake --build --preset linux-release
build/linux-release/slopcap
```

The INDI packages bring the camera drivers (ZWO, QHY, Player One, ToupTek, MallinCam) and their USB permission rules,
so cameras work without root.
They also bring `indiserver` and the device drivers for the focuser and the mount. A typical setup runs `indiserver`
as a user service that starts with the computer, for example for a Gemini EAF focuser:

```bash
indiserver indi_myfocuserpro2_focus indi_lx200classic
```

Add yourself to the `dialout` group (`sudo usermod -aG dialout $USER`, then log in again) so INDI can open the
focuser's and mount's serial ports.

## macOS

SlopCap for Mac comes as `slopcap-macos-arm64.zip`, with `SlopCap.app` inside. It needs a Mac with Apple silicon on
**macOS 27 or newer**: the app runs on the macOS version it was built on and newer, and the releases are built on
macOS 27.

1. Unzip it (double-click the zip) and drag **SlopCap.app** onto **Applications**.
2. The first time, right-click SlopCap in Applications and choose **Open**, then **Open** again. (SlopCap isn't
   notarized by Apple, so a plain double-click is refused the first time. *System Settings → Privacy & Security →
   Open Anyway* does the same.)
3. The first time SlopCap shares a camera, watches one, or talks to an INDI server on another computer, macOS asks
   whether it may use the local network. Choose **Allow**.

**Cameras:** the ZWO, QHY, Player One, ToupTek and MallinCam drivers are inside the app (none has been tried with a
real camera on a Mac yet). QHY cameras on a Mac start up without their firmware: SlopCap sends it when it starts, and
the camera then reconnects by itself, which takes a few seconds. Wait a moment after launching before you open the
camera list.

**The mount** needs nothing else: SlopCap drives it itself over its USB serial port (`/dev/cu.…`) or the network (see
[the mount](user-guide.md#8-the-mount-optional) in the user guide), or through INDI like the focuser.

**The focuser** goes through INDI. There's no ready-made INDI package for macOS, so INDI is built from source once
with `tools/macos-indi.sh` from the SlopCap source (it installs into `~/indi`), or SlopCap uses an INDI server on
another computer (Settings → INDI server). For a Gemini EAF on this Mac:

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
