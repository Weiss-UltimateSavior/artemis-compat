#pragma once
// This fixture owns plain objects; it never opens a device or native window.
struct ANativeWindow { int width; int height; };
int ANativeWindow_getWidth(ANativeWindow *);
int ANativeWindow_getHeight(ANativeWindow *);
