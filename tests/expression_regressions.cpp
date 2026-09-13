// expression_regressions.cpp — Artemis '$' expression evaluator.

#include "script/expression.h"

#include <functional>
#include <iostream>
#include <map>
#include <string>

namespace {
int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) { ++g_failures; std::cerr << "FAIL: " << what << "\n"; }
}
std::function<std::string(const std::string &)> Vars(const std::map<std::string, std::string> &m) {
    return [m](const std::string &n) {
        const auto it = m.find(n);
        return it == m.end() ? std::string("0") : it->second;
    };
}
std::string Eval(const std::string &e, const std::map<std::string, std::string> &v = {}) {
    std::string out;
    Check(artc::EvaluateExpression(e, Vars(v), out), "evaluates: " + e);
    return out;
}
} // namespace

int main() {
    Check(Eval("1+2") == "3", "addition");
    Check(Eval("10-4*2") == "2", "precedence");
    Check(Eval("0xFF") == "255", "hex literal");
    Check(Eval("(1+2)*3") == "9", "parentheses");
    Check(Eval("1==1") == "1" && Eval("1!=1") == "0", "equality");
    Check(Eval("1<2") == "1" && Eval("2<=1") == "0", "comparison");
    Check(Eval("1 && 0") == "0" && Eval("0 || 1") == "1", "logical");
    Check(Eval("'a'+'b'") == "ab", "string concat");
    Check(Eval("a+1", {{"a", "41"}}) == "42", "variable reference");
    Check(Eval("a==b", {{"a", "7"}, {"b", "7"}}) == "1", "variable equality");
    if (g_failures == 0) std::cout << "expression_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
