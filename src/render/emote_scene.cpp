#include "render/emote_scene.h"
#include "render/compositor.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

namespace artc {
namespace {
double Num(const PsbValue& v,double fallback=0) {
    double value=fallback;
    if(v.type==PsbValue::Number || v.type==PsbValue::Boolean)value=v.number;
    else if(v.type==PsbValue::String) {
        char* end=nullptr;value=std::strtod(v.string.c_str(),&end);
        if(end==v.string.c_str() || end!=v.string.c_str()+v.string.size())throw std::runtime_error("invalid scene number");
    } else if(v.type!=PsbValue::Null)throw std::runtime_error("invalid scene number type");
    if(!std::isfinite(value) || std::abs(value)>1e9)throw std::runtime_error("scene number outside supported range");
    return value;
}
std::pair<std::string,std::string> Path(const std::string& src,const char* prefix) {
    const size_t begin=std::char_traits<char>::length(prefix),slash=src.find('/',begin);
    if(src.rfind(prefix,0)!=0 || slash==std::string::npos || slash==begin || slash+1==src.size())
        throw std::runtime_error("invalid E-mote source: "+src);
    return {src.substr(begin,slash-begin),src.substr(slash+1)};
}
std::string Child(const std::string& key,size_t index) {
    char suffix[24];std::snprintf(suffix,sizeof(suffix),".%06zu",index+1);return key+suffix;
}
struct Frame {
    const PsbValue* left=nullptr;
    const PsbValue* right=nullptr;
    double ratio=0,time=0;
    bool visible=false;
};
Frame Sample(const PsbValue& list,double time) {
    Frame result;
    for(const auto& f:list.array) {
        const auto at=Num(f.At("time"));
        if(at<=time){result.left=&f;result.time=at;}
        else {result.right=&f;break;}
    }
    if(!result.left || Num(result.left->At("type"))==0)return result;
    result.visible=true;
    if(Num(result.left->At("type"))==3 && result.right && Num(result.right->At("type"))!=0) {
        const auto duration=Num(result.right->At("time"))-result.time;
        if(duration>0)result.ratio=std::clamp((time-result.time)/duration,0.0,1.0);
    }
    return result;
}
double Value(const Frame& f,const char* key,double fallback) {
    const auto left=Num(f.left->At("content").At(key),fallback);
    return f.ratio>0?left+(Num(f.right->At("content").At(key),fallback)-left)*f.ratio:left;
}
double Coordinate(const Frame& f,size_t axis) {
    auto read=[axis](const PsbValue& frame) {
        const auto& c=frame.At("content").At("coord").array;return axis<c.size()?Num(c[axis]):0;
    };
    const double left=read(*f.left);return f.ratio>0?left+(read(*f.right)-left)*f.ratio:left;
}
struct Evaluator {
    const EmoteModel& model;
    const std::map<std::string,double>& variables;
    std::vector<EmoteSceneLayer> layers;
    std::set<std::pair<std::string,std::string>> active;
    void Motion(const std::string& chara,const std::string& name,const std::string& key,double frame,size_t depth) {
        if(depth>64 || !active.emplace(chara,name).second)throw std::runtime_error("recursive E-mote child motion");
        const auto& motion=model.Document().root.At("object").At(chara).At("motion").At(name);
        if(motion.type!=PsbValue::Object)throw std::runtime_error("missing E-mote child motion: "+chara+"/"+name);
        const double end=Num(motion.At("lastTime")),loop=Num(motion.At("loopTime"),-1);
        if(end>0 && frame>=end)frame=loop>=0 && loop<end?loop+std::fmod(frame-loop,end-loop):std::nextafter(end,0.0);
        Nodes(motion.At("layer"),motion,key,frame,depth+1);
        active.erase({chara,name});
    }
    void Nodes(const PsbValue& nodes,const PsbValue& motion,const std::string& parent,double frame,size_t depth) {
        if(depth>64)throw std::runtime_error("E-mote scene nesting too deep");
        for(size_t i=0;i<nodes.array.size();++i) {
            const auto& node=nodes.array[i];double time=frame;
            const auto& parameter=node.At("parameterize");
            if(parameter.type!=PsbValue::Null) {
                const double index=Num(parameter);const auto& params=motion.At("parameter").array;
                if(index<0 || index!=std::floor(index) || index>=params.size())throw std::runtime_error("invalid E-mote parameter index");
                const auto& p=params[size_t(index)];const auto found=variables.find(p.At("id").string);
                double value=found==variables.end()?0:found->second;
                if(!std::isfinite(value))throw std::runtime_error("non-finite E-mote variable");
                const double lo=Num(p.At("rangeBegin")),hi=Num(p.At("rangeEnd")),division=Num(p.At("division"));
                if(Num(p.At("discretization"))!=0)value=std::trunc(value);
                value=std::clamp(value,std::min(lo,hi),std::max(lo,hi));
                if(Num(p.At("enabled"),1)!=0)time=hi!=lo?(value-lo)*division/(hi-lo):0;
            }
            const auto sampled=Sample(node.At("frameList"),time);
            EmoteSceneLayer out;out.key=Child(parent,i);out.visible=sampled.visible;
            if(sampled.visible) {
                out.x=Coordinate(sampled,0);out.y=Coordinate(sampled,1);
                out.angle=Value(sampled,"angle",0);out.scale_x=Value(sampled,"zx",1);out.scale_y=Value(sampled,"zy",1);
                out.opacity=std::clamp(Value(sampled,"opa",255)/255.0,0.0,1.0);
                out.origin_x=Value(sampled,"ox",0);out.origin_y=Value(sampled,"oy",0);
                const auto& content=sampled.left->At("content");const auto& src=content.At("src").string;
                if(src.rfind("src/",0)==0) {const auto path=Path(src,"src/");out.source=path.first;out.icon=path.second;}
                if(layers.size()>=4096)throw std::runtime_error("too many E-mote scene layers");
                layers.push_back(out);
                if(src.rfind("motion/",0)==0) {
                    const auto path=Path(src,"motion/");
                    Motion(path.first,path.second,out.key+".000000",time-sampled.time+Num(content.At("motion").At("timeOffset")),depth+1);
                }
                Nodes(node.At("children"),motion,out.key,frame,depth+1);
            } else layers.push_back(out);
        }
    }
};
// Inspect every reachable frame, not merely frame zero: a later mesh keyframe
// must fail loading rather than suddenly corrupting a character during playback.
void Validate(const EmoteModel& model,const std::string& chara,const std::string& motion,
              std::set<std::pair<std::string,std::string>>& active,
              std::map<std::pair<std::string,std::string>,EmoteImage>& images,size_t& count,size_t depth) {
    if(depth>64 || !active.emplace(chara,motion).second)throw std::runtime_error("recursive E-mote motion graph");
    const auto& m=model.Document().root.At("object").At(chara).At("motion").At(motion);
    if(m.type!=PsbValue::Object)throw std::runtime_error("missing E-mote motion");
    Num(m.At("lastTime"));Num(m.At("loopTime"),-1);
    if(!m.At("variable").array.empty() || m.At("parameterize").type!=PsbValue::Null)
        throw std::runtime_error("unsupported E-mote motion variable initializer");
    std::function<void(const PsbValue&,size_t)> nodes=[&](const PsbValue& list,size_t level) {
        if(level>64)throw std::runtime_error("E-mote scene nesting too deep");
        for(const auto& n:list.array) {
            if(++count>4096)throw std::runtime_error("too many E-mote nodes");
            const auto type=Num(n.At("type"));
            if(type!=0 && type!=2 && type!=3)throw std::runtime_error("unsupported E-mote node type at "+n.At("label").string);
            if(Num(n.At("coordinate")) || Num(n.At("groundCorrection")))
                throw std::runtime_error("unsupported E-mote coordinate/ground correction");
            if(Num(n.At("meshTransform")) || Num(n.At("meshCombine")) || Num(n.At("stencilType")))
                throw std::runtime_error("E-mote mesh/stencil node requires the deformation renderer: "+n.At("label").string);
            if(n.At("inheritMask").type!=PsbValue::Null && Num(n.At("inheritMask"))!=33556476)
                throw std::runtime_error("unsupported E-mote transform inheritance");
            const auto& order=n.At("transformOrder").array;
            if(!order.empty() && (order.size()!=4 || Num(order[0])!=0 || Num(order[1])!=3 || Num(order[2])!=2 || Num(order[3])!=1))
                throw std::runtime_error("unsupported E-mote transform order");
            double previous=-1;
            for(const auto& f:n.At("frameList").array) {
                const double time=Num(f.At("time")),ft=Num(f.At("type"));
                if(time<previous || time<0 || (ft!=0 && ft!=2 && ft!=3))throw std::runtime_error("unsupported E-mote scene keyframe");
                previous=time;if(ft==0)continue;
                const auto& c=f.At("content");
                const std::set<std::string> supported={"mask","src","coord","angle","zx","zy","opa","ox","oy","bm","motion"};
                for(const auto& field:c.object)if(!supported.count(field.first))throw std::runtime_error("unsupported E-mote content: "+field.first);
                for(const char* field:{"mask","angle","zx","zy","opa","ox","oy","bm"})Num(c.At(field));
                for(const auto& coordinate:c.At("coord").array)Num(coordinate);
                if(Num(c.At("bm"))!=0 || (c.At("coord").array.size()>2 && Num(c.At("coord").array[2])!=0))
                    throw std::runtime_error("unsupported E-mote blend/depth");
                const auto& src=c.At("src").string;
                if(src.rfind("src/",0)==0) {
                    auto key=Path(src,"src/");
                    if(!images.count(key)) {EmoteImage image;std::string error;
                        if(!model.Image(key.first,key.second,image,error))throw std::runtime_error(error);
                        size_t bytes=image.rgba.size();for(const auto& old:images)bytes+=old.second.rgba.size();
                        if(bytes>128*1024*1024)throw std::runtime_error("E-mote decoded images exceed memory limit");
                        images.emplace(std::move(key),std::move(image));}
                } else if(src.rfind("motion/",0)==0) {
                    if(Num(c.At("motion").At("mask"))!=0)throw std::runtime_error("unsupported E-mote child motion flags");
                    for(const auto& field:c.At("motion").object)
                        if(field.first!="mask" && field.first!="timeOffset")throw std::runtime_error("unsupported E-mote child motion control");
                    Num(c.At("motion").At("timeOffset"));
                    const auto path=Path(src,"motion/");Validate(model,path.first,path.second,active,images,count,level+1);
                } else if(src!="layout")throw std::runtime_error("unsupported E-mote source: "+src);
            }
            nodes(n.At("children"),level+1);
        }
    };
    nodes(m.At("layer"),depth+1);active.erase({chara,motion});
}
}
bool EmoteScene::Load(std::shared_ptr<const EmoteModel> model,std::string& error) {
    try {
        if(!model)throw std::runtime_error("missing E-mote model");
        EmoteScene next;next.model_=std::move(model);const auto& base=next.model_->Document().root.At("metadata").At("base");
        std::set<std::pair<std::string,std::string>> active;size_t count=0;
        Validate(*next.model_,base.At("chara").string,base.At("motion").string,active,next.images_,count,0);
        std::vector<EmoteSceneLayer> layers;
        if(!next.Evaluate(0,{},layers,error))return false;
        next.installed_layers_=std::move(installed_layers_);
        // Existing textures belong to the previous model even when icon names
        // match; discard them at the next Render before any new nodes are drawn.
        next.installed_=std::move(installed_);
        for(auto& image:next.installed_)image.second={};
        *this=std::move(next);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool EmoteScene::Evaluate(double frame,const std::map<std::string,double>& variables,
                          std::vector<EmoteSceneLayer>& output,std::string& error) const {
    try {
        if(!model_ || !std::isfinite(frame) || frame<0 || frame>1e9)throw std::runtime_error("invalid E-mote scene time");
        const auto& base=model_->Document().root.At("metadata").At("base");Evaluator evaluate{*model_,variables};
        evaluate.Motion(base.At("chara").string,base.At("motion").string,"",frame,0);
        output=std::move(evaluate.layers);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool EmoteScene::Render(Compositor& c,const std::string& id,double frame,
                        const std::map<std::string,double>& variables,std::string& error) {
    if(id.empty()){error="missing E-mote layer id";return false;}
    std::vector<EmoteSceneLayer> layers;if(!Evaluate(frame,variables,layers,error))return false;
    std::set<std::string> current;
    std::set<std::string> pictures;
    for(const auto& l:layers) {
        const auto key=id+l.key;current.insert(key);
        if(!l.source.empty()){current.insert(key+".000000");pictures.insert(key+".000000");}
    }
    // Remove obsolete ancestors before installing new descendants. A frame may
    // switch from a picture to a child motion using the same source position.
    for(const auto& old:installed_layers_)if(!current.count(old)){c.DeleteLayer(old);installed_.erase(old);}
    for(auto old=installed_.begin();old!=installed_.end();) {
        if(!pictures.count(old->first)){c.DeleteLayer(old->first);old=installed_.erase(old);}else ++old;
    }
    for(const auto& l:layers) {
        const auto key=id+l.key;current.insert(key);
        c.SetProps(key,{{"left",std::to_string(l.x)},{"top",std::to_string(l.y)},
            {"rotate",std::to_string(l.angle)},{"xscale",std::to_string(l.scale_x*100)},
            {"yscale",std::to_string(l.scale_y*100)},{"alpha",std::to_string(l.opacity*255)},
            {"visible",l.visible?"1":"0"}});
        if(!l.source.empty()) {
            const auto image_key=std::make_pair(l.source,l.icon);const auto& image=images_.at(image_key);
            const auto part=key+".000000";current.insert(part);
            if(installed_[part]!=image_key || !c.GetLayerInfo(part).found) {
                if(!c.SetPixels(part,image.rgba.data(),image.width,image.height)){error="E-mote texture upload failed";return false;}
                installed_[part]=image_key;
            }
            c.SetProps(part,{{"left",std::to_string(-image.origin_x-l.origin_x)},
                {"top",std::to_string(-image.origin_y-l.origin_y)}});
        }
    }
    installed_layers_=std::move(current);return true;
}
void EmoteScene::Remove(Compositor& c,const std::string& id) {
    // DeleteLayer on a dotted child cascades to its own descendants; keys here
    // are already full dotted paths, but the loop stays O(installed).
    for(const auto& key:installed_layers_)c.DeleteLayer(key);
    installed_layers_.clear();installed_.clear();
}
}
