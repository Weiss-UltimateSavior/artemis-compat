// preprocess_regressions.cpp — &linetag / &autoinsert / &scpsupport and the
// tag.ini positional-parameter table. Pure text transforms; no assets.

#include "script/preprocess.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) { ++g_failures; std::cerr << "FAIL: " << what << "\n"; }
}
std::vector<std::string> Lines(const std::string &s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t eol = s.find('\n', pos);
        out.push_back(s.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return out;
}

void TestLinetagNumericFallback() {
    artc::ClearTagIni();
    const std::string src = "[&linetag allow=1]\nfoo bar,hoge,fuga\n「text」\n";
    const auto out = Lines(artc::PreprocessScript(src));
    Check(out.size() == Lines(src).size(), "line count preserved");
    Check(out[1] == "[foo 0=\"bar\" 1=\"hoge\" 2=\"fuga\"]", "numeric fallback params");
    Check(out[2] == "「text」", "non-alnum-leading line stays text");
}

void TestLinetagNamedParams() {
    artc::InstallTagIniFromText("foo=a,b,c\nevent=x,y\n");
    const std::string src = "[&linetag allow=1]\nfoo bar,hoge,fuga\nevent 1,2\n";
    const auto out = Lines(artc::PreprocessScript(src));
    Check(out[1] == "[foo a=\"bar\" b=\"hoge\" c=\"fuga\"]", "named params from tag.ini");
    Check(out[2] == "[event x=\"1\" y=\"2\"]", "second tag table");
    artc::ClearTagIni();
}

void TestLinetagPrefix() {
    artc::ClearTagIni();
    // With a prefix, only prefixed lines convert; the ASCII-head rule is off.
    const std::string src = "[&linetag allow=1 prefix=\"@\"]\n@foo bar\nfoo bar\n@[rt]\n";
    const auto out = Lines(artc::PreprocessScript(src));
    Check(out[1] == "[foo 0=\"bar\"]", "prefixed line converts");
    Check(out[2] == "foo bar", "unprefixed line stays text");
    Check(out[3] == "@[rt]", "prefixed bracket syntax rejected");
}

void TestLinetagDisabled() {
    artc::ClearTagIni();
    const std::string src = "foo bar,hoge\n";
    const auto out = Lines(artc::PreprocessScript(src));
    Check(out[0] == "foo bar,hoge", "no allow -> no conversion");
}

void TestAutoInsertBlankline() {
    artc::ClearTagIni();
    const std::string src = "[&autoinsert target=\"blankline\" command=\"[rp]\"]\n\nnext\n";
    const auto out = Lines(artc::PreprocessScript(src));
    Check(out[0].empty(), "directive line stripped");
    Check(out[1] == "[rp]", "blank line expanded");
    Check(out[2] == "next", "following text intact");
}

void TestScpsupport() {
    artc::ClearTagIni();
    const std::string src =
        "[&scpsupport mode=\"init\"]\n"
        "[&scpsupport mode=\"add\" command=\"rt\"]\n"
        "hello\n"
        "[tag]\n";
    const auto out = Lines(artc::PreprocessScript(src));
    Check(out[2] == "hello[rt]", "newline rule appended to text");
    Check(out[3] == "[tag]", "no append after a tag line");
}

void TestLuaBlockPassthrough() {
    artc::ClearTagIni();
    const std::string src =
        "[&linetag allow=1]\n"
        "[lua]\n"
        "foo bar,hoge\n"
        "[/lua]\n"
        "foo real\n";
    const auto out = Lines(artc::PreprocessScript(src));
    Check(out[2] == "foo bar,hoge", "lua block untouched");
    Check(out[4] == "[foo 0=\"real\"]", "conversion resumes after lua block");
}

} // namespace

int main() {
    TestLinetagNumericFallback();
    TestLinetagNamedParams();
    TestLinetagPrefix();
    TestLinetagDisabled();
    TestAutoInsertBlankline();
    TestScpsupport();
    TestLuaBlockPassthrough();
    if (g_failures == 0) std::cout << "preprocess_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
