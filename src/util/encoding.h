// encoding.h — CP932 (Shift_JIS) <-> UTF-8 conversion.
//
// Used to match pack entry names: scripts (UTF-8 after charset decoding) ask
// for paths that a Shift_JIS container stores as raw CP932 bytes.
#pragma once
#include <string>

namespace artc {

// Decode CP932 bytes to UTF-8. Returns false on an invalid sequence.
bool ShiftJisToUtf8(const std::string &in, std::string &out);

// Encode a UTF-8 string to CP932. Unmappable code points make it fail.
bool Utf8ToShiftJis(const std::string &in, std::string &out);

// Decode script bytes to UTF-8 per a system.ini CHARSET value. Accepts
// utf-8/utf8 (returned unchanged when already valid) and shift_jis/sjis/
// cp932/ms932; anything else falls back to Shift_JIS (the engine default).
bool DecodeToUtf8(const std::string &bytes, const std::string &charset,
                  std::string &out);

// Project-wide text charset installed at boot (empty = UTF-8/auto).
void SetTextCharset(const std::string &charset);
const std::string &TextCharset();

} // namespace artc
