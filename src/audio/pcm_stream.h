#pragma once
#include <cstddef>
#include <cstdint>

namespace artc {

// A decoder owns its compressed source. Backends pull interleaved stereo
// signed 16-bit PCM from one callback thread, independent of its container.
class PcmStream {
public:
    virtual ~PcmStream() = default;
    virtual size_t ReadStereo(int16_t* output, size_t frames) = 0;
    virtual int SampleRate() const = 0;
    virtual uint64_t FrameCount() const = 0;
    virtual bool Ended() const = 0;
};

} // namespace artc
