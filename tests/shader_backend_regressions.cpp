#include "render/shader_compat.h"
#include <iostream>

int main() {
    const std::string source =
        "#version 100\nprecision mediump float;\n"
        "varying highp vec2 uv;\nvoid main(){ gl_FragColor=vec4(1.0); }\n";
    const auto converted = artc::ShaderSourceForBackend(source);
#if defined(ARTC_USE_DESKTOP_GL)
    if (converted.find("#version 120") == std::string::npos ||
        converted.find("precision") != std::string::npos ||
        converted.find("highp") != std::string::npos) return 1;
#else
    // Apple builds using ANGLE must retain GLSL ES verbatim too.
    if (converted != source) return 2;
#endif
    std::cout << "shader backend regression passed\n";
}
