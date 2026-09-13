// emote_mesh_regressions.cpp — E-mote mesh patch parsing/sampling/deformation.

#include "render/emote_mesh.h"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) { ++g_failures; std::cerr << "FAIL: " << what << "\n"; }
}
bool Close(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Identity NxN grid: point (i,j) = (i/(N-1), j/(N-1)).
std::vector<float> Identity(int n) {
    std::vector<float> p;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            p.push_back(static_cast<float>(i) / (n - 1));
            p.push_back(static_cast<float>(j) / (n - 1));
        }
    return p;
}

void TestParse() {
    int side = 0;
    Check(artc::ParseMeshPatch(Identity(4), &side) && side == 4, "4x4 patch parses");
    Check(artc::ParseMeshPatch(Identity(3), &side) && side == 3, "3x3 patch parses");
    std::vector<float> bad = {0, 0, 1, 1}; // 2 points -> not a square
    Check(!artc::ParseMeshPatch(bad, &side), "non-square point count rejected");
    Check(!artc::ParseMeshPatch({0, 0, 1}, &side), "odd length rejected");
}

void TestIdentityBilinear() {
    const auto grid = Identity(3);
    int side = 0;
    artc::ParseMeshPatch(grid, &side);
    for (float u : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        float xy[2] = {0, 0};
        Check(artc::SampleMeshPatch(grid, side, u, u, xy), "sample succeeds");
        Check(Close(xy[0], u) && Close(xy[1], u), "identity grid samples to itself");
    }
}

void TestBezierCorners() {
    // 4x4 control points: identity corners, midpoint bulge upward.
    auto grid = Identity(4);
    // Move interior points to create a known center deformation.
    // Center control point (1,1) is index (1*4+1)*2.
    grid[(1 * 4 + 1) * 2 + 1] = 0.5f;
    int side = 0;
    artc::ParseMeshPatch(grid, &side);
    float corner[2] = {0, 0};
    Check(artc::SampleMeshPatch(grid, side, 0.0f, 0.0f, corner), "corner sample");
    Check(Close(corner[0], 0.0f) && Close(corner[1], 0.0f), "bezier corner preserved");
    float far[2] = {0, 0};
    Check(artc::SampleMeshPatch(grid, side, 1.0f, 1.0f, far), "far corner sample");
    Check(Close(far[0], 1.0f) && Close(far[1], 1.0f), "bezier far corner preserved");
}

void TestBuildGrid() {
    const auto grid = Identity(4);
    int side = 0;
    artc::ParseMeshPatch(grid, &side);
    std::vector<float> xy;
    std::vector<uint32_t> indices;
    Check(artc::BuildDeformedGrid(grid, side, artc::kEmoteMeshSide, &xy, &indices),
          "build deformed grid");
    Check(xy.size() == static_cast<size_t>(artc::kEmoteMeshSide) * artc::kEmoteMeshSide * 2,
          "vertex count");
    const int cells = artc::kEmoteMeshSide - 1;
    Check(indices.size() == static_cast<size_t>(cells) * cells * 6, "index count");
    // Identity patch -> first vertex at (0,0), last at (1,1).
    Check(Close(xy.front(), 0.0f) && Close(xy[1], 0.0f), "first vertex");
    Check(Close(xy[xy.size() - 2], 1.0f) && Close(xy.back(), 1.0f), "last vertex");
}

} // namespace

int main() {
    TestParse();
    TestIdentityBilinear();
    TestBezierCorners();
    TestBuildGrid();
    if (g_failures == 0) std::cout << "emote_mesh_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
