#pragma once
#include "audio/pcm_stream.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace artc {

// Platform-independent Vorbis source. Keeps compressed bytes, decodes only the
// requested PCM block, and joins Artemis's _a intro to its _b loop sample.
// Open on the engine thread; ReadStereo on one audio callback thread at a time.
class VorbisStream : public PcmStream {
public:
    using Reader = std::function<bool(const std::string&, std::vector<uint8_t>&)>;
    VorbisStream();
    ~VorbisStream() override;
    VorbisStream(const VorbisStream&) = delete;
    VorbisStream& operator=(const VorbisStream&) = delete;
    bool Open(const Reader& read, const std::string& file, bool loop);
    size_t ReadStereo(int16_t* output, size_t frames) override;
    int SampleRate() const override;
    uint64_t FrameCount() const override; // total non-looping playback length (intro + body)
    bool Ended() const override;
    bool HasLoopSegment() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Same constant-power balance used by the original native mixer.
void ApplyStereoPan(int16_t* samples, size_t frames, int pan1000);

} // namespace artc
