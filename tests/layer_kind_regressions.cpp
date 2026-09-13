// layer_kind_regressions.cpp — pure layer classification.

#include "render/layer_kind.h"

#include <iostream>
#include <string>

namespace {

int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) { ++g_failures; std::cerr << "FAIL: " << what << "\n"; }
}

void TestClassification() {
    artc::Layer empty;
    empty.visible = false;
    Check(artc::KindOf(empty) == artc::LayerKind::None, "invisible empty -> none");

    artc::Layer group;
    group.visible = true;
    Check(artc::KindOf(group) == artc::LayerKind::Group, "visible empty -> group");

    artc::Layer image;
    image.texture = 7;
    Check(artc::KindOf(image) == artc::LayerKind::Image, "texture -> image");

    artc::Layer text;
    text.texture = 7;
    text.text = "hi";
    Check(artc::KindOf(text) == artc::LayerKind::Text, "text wins over texture -> text");

    artc::Layer mesh;
    mesh.texture = 7;
    mesh.mesh = {0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1};
    Check(artc::KindOf(mesh) == artc::LayerKind::Mesh, "mesh wins -> mesh");

    Check(std::string(artc::KindName(artc::LayerKind::Mesh)) == "mesh", "kind name");
}

} // namespace

int main() {
    TestClassification();
    if (g_failures == 0) std::cout << "layer_kind_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
