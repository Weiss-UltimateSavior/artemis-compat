// emote_mesh.h — E-mote mesh patch sampling.
//
// A [mesh] payload carries a flat blend-point array (`bp`) that forms a square
// control grid of normalized, already-deformed positions. The vertex grid is
// sampled from it (bilinear for NxN, cubic Bezier for the 4x4 authored patch)
// before the layer transform is applied. This is the platform-neutral math the
// deformation renderer consumes; it has no GL dependency.
#pragma once
#include <cstdint>
#include <vector>

namespace artc {

// Number of subdivisions used when expanding a patch into draw vertices.
constexpr int kEmoteMeshSide = 8;

// A flat `[x0,y0,x1,y1,…]` array is a patch when its point count is a perfect
// square with side >= 2. Fills *side and returns true on success.
bool ParseMeshPatch(const std::vector<float> &points, int *side);

// Sample the patch at normalized (u,v) in [0,1] (clamped) -> warped normalized
// position. `points` must be a valid patch of the given `side`.
bool SampleMeshPatch(const std::vector<float> &points, int side, float u, float v,
                     float *out_xy);

// Build a `side`x`side` normalized triangulated grid by sampling the patch.
// out_xy is 2 floats per vertex; out_indices is 6 per cell (two triangles).
bool BuildDeformedGrid(const std::vector<float> &points, int side, int side_out,
                       std::vector<float> *out_xy, std::vector<uint32_t> *out_indices);

// Expand a patch into a triangle list with interleaved (warped_x, warped_y,
// u, v): positions are the warped normalized grid, uv the identity grid the
// texture is sampled through. Used directly by the compositor mesh path.
bool BuildWarpedMesh(const std::vector<float> &points, int side, int side_out,
                     std::vector<float> *out_xyuv);

} // namespace artc
