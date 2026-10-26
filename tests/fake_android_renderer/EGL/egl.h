// Minimal synthetic EGL declarations, injected only into the renderer test.
#pragma once
#include <cstdint>
using EGLBoolean = unsigned int;
using EGLint = int;
using EGLDisplay = void *;
using EGLSurface = void *;
using EGLContext = void *;
using EGLConfig = void *;
using EGLNativeWindowType = void *;
constexpr EGLBoolean EGL_FALSE = 0, EGL_TRUE = 1;
constexpr EGLint EGL_SUCCESS = 0x3000, EGL_BAD_NATIVE_WINDOW = 0x300B;
constexpr EGLint EGL_CONTEXT_LOST = 0x300E, EGL_BAD_MATCH = 0x3009;
constexpr EGLint EGL_NONE = 0x3038, EGL_SURFACE_TYPE = 0x3033;
constexpr EGLint EGL_RENDERABLE_TYPE = 0x3040, EGL_RED_SIZE = 0x3024;
constexpr EGLint EGL_GREEN_SIZE = 0x3023, EGL_BLUE_SIZE = 0x3022;
constexpr EGLint EGL_CONTEXT_CLIENT_VERSION = 0x3098;
constexpr EGLint EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056;
constexpr EGLint EGL_WINDOW_BIT = 4, EGL_PBUFFER_BIT = 1;
constexpr EGLint EGL_OPENGL_ES2_BIT = 4;
constexpr EGLDisplay EGL_NO_DISPLAY = nullptr;
constexpr EGLSurface EGL_NO_SURFACE = nullptr;
constexpr EGLContext EGL_NO_CONTEXT = nullptr;
constexpr void *EGL_DEFAULT_DISPLAY = nullptr;
EGLDisplay eglGetDisplay(void *);
EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint *);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface);
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext);
EGLBoolean eglTerminate(EGLDisplay);
EGLBoolean eglSwapInterval(EGLDisplay, EGLint);
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface, EGLint, EGLint *);
EGLContext eglGetCurrentContext();
EGLint eglGetError();
