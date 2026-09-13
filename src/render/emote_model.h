#pragma once
#include "pack/psb.h"
#include <map>
#include <set>

namespace artc {
struct EmoteImage {
    int width=0,height=0;
    double origin_x=0,origin_y=0;
    std::vector<uint8_t> rgba;
};
struct EmoteFrame {double time=0,value=0,easing=0;bool terminal=false;};
struct EmoteTrack {std::string variable;bool instant=false;std::vector<EmoteFrame> frames;};
struct EmoteTimeline {
    double last_time=0,loop_begin=0,loop_end=0;
    bool difference=false;
    std::vector<EmoteTrack> tracks;
};
// Resource/animation-data foundation, intentionally separate from the future
// mesh/physics renderer. Loading this object does NOT mean an E-mote player is
// available; no original SDK version or complete playback support is advertised.
class EmoteModel {
public:
    bool Load(PsbDocument document,std::string& error);
    bool Image(const std::string& source,const std::string& icon,EmoteImage& out,std::string& error) const;
    const PsbDocument& Document() const {return document_;}
    const std::map<std::string,EmoteTimeline>& Timelines() const {return timelines_;}
    const std::set<std::string>& Variables() const {return variables_;}
    // Offline keyframe sampling for renderer development. Full SDK controller,
    // transition queue, difference mixing and physics evaluation are separate.
    bool Sample(const std::string& timeline,double frame,std::map<std::string,double>& values) const;
private:
    PsbDocument document_;
    std::map<std::string,EmoteTimeline> timelines_;
    std::set<std::string> variables_;
};
}
