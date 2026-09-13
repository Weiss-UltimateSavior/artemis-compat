// hlsl_glsl.h — translate the Artemis runtime-effect HLSL subset to GLSL ES.
//
// Artemis PC titles ship their layer effects as a small HLSL fragment subset
// (samplerFore/Mask/User/Back, tex2D, a ps() entry returning float4). The
// compat engine's GLES2 shader backend consumes GLSL, so this adapter handles
// the documented subset only. Unsupported constructs stay as-is and surface as
// a compile error rather than silently producing a wrong image.
#pragma once
#include <string>

namespace artc {

// Heuristic: register-qualified samplers, a ps() entry point, or tex2D.
bool LooksLikeHlsl(const std::string &source);

// Translate the supported HLSL subset to a GLSL ES fragment shader with a
// `void main()` entry that writes gl_FragColor.
std::string TranslateHlslToGlsl(const std::string &source);

} // namespace artc
