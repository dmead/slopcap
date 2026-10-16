#!/bin/bash
# Build and test SlopCap inside the pinned build image, on any machine with Docker.
#   docker/build.sh                        # linux-debug build + tests (default)
#   docker/build.sh linux-asan linux-tsan  # any presets from CMakePresets.json; tests run where the preset has them
#   docker/build.sh windows-cross          # Windows command-line tools; set SLOPCAP_WIN_SDK_DLL_DIR to also build camprobe
#   docker/build.sh windows-static         # Windows GUI as one self-contained slopcap.exe (own image: MinGW + static Qt)
#   docker/build.sh --shell                # interactive shell in the build environment
#   docker/build.sh --push                 # push the current environment image to the registry (docker/registry)
# Output goes to build/<preset>/ in the repo, owned by you. Images are rebuilt only when their Dockerfile changes.
set -euo pipefail

REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)

# Registry for sharing the environment between machines: docker/registry holds e.g. "registry.example:5000"
# (override with SLOPCAP_REGISTRY; empty = local only). Images are tagged with their Dockerfile's hash, so a pull
# gets exactly this environment, and every changed environment is pushed once, when it is first built.
REGISTRY=${SLOPCAP_REGISTRY-$(cat "$REPO/docker/registry" 2>/dev/null | tr -d '[:space:]' || true)}

# ensure_image NAME TAG DOCKERFILE [build args...]: local, else pulled, else built (and pushed).
ensure_image() {
    local name=$1 tag=$2 file=$3
    shift 3
    local remote=${REGISTRY:+$REGISTRY/$name}
    docker image inspect "$name:$tag" >/dev/null 2>&1 && return
    if [ -n "$remote" ] && docker pull -q "$remote:$tag" >/dev/null 2>&1; then
        echo "pulled $remote:$tag"
        docker tag "$remote:$tag" "$name:$tag"
        docker tag "$remote:$tag" "$name:latest"
        return
    fi
    echo "building image $name:$tag (first run only)..."
    docker build -t "$name:$tag" -t "$name:latest" -f "$file" "$@" "$REPO/docker"
    if [ -n "$remote" ]; then
        docker tag "$name:$tag" "$remote:$tag" && docker tag "$name:$tag" "$remote:latest"
        if docker push -q "$remote:$tag" && docker push -q "$remote:latest"; then
            echo "pushed $remote:$tag"
        else
            echo "warning: push to $REGISTRY failed; the image is only local (retry: docker/build.sh --push)" >&2
        fi
    fi
}

TAG=$(sha256sum < "$REPO/docker/Dockerfile" | cut -c1-12)
IMAGE="slopcap-build:$TAG"
ensure_image slopcap-build "$TAG" "$REPO/docker/Dockerfile"
REMOTE=${REGISTRY:+$REGISTRY/slopcap-build}

# The Windows single-exe build has its own image (Fedora MinGW + static Qt), derived from the Linux one.
# (hash the contents only: sha256sum's output would also carry the checkout's path, giving each machine its own tag)
MINGW_TAG=$( (sha256sum < "$REPO/docker/Dockerfile.mingw"; echo "$TAG") | sha256sum | cut -c1-12)
MINGW_IMAGE="slopcap-build-mingw:$MINGW_TAG"
needs_mingw() { for p in "$@"; do [ "$p" = windows-static ] && return 0; done; return 1; }

if [ "${1:-}" = "--push" ]; then
    [ -n "$REMOTE" ] || { echo "no registry configured (docker/registry or SLOPCAP_REGISTRY)" >&2; exit 2; }
    docker tag "$IMAGE" "$REMOTE:$TAG" && docker tag "$IMAGE" "$REMOTE:latest"
    docker push "$REMOTE:$TAG" && docker push "$REMOTE:latest"
    if docker image inspect "$MINGW_IMAGE" >/dev/null 2>&1; then
        docker tag "$MINGW_IMAGE" "$REGISTRY/slopcap-build-mingw:$MINGW_TAG" && docker push "$REGISTRY/slopcap-build-mingw:$MINGW_TAG"
    fi
    exit
fi

# The development directory is mounted at the same path as on the host, so build/ folders, compile_commands.json
# and error messages are interchangeable between host and container builds.
run=(docker run --rm --user "$(id -u):$(id -g)" -v "$REPO:$REPO" -w "$REPO")
[ -t 0 ] && run+=(-it)
if [ -n "${SLOPCAP_WIN_SDK_DLL_DIR:-}" ]; then
    run+=(-v "$(readlink -f "$SLOPCAP_WIN_SDK_DLL_DIR"):/winsdk:ro" -e SLOPCAP_WIN_SDK_DLL_DIR=/winsdk)
fi
[ -n "${SLOPCAP_TEST_SER:-}" ] && run+=(-v "$(readlink -f "$SLOPCAP_TEST_SER"):/testdata/test.ser:ro" -e SLOPCAP_TEST_SER=/testdata/test.ser)

if [ "${1:-}" = "--shell" ]; then
    exec "${run[@]}" "$IMAGE" bash
fi

presets=("$@")
[ ${#presets[@]} -eq 0 ] && presets=(linux-debug)
# windows-static runs in its own image; build it on its own.
if needs_mingw "${presets[@]}"; then
    [ ${#presets[@]} -eq 1 ] || { echo "run windows-static on its own (it uses the Windows build image)" >&2; exit 2; }
    ensure_image slopcap-build-mingw "$MINGW_TAG" "$REPO/docker/Dockerfile.mingw" --build-arg "UBUNTU_IMAGE=$IMAGE"
    exec "${run[@]}" "$MINGW_IMAGE" bash -c "set -e; cmake --preset windows-static; cmake --build --preset windows-static"
fi
# ThreadSanitizer turns off address-space randomisation at startup via personality(2), which Docker's default seccomp
# profile blocks. Relax seccomp for TSan runs only (a local, short-lived build container).
for p in "${presets[@]}"; do [[ $p == *tsan* ]] && run+=(--security-opt seccomp=unconfined) && break; done
# One virtual X display for the whole run (GL preview tests). Started directly: xvfb-run's startup handshake can hang
# in containers.
script="set -e; if command -v Xvfb >/dev/null; then Xvfb :99 -screen 0 1280x1024x24 -nolisten tcp >/dev/null 2>&1 & export DISPLAY=:99; for i in \$(seq 50); do [ -e /tmp/.X11-unix/X99 ] && break; sleep 0.1; done; fi"
for p in "${presets[@]}"; do
    script+="; echo '===== $p'; cmake --preset $p; cmake --build --preset $p"
    case $p in windows-cross|linux-release) ;; *) script+="; ctest --preset $p" ;; esac
done
exec "${run[@]}" "$IMAGE" bash -c "$script"
