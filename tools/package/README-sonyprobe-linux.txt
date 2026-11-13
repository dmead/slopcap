SlopCap Sony probe, for Linux
=============================

This small program asks a Sony camera what it can do over USB when a computer
controls it ("PC Remote"), so that SlopCap, a free astronomy capture program, can
learn to use it for focusing. It takes one picture, and changes the shutter speed
and the ISO one notch and back again. It changes nothing else on the camera and
installs nothing.

It runs on Ubuntu 22.04 or newer (or another Linux with glibc 2.35 or newer), and
in WSL on Windows. It needs libusb (sudo apt install libusb-1.0-0; gphoto2 brings
it already).

1. On the camera:
   - MENU > Setup (the toolbox) > USB Connection > PC Remote
   - mode dial on M
   - MENU > Camera Settings > Quality > Fine (or RAW & JPEG)
   - with a lens on: focus mode MF, so the picture is taken straight away
   - with no lens (on a telescope): MENU > Custom Settings > Release w/o Lens > Enable
2. Connect it by USB and switch it on.
   In WSL, attach it from Windows first, in PowerShell as administrator:
       usbipd list                         (find the camera's BUSID)
       usbipd bind --busid <BUSID>
       usbipd attach --wsl --busid <BUSID>
   (gphoto2 --auto-detect in WSL then shows "Sony Alpha-A6000 (Control)".)
3. Nothing else may hold the camera: close gphoto2 and anything using it. On a
   Linux desktop the file manager may have mounted it: unmount it there first.
4. Unzip and run it, with sudo unless you have a udev rule for the camera:
       unzip slopcap-sonyprobe-linux-x64.zip
       cd slopcap-sonyprobe-linux-x64
       sudo ./slopcap-sonyprobe
5. It leaves its report beside it, in two files:
       sonyprobe-<camera>-<date>-<time>.txt
       sonyprobe-<camera>-<date>-<time>.json
   and the picture it took. Please send back the two report files. The camera's
   serial number is left out of them. If it finds no camera it still writes them,
   with what it saw.

Options:
   --no-shot      no picture
   --no-set       leave the shutter speed and ISO alone
   --shots 3      three pictures (for the timing)
   --out FOLDER   the report somewhere else
   --list         only list the cameras it sees

SlopCap is free software under the GNU General Public License, version 3 (LICENSE).
licenses/ holds the license of the C++ runtime built into the program (GCC's).
Source: https://github.com/dmead/slopcap (tools/sonyprobe)
