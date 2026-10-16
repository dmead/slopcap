#include "preview/gl_renderer.hpp"

#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace slopcap::preview {

namespace {

// Full-screen quad from gl_VertexID (triangle strip of 4), no vertex buffers.
constexpr const char* kQuadVs = R"(#version 330 core
out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0, (gl_VertexID & 2) != 0 ? 1.0 : -1.0);
    uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// Pass 1. Output pixel (x, y) of the FBO is raw pixel (x, y); same arithmetic as render_cpu.cpp.
constexpr const char* kDebayerFs = R"(#version 330 core
uniform sampler2D raw;
uniform ivec2 size;
uniform int mosaic;      // 0 = mono
uniform ivec2 redAt;     // parity of the red sites; blue sits at the opposite parity
uniform vec3 levels;     // black, white, gamma
uniform vec3 wb;         // display white balance gains
out vec4 color;

int mirror(int v, int n) {
    if (n == 1) return 0;
    if (v < 0) v = -v;
    if (v >= n) v = 2 * n - 2 - v;
    return clamp(v, 0, n - 1);
}
ivec2 mirror2(ivec2 p) { return ivec2(mirror(p.x, size.x), mirror(p.y, size.y)); }
float at(ivec2 p) { return texelFetch(raw, mirror2(p), 0).r; }
int site(ivec2 p) {
    ivec2 par = p & 1;
    if (par == redAt) return 0;
    if (par == (ivec2(1) - redAt)) return 2;
    return 1;
}
float stretch(float v) {
    float x = clamp((v - levels.x) / max(1e-9, levels.y - levels.x), 0.0, 1.0);
    return levels.z != 1.0 ? pow(x, 1.0 / levels.z) : x;
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 rgb;
    if (mosaic == 0) {
        rgb = vec3(at(p));
    } else {
        vec3 sum = vec3(0.0);
        vec3 n = vec3(0.0);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                ivec2 q = p + ivec2(dx, dy);
                int c = site(mirror2(q));
                float v = at(q);
                if (c == 0) { sum.r += v; n.r += 1.0; }
                else if (c == 1) { sum.g += v; n.g += 1.0; }
                else { sum.b += v; n.b += 1.0; }
            }
        rgb = sum / max(n, vec3(1.0));
        int own = site(p);
        float c0 = at(p);
        if (own == 0) rgb.r = c0; else if (own == 1) rgb.g = c0; else rgb.b = c0;
    }
    rgb *= wb;
    color = vec4(stretch(rgb.r), stretch(rgb.g), stretch(rgb.b), 1.0);
}
)";

// Pass 2: raw row 0 is at the bottom of the texture, so flip to put it at the top of the window.
// uvMin/uvMax: the visible part of the image (texture coordinates; raw row 0 is t = 0, the top of the picture).
constexpr const char* kBlitFs = R"(#version 330 core
uniform sampler2D img;
uniform vec2 uvMin;
uniform vec2 uvMax;
in vec2 uv;
out vec4 color;
void main() { color = texture(img, vec2(mix(uvMin.x, uvMax.x, uv.x), mix(uvMax.y, uvMin.y, uv.y))); }
)";

// Parity (x & 1, y & 1) of the red sites for each pattern.
void redParity(Bayer b, int& x, int& y) {
    switch (b) {
        case Bayer::GRBG: x = 1, y = 0; return;
        case Bayer::GBRG: x = 0, y = 1; return;
        case Bayer::BGGR: x = 1, y = 1; return;
        case Bayer::RGGB:
        case Bayer::None: break;
    }
    x = 0, y = 0;
}

std::unique_ptr<QOpenGLShaderProgram> program(const char* fs) {
    auto p = std::make_unique<QOpenGLShaderProgram>();
    if (!p->addShaderFromSourceCode(QOpenGLShader::Vertex, kQuadVs) || !p->addShaderFromSourceCode(QOpenGLShader::Fragment, fs) ||
        !p->link())
        throw std::runtime_error("preview shader: " + p->log().toStdString());
    return p;
}

}  // namespace

GlRenderer::GlRenderer() = default;
GlRenderer::~GlRenderer() = default;

void GlRenderer::initialize() {
    initializeOpenGLFunctions();
    debayer_ = program(kDebayerFs);
    blit_ = program(kBlitFs);
    vao_ = std::make_unique<QOpenGLVertexArrayObject>();
    vao_->create();
}

void GlRenderer::destroy() {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (rawTex_) glDeleteTextures(1, &rawTex_);
    if (rgbTex_) glDeleteTextures(1, &rgbTex_);
    fbo_ = rawTex_ = rgbTex_ = 0;
    width_ = height_ = bits_ = 0;
    vao_.reset();
    debayer_.reset();
    blit_.reset();
}

