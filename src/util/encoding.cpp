#include "util/encoding.h"
#include "util/cp932_table.h"

#include <cctype>
#include <unordered_map>

namespace artc {
namespace {

void AppendUtf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Decode one UTF-8 code point starting at `i`; returns false when malformed.
bool ReadUtf8(const std::string &in, size_t &i, uint32_t &cp) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    size_t n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
    else return false;
    if (i + n > in.size()) return false;
    for (size_t k = 1; k < n; ++k) {
        const unsigned char cc = static_cast<unsigned char>(in[i + k]);
        if ((cc & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += n;
    return true;
}

bool IsLead(unsigned char b) {
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

bool LookupPair(uint16_t code, uint16_t *uni) {
    unsigned lo = 0, hi = kCp932PairCount;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (kCp932Pairs[mid].code < code) lo = mid + 1;
        else if (kCp932Pairs[mid].code > code) hi = mid;
        else { *uni = kCp932Pairs[mid].uni; return true; }
    }
    return false;
}

const std::unordered_map<uint32_t, uint16_t> &ReverseTable() {
    static const std::unordered_map<uint32_t, uint16_t> *table = [] {
        auto *m = new std::unordered_map<uint32_t, uint16_t>();
        // Prefer the lowest CP932 code for a given code point, and ASCII
        // single bytes over double-byte forms.
        for (int b = 0; b < 256; ++b)
            if (kCp932Single[b] && !m->count(kCp932Single[b])) m->emplace(kCp932Single[b], static_cast<uint16_t>(b));
        for (unsigned i = 0; i < kCp932PairCount; ++i)
            if (!m->count(kCp932Pairs[i].uni)) m->emplace(kCp932Pairs[i].uni, kCp932Pairs[i].code);
        return m;
    }();
    return *table;
}

} // namespace

bool ShiftJisToUtf8(const std::string &in, std::string &out) {
    out.clear();
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        const unsigned char b = static_cast<unsigned char>(in[i]);
        if (IsLead(b) && i + 1 < in.size()) {
            const uint16_t code = static_cast<uint16_t>(b | (static_cast<unsigned char>(in[i + 1]) << 8));
            uint16_t uni = 0;
            if (LookupPair(code, &uni)) {
                AppendUtf8(out, uni);
                i += 2;
                continue;
            }
            return false;
        }
        const uint16_t uni = kCp932Single[b];
        if (uni == 0 && b != 0) return false;
        AppendUtf8(out, uni);
        ++i;
    }
    return true;
}

bool Utf8ToShiftJis(const std::string &in, std::string &out) {
    out.clear();
    out.reserve(in.size());
    const auto &rev = ReverseTable();
    size_t i = 0;
    while (i < in.size()) {
        uint32_t cp = 0;
        if (!ReadUtf8(in, i, cp)) return false;
        if (cp == 0) { out += '\0'; continue; }
        const auto it = rev.find(cp);
        if (it == rev.end()) return false;
        const uint16_t code = it->second;
        if (code & 0xFF00) { // double byte
            out += static_cast<char>(code & 0xFF);
            out += static_cast<char>((code >> 8) & 0xFF);
        } else {
            out += static_cast<char>(code & 0xFF);
        }
    }
    return true;
}

namespace {
std::string g_text_charset;
std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

void SetTextCharset(const std::string &charset) { g_text_charset = charset; }
const std::string &TextCharset() { return g_text_charset; }

bool DecodeToUtf8(const std::string &bytes, const std::string &charset, std::string &out) {
    const std::string cs = Lower(charset);
    if (cs.empty() || cs == "utf-8" || cs == "utf8" || cs == "unicode") {
        // Already UTF-8 (a valid sequence passes through unchanged).
        out = bytes;
        return true;
    }
    // shift_jis / sjis / cp932 / ms932 and unknown charsets fall back to SJIS.
    if (ShiftJisToUtf8(bytes, out)) return true;
    out = bytes; // not decodable: keep bytes rather than dropping the script
    return false;
}

} // namespace artc
