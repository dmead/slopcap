# Remote viewing

At an outreach night, the computer at the telescope has the camera, but people want to watch somewhere else: on a
laptop by a projector, on a desktop indoors. SlopCap can share its camera with other SlopCaps on the same network.

- The **server** is the SlopCap with the camera plugged in. It keeps doing everything it normally does, and it keeps
  every recording.
- A **viewer** is any other SlopCap (Windows, Linux or Mac) that connects to it. It shows the live picture and can
  change the camera's settings.

## Share a camera

On the computer with the camera: click **⇄ Remote** at the top and choose **Share this camera on the network**.
A badge over the picture says **SHARED WITH …** and lists who's watching.

From a command line: `slopcap --camera qhy --serve`.

## Watch a camera

On the other computer: click **⇄ Remote**. Servers on the same network show up in the list; pick one. If it isn't
listed (some networks block the announcement), choose **Watch an address…** and type the server's IP address, for
example `192.168.1.20`.

From a command line: `slopcap --connect 192.168.1.20`, or `slopcap --connect auto` for the first server found.

The viewer shows **LIVE FROM …** over the picture and the link's speed in the bottom bar. If the link drops, it
reconnects by itself.

## What a viewer can do

- **Change any camera setting** (gain, exposure, ROI, bit depth, readout, white balance…). The change is made on the
  server, and both screens follow. Changes made on the server show up on every viewer too.
- **Record.** A viewer's Record button records **on the server**: the raw files never leave the telescope computer.
  The viewer shows the server's file name, time and frame count while it records.
- **Full view** (the Full screen button, or F11; Esc leaves): the live picture over a whole screen, ideal for a
  projector. With several screens, pick which one. Gain, exposure, stretch and Record sit in a bar that hides itself
  and the mouse pointer after 3 seconds; move the mouse to bring it back.

## Picture quality

The viewer receives the camera's raw frames, not a video stream, so its stretch, debayer and zoom work exactly as if
the camera were plugged in.

SlopCap puts detail first: frames are sent **lossless** (every bit, 16-bit included) as long as the link can carry
at least about 5 frames a second. If it can't, it steps down to high-quality JPEG (85, then 75, 65), and only then to
half and quarter size. A slow link means fewer frames per second, not a blurrier planet. Planetary ROIs stay lossless
easily; a full-frame 16-bit Moon from a large sensor is too much for a 100 Mbit/s link and goes as JPEG. Every change
is written to the session log with the reason.

## Network and firewall

SlopCap uses TCP port **45720** for the picture (or the next free one up to 45729) and UDP port **45721** for finding
servers. The server must allow both in:

- **Windows:** allow SlopCap when Windows asks the first time you share, on *Private* networks. Or, as administrator:

  ```powershell
  New-NetFirewallRule -DisplayName "SlopCap remote" -Direction Inbound -Program "C:\path\to\slopcap.exe" -Action Allow -Profile Private,Domain
  ```

- **macOS:** allow local network access when macOS asks (System Settings → Privacy & Security → Local Network).
- **Linux:** if a firewall is on, open 45720/tcp and 45721/udp.

Both computers must run the same SlopCap version (the remote protocol changes from time to time); the list shows
servers of another version as "update one".
