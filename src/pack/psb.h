#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace artc {
struct PsbValue {
    enum Type { Null, Boolean, Number, String, Array, Object, Resource } type=Null;
    double number=0;
    std::string string;
    std::vector<PsbValue> array;
    std::map<std::string,PsbValue> object;
    uint32_t resource=0;
    bool extra=false;
    const PsbValue& At(const std::string& key) const;
    double Num(double fallback=0) const { return type==Number ? number : fallback; }
};
// A neutral PSB reader: no TJS, graphics context or platform SDK dependency.
// Resources stay as ranges in the owning document rather than being copied
// once per reference. Decode replaces the document only after full validation.
struct PsbDocument {
    PsbValue root;
    uint16_t version=0;
    std::vector<uint8_t> bytes;
    std::vector<std::pair<size_t,size_t>> resources, extra_resources;
    bool ReadResource(const PsbValue& ref,std::vector<uint8_t>& out) const;
};
bool DecodePsb(const std::vector<uint8_t>& bytes,PsbDocument& out,std::string& error);
// PSB RL packets encode repeated or literal complete pixels/indices, not
// independent color channels. Reject underflow, overflow and trailing packets.
bool DecodePsbRl(const std::vector<uint8_t>& bytes,size_t count,unsigned stride,
                 std::vector<uint8_t>& out);
}
