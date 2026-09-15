#pragma once
#include <cstdint>
#include <vector>
namespace artc {
struct SnapshotImage {
    int width=0,height=0;
    std::vector<uint8_t> rgba; // top-down, straight RGBA
    bool EncodePng(int output_width,int output_height,std::vector<uint8_t>& output) const;
};
}
