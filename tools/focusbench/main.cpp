// slopcap-focusbench: time the per-channel focus scorer on real recordings (the preview runs it on every displayed
// frame, so it has to stay well under a frame time).
//   slopcap-focusbench FILE.ser [frames] [metric]
#include "analysis/focus.hpp"
#include "io/ser.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace slopcap;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: slopcap-focusbench FILE.ser [frames] [gradient|laplacian|variance|fourier]\n");
        return 2;
    }
    ser::Reader r(argv[1]);
    const int n = argc > 2 ? std::stoi(argv[2]) : 50;
    analysis::FocusOptions opt;
    if (argc > 3) opt.metric = analysis::metricFromName(argv[3]);
    const auto& h = r.header();
    Bayer bayer = Bayer::None;
    switch (h.colorId) {
        case ser::ColorId::BayerRGGB: bayer = Bayer::RGGB; break;
        case ser::ColorId::BayerGRBG: bayer = Bayer::GRBG; break;
        case ser::ColorId::BayerGBRG: bayer = Bayer::GBRG; break;
        case ser::ColorId::BayerBGGR: bayer = Bayer::BGGR; break;
        default: break;
    }
    std::vector<std::byte> buf(h.frameBytes());
    double total = 0, worst = 0;
    const int frames = std::min<int>(n, static_cast<int>(r.frameCount()));
    for (int i = 0; i < frames; ++i) {
        r.readFrame(i, buf);
        const auto t0 = std::chrono::steady_clock::now();
        const auto s = analysis::scoreFrame(buf, h.width, h.height, h.bitDepth > 8 ? 16 : 8, bayer, opt);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        total += ms;
        worst = std::max(worst, ms);
        if (i < 3 || i == frames - 1)
            std::printf("frame %4d  %6.2f ms  R %.5f  G %.5f  B %.5f  centre (%.1f, %.1f)\n", i, ms, s.ch[0].sharpness, s.ch[1].sharpness,
                        s.ch[2].sharpness, s.ch[1].cx, s.ch[1].cy);
    }
    std::printf("%dx%d %s, %d frames: mean %.2f ms, worst %.2f ms\n", h.width, h.height, analysis::metricName(opt.metric), frames,
                total / frames, worst);
}
