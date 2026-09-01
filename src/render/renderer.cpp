#include "render/renderer.h"
#include "log/logger.h"

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>
#endif

namespace artc {

#if defined(__ANDROID__)

bool Renderer::Init(void *native_window) {
    auto *window = static_cast<ANativeWindow *>(native_window);
    Shutdown(); // clean slate if re-initializing

    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) { last_error_ = "eglGetDisplay failed"; return false; }
    if (!eglInitialize(display_, nullptr, nullptr)) { last_error_ = "eglInitialize failed"; return false; }

    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint num = 0;
    if (!eglChooseConfig(display_, cfg_attrs, &cfg, 1, &num) || num < 1) {
        last_error_ = "eglChooseConfig failed";
        return false;
    }
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context_ = eglCreateContext(display_, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (context_ == EGL_NO_CONTEXT) { last_error_ = "eglCreateContext failed"; return false; }
    surface_ = eglCreateWindowSurface(display_, cfg, window, nullptr);
    if (surface_ == EGL_NO_SURFACE) { last_error_ = "eglCreateWindowSurface failed"; return false; }
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        last_error_ = "eglMakeCurrent failed";
        return false;
    }

    window_ = window;
    width_ = ANativeWindow_getWidth(window);
    height_ = ANativeWindow_getHeight(window);
    glViewport(0, 0, width_, height_);
    ready_ = true;
    Log(kLogInfo, "renderer: GLES2 ready " + std::to_string(width_) + "x" +
                      std::to_string(height_));
    return true;
}

void Renderer::SetStage(int stage_w, int stage_h, bool sidecut) {
    stage_w_ = stage_w;
    stage_h_ = stage_h;
    if (!ready_ || stage_w <= 0 || stage_h <= 0) return;
    const float sw = (float)width_ / stage_w, sh = (float)height_ / stage_h;
    const float scale = sidecut ? (sw > sh ? sw : sh)   // crop to cover
                                : (sw < sh ? sw : sh);  // letterbox to fit
    vp_w_ = stage_w * scale;
    vp_h_ = stage_h * scale;
    vp_x_ = (width_ - vp_w_) * 0.5f;
    vp_y_ = (height_ - vp_h_) * 0.5f;
    glViewport((int)vp_x_, (int)vp_y_, (int)vp_w_, (int)vp_h_);
    Log(kLogInfo, "viewport: " + std::to_string((int)vp_w_) + "x" +
                      std::to_string((int)vp_h_) + " at " +
                      std::to_string((int)vp_x_) + "," + std::to_string((int)vp_y_));
}

float Renderer::StageX(float win_x) const {
    return (win_x - vp_x_) / vp_w_ * stage_w_;
}
float Renderer::StageY(float win_y) const {
    return (win_y - vp_y_) / vp_h_ * stage_h_;
}

void Renderer::Resize(int width, int height) {
    width_ = width;
    height_ = height;
    if (ready_) glViewport(0, 0, width, height);
}

void Renderer::Clear() {
    if (!ready_) return;
    // stage background; composited layers draw on top of this
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Renderer::Present() {
    if (!ready_) return;
    if (!eglSwapBuffers(display_, surface_))
        Log(kLogError, "eglSwapBuffers failed");
}

void Renderer::DrawFrame() {
    Clear();
    Present();
}

void Renderer::Shutdown() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        eglTerminate(display_);
    }
    surface_ = EGL_NO_SURFACE;
    context_ = EGL_NO_CONTEXT;
    display_ = EGL_NO_DISPLAY;
    ready_ = false;
}

#else // host build: no GL

bool Renderer::Init(void *) { last_error_ = "renderer unavailable on host"; return false; }
void Renderer::Resize(int, int) {}
void Renderer::SetStage(int, int, bool) {}
float Renderer::StageX(float x) const { return x; }
float Renderer::StageY(float y) const { return y; }
void Renderer::Clear() {}
void Renderer::Present() {}
void Renderer::DrawFrame() {}
void Renderer::Shutdown() {}

#endif

} // namespace artc
