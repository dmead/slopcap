// The GPU preview (pass 1 of GlRenderer) must produce what render_cpu produces, to within float rounding.
// Needs an OpenGL 3.3 context: a display, or Xvfb + Mesa (docker/build.sh runs these under xvfb-run).
#include "preview/gl_renderer.hpp"
#include "preview/render_cpu.hpp"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdlib>
#include <memory>
#include <random>

using namespace slopcap;
using namespace slopcap::preview;

namespace {

QSurfaceFormat glFormat() {
    QSurfaceFormat f;
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    return f;
}

struct Gl {
    QOffscreenSurface surface;
    QOpenGLContext context;
    Gl() {
        surface.setFormat(glFormat());
        surface.create();
        context.setFormat(glFormat());
        if (!context.create() || !context.makeCurrent(&surface)) throw std::runtime_error("no OpenGL 3.3 context");
    }
};

std::vector<std::byte> noise(int w, int h, int bits, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<std::byte> v(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * (bits > 8 ? 2 : 1));
    for (auto& b : v) b = static_cast<std::byte>(rng() & 0xff);
    return v;
}

int maxDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    int m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

}  // namespace

TEST_CASE("GPU demosaic + stretch matches the CPU reference") {
    Gl gl;
    GlRenderer r;
    r.initialize();
    const auto bayer = GENERATE(Bayer::None, Bayer::RGGB, Bayer::GRBG, Bayer::GBRG, Bayer::BGGR);
    const int bits = GENERATE(8, 16);
    const Levels lv = GENERATE(Levels{0.0, 1.0, 1.0}, Levels{0.1, 0.6, 2.2});
    // odd sizes exercise the mirrored edges on both parities
    const int w = 37, h = 22;
    const auto raw = noise(w, h, bits, 1234);
    r.process(raw.data(), w, h, bits, bayer, lv);
    const auto gpu = r.readRgb();
    const auto cpu = renderRgb(raw, w, h, bits, bayer, lv);
    INFO("bayer " << bayerName(bayer) << ", " << bits << "-bit, gamma " << lv.gamma);
    REQUIRE(gpu.size() == cpu.size());
    CHECK(maxDiff(gpu, cpu) <= 1);
    r.destroy();
}

TEST_CASE("GPU renderer resizes between frames and re-stretches without a new upload") {
    Gl gl;
    GlRenderer r;
    r.initialize();
    auto a = noise(64, 48, 16, 1);
    r.process(a.data(), 64, 48, 16, Bayer::RGGB, {});
    CHECK(r.imageWidth() == 64);
    auto b = noise(20, 10, 8, 2);
    r.process(b.data(), 20, 10, 8, Bayer::None, {});
    CHECK(r.imageWidth() == 20);
    const Levels lv{0.2, 0.4, 1.0};
    r.reprocess(lv);
    CHECK(maxDiff(r.readRgb(), renderRgb(b, 20, 10, 8, Bayer::None, lv)) <= 1);
    r.destroy();
}

int main(int argc, char** argv) {
    QSurfaceFormat::setDefaultFormat(glFormat());
    QGuiApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
