#pragma once
#include <array>
#include <cstdint>

namespace artc {

// Artemis exposes query flags, not a single synthetic "pressed" value.
// In the Android reference CArtemisInput, IsPush/IsDown/IsDownEdge/
// IsUpEdge/IsDecide read bits 1/2/3/4/5 respectively. OverrideKey adds
// bit 0 as the override marker; status=-1 restores the physical state.
class InputState {
public:
    enum QueryFlag { Push=2, Down=4, DownEdge=8, UpEdge=16, Decide=32 };
    static constexpr int Count=320;
    void Press(int key) {
        if (key<0 || key>=256) return;
        auto& k=keys_[key];
        k.push=true;
        if (!k.down) k.down_edge=true;
        k.down=true;
    }
    void Release(int key) {
        if (key<0 || key>=256) return;
        auto& k=keys_[key];
        if (k.down) k.up_edge=true;
        k.down=false;
    }
    void Override(int key, int status) {
        const uint8_t bits=status==-1 ? 0 : (status & 0x3e) | 1;
        if (key==-1) for (auto& k:keys_) k.override_bits=bits;
        else if (key>=0 && key<Count) keys_[key].override_bits=bits;
    }
    bool Query(int key, QueryFlag flag) const {
        if (key<0 || key>=Count) return false;
        const auto& k=keys_[key];
        bool value=false;
        if (k.override_bits) value=(k.override_bits & flag)!=0;
        else switch(flag) {
            case Push: value=k.push; break;
            case Down: value=k.down; break;
            case DownEdge: value=k.down_edge; break;
            case UpEdge: value=k.up_edge; break;
            // Pointer activation occurs on release; keyboard activation on
            // press. A script may explicitly override Decide independently.
            case Decide: value=key==1 ? k.up_edge : k.down_edge; break;
        }
        // The reference aliases the first touch key (138) to mouse/tap (1).
        return value || (key==1 && Query(138,flag));
    }
    bool Overridden(int key) const {
        return key>=0 && key<Count && keys_[key].override_bits!=0;
    }
    void EndFrame() {
        for (auto& k:keys_) {
            k.push=k.down_edge=k.up_edge=false;
            k.override_bits=0;
        }
    }
private:
    struct Key {
        bool down=false, push=false, down_edge=false, up_edge=false;
        uint8_t override_bits=0;
    };
    std::array<Key,Count> keys_{};
};
} // namespace artc
