#include "util/text.h"
#include <cctype>
#include <cstdint>

namespace artc {

int Utf8Length(const std::string &s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        i += c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
        ++n;
    }
    return n;
}

std::string Utf8Substr(const std::string &s, size_t position, size_t length) {
    size_t i = 0, start = s.size();
    for (size_t cp = 0; cp < position && i < s.size(); ++cp) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        i += c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
    }
    start = i;
    for (size_t cp = 0; cp < length && i < s.size(); ++cp) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        i += c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
    }
    return start <= i ? s.substr(start, i - start) : std::string();
}

std::string Base64Encode(const std::string &in) {
    static const char *tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t b0 = static_cast<unsigned char>(in[i]);
        const uint32_t b1 = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
        const uint32_t b2 = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += i + 1 < in.size() ? tbl[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? tbl[v & 63] : '=';
    }
    return out;
}

std::string UrlEncode(const std::string &in) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char b : in) {
        if (std::isalnum(b) || b == '-' || b == '_' || b == '.' || b == '~') {
            out += static_cast<char>(b);
        } else {
            out += '%';
            out += hex[b >> 4];
            out += hex[b & 15];
        }
    }
    return out;
}

std::string UrlDecode(const std::string &in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = nib(in[i + 1]), lo = nib(in[i + 2]);
            if (hi >= 0 && lo >= 0) { out += static_cast<char>(hi * 16 + lo); i += 2; continue; }
        }
        out += in[i] == '+' ? ' ' : in[i];
    }
    return out;
}

std::vector<std::string> SplitEscaped(const std::string &source, const std::string &delimiter,
                                      const std::string &escape) {
    std::vector<std::string> parts;
    const char delim = delimiter.empty() ? ',' : delimiter[0];
    const char esc = escape.empty() ? '\\' : escape[0];
    std::string cur;
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] == esc && i + 1 < source.size()) { cur += source[++i]; }
        else if (source[i] == delim) { parts.push_back(cur); cur.clear(); }
        else cur += source[i];
    }
    parts.push_back(cur);
    return parts;
}

} // namespace artc
