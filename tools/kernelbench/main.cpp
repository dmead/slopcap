// slopcap-kernelbench: time SlopCap's per-frame kernels on a recording. The twin of the Rust rust/src/bin/kernelbench.rs
// (same frames, same kernels, same output format), for tools/rust-vs-cpp.sh.
//   slopcap-kernelbench FILE.ser [--frames N] [--repeat R] [--kernels render,focus-gradient,focus-fourier,lossless]
// Prints one line per kernel: median and mean ms per frame over every timed call, and a check value (a hash of the
// output bytes, or the sum of the focus scores) to compare with the Rust.
#include "analysis/focus.hpp"
#include "io/ser.hpp"
#include "net/frame_codec.hpp"
#include "preview/render_cpu.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace slopcap;

namespace {

/// FNV-1a, 64-bit: a cheap fingerprint of output bytes.
uint64_t fnv(uint64_t h, const uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
constexpr uint64_t kFnv0 = 0xcbf29ce484222325ull;

/// Keep a result alive so the optimizer can't drop the work (Rust: std::hint::black_box).
template <typename T>
void keep(const T& v) {
    asm volatile("" : : "g"(&v) : "memory");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: slopcap-kernelbench FILE.ser [--frames N] [--repeat R] [--kernels a,b,...]\n");
        return 2;
    }
    const std::filesystem::path path = argv[1];
    std::size_t maxFrames = SIZE_MAX, repeat = 5;
    std::string kernels = "render,focus-gradient,focus-fourier,lossless";
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--frames") maxFrames = std::stoul(argv[i + 1]);
        else if (a == "--repeat") repeat = std::stoul(argv[i + 1]);
        else if (a == "--kernels") kernels = argv[i + 1];
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    ser::Reader r(path);
    const auto& h = r.header();
    Bayer bayer = Bayer::None;
    switch (h.colorId) {
        case ser::ColorId::BayerRGGB: bayer = Bayer::RGGB; break;
        case ser::ColorId::BayerGRBG: bayer = Bayer::GRBG; break;
        case ser::ColorId::BayerGBRG: bayer = Bayer::GBRG; break;
        case ser::ColorId::BayerBGGR: bayer = Bayer::BGGR; break;
        default: break;
    }
    const int w = h.width, ht = h.height, bits = h.bitDepth > 8 ? 16 : 8;
    std::vector<std::vector<std::byte>> frames(std::min<std::size_t>(maxFrames, static_cast<std::size_t>(r.frameCount())));
    for (std::size_t k = 0; k < frames.size(); ++k) {
        frames[k].resize(h.frameBytes());
        r.readFrame(static_cast<int64_t>(k), frames[k]);
    }
    const std::string name = path.stem().string();

    std::stringstream list(kernels);
    for (std::string kernel; std::getline(list, kernel, ',');) {
        // One untimed pass (caches, page faults, the check value), then `repeat` timed passes over every frame.
        std::vector<double> times;
        uint64_t checkHash = kFnv0;
        double checkSum = 0;
        for (std::size_t pass = 0; pass <= repeat; ++pass) {
            for (const auto& f : frames) {
                const auto t0 = std::chrono::steady_clock::now();
                double ms = 0;
                auto stop = [&] { ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
                if (kernel == "render") {
                    const auto hist = preview::histogram(f, bits, 1);
                    const auto lv = preview::autoLevels(hist, 0.5, 0.9995);
                    const auto rgb = preview::renderRgb(f, w, ht, bits, bayer, lv);
                    stop();
                    if (pass == 0) checkHash = fnv(checkHash, rgb.data(), rgb.size());
                    keep(rgb);
                } else if (kernel == "focus-gradient" || kernel == "focus-fourier") {
                    analysis::FocusOptions opt;
                    opt.metric = kernel == "focus-gradient" ? analysis::Metric::Gradient : analysis::Metric::Fourier;
                    const auto s = analysis::scoreFrame(f, w, ht, bits, bayer, opt);
                    stop();
                    if (pass == 0)
                        for (int c = 0; c < s.channels; ++c) checkSum += s.ch[static_cast<std::size_t>(c)].sharpness;
                    keep(s);
                } else if (kernel == "lossless") {
                    std::vector<uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(ht) * (bits > 8 ? 2 : 1));
                    const int shift = net::losslessResiduals(f, w, ht, bits, bayer, out.data());
                    stop();
                    if (pass == 0) {
                        checkHash = fnv(checkHash, out.data(), out.size());
                        const auto s8 = static_cast<uint8_t>(shift);
                        checkHash = fnv(checkHash, &s8, 1);
                    }
                    keep(out);
                } else {
                    std::fprintf(stderr, "unknown kernel %s\n", kernel.c_str());
                    return 2;
                }
                if (pass > 0) times.push_back(ms);
            }
        }
        std::sort(times.begin(), times.end());
        const double median = times[times.size() / 2];
        const double mean = std::accumulate(times.begin(), times.end(), 0.0) / static_cast<double>(times.size());
        char check[64];
        if (kernel.starts_with("focus")) std::snprintf(check, sizeof check, "%.9e", checkSum);
        else std::snprintf(check, sizeof check, "%016llx", static_cast<unsigned long long>(checkHash));
        std::printf("cpp %s %dx%d %d-bit %s frames=%zu calls=%zu median_ms=%.4f mean_ms=%.4f min_ms=%.4f check=%s\n", name.c_str(), w, ht,
                    h.bitDepth, kernel.c_str(), frames.size(), times.size(), median, mean, times.front(), check);
    }
}
