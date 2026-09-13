#pragma once
#include "audio/pcm_stream.h"
#include <memory>
#include <vector>

namespace artc {

// Independent demux cursors share immutable pack bytes. The audio callback
// never blocks behind video decoding or GL work on the engine thread.
class VideoDecoder {
public:
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
    struct Frame {
        int width = 0, height = 0;
        double pts_ms = 0, duration_ms = 0;
        std::vector<uint8_t> rgba;
    };
    VideoDecoder();
    ~VideoDecoder();
    bool Open(Bytes bytes);
    bool Next(Frame& frame);
    bool Rewind();
    double DurationMs() const;
    std::unique_ptr<PcmStream> OpenAudio(bool loop) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace artc
