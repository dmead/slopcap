#!/bin/bash
# macOS: build INDI (indiserver and the core drivers, among them indi_myfocuserpro2_focus for the Gemini EAF,
# indi_lx200classic for the mount, and the simulators the tests use) into PREFIX. Homebrew has no INDI; no sudo needed.
#   tools/macos-indi.sh [PREFIX] [indi-2.2.5.tar.gz]     PREFIX defaults to ~/indi; the source is downloaded if not given
# Then:  PATH=PREFIX/bin:$PATH indiserver indi_myfocuserpro2_focus      (SlopCap: Focus → Connect, localhost:7624)
# and ctest finds indiserver the same way (PATH), so the INDI tests run too.
set -euo pipefail

PREFIX=${1:-$HOME/indi}
TARBALL=${2:-}
VERSION=2.2.5
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=$REPO/build/indi-src

brew install cmake ninja pkgconf cfitsio libnova libusb gsl fftw libev erfa jpeg-turbo
mkdir -p "$WORK"
if [ -z "$TARBALL" ]; then
    TARBALL=$WORK/indi-$VERSION.tar.gz
    curl -fsSL -o "$TARBALL" "https://codeload.github.com/indilib/indi/tar.gz/refs/tags/v$VERSION"
fi
rm -rf "$WORK/indi-$VERSION"
tar xzf "$TARBALL" -C "$WORK"

BREW=$(brew --prefix)
cmake -S "$WORK/indi-$VERSION" -B "$WORK/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_RPATH="$PREFIX/lib" \
    -DCMAKE_PREFIX_PATH="$BREW;$BREW/opt/jpeg-turbo" \
    -DINDI_BUILD_QT_CLIENT=OFF -DINDI_BUILD_UNITTESTS=OFF -DINDI_BUILD_INTEGTESTS=OFF -DINDI_BUILD_XISF=OFF \
    -DINDI_BUILD_STATIC=OFF -DINDI_INSTALL_UDEV_RULES=OFF
cmake --build "$WORK/build"
cmake --install "$WORK/build"
for exe in indiserver indi_myfocuserpro2_focus indi_lx200classic indi_simulator_focus indi_simulator_telescope; do
    [ -x "$PREFIX/bin/$exe" ] || { echo "missing $PREFIX/bin/$exe" >&2; exit 1; }
done
echo "INDI $VERSION in $PREFIX: PATH=$PREFIX/bin:\$PATH indiserver indi_myfocuserpro2_focus"
