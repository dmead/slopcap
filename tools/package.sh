#!/bin/bash
# A zip of SlopCap for one system, with a README, the LICENSE, the third-party licenses (licenses/) and the observer
# documentation: build/package/slopcap-<os>-<arch>.zip
#   tools/package.sh linux     the linux-release build with the Qt libraries and plugins it uses, the camera SDKs and a
#                              launcher, so it runs without Qt installed (in the build image: run it from anywhere with
#                              Docker; on Windows from Git Bash with MSYS_NO_PATHCONV=1). Build linux-release first.
#   tools/package.sh windows   build/windows-static/slopcap.exe (docker/build.sh windows-static; with
#                              SLOPCAP_WIN_SDK_DLL_DIR set it has the camera drivers inside)
#   tools/package.sh macos     build/macos-release/SlopCap.app (tools/macos-app.sh), on a Mac, with
#                              SLOPCAP_MAC_LICENSES set (Qt's and the fonts' license texts: see below)
set -euo pipefail

REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
OS=${1:?usage: tools/package.sh linux|windows|macos}
cd "$REPO"

# linux is packed in the build image (the libraries it bundles come from there), windows anywhere with Python 3; re-run
# in the image when that isn't here
in_image() { [ -e /usr/include/libqhy/qhyccd.h ] && [ -d "/usr/lib/$(uname -m)-linux-gnu/qt6" ]; }
if { [ "$OS" = linux ] && ! in_image; } || { [ "$OS" = windows ] && ! python3 -c '' 2>/dev/null; }; then
    TAG=$(sha256sum < docker/Dockerfile | cut -c1-12)
    exec docker run --rm --user "$(id -u):$(id -g)" -v "$REPO:$REPO" -w "$REPO" "slopcap-build:$TAG" tools/package.sh "$OS"
fi

case $OS in
    linux) NAME=slopcap-linux-$(uname -m | sed 's/x86_64/x64/') ;;
    windows) NAME=slopcap-windows-x64 ;;
    macos) NAME=slopcap-macos-$(uname -m) ;;
    *) echo "unknown system $OS" >&2; exit 2 ;;
esac
OUT=build/package/$NAME
rm -rf "$OUT" "$OUT.zip"
mkdir -p "$OUT/docs"
cp "tools/package/README-$OS.md" "$OUT/README.md"
cp LICENSE "$OUT/"
cp -r tools/package/licenses "$OUT/"
for f in README.md install.md user-guide.md remote-viewing.md files-and-logs.md troubleshooting.md; do cp "docs/$f" "$OUT/docs/"; done
cp -r docs/images "$OUT/docs/"

