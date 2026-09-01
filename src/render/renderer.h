// renderer.h — EGL/GLES2 render surface for the compat engine.
//
// M2.0 scope: context lifecycle (init per native window, resize, shutdown)
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
    // Called when the window geometry changes.
    void Resize(int width, int height);
    // Clear the backbuffer (stage background).
    void Clear();
    // Swap the backbuffer to the display.
    void Present();
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
    EGLContext context_ = EGL_NO_CONTEXT;
    void *window_ = nullptr;
#endif
    bool ready_ = false;
    int width_ = 0, height_ = 0;
    float vp_x_ = 0, vp_y_ = 0, vp_w_ = 1, vp_h_ = 1;   // viewport in surface px
    int stage_w_ = 1280, stage_h_ = 720;
    std::string last_error_;
};

} // namespace artc
