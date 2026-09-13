// text_rules_regressions.cpp — CJK line-break predicate classes.

#include "render/line_break.h"

#include <iostream>
#include <string>

namespace {
int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) { ++g_failures; std::cerr << "FAIL: " << what << "\n"; }
}
} // namespace

int main() {
    // Cannot start a line.
    Check(artc::ProhibitLineStart(U'」'), "closing bracket prohibits line start");
    Check(artc::ProhibitLineStart(U'。'), "full stop prohibits line start");
    Check(!artc::ProhibitLineStart(U'あ'), "kana allows line start");
    // Cannot end a line.
    Check(artc::ProhibitLineEnd(U'「'), "opening bracket prohibits line end");
    Check(!artc::ProhibitLineEnd(U'あ'), "kana allows line end");
    // Hanging punctuation.
    Check(artc::HangPunctuation(U'、') && artc::HangPunctuation(U'。'), "hanging punctuation");
    Check(!artc::HangPunctuation(U'！'), "fullwidth bang is not hanging");
    if (g_failures == 0) std::cout << "text_rules_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