void GlRenderer::ensureTextures(int width, int height, int bitDepth) {
    if (width == width_ && height == height_ && bitDepth == bits_ && rawTex_) return;
    if (!rawTex_) glGenTextures(1, &rawTex_);
    glBindTexture(GL_TEXTURE_2D, rawTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, bitDepth > 8 ? GL_R16 : GL_R8, width, height, 0, GL_RED,
                 bitDepth > 8 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    if (!rgbTex_) glGenTextures(1, &rgbTex_);
    glBindTexture(GL_TEXTURE_2D, rgbTex_);
    // Full mip chain so a 4K frame shrinks smoothly into a small window.
    const int levels = 1 + static_cast<int>(std::floor(std::log2(std::max(width, height))));
    for (int l = 0, w = width, h = height; l < levels; ++l, w = std::max(1, w / 2), h = std::max(1, h / 2))
        glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);   // zoomed in: show real pixels
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (!fbo_) glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rgbTex_, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("preview framebuffer incomplete");
    width_ = width;
    height_ = height;
    bits_ = bitDepth;
}

void GlRenderer::process(const std::byte* pixels, int width, int height, int bitDepth, Bayer bayer, const Levels& levels) {
    ensureTextures(width, height, bitDepth);
    bayer_ = bayer;
    glBindTexture(GL_TEXTURE_2D, rawTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, bitDepth > 8 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, pixels);
    runPass1(levels);
}

void GlRenderer::reprocess(const Levels& levels) {
    if (hasImage()) runPass1(levels);
}

void GlRenderer::runPass1(const Levels& levels) {
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
    glDisable(GL_BLEND);
    debayer_->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, rawTex_);
    debayer_->setUniformValue("raw", 0);
    glUniform2i(debayer_->uniformLocation("size"), width_, height_);
    debayer_->setUniformValue("mosaic", bayer_ == Bayer::None ? 0 : 1);
    int rx = 0, ry = 0;
    redParity(bayer_, rx, ry);
    glUniform2i(debayer_->uniformLocation("redAt"), rx, ry);
    debayer_->setUniformValue("levels", static_cast<float>(levels.black), static_cast<float>(levels.white),
                              static_cast<float>(levels.gamma));
    debayer_->setUniformValue("wb", static_cast<float>(levels.wb[0]), static_cast<float>(levels.wb[1]), static_cast<float>(levels.wb[2]));
    vao_->bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    vao_->release();
    debayer_->release();
    glBindTexture(GL_TEXTURE_2D, rgbTex_);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
}

void GlRenderer::draw(unsigned fbo, int targetWidth, int targetHeight, double x, double y, double w, double h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, targetWidth, targetHeight);
    glClearColor(0.04f, 0.04f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!hasImage() || w <= 0 || h <= 0) return;
    // Visible part of the image rectangle, in whole target pixels.
    const double vx0 = std::max(0.0, x), vy0 = std::max(0.0, y), vx1 = std::min<double>(targetWidth, x + w),
                 vy1 = std::min<double>(targetHeight, y + h);
    const int ix0 = static_cast<int>(std::floor(vx0)), iy0 = static_cast<int>(std::floor(vy0));
    const int ix1 = static_cast<int>(std::ceil(vx1)), iy1 = static_cast<int>(std::ceil(vy1));
    if (ix1 <= ix0 || iy1 <= iy0) return;
    glViewport(ix0, targetHeight - iy1, ix1 - ix0, iy1 - iy0);   // GL's origin is bottom-left
    blit_->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, rgbTex_);
    blit_->setUniformValue("img", 0);
    blit_->setUniformValue("uvMin", static_cast<float>((ix0 - x) / w), static_cast<float>((iy0 - y) / h));
    blit_->setUniformValue("uvMax", static_cast<float>((ix1 - x) / w), static_cast<float>((iy1 - y) / h));
    vao_->bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    vao_->release();
    blit_->release();
    // leave the state as found, so QPainter can draw overlays on top
    glBindTexture(GL_TEXTURE_2D, 0);
    glViewport(0, 0, targetWidth, targetHeight);
}

std::vector<uint8_t> GlRenderer::readRgb() {
    std::vector<uint8_t> rgba(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4);
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    // FBO row y is raw row y already (row 0 first in memory), so no flip.
    std::vector<uint8_t> rgb(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 3);
    for (std::size_t i = 0, n = rgb.size() / 3; i < n; ++i)
        for (int c = 0; c < 3; ++c) rgb[i * 3 + static_cast<std::size_t>(c)] = rgba[i * 4 + static_cast<std::size_t>(c)];
    return rgb;
}

}  // namespace slopcap::preview
