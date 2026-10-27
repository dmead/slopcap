#!/bin/bash
# A zip of SlopCap for one system, with a README and the observer documentation: build/package/slopcap-<os>-<arch>.zip
#   tools/package.sh linux     the linux-release build with the Qt libraries and plugins it uses, the camera SDKs and a
#                              launcher, so it runs without Qt installed (in the build image: run it from anywhere with
#                              Docker; on Windows from Git Bash with MSYS_NO_PATHCONV=1). Build linux-release first.
#   tools/package.sh windows   build/windows-static/slopcap.exe (docker/build.sh windows-static; with
#                              SLOPCAP_WIN_SDK_DLL_DIR set it has the camera drivers inside)
#   tools/package.sh macos     build/macos-release/SlopCap.app (tools/macos-app.sh), on a Mac
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
    for elf in "$OUT/bin/slopcap" $(find "$OUT/plugins" -name '*.so'); do
        ldd "$elf" | awk '/=> \//{print $3}'
    done | sort -u | while read -r lib; do
        base=$(basename "$lib")
        [[ $base =~ $system ]] && continue
        cp -L "$lib" "$OUT/lib/$base"
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
    # nothing the bundle needs may be missing (the system's own libraries aside)
    if LD_LIBRARY_PATH="$OUT/lib" ldd "$OUT/bin/slopcap" "$OUT"/plugins/platforms/libqxcb.so | grep 'not found'; then
        echo "missing libraries" >&2
        exit 1
    fi
    ;;
windows)
    B=build/windows-static
    [ -f $B/slopcap.exe ] || { echo "no $B/slopcap.exe: docker/build.sh windows-static first" >&2; exit 1; }
    cp $B/slopcap.exe "$OUT/"
    ;;
macos)
    B=build/macos-release
    [ -d $B/SlopCap.app ] || { echo "no $B/SlopCap.app: tools/macos-app.sh first" >&2; exit 1; }
    ditto $B/SlopCap.app "$OUT/SlopCap.app"
    ;;
esac

if [ "$OS" = macos ]; then
    ditto -c -k --sequesterRsrc --keepParent "$OUT" "$OUT.zip"   # keeps the app's signature and links
else
    python3 tools/package/zipdir.py "$OUT" "$OUT.zip"
fi
echo "$OUT.zip: $(du -h "$OUT.zip" | cut -f1)"
