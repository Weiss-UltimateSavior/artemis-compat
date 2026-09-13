#include "audio/vorbis_stream.h"
#include "log/logger.h"
#include <algorithm>
#include <climits>
#include <cmath>

#define STB_VORBIS_NO_STDIO 1
#include "../../third_party/stb_vorbis/stb_vorbis.c"

namespace artc {
struct VorbisStream::Impl {
    struct Segment {
        std::vector<uint8_t> bytes;
        stb_vorbis* decoder = nullptr;
        int rate = 0;
        uint64_t frames = 0;
        ~Segment() { if (decoder) stb_vorbis_close(decoder); }
        bool Open(const Reader& read, const std::string& file) {
            if (!read(file, bytes) || bytes.empty() || bytes.size() > INT_MAX) return false;
            int error = 0;
            decoder = stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()), &error, nullptr);
            if (!decoder) return false;
            const auto info = stb_vorbis_get_info(decoder);
            rate = info.sample_rate;
            frames = stb_vorbis_stream_length_in_samples(decoder);
            return rate > 0 && info.channels >= 1 && info.channels <= 2 && frames > 0;
        }
    };
    std::unique_ptr<Segment> first, repeat;
    Segment* current = nullptr;
    uint64_t cursor = 0;
    bool loop = false, ended = true;
};

VorbisStream::VorbisStream() : impl_(new Impl) {}
VorbisStream::~VorbisStream() = default;

bool VorbisStream::Open(const Reader& read, const std::string& file, bool loop) {
    auto next = std::make_unique<Impl>();
    next->first = std::make_unique<Impl::Segment>();
    if (!next->first->Open(read, file)) return false;
    next->loop = loop;
    next->ended = false;
    next->current = next->first.get();
    const auto dot = file.find_last_of('.');
    if (dot != std::string::npos && dot >= 2 && file.compare(dot - 2, 2, "_a") == 0) {
        std::string companion = file;
        companion[dot - 1] = 'b';
        auto repeat = std::make_unique<Impl::Segment>();
        if (repeat->Open(read, companion) && repeat->rate == next->first->rate) {
            next->repeat = std::move(repeat);
            Log(kLogInfo, "audio: intro/loop " + file + " -> " + companion);
        }
    }
    impl_ = std::move(next);
    return true;
}

size_t VorbisStream::ReadStereo(int16_t* output, size_t frames) {
    auto& s = *impl_;
    size_t written = 0;
    while (written < frames && !s.ended && s.current) {
        const int request = static_cast<int>(std::min(frames - written, size_t(INT_MAX / 2)));
        const int got = stb_vorbis_get_samples_short_interleaved(
            s.current->decoder, 2, output + written * 2, request * 2);
        written += got;
        s.cursor += got;
        if (got == 0 || s.cursor >= s.current->frames) {
            const bool next_segment = s.repeat && s.current == s.first.get();
            if ((!s.loop && !next_segment) || (got == 0 && s.cursor == 0)) {
                s.ended = true;
                break;
            }
            s.current = s.repeat ? s.repeat.get() : s.first.get();
            s.cursor = 0;
            if (!stb_vorbis_seek_start(s.current->decoder)) { s.ended = true; break; }
        }
    }
    return written;
}

int VorbisStream::SampleRate() const { return impl_->first ? impl_->first->rate : 0; }
uint64_t VorbisStream::FrameCount() const {
    return (impl_->first ? impl_->first->frames : 0) + (impl_->repeat ? impl_->repeat->frames : 0);
}
bool VorbisStream::Ended() const { return impl_->ended; }
bool VorbisStream::HasLoopSegment() const { return bool(impl_->repeat); }

void ApplyStereoPan(int16_t* samples, size_t frames, int pan1000) {
    const double angle = (std::clamp(pan1000, -1000, 1000) / 1000.0 + 1.0) * 0.7853981633974483;
    const double left = std::cos(angle), right = std::sin(angle);
    for (size_t i = 0; i < frames; ++i) {
        samples[i * 2] = static_cast<int16_t>(samples[i * 2] * left);
        samples[i * 2 + 1] = static_cast<int16_t>(samples[i * 2 + 1] * right);
    }
}
} // namespace artc
