#include "render/renderer.h"
#include "log/logger.h"

#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__) && defined(ARTC_HAS_GLES)
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>
#endif

namespace artc {

#if defined(__ANDROID__) && defined(ARTC_HAS_GLES)

void Renderer::SetEglError(const char *operation) {
    const EGLint error = eglGetError();
    if (error == EGL_CONTEXT_LOST) context_lost_ = true;
    last_error_ = std::string(operation) + " failed: " + std::to_string(error);
    Log(kLogError, last_error_);
}

void Renderer::ApplySwapInterval() {
    // The interval belongs to the current draw surface. Reapply it when a
    // window is attached, including when the existing context is retained.
    const char *swap_env = std::getenv("ARTC_SWAP_INTERVAL");
    if (!(swap_env && std::strcmp(swap_env, "0") == 0)) {
        swap_paced_ = eglSwapInterval(display_, 1) == EGL_TRUE;
        Log(kLogInfo, std::string("renderer: eglSwapInterval(1) ") +
                          (swap_paced_ ? "applied" : "rejected; host-paced"));
    } else {
        swap_paced_ = false;
        Log(kLogInfo, "renderer: swap interval disabled (ARTC_SWAP_INTERVAL=0)");
    }
}

// Create the display, configuration and a context with a parking surface.
// Once a session is live, surface replacement must not rebuild this context.
bool Renderer::CreateContext() {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) { SetEglError("eglGetDisplay"); return false; }
    if (!eglInitialize(display_, nullptr, nullptr)) {
        SetEglError("eglInitialize");
        return false;
    }
    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE};
    EGLint num = 0;
    if (!eglChooseConfig(display_, cfg_attrs, &cfg_, 1, &num) || num < 1) {
        SetEglError("eglChooseConfig");
        return false;
    }
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context_ = eglCreateContext(display_, cfg_, EGL_NO_CONTEXT, ctx_attrs);
    if (context_ == EGL_NO_CONTEXT) { SetEglError("eglCreateContext"); return false; }
    const EGLint parking_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    parking_surface_ = eglCreatePbufferSurface(display_, cfg_, parking_attrs);
    if (parking_surface_ == EGL_NO_SURFACE) {
        SetEglError("eglCreatePbufferSurface");
        return false;
    }
    return true;
}

bool Renderer::CreateSurface(ANativeWindow *window) {
    surface_ = eglCreateWindowSurface(display_, cfg_, window, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        SetEglError("eglCreateWindowSurface");
        return false;
    }
    return true;
}

bool Renderer::Init(void *native_window) {
    context_reused_ = false;
    if (!native_window) { last_error_ = "native window is null"; return false; }
    if (context_lost_) {
        last_error_ = "cannot initialize a window after EGL context loss";
        return false;
    }
    const bool reused = HasRetainedContext();
    if (!reused) {
        Shutdown();
        if (!CreateContext()) { Shutdown(); return false; }
    }
    if (!AttachWindow(native_window)) {
        // Initial setup has no scene to retain. A live context, however, must
        // survive ordinary window-surface failures so attachment can be retried.
        if (!reused) Shutdown();
        return false;
    }
    context_reused_ = reused;
    return true;
}

bool Renderer::HasRetainedContext() const {
    return !context_lost_ && display_ != EGL_NO_DISPLAY &&
           context_ != EGL_NO_CONTEXT && parking_surface_ != EGL_NO_SURFACE;
}

bool Renderer::DetachSurface() {
    ready_ = false;
    swap_paced_ = false;
    if (!HasRetainedContext()) return false;
    // Move the context before destroying the window surface. No compositor,
    // Lua or script-runner state needs to be released for this transition.
    if (!eglMakeCurrent(display_, parking_surface_, parking_surface_, context_)) {
        SetEglError("eglMakeCurrent(parking)");
        return false;
    }
    if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }
    window_ = nullptr;
    return true;
}

bool Renderer::AttachWindow(void *native_window) {
    if (!native_window || !HasRetainedContext()) {
        last_error_ = "cannot attach window without a retained EGL context";
        return false;
    }
    if (!DetachSurface()) return false;
    auto *window = static_cast<ANativeWindow *>(native_window);
    if (!CreateSurface(window)) return false; // The parking binding survives.
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        SetEglError("eglMakeCurrent(window)");
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
        // On ordinary surface failure EGL leaves the previous binding intact.
        // Do not invoke GL or claim retention after EGL_CONTEXT_LOST.
        return false;
    }
    window_ = window;
    width_ = ANativeWindow_getWidth(window);
    height_ = ANativeWindow_getHeight(window);
    EGLint actual_w = 0, actual_h = 0;
    if (!eglQuerySurface(display_, surface_, EGL_WIDTH, &actual_w) ||
        !eglQuerySurface(display_, surface_, EGL_HEIGHT, &actual_h)) {
        SetEglError("eglQuerySurface");
        if (!context_lost_) DetachSurface();
        return false;
    }
    if (actual_w < 1 || actual_h < 1) {
        last_error_ = "window surface has no drawable area";
        DetachSurface();
        return false;
    }
    width_ = actual_w;
    height_ = actual_h;
    ready_ = true;
    ApplyViewport();
    ApplySwapInterval();
    last_error_.clear();
    Log(kLogInfo, "renderer: GLES2 ready " +
                      std::to_string(width_) + "x" + std::to_string(height_));
    return true;
}

