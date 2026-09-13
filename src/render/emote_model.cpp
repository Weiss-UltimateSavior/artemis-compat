#include "render/emote_model.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace artc {
namespace {
double Number(const PsbValue& v,double fallback=0) {
    double n=fallback;
    if(v.type==PsbValue::Number || v.type==PsbValue::Boolean)n=v.number;
    else if(v.type==PsbValue::String) {
        char* end=nullptr;n=std::strtod(v.string.c_str(),&end);
        if(end==v.string.c_str() || end!=v.string.c_str()+v.string.size())throw std::runtime_error("invalid E-mote numeric string");
    } else if(v.type!=PsbValue::Null)throw std::runtime_error("invalid E-mote number type");
    if(!std::isfinite(n))throw std::runtime_error("non-finite E-mote value");return n;
}
}
bool EmoteModel::Load(PsbDocument document,std::string& error) {
    try {
        EmoteModel next;const auto& root=document.root;const auto& metadata=root.At("metadata");
        const auto& base=metadata.At("base");
        if(root.At("object").At(base.At("chara").string).At("motion").At(base.At("motion").string).type!=PsbValue::Object)
            throw std::runtime_error("E-mote base motion not found");
        for(const auto& v:metadata.At("variableList").array) {
            if(v.At("label").string.empty())throw std::runtime_error("E-mote variable has no label");
            next.variables_.insert(v.At("label").string);
        }
        std::set<std::string> instant;
        for(const auto& v:metadata.At("instantVariableList").array)instant.insert(v.string);
        for(const auto& t:metadata.At("timelineControl").array) {
            EmoteTimeline timeline;const auto label=t.At("label").string;
            if(label.empty())throw std::runtime_error("E-mote timeline has no label");
            timeline.last_time=Number(t.At("lastTime"));timeline.loop_begin=Number(t.At("loopBegin"));
            timeline.loop_end=Number(t.At("loopEnd"));timeline.difference=Number(t.At("diff"))!=0;
            if(!(timeline.loop_begin==-1 && timeline.loop_end==-1) &&
               (timeline.loop_begin<0 || timeline.loop_end<timeline.loop_begin))
                throw std::runtime_error("invalid E-mote timeline loop");
            for(const auto& track:t.At("variableList").array) {
                EmoteTrack result;result.variable=track.At("label").string;result.instant=instant.count(result.variable);
                // A variable may have multiple ordered tracks (e.g. eyebrow
                // ranges in the public Vanilla model); preserve all of them.
                if(result.variable.empty())
                    throw std::runtime_error("invalid E-mote timeline variable");
                for(const auto& f:track.At("frameList").array) {
                    EmoteFrame frame;frame.time=Number(f.At("time"));frame.terminal=Number(f.At("type"))==0;
                    if(frame.time<0 || (!result.frames.empty() && frame.time<result.frames.back().time))
                        throw std::runtime_error("unordered E-mote keyframes");
                    if(!frame.terminal) {
                        if(Number(f.At("type"))!=2 || f.At("content").At("value").type==PsbValue::Null)
                            throw std::runtime_error("unsupported E-mote variable keyframe");
                        frame.value=Number(f.At("content").At("value"));frame.easing=Number(f.At("content").At("easing"));
                    }
                    result.frames.push_back(frame);
                }
                timeline.tracks.push_back(std::move(result));
            }
            if(!next.timelines_.emplace(label,std::move(timeline)).second)
                throw std::runtime_error("duplicate E-mote timeline");
        }
        next.document_=std::move(document);*this=std::move(next);error.clear();return true;
    } catch(const std::exception& e){error=e.what();return false;}
}
bool EmoteModel::Image(const std::string& source,const std::string& icon,EmoteImage& out,std::string& error) const {
    try {
        const auto& root=document_.root;const auto& image=root.At("source").At(source).At("icon").At(icon);
        const auto spec=root.At("spec").string;
        if(spec!="krkr" && spec!="win" && spec!="common")throw std::runtime_error("unsupported E-mote texture platform");
        const double width=Number(image.At("width")),height=Number(image.At("height"));
        if(width<1 || height<1 || width>8192 || height>8192 || width!=std::floor(width) || height!=std::floor(height) || width*height>16777216)
            throw std::runtime_error("invalid E-mote texture size");
        EmoteImage result;result.width=int(width);result.height=int(height);
        result.origin_x=Number(image.At("originX"));result.origin_y=Number(image.At("originY"));
        std::vector<uint8_t> bytes,palette,decoded;
        if(!document_.ReadResource(image.At("pixel"),bytes))throw std::runtime_error("missing E-mote pixel resource");
        const auto format=image.At("type").string;
        if(!format.empty() && format!="RGBA8" && format!="CI8")throw std::runtime_error("unsupported E-mote pixel format");
        const bool indexed=format=="CI8";
        // Some RGBA models retain an unused pal reference. Format, not mere
        // presence of that field, determines whether pixels are indices.
        if(indexed && !image.At("palType").string.empty() && image.At("palType").string!="RGBA8")
            throw std::runtime_error("unsupported E-mote palette format");
        if(indexed && (!document_.ReadResource(image.At("pal"),palette) || palette.empty() || palette.size()%4 || palette.size()>1024))
            throw std::runtime_error("invalid E-mote palette");
        const unsigned stride=indexed?1:4;const size_t count=result.width*result.height;
        if(image.At("compress").string=="RL") {
            if(!DecodePsbRl(bytes,count,stride,decoded))throw std::runtime_error("invalid E-mote RL texture");
        } else if(image.At("compress").string.empty() || image.At("compress").string=="none") {
            if(bytes.size()!=count*stride)throw std::runtime_error("invalid E-mote raw texture length");decoded=std::move(bytes);
        } else throw std::runtime_error("unsupported E-mote texture compression");
        if(indexed) {
            result.rgba.resize(count*4);
            for(size_t i=0;i<count;++i) {
                const size_t at=size_t(decoded[i])*4;if(at+4>palette.size())throw std::runtime_error("E-mote palette index outside table");
                std::copy_n(palette.data()+at,4,result.rgba.data()+i*4);
            }
        } else result.rgba=std::move(decoded);
        // Desktop PSB color words are A8R8G8B8, i.e. BGRA on little endian.
        if(spec=="krkr" || spec=="win")for(size_t i=0;i<result.rgba.size();i+=4)std::swap(result.rgba[i],result.rgba[i+2]);
        out=std::move(result);error.clear();return true;
    } catch(const std::exception& e){error=e.what();return false;}
}
bool EmoteModel::Sample(const std::string& label,double frame,std::map<std::string,double>& out) const {
    const auto it=timelines_.find(label);if(it==timelines_.end() || !std::isfinite(frame))return false;
    const auto& timeline=it->second;frame=std::max(frame,0.0);
    if(timeline.loop_end>timeline.loop_begin && timeline.loop_begin>=0 && frame>=timeline.loop_end)
        frame=timeline.loop_begin+std::fmod(frame-timeline.loop_begin,timeline.loop_end-timeline.loop_begin);
    else if(timeline.last_time>=0)frame=std::min(frame,timeline.last_time);
    std::map<std::string,double> result;
    for(const auto& track:timeline.tracks) {
        const EmoteFrame* left=nullptr;const EmoteFrame* right=nullptr;
        for(const auto& f:track.frames) {
            if(f.time<=frame){if(!f.terminal)left=&f;else break;}
            else {right=&f;break;}
        }
        if(!left)continue;
        double value=left->value;
        if(!track.instant && right && !right->terminal && right->time>left->time) {
            const double ratio=(frame-left->time)/(right->time-left->time);
            const double ease=right->easing,exponent=ease>=0?ease+1:1/(1-ease);
            value+=(right->value-value)*std::pow(ratio,exponent);
        }
        result[track.variable]=value;
    }
    out=std::move(result);return true;
}
}
