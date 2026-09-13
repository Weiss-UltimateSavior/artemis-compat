// shader_compat.h — adapt GLSL ES 1.00 shader sources to the desktop GL 2.1
// context the macOS host creates.
//
// The engine's own shaders and the game-authored mobile GLSL are written for
// GLES2: they carry `precision` statements and `lowp/mediump/highp`
// qualifiers, and may declare `#version 100`. Desktop GLSL 1.10/1.20 rejects
// all of those. On every other backend the source is passed through verbatim.
#pragma once
#include <string>

#if defined(__APPLE__)
#include <regex>

namespace artc {

inline std::string ShaderSourceForBackend(const std::string &source) {
    static const std::regex precision_stmt(
        R"(precision\s+\w+\s+\w+\s*;)");
    static const std::regex precision_qual(
        R"(\b(?:lowp|mediump|highp)\b\s*)");

    std::string out;
    out.reserve(source.size() + 16);
    size_t pos = 0;
    while (pos <= source.size()) {
        size_t eol = source.find('\n', pos);
        std::string line = source.substr(
            pos, eol == std::string::npos ? std::string::npos : eol - pos);
        // #version 100 (ES) -> #version 120 (desktop), keeping indentation.
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 12, "#version 100") == 0)
            line.replace(first, 12, "#version 120");
        line = std::regex_replace(line, precision_stmt, "");
        line = std::regex_replace(line, precision_qual, "");
        out += line;
        if (eol == std::string::npos) break;
        out += '\n';
        pos = eol + 1;
    }
    return out;
}

} // namespace artc

#else

namespace artc {
inline std::string ShaderSourceForBackend(const std::string &source) {
    return source;
}
} // namespace artc

#endif
