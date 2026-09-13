// block_decode.h — BC-family texture decoding for E-mote atlases.
//
// DXT5 (BC3) is decoded here; BC7/BPTC uses the vendored bc7decomp. Both
// return RGBA8, tightly packed, row-major, trimmed to width*height.
#pragma once
#include <cstdint>
#include <vector>

namespace artc {

// `src` must hold ceil(w/4)*ceil(h/4)*16 bytes.
void DecodeDxt5Blocks(const std::vector<uint8_t> &src, int width, int height,
                      std::vector<uint8_t> &out);
void DecodeBc7Blocks(const std::vector<uint8_t> &src, int width, int height,
                     std::vector<uint8_t> &out);

} // namespace artc
