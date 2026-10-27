# SlopCap for Mac

SlopCap records video of the planets, the Moon and the Sun from an astronomy camera, and helps you get the telescope
ready for it: collimation, focus, and knowing what you are pointed at.

## Start it

1. Drag **SlopCap.app** from this folder to **Applications**.
2. The first time, right-click SlopCap in Applications and choose **Open**, then **Open** again. SlopCap isn't
   notarized by Apple, so a plain double-click is refused the first time (*System Settings → Privacy & Security →
   Open Anyway* does the same).
3. The first time SlopCap shares a camera, watches one, or talks to an INDI server on another computer, macOS asks
   whether it may use the local network: choose **Allow**.

To try it without a camera, pick **Replay a recording…** from the camera menu (the camera name at the top right) and
choose a `.ser` file: it plays as if it were a camera.

## What it needs

- A Mac with Apple silicon, on macOS 15 (Sequoia) or newer.

## Cameras

The ZWO, QHY, Player One, ToupTek and MallinCam camera drivers are inside the app. QHY cameras start up on a Mac
without their firmware: SlopCap sends it when it starts and the camera reconnects by itself, which takes a few
seconds, so wait a moment after launching before you open the camera list.

## Focuser and mount

They go through INDI. There's no ready-made INDI package for macOS: build it once with `tools/macos-indi.sh` from the
SlopCap source, or point SlopCap at an INDI server on another computer (**Settings**, the gear at the top right). See
[Installing SlopCap](docs/install.md) for the Gemini EAF focuser.

## More

`docs/` has the whole guide: [a night at the telescope](docs/user-guide.md), [remote viewing](docs/remote-viewing.md),
[files, settings and logs](docs/files-and-logs.md) and [troubleshooting](docs/troubleshooting.md). Recordings go to
`Movies/SlopCap` unless you pick another folder; each night's log is in `~/Library/Application Support/SlopCap/SlopCap/logs`.

## License

SlopCap is free software under the GNU General Public License, version 3 (`LICENSE`). The camera drivers, Qt and the
other libraries inside the app and the fonts built into it are under their own licenses, in `licenses/`.
