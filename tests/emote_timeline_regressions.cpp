// emote_timeline_regressions.cpp — E-mote timeline sampling semantics.

#include "emote_scene_fixture.h"
#include "render/emote_model.h"

#include <cmath>
#include <iostream>
#include <map>
#include <string>

namespace {
int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) { ++g_failures; std::cerr << "FAIL: " << what << "\n"; }
}
const double *Find(const std::map<std::string, double> &v, const std::string &k) {
    const auto it = v.find(k);
    return it == v.end() ? nullptr : &it->second;
}
} // namespace

int main() {
    auto doc = emote_fixture::PlayerDocument();
    artc::EmoteModel model;
    std::string error;
    Check(model.Load(doc, error), "load player document: " + error);
    Check(model.Timelines().count("delta") && model.Timelines().count("loop") &&
              model.Timelines().count("once"),
          "three timelines parsed");

    std::map<std::string, double> a, b, c;
    Check(model.Sample("loop", 5, a) && model.Sample("loop", 25, b), "sample loop");
    const double *va = Find(a, "expression");
    const double *vb = Find(b, "expression");
    Check(va && vb && std::fabs(*va - *vb) < 1e-6, "loop wraps (t=25 == t=5)");

    std::map<std::string, double> held, held_end;
    Check(model.Sample("once", 60, held) && model.Sample("once", 120, held_end), "sample once");
    const double *vh = Find(held, "expression");
    const double *ve = Find(held_end, "expression");
    Check(vh && ve && std::fabs(*vh - *ve) < 1e-6, "finite timeline holds its last frame");

    if (g_failures == 0) std::cout << "emote_timeline_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
