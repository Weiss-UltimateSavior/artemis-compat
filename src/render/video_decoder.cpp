#include "render/video_decoder.h"
#include "log/logger.h"
#include <algorithm>
#include <cstring>
#include <limits>

#if defined(ARTC_HAS_FFMPEG)
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <mutex>
#endif

namespace artc {
#if defined(ARTC_HAS_FFMPEG)
namespace {
bool DecodeError(const char* operation,int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code,message,sizeof(message));
    Log(kLogError,std::string("video: ")+operation+": "+message+" ("+std::to_string(code)+")");
    return false;
}
struct Track {
    VideoDecoder::Bytes bytes;
    size_t offset = 0;
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int stream = -1;
    bool draining = false, ended = false;
    ~Track() {
        av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&codec);
        avformat_close_input(&format);
        if(io) { av_freep(&io->buffer); av_freep(&io); }
    }
    static int Read(void* opaque, uint8_t* out, int size) {
        auto& t=*static_cast<Track*>(opaque);
        const size_t n=std::min(static_cast<size_t>(size),t.bytes->size()-t.offset);
        if(!n) return AVERROR_EOF;
        std::memcpy(out,t.bytes->data()+t.offset,n); t.offset+=n;
        return static_cast<int>(n);
    }
    static int64_t Seek(void* opaque, int64_t pos, int whence) {
        auto& t=*static_cast<Track*>(opaque);
        if(whence==AVSEEK_SIZE) return t.bytes->size();
        whence &= ~AVSEEK_FORCE;
        const int64_t base=whence==SEEK_SET ? 0 : whence==SEEK_CUR ? t.offset :
                           whence==SEEK_END ? t.bytes->size() : -1;
        if(base<0 || pos < -base || pos>static_cast<int64_t>(t.bytes->size())-base) return AVERROR(EINVAL);
        t.offset=static_cast<size_t>(base+pos); return t.offset;
    }
    bool Open(VideoDecoder::Bytes data, AVMediaType type) {
#if LIBAVFORMAT_VERSION_MAJOR < 58
        static std::once_flag init;
        std::call_once(init,[]{ av_register_all(); });
#endif
        bytes=std::move(data);
        if(!bytes || bytes->empty()) return false;
        auto* buffer=static_cast<uint8_t*>(av_malloc(32768));
        if(!buffer) return false;
        io=avio_alloc_context(buffer,32768,0,this,Read,nullptr,Seek);
        if(!io) { av_free(buffer); return false; }
        format=avformat_alloc_context(); if(!format) return false;
        format->pb=io; format->flags|=AVFMT_FLAG_CUSTOM_IO;
        AVDictionary* options=nullptr;
        av_dict_set(&options,"format_whitelist","mov,ogg,avi,matroska,webm,mpeg,mpegvideo",0);
        const int opened=avformat_open_input(&format,nullptr,nullptr,&options);
        av_dict_free(&options);
        if(opened<0) return DecodeError("open container",opened);
        const int info=avformat_find_stream_info(format,nullptr);
        if(info<0) return DecodeError("read stream info",info);
        stream=av_find_best_stream(format,type,-1,-1,nullptr,0);
        if(stream<0) return type==AVMEDIA_TYPE_AUDIO && stream==AVERROR_STREAM_NOT_FOUND ? false : DecodeError("find track",stream);
        auto* decoder=avcodec_find_decoder(format->streams[stream]->codecpar->codec_id);
        if(!decoder) return DecodeError("find codec",AVERROR_DECODER_NOT_FOUND);
        codec=avcodec_alloc_context3(decoder);
        if(!codec) return DecodeError("allocate codec",AVERROR(ENOMEM));
        const int parameters=avcodec_parameters_to_context(codec,format->streams[stream]->codecpar);
        if(parameters<0) return DecodeError("copy codec parameters",parameters);
        codec->thread_count=2;
        const int opened_codec=avcodec_open2(codec,decoder,nullptr);
        if(opened_codec<0) return DecodeError("open codec",opened_codec);
        Log(kLogInfo,std::string("video: decoded track ")+decoder->name+" bytes="+std::to_string(bytes->size()));
        frame=av_frame_alloc(); packet=av_packet_alloc();
        return frame && packet;
    }
    bool Next() {
        if(ended) return false;
        for(;;) {
            const int result=avcodec_receive_frame(codec,frame);
            if(result>=0) return true;
            if(result!=AVERROR(EAGAIN)) {
                ended=true;
                return result==AVERROR_EOF ? false : DecodeError("receive frame",result);
            }
            if(draining) { ended=true; return false; }
            int status;
            do {
                av_packet_unref(packet);
                status=av_read_frame(format,packet);
            } while(status>=0 && packet->stream_index!=stream);
            if(status<0) {
                if(status!=AVERROR_EOF) DecodeError("read packet",status);
                draining=true;
                if(avcodec_send_packet(codec,nullptr)<0) { ended=true; return false; }
            } else {
                const int sent=avcodec_send_packet(codec,packet);
                if(sent<0) { ended=true; return DecodeError("send packet",sent); }
            }
        }
    }
    bool Rewind() {
        const int64_t start=format->streams[stream]->start_time;
        if(av_seek_frame(format,stream,start==AV_NOPTS_VALUE ? 0 : start,AVSEEK_FLAG_BACKWARD)<0) return false;
        avcodec_flush_buffers(codec); av_packet_unref(packet); draining=ended=false;
        return true;
    }
    double Duration() const {
        return format && format->duration!=AV_NOPTS_VALUE ? format->duration/1000.0 : 0;
    }
    double Pts() const {
        const int64_t pts=frame->best_effort_timestamp;
        const double start=format->start_time==AV_NOPTS_VALUE ? 0 : format->start_time/1000.0;
        return pts==AV_NOPTS_VALUE ? -1 : pts*av_q2d(format->streams[stream]->time_base)*1000-start;
    }
};

