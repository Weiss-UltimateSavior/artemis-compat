// gles2_headers.h — the GLES2 entry points the compositor/layer shaders use.
//
// On OpenHarmony/Android this is just <GLES2/gl2.h>. On macOS there is no
// GLES2: the host creates a legacy (OpenGL 2.1) context, so the core symbols
// used here live behind the EXT suffix for framebuffer objects. Everything
// else the renderer touches (shaders, client vertex arrays, VAO-free attrib
// state) is present in OpenGL 2.1. Include this header instead of <GLES2/gl2.h>.
#pragma once

#if defined(__APPLE__)
// Prototypes for the extension entry points (OpenGL.framework exports them).
#define GL_GLEXT_PROTOTYPES 1
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER GL_FRAMEBUFFER_EXT
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING GL_FRAMEBUFFER_BINDING_EXT
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 GL_COLOR_ATTACHMENT0_EXT
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE GL_FRAMEBUFFER_COMPLETE_EXT
#endif
#ifndef glGenFramebuffers
#define glGenFramebuffers glGenFramebuffersEXT
#endif
#ifndef glBindFramebuffer
#define glBindFramebuffer glBindFramebufferEXT
#endif
#ifndef glDeleteFramebuffers
#define glDeleteFramebuffers glDeleteFramebuffersEXT
#endif
#ifndef glFramebufferTexture2D
#define glFramebufferTexture2D glFramebufferTexture2DEXT
#endif
#ifndef glCheckFramebufferStatus
#define glCheckFramebufferStatus glCheckFramebufferStatusEXT
#endif
#else
#include <GLES2/gl2.h>
#endif
