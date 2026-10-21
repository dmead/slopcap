#!/bin/bash
# C++ vs Rust: SlopCap's per-frame kernels (debayer + stretch, focus scoring with the gradient and Fourier metrics, the
# lossless codec's predictor) in C++ built by GCC and by clang, and their Rust ports (rust/), on the same recordings.
#   MSYS_NO_PATHCONV=1 tools/rust-vs-cpp.sh [ROUNDS] FILE.ser...      (on Windows: from Git Bash)
# Runs in the slopcap-rust image (docker/Dockerfile.rust: the build image plus rustc, cargo, clang). Every build
# targets x86-64-v3 (AVX2) at -O3 / opt-level 3. Rust is built twice: safe code only, and with the focus scorer's sums
# through AVX2 intrinsics (rust/README.md). The four run in rotating order, ROUNDS times (default 5), pinned to one
# CPU; each keeps its best median. Outputs must agree (render and lossless byte for byte, focus scores to float
# rounding) before any timing is reported. Results: build/rust-vs-cpp/results.md.
set -euo pipefail

REPO=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
ROUNDS=5
if [[ "${1:-}" =~ ^[0-9]+$ ]]; then ROUNDS=$1; shift; fi
[ $# -gt 0 ] || { echo "usage: tools/rust-vs-cpp.sh [ROUNDS] FILE.ser..." >&2; exit 2; }
BASE=slopcap-build:$(sha256sum < "$REPO/docker/Dockerfile" | cut -c1-12)
docker image inspect "$BASE" >/dev/null 2>&1 || { echo "no $BASE yet: run docker/build.sh once" >&2; exit 2; }
(cd "$REPO/docker" && docker build -q -t slopcap-rust --build-arg "BASE=$BASE" -f Dockerfile.rust . >/dev/null)   # relative: Windows' docker can't read /d/... contexts

mounts=(-v "$REPO:$REPO")
files=()
for f in "$@"; do
    f=$(readlink -f "$f")
    mounts+=(-v "$(dirname "$f"):/data/$(basename "$(dirname "$f")"):ro")
    files+=("/data/$(basename "$(dirname "$f")")/$(basename "$f")")
done
OUT=$REPO/build/rust-vs-cpp
mkdir -p "$OUT"

docker run --rm --user "$(id -u):$(id -g)" "${mounts[@]}" -w "$REPO" -e ROUNDS="$ROUNDS" slopcap-rust bash -c '
set -euo pipefail
OUT=build/rust-vs-cpp
flags="-O3 -march=x86-64-v3"
echo "== building: GCC, clang, Rust" >&2
for cc in g++ clang++; do   # the linux-release preset'"'"'s settings, in folders of their own
    cmake -S . -B $OUT/$cc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=$cc "-DCMAKE_CXX_FLAGS=$flags" \
          -DSLOPCAP_WERROR=ON -DSLOPCAP_BUILD_TESTS=OFF -DSLOPCAP_BUILD_CAMPROBE=OFF >/dev/null
    cmake --build $OUT/$cc --target slopcap-kernelbench >/dev/null
done
# Rust twice: plain safe code (no-default-features) and with the AVX2 lanes (feature simd, the default).
CARGO_TARGET_DIR=$OUT/rust-safe RUSTFLAGS="-C target-cpu=x86-64-v3" \
    cargo build --release --offline --quiet --no-default-features --manifest-path rust/Cargo.toml
CARGO_TARGET_DIR=$OUT/rust RUSTFLAGS="-C target-cpu=x86-64-v3" \
    cargo build --release --offline --quiet --manifest-path rust/Cargo.toml
declare -A bin=([gcc]=$OUT/g++/slopcap-kernelbench [clang]=$OUT/clang++/slopcap-kernelbench
                [rust-safe]=$OUT/rust-safe/release/kernelbench [rust-simd]=$OUT/rust/release/kernelbench)
{
    echo "# $(g++ --version | head -1) | $(clang++ --version | head -1) | $(rustc --version)"
    echo "# $(grep -m1 "model name" /proc/cpuinfo | cut -d: -f2- | xargs)"
} > $OUT/raw.txt
cpu=$(( $(nproc) / 2 ))
impls=(gcc clang rust-safe rust-simd)
for round in $(seq 1 "$ROUNDS"); do
    for f in '"${files[*]}"'; do
        for k in 0 1 2 3; do
            i=${impls[$(( (k + round) % 4 ))]}
            echo "== round $round/$ROUNDS  $(basename $f)  $i" >&2
            taskset -c $cpu ${bin[$i]} "$f" --repeat 5 | sed "s/^[a-z]* /$i /" >> $OUT/raw.txt
        done
    done
done
'
docker run --rm -i --user "$(id -u):$(id -g)" -v "$REPO:$REPO" -w "$REPO" slopcap-rust python3 - "$OUT/raw.txt" "$OUT/results.md" <<'PY'   # the image's Python: a host may have none
import sys, collections
raw, md = sys.argv[1], sys.argv[2]
best, checks, env = {}, collections.defaultdict(dict), []
for line in open(raw):
    if line.startswith("#"):
        env.append(line[1:].strip()); continue
    impl, name, size, depth, kernel, *kv = line.split()
    d = dict(x.split("=", 1) for x in kv)
    key = (name, size, depth, kernel)
    t = float(d["median_ms"])
    best[key, impl] = min(best.get((key, impl), t), t)
    checks[key][impl] = d["check"]
bad = []
for key, c in checks.items():
    vals = list(c.values())
    if key[3].startswith("focus"):
        f = [float(v) for v in vals]
        if max(f) - min(f) > 1e-4 * max(1e-12, max(abs(x) for x in f)): bad.append((key, c))
    elif len(set(vals)) > 1:
        bad.append((key, c))
out = ["# C++ vs Rust: SlopCap's per-frame kernels", "", *[f"- {e}" for e in env], "",
       "Best median per frame (ms) over every round; lower is better. Ratios: Rust (SIMD) time over the C++ time "
       "(below 1 = Rust faster). Rust safe: no intrinsics; Rust SIMD: the focus scorer's sums through AVX2 intrinsics.", "",
       "| Recording | Kernel | C++ GCC | C++ clang | Rust safe | Rust SIMD | SIMD / GCC | SIMD / clang |",
       "|---|---|---:|---:|---:|---:|---:|---:|"]
for key in sorted(checks):
    g, c, rs, r = (best.get((key, i)) for i in ("gcc", "clang", "rust-safe", "rust-simd"))
    out.append(f"| {key[0]} {key[1]} {key[2]} | {key[3]} | {g:.3f} | {c:.3f} | {rs:.3f} | {r:.3f} | {r/g:.2f} | {r/c:.2f} |")
out += ["", "Outputs: " + ("all match (render and lossless byte for byte, focus scores within 1e-4)." if not bad else
        "MISMATCH: " + "; ".join(f"{k[0]} {k[3]}: {v}" for k, v in bad))]
open(md, "w").write("\n".join(out) + "\n")
print("\n".join(out))
sys.exit(1 if bad else 0)
PY
