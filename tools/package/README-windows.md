# SlopCap for Windows

SlopCap records video of the planets, the Moon and the Sun from an astronomy camera, and helps you get the telescope
ready for it: collimation, focus, and knowing what you are pointed at.

## Start it

1. Unzip this folder anywhere, for example to `C:\Users\<you>\slopcap\`.
2. Double-click `slopcap.exe`.

There is no installer: `slopcap.exe` is the whole program. Windows may say it protected your PC from an unknown app
the first time (SlopCap isn't signed): choose **More info**, then **Run anyway**.

To try it without a camera, pick **Replay a recording…** from the camera menu (the camera name at the top right) and
choose a `.ser` file: it plays as if it were a camera.

## What it needs

- 64-bit Windows 10 or 11.
- A graphics card that can do OpenGL 3.3 for the live preview (any computer from the last ten years). Without it the
  preview stays blank and says why; recording still works.

## Cameras

The ZWO and QHY camera drivers are built into `slopcap.exe` when it was made with them (the camera menu says
"SDK not found" for a brand it doesn't have). The camera itself still needs the vendor's USB driver, the one SharpCap
and FireCapture use too: if the camera works in SharpCap, it works in SlopCap. To try a newer ZWO or QHY driver, put
its `ASICamera2.dll` or `qhyccd.dll` next to `slopcap.exe`.

## Focuser and mount

They go through INDI, which runs on Linux and macOS: SlopCap on Windows can use an INDI server on another computer
(**Settings**, the gear at the top right, then the INDI server's address).

## Remote viewing

SlopCap can share its camera with other SlopCaps on the network, for example to a projector at an outreach night.
Windows asks the first time you share; allow it on **Private networks**.

## More

`docs\` has the whole guide: [a night at the telescope](docs/user-guide.md), [remote viewing](docs/remote-viewing.md),
[files, settings and logs](docs/files-and-logs.md) and [troubleshooting](docs/troubleshooting.md). Recordings go to
`Videos\SlopCap` unless you pick another folder; each night's log is in `%LOCALAPPDATA%\SlopCap\SlopCap\logs`.
