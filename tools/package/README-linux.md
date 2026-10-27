# SlopCap for Linux

SlopCap records video of the planets, the Moon and the Sun from an astronomy camera, and helps you get the telescope
ready for it: collimation, focus, and knowing what you are pointed at.

This folder runs as it is: Qt and the camera drivers (ZWO, QHY, Player One, ToupTek, MallinCam) are inside it.
Nothing is installed.

## Start it

```bash
unzip slopcap-linux-x64.zip
slopcap-linux-x64/slopcap
```

Keep the folder together (`slopcap` is a small launcher for `bin/slopcap` and the libraries in `lib/`). To start it
from the desktop's menu, make a launcher that runs the full path of `slopcap-linux-x64/slopcap`.

To try it without a camera, replay a recording as one: `slopcap --camera sim-loop:/path/to/recording.ser`, or pick
**Replay a recording…** from the camera menu.

## What it needs

- A 64-bit PC Linux as new as **Ubuntu 26.04** (glibc 2.43 or newer). Older systems refuse to start it.
- A desktop session (X11 or Wayland).
- A graphics card that can do OpenGL 3.3 for the live preview. Without it the preview stays blank and says why;
  recording still works.

## Cameras

Cameras need permission to be opened by a normal user, and QHY cameras need their firmware sent when they're plugged
in. Both come with the INDI packages, which most Linux astronomy users have already:

```bash
sudo add-apt-repository ppa:mutlaqja/ppa && sudo apt install indi-3rdparty-libs indi-bin
```

Without those, the cameras' USB rules are in `udev/`: `sudo cp udev/*.rules /etc/udev/rules.d/`, then
unplug and plug in the camera. (QHY's rule also needs QHY's firmware files and `fxload`, which those packages provide.)

## Focuser and mount

They are reached through an INDI server (`indiserver`, from `indi-bin`), on this computer or another one. Set it in
**Settings** (the gear at the top right). SlopCap can also drive the common mounts itself, over their serial port or
the network (see `docs/user-guide.md`, section 8). For a Gemini EAF focuser and an LX200 mount through INDI:

```bash
indiserver indi_myfocuserpro2_focus indi_lx200classic
```

## More

`docs/` has the whole guide: [a night at the telescope](docs/user-guide.md), [remote viewing](docs/remote-viewing.md),
[files, settings and logs](docs/files-and-logs.md) and [troubleshooting](docs/troubleshooting.md). Recordings go to
`~/Videos/SlopCap` unless you pick another folder; each night's log is in `~/.local/share/SlopCap/SlopCap/logs`.
