#include "config/ini.h"

#include <cstdio>

namespace artc {

namespace {
std::string Trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}
} // namespace

bool Ini::Parse(const std::string &bytes) {
    sections_.clear();
    order_.clear();
    current_.clear();

    size_t pos = 0;
    while (pos <= bytes.size()) {
        size_t eol = bytes.find('\n', pos);
        if (eol == std::string::npos) eol = bytes.size();
        std::string line = Trim(bytes.substr(pos, eol - pos));
        pos = eol + 1;

        if (line.empty() || line[0] == ';') continue; // comment / blank
        if (!line.empty() && line[0] == '#') continue; // marker comment runs
        if (line.front() == '[') {
            const size_t close = line.find(']');
            if (close != std::string::npos) {
                current_ = Trim(line.substr(1, close - 1));
                if (!sections_.count(current_)) order_.push_back(current_);
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || current_.empty()) continue;
        const std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (!key.empty()) sections_[current_][key] = val;
    }
    return true;
}

bool Ini::ParseFile(const std::string &path) {
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    std::string bytes;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) bytes.append(buf, n);
    std::fclose(fp);
    return Parse(bytes);
}

bool Ini::HasSection(const std::string &section) const {
    return sections_.count(section) != 0;
}

std::string Ini::Get(const std::string &section, const std::string &key,
                     const std::string &def) const {
    auto sit = sections_.find(section);
    if (sit == sections_.end()) return def;
    auto kit = sit->second.find(key);
    return kit == sit->second.end() ? def : kit->second;
}

int Ini::GetInt(const std::string &section, const std::string &key, int def) const {
    const std::string v = Get(section, key, "");
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

} // namespace artc
