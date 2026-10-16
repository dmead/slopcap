// slopcap-bench: record through the full capture pipeline (camera -> ring -> SER writer) and report whether it kept
// up: fps, frames lost by the camera vs by the pipeline, peak ring fill, write speed.
//   slopcap-bench --camera asi|asi:N|qhy|qhy:N|sim:FILE.ser [--readout N] [--set NAME=VALUE ...]
//                 [--seconds N] [--frames N] [--roi WxH] [--bits 8|16] [--ring-mb N] [--out FILE.ser] [--keep]
//                 [--writer buffered|direct] [--chunk-mb N] [--inflight N] [--reserve-gb N]
//   slopcap-bench --sim FILE.ser [--fast] [--loop] ...     (shorthand for a simulated camera)
//   slopcap-bench --list                                   (connected cameras)
// Exit status: 0 = nothing dropped, 1 = frames dropped, 2 = usage / setup error.
#include "camera/open_camera.hpp"
#include "camera/sim_camera.hpp"
#include "pipeline/capture_session.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace slopcap;
using namespace std::chrono_literals;

namespace {

[[noreturn]] void usage(const std::string& msg = {}) {
    if (!msg.empty()) std::cerr << "error: " << msg << "\n";
    std::cerr << "usage: slopcap-bench --camera asi|asi:N|qhy|qhy:N|sim:FILE.ser [--readout N] [--set NAME=VALUE ...]\n"
                 "                     [--seconds N] [--frames N] [--roi WxH] [--bits 8|16] [--ring-mb N] [--out FILE.ser] [--keep]\n"
                 "                     [--writer buffered|direct] [--chunk-mb N] [--inflight N] [--reserve-gb N]\n"
                 "       slopcap-bench --sim FILE.ser [--fast] [--loop] ...\n"
                 "       slopcap-bench --list\n";
    std::exit(2);
}

void printStats(const CaptureStats& s, const char* prefix) {
    std::cout << std::format("{}{:6.1f} s  {:8.2f} fps  delivered {:>8}  written {:>8}  camera-dropped {:>5}  ring-dropped {:>5}  "
                             "ring peak {}/{}  write {:7.1f} MB/s  frame-write p50/p99/max {}/{}/{} us",
                             prefix, s.seconds, s.captureFps, s.delivered, s.written, s.cameraDropped, s.ringDropped,
                             s.ringHighWater, s.ringCapacity, s.writeMBps, s.writeP50Us, s.writeP99Us, s.writeMaxUs);
    if (s.chunkMaxUs) std::cout << std::format("  chunk p99/max {}/{} us  stall max {} us", s.chunkP99Us, s.chunkMaxUs, s.stallMaxUs);
    std::cout << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string sim, spec, out, roiArg;
    std::vector<std::pair<std::string, double>> sets;
    bool fast = false, loop = false, keep = false;
    int bits = 0, readout = -1;
    double seconds = 0;
    uint64_t frames = 0;
    std::size_t ringMb = 1024;
    std::string writer = "buffered";
    std::size_t chunkMb = 8;
    int inflight = 8;
    double reserveGb = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) usage(a + " needs a value");
            return argv[++i];
        };
        if (a == "--list") {
            for (const auto& c : listCameras()) std::cout << c << "\n";
            return 0;
        }
        if (a == "--sim") sim = val();
        else if (a == "--camera") spec = val();
        else if (a == "--readout") readout = std::stoi(val());
        else if (a == "--set") {
            const std::string kv = val();
            const auto eq = kv.find('=');
            if (eq == std::string::npos) usage("--set needs NAME=VALUE");
            sets.emplace_back(kv.substr(0, eq), std::stod(kv.substr(eq + 1)));
        }
        else if (a == "--fast") fast = true;
        else if (a == "--loop") loop = true;
        else if (a == "--keep") keep = true;
        else if (a == "--seconds") seconds = std::stod(val());
        else if (a == "--frames") frames = std::stoull(val());
        else if (a == "--roi") roiArg = val();
        else if (a == "--bits") bits = std::stoi(val());
        else if (a == "--ring-mb") ringMb = std::stoull(val());
        else if (a == "--out") out = val();
        else if (a == "--writer") writer = val();
        else if (a == "--chunk-mb") chunkMb = std::stoull(val());
        else if (a == "--inflight") inflight = std::stoi(val());
        else if (a == "--reserve-gb") reserveGb = std::stod(val());
        else usage("unknown option " + a);
    }
    if (sim.empty() == spec.empty()) usage("give exactly one of --camera SPEC or --sim FILE.ser");

    try {
        std::unique_ptr<ICamera> owned = sim.empty() ? openCamera(spec)
                                                     : std::make_unique<SimCamera>(sim, SimOptions{fast ? SimPace::AsFastAsPossible : SimPace::Realtime, loop});
        ICamera& cam = *owned;
        if (readout >= 0) cam.setReadoutMode(readout);
        if (!roiArg.empty()) {
            const auto x = roiArg.find('x');
            if (x == std::string::npos) usage("--roi must look like 1040x560");
            const int w = std::stoi(roiArg.substr(0, x)), h = std::stoi(roiArg.substr(x + 1));
            const auto& inf = cam.info();
            cam.setRoi({(inf.sensorWidth - w) / 4 * 2, (inf.sensorHeight - h) / 4 * 2, w, h, 1});   // centred, even
        }
        if (bits) cam.setBitDepth(bits);
        for (const auto& [name, value] : sets) cam.setControl(name, value);

        CaptureOptions opt;
        const bool tempOut = out.empty();
        opt.output = tempOut ? std::filesystem::temp_directory_path() /
                                     std::format("slopcap-bench-{}.ser", std::chrono::steady_clock::now().time_since_epoch().count())
                              : std::filesystem::path(out);
        if (seconds > 0) opt.duration = std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000));
        if (frames > 0) opt.frameLimit = frames;
        opt.ringBytes = ringMb << 20;
        if (writer != "buffered" && writer != "direct") usage("--writer must be buffered or direct");
        opt.directIO = writer == "direct";
        opt.direct.chunkBytes = chunkMb << 20;
        opt.direct.inflight = inflight;
        opt.direct.reserveBytes = static_cast<uint64_t>(reserveGb * 1e9);

        const auto r = cam.roi();
        const auto& inf = cam.info();
        std::cout << std::format("camera {} {} [{}]  roi {}x{}+{}+{}  {}-bit", inf.vendor, inf.model, inf.serial, r.width, r.height, r.x, r.y,
                                 cam.bitDepth());
        if (auto modes = cam.readoutModes(); !modes.empty()) std::cout << "  readout '" << modes[static_cast<std::size_t>(cam.readoutMode())].name << "'";
        for (const char* c : {"Gain", "Exposure", "Offset", "BandWidth", "USBTraffic"})
            if (auto v = cam.control(c)) std::cout << std::format("  {}={}", c, *v);
        std::cout << std::format("\nring {} MB, {} writer{} -> {}\n", ringMb, writer,
                                 opt.directIO ? std::format(" ({} MB chunks x {})", chunkMb, inflight) : "", opt.output.string());

        CaptureSession session(cam, opt);
        session.start();
        while (!session.wait(1s)) printStats(session.stats(), "  ");
        const auto s = session.stats();
        printStats(s, "done ");
        if (!s.error.empty()) std::cout << "error: " << s.error << "\n";
        if (tempOut && !keep) std::filesystem::remove(opt.output);
        const bool clean = s.error.empty() && s.cameraDropped == 0 && s.ringDropped == 0 && s.written == s.delivered;
        std::cout << (clean ? "RESULT: 0 dropped\n" : "RESULT: frames dropped or not written\n");
        return clean ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