case $OS in
linux)
    B=build/linux-release
    [ -x $B/slopcap ] || { echo "no $B/slopcap: cmake --preset linux-release && cmake --build --preset linux-release first" >&2; exit 1; }
    mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/plugins" "$OUT/udev"
    cp $B/slopcap "$OUT/bin/"
    strip "$OUT/bin/slopcap"
    printf '[Paths]\nPrefix = ..\nPlugins = plugins\n' > "$OUT/bin/qt.conf"
    QT=$(qtpaths6 --query QT_INSTALL_PLUGINS 2>/dev/null || echo /usr/lib/$(uname -m)-linux-gnu/qt6/plugins)
    for p in platforms/libqxcb.so platforms/libqwayland.so platforms/libqoffscreen.so xcbglintegrations/libqxcb-glx-integration.so \
             xcbglintegrations/libqxcb-egl-integration.so imageformats/libqjpeg.so imageformats/libqgif.so imageformats/libqico.so \
             platforminputcontexts/libcomposeplatforminputcontextplugin.so wayland-shell-integration wayland-decoration-client \
             wayland-graphics-integration-client; do
        mkdir -p "$OUT/plugins/$(dirname $p)"
        cp -r "$QT/$p" "$OUT/plugins/$(dirname $p)/"
    done
    # What the program and the plugins load, less what every Linux desktop has and must provide itself: the C library,
    # the graphics stack (OpenGL, X11, Wayland, DRM), fonts, D-Bus, GLib, udev and the common compression libraries.
    system='^(libc|libm|libdl|librt|libpthread|libresolv|libutil|ld-linux.*|libGL|libGLX|libEGL|libGLdispatch|libOpenGL|libdrm|libgbm'
    system+='|libX11|libX11-xcb|libxcb|libXau|libXdmcp|libXext|libXrender|libXi|libXfixes|libwayland-.*|libfontconfig|libfreetype'
    system+='|libexpat|libz|libudev|libsystemd|libdbus-1|libglib-2\.0|libgthread-2\.0|libgobject-2\.0|libgio-2\.0|libgmodule-2\.0'
    system+='|libmount|libblkid|libselinux|libffi|libpcre2-8|libcap|libgcrypt|libgpg-error|liblzma|liblz4|libzstd|libbz2|libbrotli.*'
    system+='|libpng16|libharfbuzz|libgraphite2|libuuid)\.so'   # (libjpeg goes in: its name differs between distros)
    sources=(/usr/share/fonts/truetype/ibm-plex/*)   # what the zip carries from the system (the fonts: inside the program)
    for f in $(cd "$OUT/plugins" && find . -type f); do sources+=("$QT/${f#./}"); done
    for lib in $(for elf in "$OUT/bin/slopcap" $(find "$OUT/plugins" -name '*.so'); do
                     ldd "$elf" | awk '/=> \//{print $3}'
                 done | sort -u); do
        base=$(basename "$lib")
        [[ $base =~ $system ]] && continue
        cp -L "$lib" "$OUT/lib/$base"
        sources+=("$lib")
    done
    # the camera SDKs are opened by the program, not linked by the plugins: make sure they're there
    for l in libASICamera2.so libqhyccd.so libPlayerOneCamera.so libtoupcam.so libmallincam.so; do
        ls "$OUT"/lib/$l* >/dev/null 2>&1 || echo "warning: $l not bundled" >&2
    done
    cp /lib/udev/rules.d/99-asi.rules /lib/udev/rules.d/85-qhyccd.rules "$OUT/udev/"
    for r in 99-player_one_astronomy.rules 99-toupcam.rules 99-mallincam.rules; do
        if [ -e /lib/udev/rules.d/$r ]; then cp /lib/udev/rules.d/$r "$OUT/udev/"; fi
    done
    cat > "$OUT/slopcap" <<'LAUNCHER'
#!/bin/sh
# Starts SlopCap with the libraries in this folder.
here=$(dirname "$(readlink -f "$0")")
LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$here/bin/slopcap" "$@"
LAUNCHER
    chmod +x "$OUT/slopcap"
    # Their licenses: the copyright file of each Ubuntu package they come from, and the license texts those point to
    # (/usr/share/common-licenses isn't on every system). INDI's covers only INDI's code: the camera SDKs' own are in
    # licenses/camera-sdks.
    mkdir -p "$OUT/licenses/debian/common-licenses"
    for f in "${sources[@]}"; do
        f=$(readlink -f "$f")
        dpkg -S "$f" 2>/dev/null || dpkg -S "${f/#\/usr\/lib\//\/lib\/}" 2>/dev/null || echo "warning: no package has $f" >&2
    done | { grep -v '^diversion' || true; } | cut -d: -f1 | sort -u | while read -r pkg; do
        if [ -f "/usr/share/doc/$pkg/copyright" ]; then
            cp -L "/usr/share/doc/$pkg/copyright" "$OUT/licenses/debian/$pkg.txt"
        else
            echo "warning: $pkg has no copyright file" >&2
        fi
    done
    { grep -ho '/usr/share/common-licenses/[A-Za-z0-9.+_-]*[A-Za-z0-9]' "$OUT"/licenses/debian/*.txt || true; } | sort -u | while read -r l; do
        if [ -f "$l" ]; then cp "$l" "$OUT/licenses/debian/common-licenses/"; fi
    done
    # nothing the bundle needs may be missing (the system's own libraries aside)
    if LD_LIBRARY_PATH="$OUT/lib" ldd "$OUT/bin/slopcap" "$OUT"/plugins/platforms/libqxcb.so | grep 'not found'; then
        echo "missing libraries" >&2
        exit 1
    fi
    ;;
windows)
    B=build/windows-static
    [ -f $B/slopcap.exe ] || { echo "no $B/slopcap.exe: docker/build.sh windows-static first" >&2; exit 1; }
    [ -d $B/licenses/qt ] || { echo "no $B/licenses: docker/build.sh windows-static puts them there" >&2; exit 1; }
    cp $B/slopcap.exe "$OUT/"
    cp -r $B/licenses/. "$OUT/licenses/"
    ;;
macos)
    B=build/macos-release
    [ -d $B/SlopCap.app ] || { echo "no $B/SlopCap.app: tools/macos-app.sh first" >&2; exit 1; }
    # Homebrew has neither Qt's license texts nor the fonts': SLOPCAP_MAC_LICENSES names a folder with both, laid out as
    # the Windows build leaves them in build/windows-static/licenses (qt/ with qtbase's LICENSES/, ibm-plex-fonts.txt)
    L=${SLOPCAP_MAC_LICENSES:-}
    [ -n "$L" ] && [ -f "$L/qt/LGPL-3.0-only.txt" ] && [ -f "$L/ibm-plex-fonts.txt" ] || {
        echo "SLOPCAP_MAC_LICENSES must name a folder with qt/ (qtbase's LICENSES/) and ibm-plex-fonts.txt," \
             "as build/windows-static/licenses has them" >&2; exit 1; }
    BREW=$(brew --prefix)
    QTB=$BREW/opt/qtbase
    ditto $B/SlopCap.app "$OUT/SlopCap.app"
    FW=$OUT/SlopCap.app/Contents/Frameworks
    for fw in "$FW"/Qt*.framework; do   # (the SBOM below is qtbase's)
        [ -e "$QTB/lib/$(basename "$fw")" ] || { echo "$(basename "$fw") isn't qtbase's: add its module's SBOM" >&2; exit 1; }
    done
    mkdir -p "$OUT/licenses/qt" "$OUT/licenses/homebrew"
    cp "$L"/qt/*.txt "$QTB"/share/qt/sbom/qtbase-*.spdx "$OUT/licenses/qt/"
    cp "$L/ibm-plex-fonts.txt" "$OUT/licenses/"
    # every license this Qt's SBOM names needs its text (the texts can come from another Qt version)
    missing=
    for id in $(grep -hE '^(PackageLicenseConcluded|PackageLicenseDeclared|LicenseConcluded|LicenseInfoInFile):' \
                    "$OUT"/licenses/qt/qtbase-*.spdx | cut -d: -f2- | tr -d '\r' | tr '() ' '\n\n\n' |
                    { grep -vxE 'AND|OR|WITH|NOASSERTION|NONE' || true; } | sort -u); do
        [ -f "$OUT/licenses/qt/$id.txt" ] || grep -qx "LicenseID: $id" "$OUT"/licenses/qt/qtbase-*.spdx || missing="$missing $id"
    done
    [ -z "$missing" ] || { echo "no license text for$missing in $L/qt" >&2; exit 1; }
    # The other libraries macdeployqt copied in are Homebrew's: the license files of each one's formula, which Homebrew
    # keeps at the top of the keg. (The camera SDKs' are in camera-sdks/.)
    for lib in "$FW"/*.dylib; do
        name=$(basename "$lib")
        case $name in libASICamera2.dylib | libqhyccd.dylib | libPlayerOneCamera.dylib | libtoupcam.dylib | libmallincam.dylib) continue ;; esac
        keg=
        for f in "$BREW"/opt/*/lib/"$name"; do
            if [ -e "$f" ]; then keg=$(readlink -f "$f"); keg=${keg%/lib/*}; break; fi
        done
        [ -n "$keg" ] || { echo "$name: no Homebrew formula has it" >&2; exit 1; }
        formula=$(basename "$(dirname "$keg")")
        mkdir -p "$OUT/licenses/homebrew/$formula"
        n=$(find "$keg" -maxdepth 1 -type f \( -iname 'LICEN[CS]E*' -o -iname 'COPYING*' -o -iname 'COPYRIGHT*' -o -iname '*GPL*' \) \
                -exec cp {} "$OUT/licenses/homebrew/$formula/" \; -print | wc -l | tr -d ' ')
        [ "$n" -gt 0 ] || { echo "$formula ($name): no license file in $keg" >&2; exit 1; }
    done
    ;;
esac

if [ "$OS" = macos ]; then
    ditto -c -k --sequesterRsrc --keepParent "$OUT" "$OUT.zip"   # keeps the app's signature and links
else
    python3 tools/package/zipdir.py "$OUT" "$OUT.zip"
fi
echo "$OUT.zip: $(du -h "$OUT.zip" | cut -f1)"
