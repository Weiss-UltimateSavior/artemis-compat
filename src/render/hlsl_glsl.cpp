#include "render/hlsl_glsl.h"

#include <regex>

namespace artc {
namespace {

// Replace every `name(...)` call, optionally renaming it and appending extra
// arguments before the closing parenthesis. Parentheses are balanced.
std::string ReplaceCall(const std::string &src, const std::string &name,
                        const std::string &replacement, const std::string &extraArgs) {
    std::string out;
    size_t pos = 0;
    while (pos < src.size()) {
        const size_t at = src.find(name, pos);
        if (at == std::string::npos) { out += src.substr(pos); break; }
        // Require a non-identifier character before the name.
        if (at > 0) {
            const char prev = src[at - 1];
            if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_') {
                out += src.substr(pos, at - pos + 1);
                pos = at + 1;
                continue;
            }
        }
        size_t i = at + name.size();
        while (i < src.size() && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
        if (i >= src.size() || src[i] != '(') {
            out += src.substr(pos, at - pos + 1);
            pos = at + 1;
            continue;
        }
        out += src.substr(pos, at - pos);
        out += replacement;
        // Copy the balanced argument list.
        size_t depth = 0;
        size_t j = i;
        for (; j < src.size(); ++j) {
            if (src[j] == '(') ++depth;
            else if (src[j] == ')') { --depth; if (depth == 0) break; }
        }
        if (j >= src.size()) { out += src.substr(at + name.size()); return out; }
        out += src.substr(i, j - i); // "(...args" without the closing paren
        if (!extraArgs.empty()) out += extraArgs;
        out += ')';
        pos = j + 1;
    }
    return out;
}

std::string ReplaceWord(const std::string &src, const std::string &word,
                        const std::string &replacement) {
    return std::regex_replace(src, std::regex("\\b" + word + "\\b"), replacement);
}

} // namespace

bool LooksLikeHlsl(const std::string &source) {
    return source.find("register(") != std::string::npos ||
           source.find(" register ") != std::string::npos ||
           source.find("tex2D") != std::string::npos ||
           std::regex_search(source, std::regex(R"((?:float4|half4)\s+ps\s*\()"));
}

std::string TranslateHlslToGlsl(const std::string &source) {
    std::string s = source;

    // 1) `sampler2D name : register(s0);` -> `uniform sampler2D name;`
    s = std::regex_replace(
        s, std::regex(R"((\w+)\s+(\w+)\s*:\s*register\s*\(\s*\w+\s*\)\s*;)"),
        "uniform $1 $2;");
    // Any leftover register annotations.
    s = std::regex_replace(s, std::regex(R"(\s*:\s*register\s*\(\s*\w+\s*\))"), "");

    // 2) ps() entry -> main(), binding the uv parameter to the varying.
    std::string uvName = "uv";
    std::smatch m;
    const std::regex ps_re(
        R"((?:float4|half4|vec4)\s+ps\s*\(\s*\w*\s+(\w+)[^)]*\)[^{]*\{)");
    if (std::regex_search(s, m, ps_re)) {
        uvName = m[1].str();
        s = std::regex_replace(s, ps_re, "void main() {\n    vec2 " + uvName + " = resultCoord1;\n");
    } else {
        // Parameterless or unusual ps() shape: just rename the entry point.
        s = std::regex_replace(s, std::regex(R"(\bps\s*\()"), "main(");
    }

    // 3) texture sampling and sampler aliases.
    s = ReplaceCall(s, "tex2D", "texture2D", "");
    s = ReplaceWord(s, "samplerFore", "textureFore");
    s = ReplaceWord(s, "samplerBack", "textureBack");

    // 4) type keywords (longest first so float4x4 wins over float4).
    s = ReplaceWord(s, "float4x4", "mat4");
    s = ReplaceWord(s, "float3x3", "mat3");
    s = ReplaceWord(s, "float2x2", "mat2");
    s = ReplaceWord(s, "float4", "vec4");
    s = ReplaceWord(s, "float3", "vec3");
    s = ReplaceWord(s, "float2", "vec2");
    s = ReplaceWord(s, "half4", "vec4");
    s = ReplaceWord(s, "half3", "vec3");
    s = ReplaceWord(s, "half2", "vec2");
    s = ReplaceWord(s, "half", "float");
    s = ReplaceWord(s, "fixed4", "vec4");
    s = ReplaceWord(s, "fixed3", "vec3");
    s = ReplaceWord(s, "fixed2", "vec2");
    s = ReplaceWord(s, "fixed", "float");

    // 5) intrinsics.
    s = ReplaceCall(s, "saturate", "clamp", ", 0.0, 1.0");
    s = ReplaceCall(s, "lerp", "mix", "");
    s = ReplaceCall(s, "frac", "fract", "");
    s = ReplaceCall(s, "fmod", "mod", "");
    s = ReplaceCall(s, "atan2", "atan", "");

    // 6) `return expr;` -> `gl_FragColor = expr;` (ps had a float4 return).
    s = std::regex_replace(s, std::regex(R"(\breturn\b)"), "gl_FragColor =");

    return s;
}

} // namespace artc
