// emote_preview.cpp — dev tool (ARTC_TEST_CGL): render one E-mote model to a
// PNG on a macOS offscreen CGL context, using the same player/container
// transforms the game passes for a real 立绘 layer. Used to inspect layer
// order / masking locally without a device round-trip.
#include "render/emote_model.h"
#include "render/emote_player.h"
#include "render/emote_scene.h"
#include "render/compositor.h"
#include "render/gles2_headers.h"
#include "pack/psb.h"
#include "util/snapshot_image.h"
#include <OpenGL/OpenGL.h>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
bool MakeCurrent() {
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)32,
        kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8, (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pix = nullptr; GLint npix = 0;
    if (CGLChoosePixelFormat(attrs, &pix, &npix) || !pix) return false;
    CGLContextObj ctx = nullptr;
    if (CGLCreateContext(pix, nullptr, &ctx) || !ctx) return false;
    return CGLSetCurrentContext(ctx) == kCGLNoError;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: emote_preview <model.psb> <out.png> [frame]\n";
        return 2;
    }
    const double frame = argc > 3 ? std::atof(argv[3]) : 0.0;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    artc::PsbDocument document;
    std::string error;
    if (!artc::DecodePsb(bytes, document, error)) {
        std::cerr << "decode: " << error << '\n';
        return 1;
    }
    auto model = std::make_shared<artc::EmoteModel>();
    if (!model->Load(std::move(document), error)) {
        std::cerr << "model: " << error << '\n';
        return 1;
    }
    if (!MakeCurrent()) {
        std::cerr << "no GL context\n";
        return 1;
    }
    const int W = 1920, H = 1080;
    // Drawable-less CGL has no default framebuffer; draw into an offscreen FBO.
    GLuint screen_tex = 0, screen_fbo = 0;
    glGenTextures(1, &screen_tex);
    glBindTexture(GL_TEXTURE_2D, screen_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &screen_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, screen_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, screen_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "incomplete screen FBO\n";
        return 1;
    }
    artc::Compositor compositor;
    compositor.Init(W, H);
    artc::EmotePlayer player;
    if (!player.Load(model, error)) {
        std::cerr << "player: " << error << '\n';
        return 1;
    }
    // Device container transform for 甜蜜女友3 fg 立绘: box 1920x2048, setCoord
    // and setScale from ex.lyc, game side top=-484/anchor(960,540)/scale 86.
    player.SetLayerSize(1920, 2048);
    player.SetCoordAngle(-64, 309, 0, 0);
    player.SetScaleOrigin(0.5, 0, 0);
    if (frame > 0) player.Progress(frame * 1000.0 / 60.0);
    compositor.SetProps("g", {{"left", "0"}, {"top", "-484"}, {"anchorx", "960"},
                              {"anchory", "540"}, {"xscale", "86"}, {"yscale", "86"}});
    if (!player.Render(compositor, "g.0", error)) {
        std::cerr << "render: " << error << '\n';
        return 1;
    }
    // InitGl may have rebound FBO 0; target the offscreen surface again.
    glBindFramebuffer(GL_FRAMEBUFFER, screen_fbo);
    glViewport(0, 0, W, H);
    compositor.Draw();
    artc::SnapshotImage image;
    if (!compositor.Snapshot(image) || image.rgba.size() != size_t(W) * H * 4) {
        std::cerr << "snapshot unavailable\n";
        return 1;
    }
    std::vector<uint8_t> png;
    if (!image.EncodePng(W, H, png)) {
        std::cerr << "encode failed\n";
        return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), png.size());
    std::cout << "wrote " << argv[2] << " frame=" << frame << '\n';
    return 0;
}
