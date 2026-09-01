#include "script/iet_interpreter.h"
#include "log/logger.h"

#include <algorithm>
#include <cctype>

namespace artc {

namespace {

std::string TrimCopy(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Split one bracket-group body into the tag name and key/value attributes.
//   `debug mode="1" level=2 bare`  ->  tag=debug, attrs={mode:1, level:2, bare:""}
void ParseAttrs(const std::string &body, std::string &tag,
                std::vector<std::pair<std::string, std::string>> &attrs) {
    size_t i = 0;
    auto read_token = [&](std::string &tok) {
        tok.clear();
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
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
    };
    std::string first;
    read_token(first);
    tag = first;
    while (i < body.size()) {
        std::string key;
        read_token(key);
        if (key.empty()) { ++i; continue; }
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
            attrs.emplace_back(key, ""); // bare flag
        }
    }
}

} // namespace

std::vector<std::string> IetRunner::SplitLines(const std::string &s) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t eol = s.find('\n', pos);
        if (eol == std::string::npos) eol = s.size();
        std::string line = s.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (eol == s.size()) break;
        pos = eol + 1;
    }
    return lines;
}

IetRunner::IetRunner(PackManager *packs, LuaEngine *lua)
    : packs_(packs), lua_(lua) {}

bool IetRunner::Run(const std::string &path) {
    current_path_ = path;
    stopped_ = false;
    labels_.clear();

    std::vector<uint8_t> bytes;
    if (!packs_->Read(path, bytes)) {
        Log(kLogError, "iet: not found in packs: " + path);
        return false;
    }
    const std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());

    const auto lines = SplitLines(text);
    bool in_lua = false;
    std::string lua_code;
    std::string chunk = path + ":lua";

    for (const std::string &raw : lines) {
        if (stopped_) break;
        const std::string line = TrimCopy(raw);
        if (in_lua) {
            if (line == "[/lua]") {
                ExecLuaChunk(lua_code, chunk);
                in_lua = false;
                lua_code.clear();
            } else {
                lua_code += raw;   // preserve indentation inside the chunk
                lua_code += "\n";
            }
            continue;
        }
        if (line.empty()) continue;
        if (line.rfind("//", 0) == 0) continue;              // comment
        if (line[0] == '*') {                                // label
            Log(kLogInfo, "iet label: " + line.substr(1));
            continue;
        }
        if (line == "[lua]") {                       // multi-line chunk start
            in_lua = true;
            lua_code.clear();
            continue;
        }
        if (line.front() == '[') {
            ExecBrackets(line);
            continue;
        }
        Log(kLogInfo, "iet text: " + line);                  // M2 routes to message layer
    }
    if (in_lua) Log(kLogError, "iet: unterminated [lua] block in " + path);
    return true;
}

void IetRunner::ExecBrackets(const std::string &line) {
    // a line may hold several [ ... ] groups; scan them in order
    size_t i = 0;
    while (i < line.size() && !stopped_) {
        if (line[i] != '[') { ++i; continue; }
        size_t depth = 0, j = i;
        bool in_quote = false;
        for (; j < line.size(); ++j) {
            char c = line[j];
            if (in_quote) { if (c == '"') in_quote = false; continue; }
            if (c == '"') { in_quote = true; continue; }
            if (c == '[') ++depth;
            if (c == ']') { --depth; if (depth == 0) break; }
        }
        if (j >= line.size()) break;
        ExecBracket(line.substr(i + 1, j - i - 1));
        i = j + 1;
    }
}

void IetRunner::ExecBracket(const std::string &inner) {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    ParseAttrs(inner, tag, attrs);

    if (tag == "stop" || tag == "return") { stopped_ = true; return; }
    if (tag == "wt" || tag == "wait") return;              // linear exec: no-op
    if (tag == "lua") return;                              // inline chunk (rare)
    if (tag == "calllua") {
        for (const auto &kv : attrs) {
            if (kv.first == "function") {
                Log(kLogInfo, "iet calllua: " + kv.second);
                if (!lua_->CallGlobal(kv.second))
                    Log(kLogError, "iet calllua failed: " + kv.second);
                return;
            }
        }
        Log(kLogError, "iet calllua without function attr");
        return;
    }
    // everything else is an engine tag
    if (!lua_->DispatchTag(tag, attrs))
        Log(kLogError, "iet tag dispatch failed: " + tag);
}

void IetRunner::ExecLuaChunk(const std::string &code, const std::string &chunk) {
    lua_->DoString(code, chunk);
}

} // namespace artc
