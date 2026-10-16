// slopcap-bench: record through the full capture pipeline (camera -> ring -> SER writer) and report whether it kept
// up: fps, frames lost by the camera vs by the pipeline, peak ring fill, write speed.
//   slopcap-bench --sim FILE.ser [--fast] [--loop] [--seconds N] [--frames N] [--roi WxH] [--bits 8|16]
//                 [--ring-mb N] [--out FILE.ser] [--keep]
// Exit status: 0 = nothing dropped, 1 = frames dropped, 2 = usage / setup error.
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
    std::cerr << "usage: slopcap-bench --sim FILE.ser [--fast] [--loop] [--seconds N] [--frames N] [--roi WxH]\n"
                 "                     [--bits 8|16] [--ring-mb N] [--out FILE.ser] [--keep]\n";
    std::exit(2);
}

void printStats(const CaptureStats& s, const char* prefix) {
    std::cout << std::format("{}{:6.1f} s  {:8.2f} fps  delivered {:>8}  written {:>8}  camera-dropped {:>5}  ring-dropped {:>5}  "
                             "ring peak {}/{}  write {:7.1f} MB/s\n",
                             prefix, s.seconds, s.captureFps, s.delivered, s.written, s.cameraDropped, s.ringDropped,
                             s.ringHighWater, s.ringCapacity, s.writeMBps);
}

}  // namespace

int main(int argc, char** argv) {
    std::string sim, out, roiArg;
    bool fast = false, loop = false, keep = false;
    int bits = 0;
    double seconds = 0;
    uint64_t frames = 0;
    std::size_t ringMb = 1024;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) usage(a + " needs a value");
            return argv[++i];
        };
        if (a == "--sim") sim = val();
        else if (a == "--fast") fast = true;
        else if (a == "--loop") loop = true;
        else if (a == "--keep") keep = true;
        else if (a == "--seconds") seconds = std::stod(val());
        else if (a == "--frames") frames = std::stoull(val());
        else if (a == "--roi") roiArg = val();
        else if (a == "--bits") bits = std::stoi(val());
        else if (a == "--ring-mb") ringMb = std::stoull(val());
        else if (a == "--out") out = val();
        else usage("unknown option " + a);
    }
    if (sim.empty()) usage("--sim FILE.ser is required (real cameras come with the ZWO/QHY backends)");

    try {
        SimCamera cam(sim, {fast ? SimPace::AsFastAsPossible : SimPace::Realtime, loop});
        if (!roiArg.empty()) {
            const auto x = roiArg.find('x');
            if (x == std::string::npos) usage("--roi must look like 1040x560");
            const int w = std::stoi(roiArg.substr(0, x)), h = std::stoi(roiArg.substr(x + 1));
            const auto& inf = cam.info();
            cam.setRoi({(inf.sensorWidth - w) / 4 * 2, (inf.sensorHeight - h) / 4 * 2, w, h, 1});   // centred, even
        }
        if (bits) cam.setBitDepth(bits);

        CaptureOptions opt;
        const bool tempOut = out.empty();
        opt.output = tempOut ? std::filesystem::temp_directory_path() /
                                     std::format("slopcap-bench-{}.ser", std::chrono::steady_clock::now().time_since_epoch().count())
                              : std::filesystem::path(out);
        if (seconds > 0) opt.duration = std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000));
        if (frames > 0) opt.frameLimit = frames;
        opt.ringBytes = ringMb << 20;

        const auto r = cam.roi();
        std::cout << std::format("source {} ({} frames)  roi {}x{}  {}-bit  {}  ring {} MB  -> {}\n", sim, cam.sourceFrames(), r.width,
                                 r.height, cam.bitDepth(), fast ? "as fast as possible" : "real-time", ringMb, opt.output.string());

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
