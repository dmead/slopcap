// GPU preview: upload a raw frame as a one-channel texture (R8 / R16), demosaic + stretch it in a fragment shader into
// an RGBA8 image the size of the frame (pass 1), then draw that image aspect-fitted, mipmapped, into the window
// (pass 2). Pass 1 matches render_cpu's renderRgb to within rounding; tests/test_gl_render.cpp checks that.
// Needs a current OpenGL 3.3 (core or compatibility) context for every call.
#pragma once

#include "camera/camera.hpp"
#include "preview/render_cpu.hpp"

#include <QOpenGLExtraFunctions>

#include <cstdint>
#include <memory>
#include <vector>

class QOpenGLShaderProgram;
class QOpenGLVertexArrayObject;

namespace slopcap::preview {

class GlRenderer : protected QOpenGLExtraFunctions {
public:
    GlRenderer();
    ~GlRenderer();   ///< call destroy() first, with the context current
    GlRenderer(const GlRenderer&) = delete;
    GlRenderer& operator=(const GlRenderer&) = delete;

    void initialize();   ///< compile shaders; throws std::runtime_error on failure
    void destroy();      ///< release GL objects

    /// Upload one raw frame (8-bit, or 16-bit little-endian) and run pass 1 with the given levels.
    void process(const std::byte* pixels, int width, int height, int bitDepth, Bayer bayer, const Levels& levels);
    /// Re-run pass 1 on the last frame with new levels (slider moved while the stream is paused).
    void reprocess(const Levels& levels);

    /// Pass 2: draw the processed image into framebuffer `fbo` (targetWidth x targetHeight pixels) so that it covers
    /// the rectangle (x, y, w, h) in target pixels, top-left origin; the rectangle may extend past the edges (zoom).
    void draw(unsigned fbo, int targetWidth, int targetHeight, double x, double y, double w, double h);

    bool hasImage() const { return width_ > 0; }
    int imageWidth() const { return width_; }
    int imageHeight() const { return height_; }
    /// Read pass 1's output back as RGB8, rows top to bottom (for tests).
    std::vector<uint8_t> readRgb();

private:
    void ensureTextures(int width, int height, int bitDepth);
    void runPass1(const Levels& levels);

    std::unique_ptr<QOpenGLShaderProgram> debayer_, blit_;
    std::unique_ptr<QOpenGLVertexArrayObject> vao_;
    unsigned rawTex_ = 0, rgbTex_ = 0, fbo_ = 0;
    int width_ = 0, height_ = 0, bits_ = 0;
    Bayer bayer_ = Bayer::None;
};

}  // namespace slopcap::preview
