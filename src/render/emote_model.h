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
// metadata.eyeControl — automatic blinking. The label names a model variable
// (writers target it like any other variable); begin/end are the variable
// values of the blink arc and frames is its duration at 60 fps.
struct EmoteBlink {
    std::string variable;
    double begin=0,end=0;
    double frames=1;              // blinkFrameCount, >=1
    double interval_min=0,interval_max=0;
    bool enabled=true,blink_enabled=true;
};
// metadata.selectorControl — option lists. Selecting option v (a variable
// value, usually an index) crossfades each item: the selected item gets
// onValue, neighbours blend linearly (reference host formula).
struct EmoteSelectorItem {
    std::string label;
    double on=1,off=0;
};
struct EmoteSelector {
    std::string label;
    bool enabled=true;
    std::vector<EmoteSelectorItem> items;
};
// metadata.attrcomp remove rules with value<=0: the named layer is not part of
// the motion (the reference host marks it removed at load time).
struct EmoteRemoval {
    std::string chara,motion,layer;
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
    const std::vector<EmoteBlink>& Blinks() const {return blinks_;}
    const std::vector<EmoteSelector>& Selectors() const {return selectors_;}
    const std::vector<EmoteRemoval>& Removals() const {return removals_;}
    bool Mirrored() const {return mirror_;}
    // Derivation of the motion's sync frame: the maximum authored frame time
    // over non-parameterized nodes, following child motions (the shipped
    // scripts use it as the "animation meaningful end"). <0 when unknown.
    double SyncFrame(const std::string& chara,const std::string& motion) const;
    // Offline keyframe sampling for renderer development. Full SDK controller,
    // transition queue, difference mixing and physics evaluation are separate.
    bool Sample(const std::string& timeline,double frame,std::map<std::string,double>& values) const;
private:
    PsbDocument document_;
    std::map<std::string,EmoteTimeline> timelines_;
    std::set<std::string> variables_;
    std::vector<EmoteBlink> blinks_;
    std::vector<EmoteSelector> selectors_;
    std::vector<EmoteRemoval> removals_;
    std::map<std::pair<std::string,std::string>,double> sync_frames_;
    bool mirror_=false;
};
}
