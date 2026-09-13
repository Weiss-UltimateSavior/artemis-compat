#pragma once
#include <cstdint>
#include <string_view>

namespace artc {
// Basic CJK kinsoku classes (JLReq 3.1.7–3.1.10). Explicit newlines take
// precedence; these restrictions apply only to automatic line breaks.
inline bool ProhibitLineStart(uint32_t cp) {
    constexpr std::u32string_view characters=U")]},.!?:;、。，．・：；？！ー〜～…‥々〻ヽヾゝゞ’”）］｝〕〉》」』】〙〗〟»ぁぃぅぇぉっゃゅょゎゕゖァィゥェォッャュョヮヵヶｧｨｩｪｫｯｬｭｮｰ";
    return characters.find(cp)!=characters.npos || (cp>=0x300 && cp<=0x36f) ||
        cp==0x3099 || cp==0x309a || (cp>=0xfe00 && cp<=0xfe0f);
}
inline bool ProhibitLineEnd(uint32_t cp) {
    constexpr std::u32string_view characters=U"([{‘“（［｛〔〈《「『【〘〖〝«";
    return characters.find(cp)!=characters.npos;
}
inline bool HangPunctuation(uint32_t cp) {
    return cp==U'、' || cp==U'。' || cp==U'，' || cp==U'．';
}
} // namespace artc
