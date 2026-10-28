// slopcap-serinfo: summarise SER recordings: header, frames on disk vs header, timestamps, real frame rate, and the
// gaps that point to lost frames. Reads any program's SER files the same way, so it's the yardstick for comparing them.
//   slopcap-serinfo FILE.ser [...]
//   slopcap-serinfo --csv FILE.ser [...]     (one line per file, with a header line; --no-header leaves it out)
#include "io/ser.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>

using namespace slopcap;

namespace {

std::string utc(int64_t ticks) {
    if (ticks <= 0) return "-";
    auto tp = std::chrono::floor<std::chrono::milliseconds>(ser::fromTicks(ticks));
    return std::format("{:%Y-%m-%d %H:%M:%S} UTC", tp);
}

std::string iso(std::chrono::system_clock::time_point tp) {
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", std::chrono::floor<std::chrono::milliseconds>(tp));
}

std::string isoTicks(int64_t ticks) { return ticks > 0 ? iso(ser::fromTicks(ticks)) : ""; }

std::string modified(const std::filesystem::path& p) {
    const auto t = std::filesystem::last_write_time(p);
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    return iso(std::chrono::clock_cast<std::chrono::system_clock>(t));   // MSVC's file_clock has no to_sys
#else
    return iso(std::chrono::file_clock::to_sys(t));
#endif
}

std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string q = "\"";
    for (char c : s) q += c == '"' ? std::string("\"\"") : std::string(1, c);
    return q + "\"";
}

constexpr const char* kCsvHeader =
    "file,width,height,bits,color,frames,header_frames,truncated,file_gb,timestamps,first_utc,last_utc,modified_utc,"
    "span_s,fps,period_ms,missing,max_gap_ms,p99_ms,backwards,mb_s,instrument";

int csvLine(const char* path) {
    try {
        ser::Reader r(path);
        const auto& h = r.header();
        const auto& ts = r.timestamps();
        const ser::Timing t = ts ? ser::timing(*ts) : ser::Timing{};
        const double mbps = t.spanSeconds > 0 ? static_cast<double>(r.frameCount()) * static_cast<double>(h.frameBytes()) / t.spanSeconds / 1e6 : 0;
        std::cout << std::format("{},{},{},{},{},{},{},{},{:.3f},{},{},{},{},{:.3f},{:.3f},{:.4f},{},{:.1f},{:.1f},{},{:.1f},{}\n", csvField(path),
                                 h.width, h.height, h.bitDepth, ser::colorName(h.colorId), r.frameCount(), h.frameCount, r.truncated() ? 1 : 0,
                                 static_cast<double>(r.fileSize()) / 1e9, ts ? ts->size() : 0, ts && !ts->empty() ? isoTicks(ts->front()) : "",
                                 ts && !ts->empty() ? isoTicks(ts->back()) : "", modified(path), t.spanSeconds, t.fps, t.periodMs, t.missing,
                                 t.maxGapMs, t.p99Ms, t.backwards, mbps, csvField(h.instrument));
        return 0;
    } catch (const std::exception& e) {
        std::cerr << std::format("{}: {}\n", path, e.what());
        return 1;
    }
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
            const ser::Timing t = ser::timing(*ts);
            std::cout << std::format("  timestamps: {} -> {}  ({:.3f} s, {:.2f} fps, frame period {:.3f} ms)\n", utc(ts->front()),
                                     utc(ts->back()), t.spanSeconds, t.fps, t.periodMs);
            std::cout << std::format("  ~{} frames missing; intervals: longest {:.1f} ms, 99% under {:.1f} ms{}\n", t.missing, t.maxGapMs,
                                     t.p99Ms, t.backwards ? std::format(", {} not after the one before", t.backwards) : "");
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
    bool csv = false, header = true;
    std::vector<const char*> files;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--csv") == 0) csv = true;
        else if (std::strcmp(argv[i], "--no-header") == 0) header = false;
        else files.push_back(argv[i]);
    }
    if (files.empty()) {
        std::cerr << "usage: slopcap-serinfo [--csv [--no-header]] FILE.ser [...]\n";
        return 2;
    }
    if (csv && header) std::cout << kCsvHeader << "\n";
    int rc = 0;
    for (const char* f : files) rc |= csv ? csvLine(f) : summarise(f);
    return rc;
}
