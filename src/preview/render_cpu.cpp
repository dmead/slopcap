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

uint8_t stretch(double v, const Levels& lv, int channel) {
    v *= lv.wb[static_cast<std::size_t>(channel)];
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

RawHistogram rawHistogram(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer,
                          std::optional<int> adcBits, int bins, std::size_t maxSamples) {
    RawHistogram r;
    const std::size_t bytesPer = bitDepth > 8 ? 2 : 1;
    if (width < 2 || height < 2 || pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bytesPer) return r;
    r.channels = bayer == Bayer::None ? 1 : 3;
    // 8-bit: as delivered. 16-bit: MSB-aligned, so shift down to the ADC's range when it's known.
    r.adcBits = bitDepth > 8 ? std::clamp(adcBits.value_or(16), 8, 16) : 8;
    const int shift = bitDepth > 8 ? 16 - r.adcBits : 0;
    r.fullScale = (1 << r.adcBits) - 1;

    // Whole 2x2 blocks, every `step`-th block each way, so each channel is sampled evenly.
    const double total = static_cast<double>(width) * height;
    const int step = std::max(1, static_cast<int>(std::ceil(std::sqrt(total / static_cast<double>(std::max<std::size_t>(4, maxSamples))))));
    std::array<std::vector<uint32_t>, 3> exact;   // per ADU value, for exact medians
    for (int c = 0; c < r.channels; ++c) exact[static_cast<std::size_t>(c)].assign(static_cast<std::size_t>(r.fullScale) + 1, 0);
    for (int by = 0; by + 1 < height; by += 2 * step) {
        for (int bx = 0; bx + 1 < width; bx += 2 * step) {
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int x = bx + dx, y = by + dy;
                    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
                    unsigned v = bitDepth > 8 ? (std::to_integer<unsigned>(pixels[2 * i]) | (std::to_integer<unsigned>(pixels[2 * i + 1]) << 8)) >> shift
                                              : std::to_integer<unsigned>(pixels[i]);
                    v = std::min(v, static_cast<unsigned>(r.fullScale));
                    const int c = r.channels == 1 ? 0 : siteColor(bayer, x, y);
                    ++exact[static_cast<std::size_t>(c)][v];
                }
            }
        }
    }
    bins = std::max(1, bins);
    for (int c = 0; c < r.channels; ++c) {
        const auto ci = static_cast<std::size_t>(c);
        const auto& e = exact[ci];
        auto& b = r.bins[ci];
        b.assign(static_cast<std::size_t>(bins), 0);
        uint64_t n = 0;
        for (std::size_t v = 0; v < e.size(); ++v) {
            if (!e[v]) continue;
            n += e[v];
            b[std::min(static_cast<std::size_t>(bins) - 1, v * static_cast<std::size_t>(bins) / e.size())] += e[v];
            r.max[ci] = static_cast<int>(v);
        }
        r.samples[ci] = n;
        if (!n) continue;
        r.clipped[ci] = static_cast<double>(e.back()) / static_cast<double>(n);
        uint64_t seen = 0;
        for (std::size_t v = 0; v < e.size(); ++v) {
            seen += e[v];
            if (2 * seen >= n) {
                r.median[ci] = static_cast<int>(v);
                break;
            }
        }
    }
    return r;
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
            for (int c = 0; c < 3; ++c) o[c] = stretch(rgb[c], lv, c);
        }
    }
    return out;
}

