// text.h — shared text-domain types and string utilities.
//
// TextRuby is consumed by both the script message pipeline (lua_engine) and
// the compositor's SetText; it lives here so neither module includes the
// other for it. The free functions are the UTF-8 / encoding helpers used by
// the tag handlers (var substr / explode / base64 / urlencode tags).
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace artc {

// Ruby annotation: a UTF-8 byte range [start, start+length) in the base text
// carries the reading in `text`.
struct TextRuby {
    size_t start = 0, length = 0; // UTF-8 byte range in the base text
    std::string text;
};

// Number of UTF-8 code points in `s`.
int Utf8Length(const std::string &s);
// Code-point substring [position, position+length) as UTF-8 bytes.
std::string Utf8Substr(const std::string &s, size_t position, size_t length);

std::string Base64Encode(const std::string &in);
std::string UrlEncode(const std::string &in);
std::string UrlDecode(const std::string &in);

// Split `source` on `delimiter`, honouring a single-character escape.
std::vector<std::string> SplitEscaped(const std::string &source, const std::string &delimiter,
                                      const std::string &escape);

} // namespace artc
