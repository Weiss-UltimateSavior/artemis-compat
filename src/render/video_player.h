#pragma once
#include "render/video_decoder.h"
#include <string>

namespace artc {
class Audio;
class Compositor;
class VideoPlayer {
public:
    VideoPlayer(Compositor& compositor, Audio& audio);
    ~VideoPlayer();
    bool Start(VideoDecoder::Bytes bytes, const std::string& layer, bool loop,
               bool fullscreen, int volume, double now_ms);
    void Update(double now_ms);
    void Stop();
    bool Active() const { return active_; }
    bool Fullscreen() const { return fullscreen_; }
private:
    void Present(const VideoDecoder::Frame& frame);
    Compositor& compositor_;
    Audio& audio_;
    VideoDecoder decoder_;
    VideoDecoder::Frame next_;
    std::string layer_, audio_key_;
    double started_ms_=0, cycle_ms_=0, end_ms_=0, elapsed_ms_=0;
    bool active_=false, loop_=false, fullscreen_=false, next_valid_=false;
};
} // namespace artc