class MovieAudio final : public PcmStream {
public:
    Track track;
    SwrContext* swr=nullptr;
    std::vector<int16_t> pcm;
    size_t pos=0;
    bool loop=false, ended=false, flushed=false;
    ~MovieAudio() override { swr_free(&swr); }
    bool Open(VideoDecoder::Bytes data, bool repeat) {
        loop=repeat;
        if(!track.Open(std::move(data),AVMEDIA_TYPE_AUDIO)) return false;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
        if(swr_alloc_set_opts2(&swr,&stereo,AV_SAMPLE_FMT_S16,48000,&track.codec->ch_layout,
            track.codec->sample_fmt,track.codec->sample_rate,0,nullptr)<0) return false;
#else
        const int64_t layout=track.codec->channel_layout ? track.codec->channel_layout :
            av_get_default_channel_layout(track.codec->channels);
        swr=swr_alloc_set_opts(nullptr,AV_CH_LAYOUT_STEREO,AV_SAMPLE_FMT_S16,48000,
            layout,track.codec->sample_fmt,track.codec->sample_rate,0,nullptr);
#endif
        return swr && swr_init(swr)>=0;
    }
    size_t ReadStereo(int16_t* output, size_t frames) override {
        size_t written=0;
        bool rewound=false;
        while(written<frames && !ended) {
            if(pos<pcm.size()) {
                const size_t count=std::min(frames-written,(pcm.size()-pos)/2);
                std::copy_n(pcm.data()+pos,count*2,output+written*2);
                pos+=count*2; written+=count; rewound=false; continue;
            }
            const bool got=track.Next();
            if(!got && flushed) {
                if(!loop || rewound || !track.Rewind()) { ended=true; break; }
                swr_close(swr); if(swr_init(swr)<0) { ended=true; break; }
                rewound=true; flushed=false; continue;
            }
            const int samples=got ? track.frame->nb_samples : 0;
            const int capacity=static_cast<int>(av_rescale_rnd(swr_get_delay(swr,track.codec->sample_rate)+samples,
                48000,track.codec->sample_rate,AV_ROUND_UP))+32;
            pcm.resize(static_cast<size_t>(capacity)*2); pos=0;
            uint8_t* destination=reinterpret_cast<uint8_t*>(pcm.data());
            const int count=swr_convert(swr,&destination,capacity,
                got ? const_cast<const uint8_t**>(track.frame->extended_data) : nullptr,samples);
            if(count<0) { ended=true; break; }
            pcm.resize(static_cast<size_t>(count)*2);
            if(!got) flushed=true;
        }
        return written;
    }
    int SampleRate() const override { return 48000; }
    uint64_t FrameCount() const override { return static_cast<uint64_t>(std::max(0.0,track.Duration())*48); }
    bool Ended() const override { return ended && pos>=pcm.size(); }
};
} // namespace

