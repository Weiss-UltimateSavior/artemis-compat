#include "render/compositor.h"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void Check(bool ok, const char* why) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << why << '\n';
    }
}
bool Near(float a, float b) { return std::abs(a - b) < 0.0001f; }
} // namespace

int main() {
    artc::Compositor c;
    c.Init(1920, 1080);

    c.SetProps("quoted", {
        {"left", "\"12.5\""}, {"anchorx", "\"960\""},
        {"anchory", "\"540\""}, {"alpha", "\"128\""},
        {"xscale", "\"150\""}, {"yscale", "\"75\""}, {"z", "\"7\""},
    });
    const auto& quoted = c.Layers().front();
    Check(Near(quoted.x, 12.5f), "quoted left is parsed");
    Check(Near(quoted.ax, 960) && Near(quoted.ay, 540), "quoted anchors are parsed");
    Check(Near(quoted.alpha, 128.0f / 255.0f), "quoted alpha is parsed");
    Check(Near(quoted.sx, 1.5f) && Near(quoted.sy, 0.75f), "quoted scales are parsed");
    Check(quoted.z == 0, "quoted z is consumed without changing the authoritative layer z");

    c.SetProps("malformed", {
        {"alpha", "not-a-number"}, {"xscale", "not-a-number"},
        {"yscale", "not-a-number"}, {"z", "not-a-number"},
    });
    const auto& malformed = c.Layers().back();
    Check(Near(malformed.alpha, 1.0f), "malformed alpha keeps the layer visible");
    Check(Near(malformed.sx, 1.0f) && Near(malformed.sy, 1.0f),
          "malformed scales retain neutral geometry");
    Check(malformed.z == 0, "malformed z has a stable fallback");

    if (!failures) std::cout << "numeric property regressions passed\n";
    return failures ? 1 : 0;
}
