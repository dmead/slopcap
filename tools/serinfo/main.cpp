// slopcap-serinfo: summarise SER recordings: header, frames on disk vs header, timestamps, real frame rate.
//   slopcap-serinfo FILE.ser [...]
#include "io/ser.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <iostream>

using namespace slopcap;

namespace {

std::string utc(int64_t ticks) {
    if (ticks <= 0) return "-";
    auto tp = std::chrono::floor<std::chrono::milliseconds>(ser::fromTicks(ticks));
    return std::format("{:%Y-%m-%d %H:%M:%S} UTC", tp);
}

int summarise(const char* path) {
    try {
        ser::Reader r(path);
        const auto& h = r.header();
        std::cout << std::format("{}\n", path);
        std::cout << std::format("  {} x {}  {}-bit  {}  {:.2f} GB\n", h.width, h.height, h.bitDepth, ser::colorName(h.colorId),
                                 static_cast<double>(r.fileSize()) / 1e9);
        std::cout << std::format("  frames: {} on disk, {} in header{}\n", r.frameCount(), h.frameCount,
                                 r.truncated() ? "  TRUNCATED (recording cut off; header not finalised)" : "");
        if (!h.observer.empty() || !h.instrument.empty() || !h.telescope.empty())
            std::cout << std::format("  observer '{}'  instrument '{}'  telescope '{}'\n", h.observer, h.instrument, h.telescope);
        std::cout << std::format("  started {}\n", utc(h.dateTimeUtcTicks));
        if (const auto& ts = r.timestamps(); ts && ts->size() > 1) {
            const double span = static_cast<double>(ts->back() - ts->front()) / ser::kTicksPerSecond;
            // largest gap between consecutive frames: a hint of dropped frames or stalls
            int64_t maxGap = 0;
            for (std::size_t i = 1; i < ts->size(); ++i) maxGap = std::max(maxGap, (*ts)[i] - (*ts)[i - 1]);
            const double avg = span / static_cast<double>(ts->size() - 1);
            std::cout << std::format("  timestamps: {} -> {}  ({:.3f} s, {:.2f} fps, longest gap {:.1f} ms = {:.1f} frames)\n",
                                     utc(ts->front()), utc(ts->back()), span, 1.0 / avg,
                                     static_cast<double>(maxGap) / 1e4, static_cast<double>(maxGap) / ser::kTicksPerSecond / avg);
        } else {
            std::cout << "  timestamps: none\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << std::format("{}: {}\n", path, e.what());
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: slopcap-serinfo FILE.ser [...]\n";
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; ++i) rc |= summarise(argv[i]);
    return rc;
}
