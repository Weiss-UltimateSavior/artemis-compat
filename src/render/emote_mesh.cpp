#include "render/emote_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace artc {
namespace {

std::pair<float, float> Lerp(std::pair<float, float> a, std::pair<float, float> b, float t) {
    return {a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t};
}

std::pair<float, float> BezierPatch(const std::vector<float> &p, float u, float v) {
    auto basis = [](float value) {
        const float x = std::clamp(value, 0.0f, 1.0f);
        const float inv = 1.0f - x;
        return std::array<float, 4>{inv * inv * inv, 3 * inv * inv * x, 3 * inv * x * x,
                                    x * x * x};
    };
    const auto bx = basis(u), by = basis(v);
    float rx = 0, ry = 0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const float w = bx[x] * by[y];
            const size_t index = static_cast<size_t>((y * 4 + x) * 2);
            rx += p[index] * w;
            ry += p[index + 1] * w;
        }
    }
    return {rx, ry};
}

} // namespace

bool ParseMeshPatch(const std::vector<float> &points, int *side) {
    if (points.empty() || (points.size() % 2) != 0) return false;
    const size_t count = points.size() / 2;
    const int s = static_cast<int>(std::sqrt(static_cast<double>(count)));
    if (s < 2 || static_cast<size_t>(s) * static_cast<size_t>(s) != count) return false;
    if (side) *side = s;
    return true;
}

bool SampleMeshPatch(const std::vector<float> &points, int side, float u, float v,
                     float *out_xy) {
    int parsed = 0;
    if (!ParseMeshPatch(points, &parsed) || side != parsed || side < 2) return false;
    if (side == 4) {
        const auto r = BezierPatch(points, u, v);
        out_xy[0] = r.first;
        out_xy[1] = r.second;
        return true;
    }
    auto axis = [side](float value) {
        const float scaled = std::clamp(value, 0.0f, 1.0f) * static_cast<float>(side - 1);
        const int cell = std::min(static_cast<int>(std::floor(scaled)), side - 2);
        return std::pair<int, float>{cell, scaled - static_cast<float>(cell)};
    };
    auto point = [&](int x, int y) {
        const size_t index = static_cast<size_t>((y * side + x) * 2);
        return std::pair<float, float>{points[index], points[index + 1]};
    };
    const auto [x, tx] = axis(u);
    const auto [y, ty] = axis(v);
    const auto top = Lerp(point(x, y), point(x + 1, y), tx);
    const auto bottom = Lerp(point(x, y + 1), point(x + 1, y + 1), tx);
    const auto r = Lerp(top, bottom, ty);
    out_xy[0] = r.first;
    out_xy[1] = r.second;
    return true;
}

bool BuildDeformedGrid(const std::vector<float> &points, int side, int side_out,
                       std::vector<float> *out_xy, std::vector<uint32_t> *out_indices) {
    if (!out_xy || !out_indices || side_out < 2) return false;
    out_xy->clear();
    out_indices->clear();
    out_xy->reserve(static_cast<size_t>(side_out) * side_out * 2);
    for (int j = 0; j < side_out; ++j) {
        for (int i = 0; i < side_out; ++i) {
            const float u = static_cast<float>(i) / (side_out - 1);
            const float v = static_cast<float>(j) / (side_out - 1);
            float xy[2] = {u, v};
            if (!SampleMeshPatch(points, side, u, v, xy)) return false;
            out_xy->push_back(xy[0]);
            out_xy->push_back(xy[1]);
        }
    }
    out_indices->reserve(static_cast<size_t>(side_out - 1) * (side_out - 1) * 6);
    for (int j = 0; j < side_out - 1; ++j) {
        for (int i = 0; i < side_out - 1; ++i) {
            const uint32_t a = static_cast<uint32_t>(j * side_out + i);
            const uint32_t b = a + 1;
            const uint32_t c = a + static_cast<uint32_t>(side_out);
            const uint32_t d = c + 1;
            for (uint32_t v : {a, b, c, b, d, c}) out_indices->push_back(v);
        }
    }
    return true;
}

bool BuildWarpedMesh(const std::vector<float> &points, int side, int side_out,
                     std::vector<float> *out_xyuv) {
    if (!out_xyuv) return false;
    std::vector<float> xy;
    std::vector<uint32_t> indices;
    if (!BuildDeformedGrid(points, side, side_out, &xy, &indices)) return false;
    out_xyuv->clear();
    out_xyuv->reserve(indices.size() * 4);
    for (uint32_t v : indices) {
        const int i = static_cast<int>(v % side_out);
        const int j = static_cast<int>(v / side_out);
        out_xyuv->push_back(xy[static_cast<size_t>(v) * 2]);
        out_xyuv->push_back(xy[static_cast<size_t>(v) * 2 + 1]);
        out_xyuv->push_back(static_cast<float>(i) / (side_out - 1));
        out_xyuv->push_back(static_cast<float>(j) / (side_out - 1));
    }
    return true;
}

} // namespace artc
