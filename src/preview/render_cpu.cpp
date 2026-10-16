#include "preview/render_cpu.hpp"

#include <algorithm>
#include <cmath>

namespace slopcap::preview {

namespace {

/// Mirror an out-of-range coordinate back inside (-1 -> 1, n -> n-2): unlike clamping this keeps the Bayer parity.
int mirror(int v, int n) {
    if (n == 1) return 0;
    if (v < 0) v = -v;
    if (v >= n) v = 2 * n - 2 - v;
    return std::clamp(v, 0, n - 1);
}

/// Sample at (x, y) as 0..1, edges mirrored.
struct Raw {
    const std::byte* p;
    int w, h, bits;
    double at(int x, int y) const {
        x = mirror(x, w);
        y = mirror(y, h);
        const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
        if (bits > 8) {
            const unsigned v = std::to_integer<unsigned>(p[2 * i]) | (std::to_integer<unsigned>(p[2 * i + 1]) << 8);
            return v / 65535.0;
        }
        return std::to_integer<unsigned>(p[i]) / 255.0;
    }
};

/// Colour of the Bayer site at (x, y): 0 = R, 1 = G, 2 = B. The pattern names the top-left 2x2 block in reading order.
int siteColor(Bayer b, int x, int y) {
    const int idx = (y & 1) * 2 + (x & 1);
    static constexpr int kRGGB[4] = {0, 1, 1, 2}, kGRBG[4] = {1, 0, 2, 1}, kGBRG[4] = {1, 2, 0, 1}, kBGGR[4] = {2, 1, 1, 0};
    switch (b) {
        case Bayer::RGGB: return kRGGB[idx];
        case Bayer::GRBG: return kGRBG[idx];
        case Bayer::GBRG: return kGBRG[idx];
        case Bayer::BGGR: return kBGGR[idx];
        case Bayer::None: break;
    }
    return 1;
}

uint8_t stretch(double v, const Levels& lv) {
    const double span = std::max(1e-9, lv.white - lv.black);
    double x = std::clamp((v - lv.black) / span, 0.0, 1.0);
    if (lv.gamma != 1.0) x = std::pow(x, 1.0 / lv.gamma);
    return static_cast<uint8_t>(std::lround(x * 255.0));
}

}  // namespace

std::array<uint32_t, 256> histogram(std::span<const std::byte> pixels, int bitDepth, int stride) {
    std::array<uint32_t, 256> h{};
    stride = std::max(1, stride);
    if (bitDepth > 8) {
        for (std::size_t i = 1; i < pixels.size(); i += 2 * static_cast<std::size_t>(stride)) ++h[std::to_integer<uint8_t>(pixels[i])];
    } else {
        for (std::size_t i = 0; i < pixels.size(); i += static_cast<std::size_t>(stride)) ++h[std::to_integer<uint8_t>(pixels[i])];
    }
    return h;
}

Levels autoLevels(const std::array<uint32_t, 256>& hist, double blackPercentile, double whitePercentile) {
    uint64_t total = 0;
    for (auto c : hist) total += c;
    if (total == 0) return {};
    auto levelAt = [&](double p) {
        const auto target = static_cast<uint64_t>(p * static_cast<double>(total));
        uint64_t seen = 0;
        for (int b = 0; b < 256; ++b) {
            seen += hist[static_cast<std::size_t>(b)];
            if (seen > target) return b;
        }
        return 255;
    };
    Levels lv;
    lv.black = levelAt(blackPercentile) / 255.0;
    lv.white = std::max(levelAt(whitePercentile) + 1, static_cast<int>(lv.black * 255.0) + 2) / 255.0;
    lv.white = std::min(lv.white, 1.0);
    return lv;
}

std::vector<uint8_t> renderRgb(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer, const Levels& lv) {
    std::vector<uint8_t> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    const Raw raw{pixels.data(), width, height, bitDepth};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double rgb[3];
            if (bayer == Bayer::None) {
                rgb[0] = rgb[1] = rgb[2] = raw.at(x, y);
            } else {
                // Bilinear: own colour from the site, the others averaged from the neighbours that carry them.
                double sum[3] = {0, 0, 0};
                int n[3] = {0, 0, 0};
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int c = siteColor(bayer, mirror(x + dx, width), mirror(y + dy, height));
                        sum[c] += raw.at(x + dx, y + dy);
                        ++n[c];
                    }
                const int own = siteColor(bayer, x, y);
                for (int c = 0; c < 3; ++c) rgb[c] = c == own ? raw.at(x, y) : (n[c] ? sum[c] / n[c] : 0.0);
            }
            uint8_t* o = &out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 3];
            for (int c = 0; c < 3; ++c) o[c] = stretch(rgb[c], lv);
        }
    }
    return out;
}

}  // namespace slopcap::preview
