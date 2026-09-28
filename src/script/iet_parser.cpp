#include "script/iet_parser.h"

#include <cctype>

namespace artc {

// Split one bracket-group body into the tag name and key/value attributes.
//   `debug mode="1" level=2 bare` -> tag=debug, attrs={mode:1, level:2, 0:"bare"}
void ParseIetInstruction(const std::string &body, std::string &tag,
                         std::vector<std::pair<std::string, std::string>> &attrs) {
    size_t i = 0;
    size_t positional = 0;
    attrs.clear();
    auto read_token = [&](std::string &tok) {
        tok.clear();
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
        const size_t start = i;
        bool quoted = false;
        if (i < body.size() && body[i] == '"') { quoted = true; ++i; }
        while (i < body.size()) {
            char c = body[i];
            if (quoted) {
                if (c == '"') { ++i; break; }
                tok += c; ++i;
            } else {
                if (std::isspace(static_cast<unsigned char>(c)) || c == '=') break;
                tok += c; ++i;
            }
        }
        return i != start;
    };
    std::string first;
    read_token(first);
    tag = first;
    while (i < body.size()) {
        std::string key;
        if (!read_token(key)) { ++i; continue; }
        if (i < body.size() && body[i] == '=') {
            ++i;
            std::string val;
            bool quoted = false;
            if (i < body.size() && body[i] == '"') { quoted = true; ++i; }
            while (i < body.size()) {
                char c = body[i];
                if (quoted) {
                    if (c == '"') { ++i; break; }
                    val += c; ++i;
                } else {
                    if (std::isspace(static_cast<unsigned char>(c))) break;
                    val += c; ++i;
                }
            }
            attrs.emplace_back(key, val);
        } else {
            attrs.emplace_back(std::to_string(positional++), key);
        }
    }
}

} // namespace artc
