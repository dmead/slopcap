#!/bin/bash
# Build and test SlopCap inside the pinned build image, on any machine with Docker.
#   docker/build.sh                        # linux-debug build + tests (default)
#   docker/build.sh linux-asan linux-tsan  # any presets from CMakePresets.json; tests run where the preset has them
#   docker/build.sh windows-cross          # Windows .exe files; set SLOPCAP_WIN_SDK_DLL_DIR to also build camprobe
#   docker/build.sh --shell                # interactive shell in the build environment
#   docker/build.sh --push                 # push the current environment image to the registry (docker/registry)
# Output goes to build/<preset>/ in the repo, owned by you. The image is rebuilt only when docker/Dockerfile changes.
set -euo pipefail

REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
TAG=$(sha256sum "$REPO/docker/Dockerfile" | cut -c1-12)
IMAGE="slopcap-build:$TAG"

# Registry for sharing the environment between machines: docker/registry holds e.g. "registry.example:5000"
# (override with SLOPCAP_REGISTRY; empty = local only). Images are tagged with the Dockerfile's hash, so a pull
# gets exactly this environment, and every changed environment is pushed once, when it is first built.
REGISTRY=${SLOPCAP_REGISTRY-$(cat "$REPO/docker/registry" 2>/dev/null | tr -d '[:space:]' || true)}
REMOTE=${REGISTRY:+$REGISTRY/slopcap-build}

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    if [ -n "$REMOTE" ] && docker pull -q "$REMOTE:$TAG" >/dev/null 2>&1; then
        echo "pulled $REMOTE:$TAG"
        docker tag "$REMOTE:$TAG" "$IMAGE"
        docker tag "$REMOTE:$TAG" slopcap-build:latest
    else
        echo "building image $IMAGE (first run only)..."
        docker build -t "$IMAGE" -t slopcap-build:latest -f "$REPO/docker/Dockerfile" "$REPO/docker"
        if [ -n "$REMOTE" ]; then
            docker tag "$IMAGE" "$REMOTE:$TAG" && docker tag "$IMAGE" "$REMOTE:latest"
            if docker push -q "$REMOTE:$TAG" && docker push -q "$REMOTE:latest"; then
                echo "pushed $REMOTE:$TAG"
            else
                echo "warning: push to $REGISTRY failed; the image is only local (retry: docker/build.sh --push)" >&2
            fi
        fi
    fi
fi

if [ "${1:-}" = "--push" ]; then
    [ -n "$REMOTE" ] || { echo "no registry configured (docker/registry or SLOPCAP_REGISTRY)" >&2; exit 2; }
    docker tag "$IMAGE" "$REMOTE:$TAG" && docker tag "$IMAGE" "$REMOTE:latest"
    docker push "$REMOTE:$TAG" && docker push "$REMOTE:latest"
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
script="set -e"
for p in "${presets[@]}"; do
    script+="; echo '===== $p'; cmake --preset $p; cmake --build --preset $p"
    case $p in windows-cross|linux-release) ;; *) script+="; ctest --preset $p" ;; esac
done
exec "${run[@]}" "$IMAGE" bash -c "$script"
