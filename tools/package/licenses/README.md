# Third-party licenses

SlopCap itself is under the GNU General Public License, version 3: `../LICENSE`. It comes with other people's
software, each under its own license:

| | |
|---|---|
| `camera-sdks/` | the camera drivers: ZWO (`zwo-asi.txt`), QHY (`qhyccd.txt`), Player One (`player-one.txt`), ToupTek and MallinCam (`touptek-mallincam-LGPL-2.1.txt`). The texts are the ones INDI ships with each SDK (indi-3rdparty). |
| `qt/` (Windows, macOS) | Qt 6 (built into `slopcap.exe`; on macOS the frameworks and plugins in `SlopCap.app`), used under the GPL-3.0, with the libraries Qt includes (zlib, PCRE2, libpng, libjpeg, FreeType, HarfBuzz and others). `qtbase-*.spdx` lists each one with its license and copyright; the license texts are beside it. |
| `mingw-runtime/` (Windows) | the GCC C++ runtime, winpthreads and the MinGW-w64 runtime, built into `slopcap.exe`. |
| `homebrew/` (macOS) | the license files of each Homebrew package that a library in `SlopCap.app/Contents/Frameworks` comes from (ICU, GLib, OpenSSL, FreeType, HarfBuzz, libusb and others). |
| `ibm-plex-fonts.txt` (Windows, macOS) | the IBM Plex fonts inside the program (SIL Open Font License 1.1). |
| `debian/` (Linux) | the copyright file of each Ubuntu package that a bundled library, plugin or the IBM Plex fonts come from, and in `debian/common-licenses/` the license texts they refer to. |
