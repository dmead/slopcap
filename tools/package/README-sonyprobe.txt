SlopCap Sony probe
==================

This small program asks a Sony camera what it can do over USB when a computer
controls it ("PC Remote"), so that SlopCap, a free astronomy capture program, can
learn to use it for focusing. It takes one picture, and changes the shutter speed
and the ISO one notch and back again. It changes nothing else on the camera and
installs nothing on the computer.

You need: the camera (an a6000, or another Sony Alpha or NEX), its USB cable, a
charged battery, and a Windows 10 or 11 computer.

1. On the camera:
   - MENU > Setup (the toolbox) > USB Connection > PC Remote
   - mode dial on M
   - MENU > Camera Settings > Quality > Fine (or RAW & JPEG)
   - with a lens on: focus mode MF, so the picture is taken straight away
   - with no lens (on a telescope): MENU > Custom Settings > Release w/o Lens > Enable
2. Close anything that may take the camera over: Imaging Edge, the Photos app's
   import, Sony's PlayMemories.
3. Connect the camera to the computer by USB and switch it on.
4. Unzip this folder somewhere (the Desktop is fine) and double-click
   slopcap-sonyprobe.exe. Windows may warn about a program it doesn't know:
   "More info", then "Run anyway". The probe takes a few seconds and the window
   says what it found; press Enter to close it.
5. It leaves its report beside it, in two files:
       sonyprobe-<camera>-<date>-<time>.txt
       sonyprobe-<camera>-<date>-<time>.json
   and the picture it took (...-shot1.jpg). Please send back the two report files.
   The camera's serial number is left out of them. The picture is yours: send it
   too if you like, it shows how sharp the computer's copy is.

If it says "No Sony camera found": check step 1 (PC Remote) and the cable (some
USB cables only charge), and try another USB port.

Options, for a Command Prompt in this folder:
   slopcap-sonyprobe --no-shot      no picture
   slopcap-sonyprobe --no-set       leave the shutter speed and ISO alone
   slopcap-sonyprobe --shots 3      three pictures (for the timing)
   slopcap-sonyprobe --out FOLDER   the report somewhere else
   slopcap-sonyprobe --list         only list the cameras and phones it sees

SlopCap is free software under the GNU General Public License, version 3 (LICENSE).
licenses/ holds the licenses of the compiler's runtime built into the program.
Source: https://github.com/dmead/slopcap (tools/sonyprobe)
