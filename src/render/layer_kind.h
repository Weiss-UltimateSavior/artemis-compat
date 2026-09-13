// layer_kind.h — layer classification as a single pure function.
//
// "Kind = content role", not lifetime. Diagnostics, hit-testing and the draw
// loop branch on this rather than re-deriving role from scattered fields.
#pragma once
#include "render/compositor.h"

#include <cstdint>

namespace artc {

enum class LayerKind : uint8_t {
    None,   // no texture, glyphs or mesh (placeholder / awaiting rebind)
    Group,  // container: no drawable content of its own
    Image,  // textured quad (static or animated image)
    Text,   // rasterized message text (glyphs and/or text present)
    Mesh,   // warped triangle list (E-mote per-icon mesh)
};

inline LayerKind KindOf(const Layer &l) {
    if (!l.mesh.empty()) return LayerKind::Mesh;
    if (!l.glyphs.empty() || !l.text.empty()) return LayerKind::Text;
    if (l.texture != 0) return LayerKind::Image;
    return l.visible ? LayerKind::Group : LayerKind::None;
}

inline const char *KindName(LayerKind k) {
    switch (k) {
    case LayerKind::None: return "none";
    case LayerKind::Group: return "group";
    case LayerKind::Image: return "image";
    case LayerKind::Text: return "text";
    case LayerKind::Mesh: return "mesh";
    }
    return "?";
}

} // namespace artc
