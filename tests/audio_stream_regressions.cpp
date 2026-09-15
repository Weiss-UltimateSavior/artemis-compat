#include "audio/pcm_ring.h"
#include "audio/vorbis_stream.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <atomic>
#include <map>
#include <thread>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    std::map<std::string, std::vector<uint8_t>> files;
    for (const std::string name : {"tone_a.ogg", "tone_b.ogg"}) {
        std::ifstream f(std::string(ARTC_TEST_DATA) + "/" + name, std::ios::binary);
        Check(bool(f), "open synthetic test tone");
        files[name] = {std::istreambuf_iterator<char>(f), {}};
    }
    auto read = [&](const std::string& name, std::vector<uint8_t>& bytes) {
        const auto f = files.find(name);
        if (f == files.end()) return false;
        bytes = f->second;
        return true;
    };
    auto decode = [&](const std::string& name) {
        artc::VorbisStream s;
        auto single = [&](const std::string& n, std::vector<uint8_t>& bytes) {
            return n == name && read(n, bytes);
        };
        Check(s.Open(single, name, false), "open non-looping tone");
        Check(s.SampleRate() == 44100 && !s.HasLoopSegment(), "source format and no-loop semantics");
        std::vector<int16_t> pcm(s.FrameCount() * 2);
        Check(s.ReadStereo(pcm.data(), pcm.size() / 2) == pcm.size() / 2, "decode exact stream length");
        Check(s.Ended() && s.ReadStereo(pcm.data(), 1) == 0, "finish on exact buffer boundary");
        for (size_t i=0; i<pcm.size(); i+=2) Check(pcm[i]==pcm[i+1], "mono expands to stereo");
        return pcm;
    };
    const auto intro = decode("tone_a.ogg"), repeat = decode("tone_b.ogg");
    std::vector<int16_t> expected = intro;
    for (int i=0;i<3;++i) expected.insert(expected.end(), repeat.begin(), repeat.end());
    artc::VorbisStream stream;
    Check(stream.Open(read, "tone_a.ogg", true) && stream.HasLoopSegment(), "discover intro companion");
    std::vector<int16_t> actual(expected.size());
    for (size_t i=0;i<actual.size()/2;) {
        const size_t count=std::min(size_t(513), actual.size()/2-i);
        Check(stream.ReadStereo(actual.data()+i*2, count)==count, "fill a block across segment boundaries");
        i+=count;
    }
    Check(actual == expected && !stream.Ended(), "intro plays once, loop repeats without gaps or duplicate samples");
    Check(stream.Open(read,"tone_a.ogg",false), "open two-segment non-looping source");
    actual.resize(intro.size()+repeat.size());
    Check(stream.FrameCount()*2==actual.size() &&
          stream.ReadStereo(actual.data(),actual.size()/2)==actual.size()/2 && stream.Ended(),
          "loop=0 plays both intro and body exactly once");
    Check(std::equal(actual.begin(),actual.end(),expected.begin()),"non-looping segment PCM");
    files.erase("tone_b.ogg");
    Check(stream.Open(read, "tone_a.ogg", true) && !stream.HasLoopSegment(), "missing companion falls back to full-file loop");
    actual.resize(intro.size()*2);
    Check(stream.ReadStereo(actual.data(), actual.size()/2)==actual.size()/2, "whole-file loop fills output");
    Check(std::equal(intro.begin(), intro.end(), actual.begin()) &&
          std::equal(intro.begin(), intro.end(), actual.begin()+intro.size()), "fallback repeats exact PCM");
    files["broken.ogg"] = {1,2,3};
    Check(!stream.Open(read,"broken.ogg",true), "invalid stream rejected");
    int16_t pan[] = {10000,10000};
    artc::ApplyStereoPan(pan,1,-1000);Check(pan[0]==10000 && pan[1]==0,"left pan");
    pan[0]=pan[1]=10000;
    artc::ApplyStereoPan(pan,1,1000);Check(pan[0]==0 && pan[1]==10000,"right pan");
    pan[0]=pan[1]=10000;
    artc::ApplyStereoPan(pan,1,0);Check(pan[0]==7071 && pan[1]==7071,"constant-power center");

    // ---- PcmRing (T2-3 decode-ahead buffer) --------------------------------
    // Water-level accounting, wrap-around, truncation at both ends, and a
    // producer/consumer thread hammering the SPSC contract.
    {
        artc::PcmRing ring(64);              // pow2 buffer, one slot kept free
        const size_t cap = ring.Capacity();
        Check(cap >= 64 && ring.Available() == 0 && ring.Space() == cap,
              "ring starts empty with full space");
        int16_t src[512];
        for (size_t i = 0; i < 512; ++i) src[i] = (int16_t)(i & 0x7fff);
        Check(ring.Write(src, cap + 16) == cap, "write truncates at capacity");
        Check(ring.Available() == cap && ring.Space() == 0, "water level after fill");
        int16_t dst[512] = {0};
        Check(ring.Read(dst, cap + 16) == cap, "read drains what was stored");
        Check(dst[0] == 0 && dst[cap - 1] == (int16_t)((cap - 1) & 0x7fff),
              "FIFO order preserved");
        Check(ring.Read(dst, 1) == 0, "empty read returns zero");
        // wrap: leave half the ring populated, drain most of it, then write a
        // block that rolls the producer cursor past the buffer end.
        const size_t half = cap / 2;
        Check(ring.Write(src, half) == half, "write before wrap");
        Check(ring.Read(dst, half - 10) == half - 10 && dst[0] == 0, "partial drain");
        Check(ring.Write(src, cap) == cap - 10, "write wraps and truncates at free space");
        Check(ring.Read(dst, cap) == cap, "read wraps across the buffer end");
        Check(dst[0] == (int16_t)((half - 10) & 0x7fff) &&
              dst[9] == (int16_t)((half - 1) & 0x7fff) &&
              dst[10] == 0 && dst[cap - 1] == (int16_t)((cap - 11) & 0x7fff),
              "wrapped data order preserved");

        artc::PcmRing spsc(1024);
        std::atomic<bool> done{false};
        std::atomic<uint64_t> written{0}, read{0};
        int16_t first_bad = 0;
        std::thread producer([&] {
            int16_t buf[128];
            for (uint64_t n = 0; n < 20000;) {
                const size_t take = (size_t)std::min<uint64_t>(128, 20000 - n);
                for (size_t i = 0; i < take; ++i) buf[i] = (int16_t)((n + i) & 0x7fff);
                const size_t put = spsc.Write(buf, take);
                n += put;
                written.fetch_add(put);
            }
        });
        std::thread consumer([&] {
            int16_t buf[97];   // deliberately not a divisor of the write size
            uint64_t expect = 0;
            while (read.load() < 20000) {
                const size_t got = spsc.Read(buf, 97);
                for (size_t i = 0; i < got; ++i) {
                    if (buf[i] != (int16_t)((expect + i) & 0x7fff)) first_bad = buf[i];
                }
                expect += got;
                read.fetch_add(got);
            }
        });
        producer.join();
        consumer.join();
        done = true;
        Check(written.load() == 20000 && read.load() == 20000 && first_bad == 0,
              "SPSC producer/consumer preserves every sample in order");
    }

    std::cout << "Vorbis segment and pan regressions passed\n";
}
