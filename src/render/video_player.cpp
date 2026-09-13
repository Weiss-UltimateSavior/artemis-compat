#include "render/video_player.h"
#include "render/compositor.h"
#include "audio/audio.h"
#include <algorithm>

namespace artc {
VideoPlayer::VideoPlayer(Compositor& compositor, Audio& audio) : compositor_(compositor),audio_(audio) {}
VideoPlayer::~VideoPlayer() { Stop(); }

bool VideoPlayer::Start(VideoDecoder::Bytes bytes, const std::string& layer, bool loop,
                        bool fullscreen, int volume, double now_ms) {
    Stop();
    if(!decoder_.Open(std::move(bytes)) || !decoder_.Next(next_)) return false;
    layer_=layer; audio_key_="movie:"+layer;
    loop_=loop; fullscreen_=fullscreen; started_ms_=now_ms; cycle_ms_=elapsed_ms_=0;
    next_valid_=active_=true;
    end_ms_=std::max(decoder_.DurationMs(),next_.pts_ms+next_.duration_ms);
    if(fullscreen_) {
        const uint8_t black[]={0,0,0,255};
        compositor_.SetPixels(layer_+".0",black,1,1);
        compositor_.SetProps(layer_+".0",{{"w",std::to_string(compositor_.StageWidth())},
                                        {"h",std::to_string(compositor_.StageHeight())}});
    }
    Present(next_);
    if(auto pcm=decoder_.OpenAudio(loop)) audio_.PlayStream(audio_key_,std::move(pcm),volume);
    return true;
}

void VideoPlayer::Present(const VideoDecoder::Frame& frame) {
    const std::string id=fullscreen_ ? layer_+".1" : layer_;
    compositor_.SetPixels(id,frame.rgba.data(),frame.width,frame.height);
    if(fullscreen_) {
        const float scale=std::min(float(compositor_.StageWidth())/frame.width,
                                  float(compositor_.StageHeight())/frame.height);
        const float w=frame.width*scale,h=frame.height*scale;
        compositor_.SetProps(id,{{"w",std::to_string(w)},{"h",std::to_string(h)},
            {"left",std::to_string((compositor_.StageWidth()-w)/2)},
            {"top",std::to_string((compositor_.StageHeight()-h)/2)}});
    }
}

void VideoPlayer::Update(double now_ms) {
    if(!active_) return;
    // OHAudio/OpenSL report consumed frames, not PCM queued ahead. Silent
    // hosts and video-only clips use the same paused clock as script waits.
    const bool audio_playing=audio_.IsPlaying(audio_key_);
    const double audio_ms=audio_playing ? audio_.PlaybackMs(audio_key_) : -1;
    // Preserve the last audio position when the track drains or its clock
    // becomes unavailable. Some backends retain a stopped position forever;
    // the remaining video must continue on the engine's paused clock.
    const double elapsed=elapsed_ms_=std::max(elapsed_ms_,
        audio_ms>=0 ? audio_ms : now_ms-started_ms_);
    if(audio_ms>=0) started_ms_=now_ms-elapsed;
    int decoded=0;
    while(next_valid_ && next_.pts_ms+cycle_ms_<=elapsed && decoded++<8) {
        Present(next_);
        end_ms_=std::max(end_ms_,cycle_ms_+next_.pts_ms+next_.duration_ms);
        next_valid_=decoder_.Next(next_);
    }
    if(!next_valid_ && elapsed>=end_ms_) {
        if(loop_ && decoder_.Rewind() && decoder_.Next(next_)) {
            cycle_ms_=end_ms_;
            next_valid_=true;
            end_ms_=cycle_ms_+std::max(decoder_.DurationMs(),next_.pts_ms+next_.duration_ms);
        } else if(!audio_playing) Stop();
    }
}

void VideoPlayer::Stop() {
    if(!active_) return;
    active_=false;
    audio_.Stop(audio_key_);
    compositor_.DeleteLayer(layer_);
}
} // namespace artc
