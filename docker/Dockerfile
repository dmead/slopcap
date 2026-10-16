# SlopCap build environment: everything needed to build, test and cross-compile SlopCap, pinned in one image.
#   docker/build.sh                    # build the image (once) and run the default presets in it
# Ubuntu 26.04 matches the development laptop and the observatory machine's WSL Linux.
FROM ubuntu:26.04

ARG DEBIAN_FRONTEND=noninteractive

# Toolchain, tests, and the Windows cross-compiler (MinGW-w64, posix threads).
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential g++ cmake ninja-build catch2 git ca-certificates \
      g++-mingw-w64-x86-64-posix software-properties-common gpg-agent \
 && rm -rf /var/lib/apt/lists/*

# ZWO and QHY camera SDKs (headers + libraries) from the INDI project's PPA, for tools/camprobe and the camera backends.
RUN add-apt-repository -y ppa:mutlaqja/ppa \
 && apt-get update \
 && apt-get install -y --no-install-recommends indi-3rdparty-libs \
 && rm -rf /var/lib/apt/lists/*

# Builds run as the invoking user (docker run --user), so give that user somewhere writable for caches.
ENV HOME=/tmp
LABEL org.opencontainers.image.title="slopcap-build" \
      org.opencontainers.image.description="Build environment for SlopCap (Ubuntu 26.04, GCC 15, CMake, Ninja, Catch2, MinGW-w64, ZWO/QHY SDKs)"
