#include "render/emote_model.h"
#include "render/block_decode.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
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
        for(const auto& e:metadata.At("eyeControl").array) {
            EmoteBlink blink;
            blink.variable=e.At("label").string;
            if(blink.variable.empty())throw std::runtime_error("E-mote eye control has no label");
            blink.begin=Number(e.At("beginFrame"));
            blink.end=Number(e.At("endFrame"),blink.begin);
            blink.frames=std::max(1.0,Number(e.At("blinkFrameCount"),1));
            blink.interval_min=std::max(0.0,Number(e.At("blinkIntervalMin")));
            blink.interval_max=std::max(blink.interval_min,Number(e.At("blinkIntervalMax")));
            blink.enabled=Number(e.At("enabled"),1)!=0;
            blink.blink_enabled=Number(e.At("blinkEnabled"),1)!=0;
            next.blinks_.push_back(std::move(blink));
        }
        next.mirror_=Number(metadata.At("mirror"))!=0;
        for(const auto& s:metadata.At("selectorControl").array) {
            EmoteSelector selector;
            selector.label=s.At("label").string;
            if(selector.label.empty())throw std::runtime_error("E-mote selector has no label");
            selector.enabled=Number(s.At("enabled"),1)!=0;
            for(const auto& o:s.At("optionList").array) {
                EmoteSelectorItem item;
                item.label=o.At("label").string;
                if(item.label.empty())throw std::runtime_error("E-mote selector option has no label");
                // Selector targets are variables; models normally declare them
                // in variableList, but some variants rely on the selector to
                // introduce them (the reference host inserts on demand).
                next.variables_.insert(item.label);
                item.on=Number(o.At("onValue"),1);
                item.off=Number(o.At("offValue"));
                selector.items.push_back(std::move(item));
            }
            if(selector.items.empty())throw std::runtime_error("E-mote selector has no options");
            // The selector label itself is frequently also a variableList
            // entry (the game writes the option index through it).
            next.variables_.insert(selector.label);
            next.selectors_.push_back(std::move(selector));
        }
        for(const auto& a:metadata.At("attrcomp").array) {
            for(const auto& r:a.At("data").At("remove").array) {
                if(Number(r.At("value"))>0)continue;  // only removal rules are applied
                EmoteRemoval removal;
                removal.chara=r.At("id").At("chara").string;
                removal.motion=r.At("id").At("motion").string;
                removal.layer=r.At("id").At("layer").string;
                if(removal.layer.empty())throw std::runtime_error("invalid E-mote attrcomp removal");
                next.removals_.push_back(std::move(removal));
            }
        }
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
        // Sync frame per motion (reference host/krkrsdl3): max content frame
        // over non-parameterized nodes, following child motion references.
        std::set<std::pair<std::string,std::string>> visiting;
        std::function<double(const std::string&,const std::string&)> sync=
            [&](const std::string& chara,const std::string& motion)->double {
            const auto& m=document.root.At("object").At(chara).At("motion").At(motion);
            if(m.type!=PsbValue::Object)return -1;
            if(m.At("parameterize").type!=PsbValue::Null)return -1;
            if(!visiting.insert({chara,motion}).second)return -1;
            double best=-1;
            std::function<void(const PsbValue&)> walk=[&](const PsbValue& nodes) {
                for(const auto& n:nodes.array) {
                    const bool count_times=n.At("parameterize").type==PsbValue::Null;
                    for(const auto& f:n.At("frameList").array) {
                        if(Number(f.At("type"))==0)continue;
                        if(f.At("content").type==PsbValue::Null)continue;
                        const auto& src=f.At("content").At("src").string;
                        if(src.rfind("motion/",0)==0) {
                            const size_t slash=src.find('/',7);
                            if(slash!=std::string::npos) {
                                const double child=sync(src.substr(7,slash-7),src.substr(slash+1));
                                if(child>best)best=child;
                            }
                        }
                        if(!count_times)continue;
                        const double t=Number(f.At("time"));
                        if(t>best)best=t;
                    }
                    walk(n.At("children"));
                }
            };
            walk(m.At("layer"));
            visiting.erase({chara,motion});
            return best;
        };
        for(const auto& obj:document.root.At("object").object)
            for(const auto& mtn:obj.second.At("motion").object) {
                const double s=sync(obj.first,mtn.first);
                if(s>=0)next.sync_frames_[{obj.first,mtn.first}]=s;
            }
        next.document_=std::move(document);*this=std::move(next);error.clear();return true;
    } catch(const std::exception& e){error=e.what();return false;}
}
double EmoteModel::SyncFrame(const std::string& chara,const std::string& motion) const {
    const auto it=sync_frames_.find({chara,motion});
    return it==sync_frames_.end()?-1:it->second;
}
bool EmoteModel::Image(const std::string& source,const std::string& icon,EmoteImage& out,std::string& error) const {
    try {
        const auto& root=document_.root;const auto& src=root.At("source").At(source);
        const auto& image=src.At("icon").At(icon);
        const auto spec=root.At("spec").string;
        if(spec!="krkr" && spec!="win" && spec!="common")throw std::runtime_error("unsupported E-mote texture platform");
        // win/common store one shared texture per source and crop every icon
        // out of it (left/top/width/height); krkr keeps a pixel resource per
        // icon. Reference host: art3m1s-core atlas.rs / krkrsdl3 readIconTobuffer.
        const auto& shared=src.At("texture");
        const bool atlas=shared.type==PsbValue::Object;
        const auto& size_src=atlas?shared:image;
        const double tex_w=Number(size_src.At("width")),tex_h=Number(size_src.At("height"));
        if(tex_w<1 || tex_h<1 || tex_w>8192 || tex_h>8192 ||
           tex_w!=std::floor(tex_w) || tex_h!=std::floor(tex_h) || tex_w*tex_h>16777216)
            throw std::runtime_error("invalid E-mote texture size");
        double left=0,top=0,crop_w=tex_w,crop_h=tex_h;
        if(atlas) {
            left=Number(image.At("left"));top=Number(image.At("top"));
            crop_w=Number(image.At("width"));crop_h=Number(image.At("height"));
            if(left<0 || top<0 || crop_w<1 || crop_h<1 ||
               left!=std::floor(left) || top!=std::floor(top) ||
               crop_w!=std::floor(crop_w) || crop_h!=std::floor(crop_h) ||
               left+crop_w>tex_w || top+crop_h>tex_h)
                throw std::runtime_error("invalid E-mote atlas crop");
        }
        EmoteImage result;result.width=int(crop_w);result.height=int(crop_h);
        result.origin_x=Number(image.At("originX"));result.origin_y=Number(image.At("originY"));
        std::vector<uint8_t> bytes,palette,decoded;
        if(!document_.ReadResource(size_src.At("pixel"),bytes))throw std::runtime_error("missing E-mote pixel resource");
        const auto format=size_src.At("type").string;
        const bool indexed=!atlas && format=="CI8";
        const bool dxt5=format=="DXT5";
        const bool dxt1=format=="DXT1";
        const bool dxt3=format=="DXT3";
        const bool bc7=format=="BC7";
        const bool rgba8=format.empty() || format=="RGBA8";
        const bool rgbx8=format=="RGBX8";
        const bool a8l8=format=="A8L8";
        const bool rgba4444=format=="RGBA4444";
        const bool rgba5551=format=="RGBA5551";
        const bool rgba5650=format=="RGBA5650";
        if(!rgba8 && !rgbx8 && !indexed && !dxt5 && !dxt1 && !dxt3 && !bc7 &&
           !a8l8 && !rgba4444 && !rgba5551 && !rgba5650)
            throw std::runtime_error("unsupported E-mote pixel format");
        // Some RGBA models retain an unused pal reference. Format, not mere
        // presence of that field, determines whether pixels are indices.
        if(indexed && !image.At("palType").string.empty() && image.At("palType").string!="RGBA8")
            throw std::runtime_error("unsupported E-mote palette format");
        if(indexed && (!document_.ReadResource(image.At("pal"),palette) || palette.empty() || palette.size()%4 || palette.size()>1024))
            throw std::runtime_error("invalid E-mote palette");
        const size_t tex_count=size_t(tex_w)*size_t(tex_h);
        unsigned stride;
        if(indexed)stride=1;
        else if(a8l8 || rgba4444 || rgba5551 || rgba5650)stride=2;
        else stride=4;
        size_t raw_bytes;
        if(dxt1 || dxt3 || dxt5 || bc7) {
            const size_t blocks=size_t((tex_w+3)/4)*size_t((tex_h+3)/4);
            raw_bytes=blocks*((dxt1)?8:16); // 8/16 bytes per 4x4 block
        } else {
            raw_bytes=tex_count*stride;
        }
        const auto compress=atlas?std::string():image.At("compress").string;
        if(compress=="RL") {
            if(!DecodePsbRl(bytes,raw_bytes/stride,stride,decoded))throw std::runtime_error("invalid E-mote RL texture");
        } else if(compress.empty() || compress=="none") {
            // A mip chain appends levels after level 0; only level 0 is read.
            const auto has_mip=[](const PsbValue& v) {
                if(v.type==PsbValue::Number)return v.number!=0;
                return v.type!=PsbValue::Null;
            };
            const bool mips=has_mip(image.At("mipMap")) || has_mip(image.At("mipMapLevel"));
            if(bytes.size()<raw_bytes || (!mips && bytes.size()!=raw_bytes))
                throw std::runtime_error("invalid E-mote raw texture length");
            bytes.resize(raw_bytes);
            decoded=std::move(bytes);
        } else throw std::runtime_error("unsupported E-mote texture compression");
        std::vector<uint8_t> full;
        if(indexed) {
            full.resize(tex_count*4);
            for(size_t i=0;i<tex_count;++i) {
                const size_t at=size_t(decoded[i])*4;if(at+4>palette.size())throw std::runtime_error("E-mote palette index outside table");
                std::copy_n(palette.data()+at,4,full.data()+i*4);
            }
        } else if(dxt5) DecodeDxt5Blocks(decoded,int(tex_w),int(tex_h),full);
        else if(dxt1) DecodeDxt1Blocks(decoded,int(tex_w),int(tex_h),full);
        else if(dxt3) DecodeDxt3Blocks(decoded,int(tex_w),int(tex_h),full);
        else if(bc7) DecodeBc7Blocks(decoded,int(tex_w),int(tex_h),full);
        else if(a8l8) {
            full.resize(tex_count*4);
            for(size_t i=0;i<tex_count;++i) {
                const uint8_t a=decoded[i*2],l=decoded[i*2+1];
                full[i*4+0]=l;full[i*4+1]=l;full[i*4+2]=l;full[i*4+3]=a;
            }
        } else if(rgba4444 || rgba5551 || rgba5650) {
            full.resize(tex_count*4);
            for(size_t i=0;i<tex_count;++i) {
                const uint16_t v=uint16_t(decoded[i*2]|(uint16_t(decoded[i*2+1])<<8));
                uint8_t* o=full.data()+i*4;
                if(rgba4444) {
                    o[0]=uint8_t(((v>>12)&0xF)*17);o[1]=uint8_t(((v>>8)&0xF)*17);
                    o[2]=uint8_t(((v>>4)&0xF)*17);o[3]=uint8_t((v&0xF)*17);
                } else if(rgba5551) {
                    o[0]=uint8_t(((v>>11)&0x1F)*255/31);o[1]=uint8_t(((v>>6)&0x1F)*255/31);
                    o[2]=uint8_t(((v>>1)&0x1F)*255/31);o[3]=(v&1)?255:0;
                } else { // RGBA5650
                    o[0]=uint8_t(((v>>11)&0x1F)*255/31);o[1]=uint8_t(((v>>5)&0x3F)*255/63);
                    o[2]=uint8_t((v&0x1F)*255/31);o[3]=255;
                }
            }
        } else if(rgbx8) {
            full=std::move(decoded);
            for(size_t i=0;i<full.size();i+=4)full[i+3]=255;
        } else full=std::move(decoded);
        // Desktop PSB color words are A8R8G8B8, i.e. BGRA on little endian
        // (32-bit words and palette entries alike). Compressed atlases already
        // decode to natural RGBA and are not swapped; the 16-bit formats decode
        // naturally as well.
        if((spec=="krkr" || spec=="win") && (rgba8 || rgbx8 || indexed))
            for(size_t i=0;i<full.size();i+=4)std::swap(full[i],full[i+2]);
        if(atlas && (left>0 || top>0 || crop_w!=tex_w || crop_h!=tex_h)) {
            result.rgba.resize(size_t(crop_w)*size_t(crop_h)*4);
            for(int y=0;y<int(crop_h);++y) {
                const size_t src_row=(size_t(top)+size_t(y))*size_t(tex_w)+size_t(left);
                std::copy_n(full.data()+src_row*4,size_t(crop_w)*4,
                            result.rgba.data()+size_t(y)*size_t(crop_w)*4);
            }
        } else result.rgba=std::move(full);
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