std::array<double, 3> autoWhiteBalance(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer) {
    if (bayer == Bayer::None || width < 4 || height < 4) return {1, 1, 1};
    const Raw raw{pixels.data(), width, height, bitDepth};
    // sample ~20k 2x2 cells
    const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(width) * height / 4 / 20000.0)));
    struct Cell { double c[3]; };
    std::vector<Cell> cells;
    double gmax = 0;
    for (int y = 0; y + 1 < height; y += 2 * step)
        for (int x = 0; x + 1 < width; x += 2 * step) {
            Cell cell{{0, 0, 0}};
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int c = siteColor(bayer, x + dx, y + dy);
                    cell.c[c] += raw.at(x + dx, y + dy) * (c == 1 ? 0.5 : 1.0);
                }
            gmax = std::max(gmax, cell.c[1]);
            cells.push_back(cell);
        }
    // the target: cells well above the sky, not clipped
    double sum[3] = {0, 0, 0};
    for (const auto& cell : cells)
        if (cell.c[1] > 0.3 * gmax && cell.c[0] < 0.98 && cell.c[1] < 0.98 && cell.c[2] < 0.98)
            for (int c = 0; c < 3; ++c) sum[c] += cell.c[c];
    if (sum[0] <= 0 || sum[2] <= 0 || sum[1] <= 0) return {1, 1, 1};
    return {std::clamp(sum[1] / sum[0], 0.25, 4.0), 1.0, std::clamp(sum[1] / sum[2], 0.25, 4.0)};
}

double balancedPeak(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer,
                    const std::array<double, 3>& gains) {
    if (bayer == Bayer::None || width < 4 || height < 4) return 0;
    const Raw raw{pixels.data(), width, height, bitDepth};
    const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(width) * height / 4 / 20000.0)));
    std::vector<double> bright;   // each cell's brightest channel after the gains
    double gmax = 0;
    std::vector<std::array<double, 3>> cells;
    for (int y = 0; y + 1 < height; y += 2 * step)
        for (int x = 0; x + 1 < width; x += 2 * step) {
            std::array<double, 3> c{0, 0, 0};
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int s = siteColor(bayer, x + dx, y + dy);
                    c[static_cast<std::size_t>(s)] += raw.at(x + dx, y + dy) * (s == 1 ? 0.5 : 1.0);
                }
            gmax = std::max(gmax, c[1]);
            cells.push_back(c);
        }
    for (const auto& c : cells)
        if (c[1] > 0.3 * gmax) bright.push_back(std::max({c[0] * gains[0], c[1] * gains[1], c[2] * gains[2]}));
    if (bright.empty()) return 0;
    const auto k = static_cast<std::ptrdiff_t>(0.999 * static_cast<double>(bright.size() - 1));
    std::nth_element(bright.begin(), bright.begin() + k, bright.end());
    return bright[static_cast<std::size_t>(k)];
}

std::vector<std::byte> sampleMosaic(std::span<const std::byte> pixels, int width, int height, int bitDepth, int x, int y, int w, int h,
                                    int cellStep, int& outWidth, int& outHeight) {
    outWidth = outHeight = 0;
    const int bpp = bitDepth > 8 ? 2 : 1;
    const int x0 = std::clamp(x, 0, width) & ~1, y0 = std::clamp(y, 0, height) & ~1;
    const int x1 = std::clamp(x + w, 0, width), y1 = std::clamp(y + h, 0, height);
    const int cellsX = (x1 - x0) / 2, cellsY = (y1 - y0) / 2;
    if (cellsX <= 0 || cellsY <= 0 || pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bpp) return {};
    const int k = std::max(1, cellStep);
    const int outCx = (cellsX + k - 1) / k, outCy = (cellsY + k - 1) / k;
    outWidth = outCx * 2, outHeight = outCy * 2;
    std::vector<std::byte> out(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight) * bpp);
    const auto rowBytes = static_cast<std::size_t>(width) * bpp, outRowBytes = static_cast<std::size_t>(outWidth) * bpp;
    for (int cy = 0; cy < outCy; ++cy)
        for (int r = 0; r < 2; ++r) {
            const std::byte* src = pixels.data() + static_cast<std::size_t>(y0 + cy * k * 2 + r) * rowBytes;
            std::byte* dst = out.data() + static_cast<std::size_t>(cy * 2 + r) * outRowBytes;
            for (int cx = 0; cx < outCx; ++cx)
                std::copy_n(src + static_cast<std::size_t>(x0 + cx * k * 2) * bpp, 2 * bpp, dst + static_cast<std::size_t>(cx) * 2 * bpp);
        }
    return out;
}

}  // namespace slopcap::preview
