#include "audio/vorbis_stream.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>

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
    std::cout << "Vorbis segment and pan regressions passed\n";
}