void Renderer::SetStage(int stage_w, int stage_h, bool sidecut) {
    stage_w_ = stage_w;
    stage_h_ = stage_h;
    stage_set_ = stage_w > 0 && stage_h > 0;
    sidecut_ = sidecut;
    if (!ready_) return;
    ApplyViewport();
}

// Recompute the stage→surface viewport for the current width_/height_ and
// apply it. Requires a current GL context.
void Renderer::ApplyViewport() {
    if (stage_set_) {
        const float sw = (float)width_ / stage_w_, sh = (float)height_ / stage_h_;
        const float scale = sidecut_ ? (sw > sh ? sw : sh)   // crop to cover
                                     : (sw < sh ? sw : sh);  // letterbox to fit
        vp_w_ = stage_w_ * scale;
        vp_h_ = stage_h_ * scale;
        vp_x_ = (width_ - vp_w_) * 0.5f;
        vp_y_ = (height_ - vp_h_) * 0.5f;
    } else {
        vp_x_ = 0;
        vp_y_ = 0;
        vp_w_ = (float)width_;
        vp_h_ = (float)height_;
    }
    glViewport((int)vp_x_, (int)vp_y_, (int)vp_w_, (int)vp_h_);
    Log(kLogInfo, "viewport: " + std::to_string((int)vp_w_) + "x" +
                      std::to_string((int)vp_h_) + " at " +
                      std::to_string((int)vp_x_) + "," + std::to_string((int)vp_y_) +
                      " (surface " + std::to_string(width_) + "x" +
                      std::to_string(height_) + ")");
}

float Renderer::StageX(float win_x) const {
    return (win_x - vp_x_) / vp_w_ * stage_w_;
}
float Renderer::StageY(float win_y) const {
    return (win_y - vp_y_) / vp_h_ * stage_h_;
}

bool Renderer::RefreshSize() {
    if (!ready_) return false;
    EGLint width = 0, height = 0;
    if (!eglQuerySurface(display_, surface_, EGL_WIDTH, &width) ||
        !eglQuerySurface(display_, surface_, EGL_HEIGHT, &height)) {
        SetEglError("eglQuerySurface(resize)");
        ready_ = false;
        return false;
    }
    if (width < 1 || height < 1) {
        last_error_ = "window surface has no drawable area";
        ready_ = false;
        return false;
    }
    if (width == width_ && height == height_) return false;
    Resize(width, height);
    return true;
}

void Renderer::Resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width == width_ && height == height_) return;
    Log(kLogInfo, "renderer: window resized " + std::to_string(width_) + "x" +
                      std::to_string(height_) + " -> " + std::to_string(width) + "x" +
                      std::to_string(height));
    width_ = width;
    height_ = height;
    if (ready_) ApplyViewport();
}

void Renderer::Clear() {
    if (!ready_) return;
    // stage background; composited layers draw on top of this
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

bool Renderer::Present() {
    if (!ready_) return false;
    if (eglSwapBuffers(display_, surface_)) return true;
    const EGLint error = eglGetError();
    if (error == EGL_CONTEXT_LOST) context_lost_ = true;
    ready_ = false; // Reattach the surface; do not replace a healthy context.
    Log(kLogError, "eglSwapBuffers failed: " + std::to_string(error));
    return false;
}

void Renderer::DrawFrame() {
    Clear();
    Present();
}

bool Renderer::HasCurrentContext() const {
    return !context_lost_ && context_ != EGL_NO_CONTEXT && eglGetCurrentContext() == context_;
}

void Renderer::Shutdown() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        if (parking_surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, parking_surface_);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        eglTerminate(display_);
    }
    surface_ = EGL_NO_SURFACE;
    parking_surface_ = EGL_NO_SURFACE;
    context_ = EGL_NO_CONTEXT;
    display_ = EGL_NO_DISPLAY;
    cfg_ = nullptr;
    window_ = nullptr;
    ready_ = false;
    context_lost_ = false;
    swap_paced_ = false;
    context_reused_ = false;
}

#else // host build: no GL

bool Renderer::Init(void *) { last_error_ = "renderer unavailable on host"; return false; }
bool Renderer::DetachSurface() { return false; }
bool Renderer::AttachWindow(void *) { return false; }
bool Renderer::HasRetainedContext() const { return false; }
void Renderer::Resize(int, int) {}
void Renderer::SetStage(int, int, bool) {}
void Renderer::ApplyViewport() {}
float Renderer::StageX(float x) const { return x; }
float Renderer::StageY(float y) const { return y; }
void Renderer::Clear() {}
bool Renderer::Present() { return false; }
bool Renderer::RefreshSize() { return false; }
bool Renderer::HasCurrentContext() const { return false; }
void Renderer::DrawFrame() {}
void Renderer::Shutdown() {}

#endif

// Preserve the upstream surface/context queries with the retained-context policy.
bool Renderer::ReleaseSurface() { return DetachSurface(); }
bool Renderer::ContextAlive() const { return HasRetainedContext(); }

} // namespace artc