struct VideoDecoder::Impl {
    Track video;
    SwsContext* sws=nullptr;
    double last_pts=0, frame_ms=40;
    ~Impl() { sws_freeContext(sws); }
};
VideoDecoder::VideoDecoder() : impl_(new Impl) {}
VideoDecoder::~VideoDecoder() = default;
bool VideoDecoder::Open(Bytes bytes) {
    impl_=std::make_unique<Impl>();
    if(!impl_->video.Open(std::move(bytes),AVMEDIA_TYPE_VIDEO)) return false;
    const auto rate=av_guess_frame_rate(impl_->video.format,impl_->video.format->streams[impl_->video.stream],nullptr);
    if(rate.num>0 && rate.den>0) impl_->frame_ms=1000.0*rate.den/rate.num;
    return true;
}
bool VideoDecoder::Next(Frame& out) {
    auto& v=impl_->video;
    if(!v.Next()) return false;
    const int w=v.frame->width,h=v.frame->height;
    if(w<=0 || h<=0 || w>8192 || h>8192) {
        Log(kLogError,"video: invalid frame dimensions "+std::to_string(w)+"x"+std::to_string(h));
        return false;
    }
    impl_->sws=sws_getCachedContext(impl_->sws,w,h,static_cast<AVPixelFormat>(v.frame->format),
        w,h,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
    if(!impl_->sws) {
        Log(kLogError,"video: cannot convert pixel format "+std::to_string(v.frame->format)+" to RGBA");
        return false;
    }
    out.width=w; out.height=h; out.rgba.resize(static_cast<size_t>(w)*h*4);
    uint8_t* pixels[4]={out.rgba.data(),nullptr,nullptr,nullptr}; int stride[4]={w*4,0,0,0};
    const int rows=sws_scale(impl_->sws,v.frame->data,v.frame->linesize,0,h,pixels,stride);
    if(rows<=0) return DecodeError("convert frame",rows);
    const double pts=v.Pts(); out.pts_ms=pts<0 ? impl_->last_pts : pts;
    out.duration_ms=impl_->frame_ms; impl_->last_pts=out.pts_ms+out.duration_ms;
    return true;
}
bool VideoDecoder::Rewind() { impl_->last_pts=0; return impl_->video.Rewind(); }
double VideoDecoder::DurationMs() const { return impl_->video.Duration(); }
std::unique_ptr<PcmStream> VideoDecoder::OpenAudio(bool loop) const {
    auto audio=std::make_unique<MovieAudio>();
    return audio->Open(impl_->video.bytes,loop) ? std::move(audio) : nullptr;
}
#else
struct VideoDecoder::Impl {};
VideoDecoder::VideoDecoder() : impl_(new Impl) {}
VideoDecoder::~VideoDecoder() = default;
bool VideoDecoder::Open(Bytes) { return false; }
bool VideoDecoder::Next(Frame&) { return false; }
bool VideoDecoder::Rewind() { return false; }
double VideoDecoder::DurationMs() const { return 0; }
std::unique_ptr<PcmStream> VideoDecoder::OpenAudio(bool) const { return {}; }
#endif
} // namespace artc
