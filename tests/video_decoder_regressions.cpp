#include "render/video_decoder.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdlib>

static void Check(bool value, const char* message) {
    if(!value) { std::cerr<<message<<'\n'; std::exit(1); }
}
int main(int argc,char** argv) {
    Check(argc==2,"provide a synthetic movie");
    std::ifstream file(argv[1],std::ios::binary);
    auto bytes=std::make_shared<std::vector<uint8_t>>(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
    artc::VideoDecoder video;
    Check(video.Open(bytes),"open movie from memory");
    artc::VideoDecoder::Frame frame;
    int frames=0; double last=-1;
    while(video.Next(frame)) {
        Check(frame.width==32 && frame.height==32 && frame.rgba.size()==4096,"decoded frame dimensions");
        Check(frame.pts_ms>=last,"presentation timestamps are ordered"); last=frame.pts_ms;
        Check(frame.rgba[0]>220 && frame.rgba[1]<30 && frame.rgba[2]<30,"decoded red frame pixels");
        ++frames;
    }
    Check(frames==5 && last>=399 && last<=401,"decode all frames including delayed codec output");
    Check(video.Rewind() && video.Next(frame) && std::abs(frame.pts_ms)<1,"rewind resets the video cursor");
    auto audio=video.OpenAudio(false);
    Check(bool(audio) && audio->SampleRate()==48000,"decode movie audio as stereo PCM");
    int16_t pcm[1024]; size_t total=0; int peak=0;
    for(int n=0;n<200 && !audio->Ended();++n) {
        const size_t read=audio->ReadStereo(pcm,512); total+=read;
        for(size_t i=0;i<read*2;++i) peak=std::max(peak,std::abs(int(pcm[i])));
    }
    Check(audio->Ended() && total>=23000 && total<26000 && peak>1000,"audio drains its resampler and decoder tail");
    auto loop=video.OpenAudio(true);
    size_t repeated=0;
    for(int n=0;n<110;++n) repeated+=loop->ReadStereo(pcm,512);
    Check(repeated==110*512 && !loop->Ended(),"loop audio keeps filling output across the end");
    auto broken=std::make_shared<std::vector<uint8_t>>(40,0);
    artc::VideoDecoder invalid;
    Check(!invalid.Open(broken),"reject malformed movie data");
    std::cout<<"video and synchronized PCM decoder regressions passed\n";
}
