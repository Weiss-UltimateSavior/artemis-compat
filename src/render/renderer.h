// renderer.h — EGL/GLES2 render surface for the compat engine.
//
// M2.0 scope: context lifecycle (retained across native windows, resize, shutdown)
// and frame presentation (clear + swap). Layer compositing and texture
// drawing land in M2.1 on top of this.
//
// Host builds get a no-op implementation (no EGL) so the CLI keeps working.
#pragma once

#include <string>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>
#endif

namespace artc {

class Renderer {
public:
    // Create EGL/GLES context bound to the native window.
    bool Init(void *native_window);
    // Existing surface/context entry points share the parking policy below.
    // A failed release does not destroy a context still bound to its window.
    bool ReleaseSurface();
    bool ContextAlive() const;
    // True only when the most recent Init successfully reused the context.
    bool ContextReused() const { return context_reused_; }
    // A window surface can disappear without losing the EGL context or scene.
    // Parking on a tiny pbuffer keeps GL objects valid until a new window exists.
    bool DetachSurface();
    bool AttachWindow(void *native_window);
    bool HasRetainedContext() const;
    bool ContextLost() const { return context_lost_; }
    bool RefreshSize();
    bool HasCurrentContext() const;
    // Reapply the stage viewport when the same surface changes geometry.
    void Resize(int width, int height);
    // Clear the backbuffer (stage background).
    void Clear();
    // Swap the backbuffer to the display.
    bool Present();
    // True when eglSwapBuffers paces the frame loop to the display refresh
    // (eglSwapInterval(1)); false when the host must pace itself (fallback
    // build/device: ARTC_SWAP_INTERVAL=0).
    bool SwapPaced() const { return swap_paced_; }
    // Present one frame (clear + swap).
    void DrawFrame();
    // Release the context/surface.
    void Shutdown();

    bool Ready() const { return ready_; }
    // Stage→surface mapping: letterbox/crop viewport + touch conversion.
    void SetStage(int stage_w, int stage_h, bool sidecut);
    float StageX(float win_x) const;   // window px → stage coords
    float StageY(float win_y) const;
    int Width() const { return width_; }
    int Height() const { return height_; }
    const std::string &LastError() const { return last_error_; }

private:
#if defined(__ANDROID__)
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLSurface parking_surface_ = EGL_NO_SURFACE;
    EGLConfig cfg_ = nullptr;   // retained for replacement window surfaces
    EGLContext context_ = EGL_NO_CONTEXT;
    void *window_ = nullptr;
    bool CreateContext();                       // display + config + parking context
    bool CreateSurface(ANativeWindow *window);  // window EGLSurface on that context
    void SetEglError(const char *operation);
    void ApplySwapInterval();
#endif
    bool ready_ = false;
    bool context_lost_ = false;
    bool swap_paced_ = false;   // eglSwapInterval(1) applied successfully
    bool context_reused_ = false; // last Init rebound an existing context
    int width_ = 0, height_ = 0;
    float vp_x_ = 0, vp_y_ = 0, vp_w_ = 1, vp_h_ = 1;   // viewport in surface px
    int stage_w_ = 1280, stage_h_ = 720;
    bool stage_set_ = false;    // stage mapping survives surface replacement
    bool sidecut_ = false;      // crop-to-cover instead of letterbox-to-fit
    void ApplyViewport();       // current surface dimensions + stage mapping
    std::string last_error_;
};

} // namespace artc
