# SlopCap documentation

SlopCap records video of the planets, the Moon and the Sun from an astronomy camera, and helps you get the telescope
ready for it: collimation, focus, and knowing what you are pointed at. It runs on Windows, Linux and macOS.

## For observers

| Page | Read it when |
|---|---|
| [Installing SlopCap](install.md) | you want SlopCap on a computer (Windows, Linux or Mac) |
| [A night at the telescope](user-guide.md) | you're about to observe: the screen, the camera, collimating, focusing, the ADC, recording, the mount |
| [Remote viewing](remote-viewing.md) | you want other people to watch the telescope's camera on another computer or a projector |
| [Files, settings and logs](files-and-logs.md) | you're looking for your recordings, or want to know what happened during a night |
| [Troubleshooting](troubleshooting.md) | something doesn't work |

## For developers

| Page | Read it when |
|---|---|
| [Building and testing](developing.md) | you want to build SlopCap from source, run the tests, or understand the CI |
| [PLAN.md](../PLAN.md) | you want the design, the reasoning behind it and what's still to do |
| [Mount support (design)](design/mounts.md) | you're working on mounts: INDI, ASCOM, Alpaca and the built-in drivers |

## The short version

1. Plug in the camera and start SlopCap. Click the camera name at the top right and pick your camera.
2. Set the exposure and gain on the left until the planet looks right and the histogram shows little or no clipping.
3. Collimate on a star (**Collimate** tab), then focus (**Focus** tab: a focus sweep finds the best position for each
   colour). With an ADC, line up red and blue on the **ADC** tab.
4. Press **Record** (Ctrl+R). Each recording is a `.ser` file named after the time and the planet, ready for
   AutoStakkert!, PIPP or WinJUPOS, with a `.txt` of the settings next to it.
