# Troubleshooting

When something goes wrong, the [session log](files-and-logs.md#session-logs) usually says why. Look there first.

## The camera isn't in the list

- **Is it in use?** Only one program can have a camera open. Close SharpCap, FireCapture, NINA or another SlopCap
  first. A camera that's busy may be listed but fail to open with "in use by another program?".
- **"ZWO support not built" / "QHY support not built":** this copy of SlopCap was built without that vendor's driver
  (see [Building and testing](developing.md)). The official Windows `slopcap.exe` and Mac `SlopCap.app` include both.
- **"ZWO SDK not found (ASICamera2.dll)"** (Windows): the driver couldn't be unpacked or loaded. Put `ASICamera2.dll`
  (or `qhyccd.dll`) next to `slopcap.exe`; SharpCap's program folder has copies.
- **Linux:** check the camera with `lsusb`. If it shows there but not in SlopCap, the USB permission rules from the
  INDI packages are missing: reinstall `indi-3rdparty-libs` and unplug and replug the camera.
- **Mac, QHY camera:** QHY cameras get their firmware from SlopCap when it starts, then reconnect. Give it about
  5 seconds after launching, then open the camera list again. Unplugging and replugging the camera while SlopCap is
  running needs a restart of SlopCap.
- **Windows:** the camera needs the vendor's USB driver installed (ZWO's or QHY's "system driver"). If SharpCap sees the
  camera, SlopCap will.

## The live view is black, but the camera is connected

- SlopCap's preview needs OpenGL 3.3. Over Windows Remote Desktop, and on some very old or virtual graphics, it isn't
  available; the preview then says so. **Recording still works.** Use remote viewing instead of Remote Desktop to watch
  from another computer.
- Very short exposures and low gain can simply be dark: try **Auto** stretch on the toolbar.

## Frames are being dropped

There are two kinds, and the session log's stats lines (`dropped camera … disk …`) tell them apart:

- **Dropped by the camera:** the USB link couldn't deliver. Lower the camera's USB traffic setting (All controls),
  use a smaller ROI or 8-bit, plug the camera straight into the computer (not a hub), use a USB 3 port and cable.
- **Dropped by the disk:** the disk didn't keep up. Record to a faster disk (an internal SSD), use a smaller ROI or
  8-bit, or close other programs that use the disk. The session log's stats lines show the disk speed.

## The focuser (or mount) won't connect

- The Focuser panel needs an **INDI server** running with the focuser's driver (for a Gemini EAF,
  `indi_myfocuserpro2_focus`). Check Settings → INDI server (normally `localhost:7624`).
- After a failed attempt SlopCap tries again every 10 seconds, so plugging the focuser in late is fine.
- The session log has what the driver said, e.g. *Failed to connect to port /dev/ttyUSB0*: the driver is using the
  wrong serial port. Set it once with `indi_setprop 'MyFocuserPro2.DEVICE_PORT.PORT=…'` and save the configuration
  (see [Installing SlopCap](install.md)). On Linux, the `/dev/serial/by-id/…` name of the port never changes, unlike
  `/dev/ttyUSB0`.
- **Linux:** permission denied on the port means you're not in the `dialout` group.

## The automatic target says "undecided"

It needs the image scale and your location to rule things out: set **Settings → Optics → Focal length** and
**Settings → Site**. Uranus and Neptune are hard to tell apart from the image alone. When in doubt, pick the target
from the list yourself.

## Remote viewing: the server doesn't show up

- Both computers must be on the same network (the same Wi-Fi or wired LAN) and on the same SlopCap version.
- The server's firewall must let SlopCap in ([Remote viewing](remote-viewing.md#network-and-firewall)). On Windows
  the network must be set to *Private*.
- Some networks block the announcement SlopCap uses to find servers. Use **Watch an address…** with the server's
  IP address.
- A slow link (for example a long outdoor cable running at 100 Mbit/s) gives fewer frames per second rather than a
  worse picture; that's on purpose.

## Mac: "SlopCap can't be opened"

SlopCap isn't notarized by Apple. Right-click it in Applications, choose **Open**, then **Open** again; or *System
Settings → Privacy & Security → Open Anyway*. You only need to do this once.
