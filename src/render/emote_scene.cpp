#include "render/emote_scene.h"
#include "render/compositor.h"
#include "render/emote_mesh.h"
#include "log/logger.h"
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
// `shape/<kind>` hit-test figures (SDK unit square is 16x16).
EmoteSceneLayer::Shape ParseShape(const std::string& src) {
    if(src.rfind("shape/",0)!=0)return EmoteSceneLayer::Shape::None;
    const std::string kind=src.substr(6);
    if(kind=="rect")return EmoteSceneLayer::Shape::Rect;
    if(kind=="circle")return EmoteSceneLayer::Shape::Circle;
    if(kind=="point")return EmoteSceneLayer::Shape::Point;
    if(kind=="quad")return EmoteSceneLayer::Shape::Quad;
    throw std::runtime_error("unsupported E-mote shape: "+src);
}
std::string Child(const std::string& key,size_t index) {
    char suffix[24];std::snprintf(suffix,sizeof(suffix),".%06zu",index+1);return key+suffix;
}
// `blank` frames describe their extent as "w:h:ox:oy" (or "blank:w:h:ox:oy").
bool ParseBlankDescriptor(const std::string& s,double* ox,double* oy) {
    int w=0,h=0;double x=0,y=0;
    if(s.rfind("blank:",0)==0) {
        if(std::sscanf(s.c_str(),"blank:%d:%d:%lf:%lf",&w,&h,&x,&y)!=4)return false;
    } else {
        if(std::sscanf(s.c_str(),"%d:%d:%lf:%lf",&w,&h,&x,&y)!=4)return false;
    }
    if(ox)*ox=x;if(oy)*oy=y;return true;
}
// Frame colors are 0xRRGGBBAA in MODULATE2X space (0x80 = neutral); fold both
// the player tint and the frame color into the straight compositor factor.
uint32_t FrameTint(uint32_t rrggbbaa) {
    const auto dbl=[](uint32_t c){return std::min(0xFFu,c*2u);};
    return (dbl((rrggbbaa>>24)&0xFFu)<<16)|(dbl((rrggbbaa>>16)&0xFFu)<<8)|dbl((rrggbbaa>>8)&0xFFu);
}
uint32_t MultiplyColor(uint32_t a,uint32_t b) {
    const auto mix=[](uint32_t x,uint32_t y){return (x*y+127u)/255u;};
    return (mix((a>>16)&0xFFu,(b>>16)&0xFFu)<<16)|
           (mix((a>>8)&0xFFu,(b>>8)&0xFFu)<<8)|
           mix(a&0xFFu,b&0xFFu);
}
struct Frame {
    const PsbValue* left=nullptr;
    const PsbValue* right=nullptr;
    double ratio=0,time=0;
    bool visible=false;
};
Frame Sample(const PsbValue& list,double time) {
    Frame result;
    const PsbValue* last_live=nullptr;
    for(const auto& f:list.array) {
        const auto at=Num(f.At("time"));
        if(at<=time){result.left=&f;result.time=at;if(Num(f.At("type"))!=0)last_live=&f;}
        else {result.right=&f;break;}
    }
    if(!result.left || Num(result.left->At("type"))==0) {
        // HOLD frame: the node itself is not drawn, but the last live frame's
        // authored state still drives its transform and its children (the
        // reference host visits children regardless of the live-frame cursor).
        if(last_live)result.left=last_live;
        return result;
    }
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
    const std::set<std::string>& removed;
    int mesh_side=kEmoteMeshSide;
    std::vector<EmoteSceneLayer> layers;
    std::set<std::pair<std::string,std::string>> active;
    static std::string RemovalKey(const std::string& chara,const std::string& motion,const std::string& layer) {
        return chara+'\n'+motion+'\n'+layer;
    }
    double ParameterTime(const PsbValue& parameter,const PsbValue& param_list,double fallback) const {
        if(parameter.type==PsbValue::Null)return fallback;
        const PsbValue* p=nullptr;
        if(parameter.type==PsbValue::Number) {
            const double index=Num(parameter);
            if(index<0 || index!=std::floor(index) || index>=param_list.array.size())
                throw std::runtime_error("invalid E-mote parameter index");
            p=&param_list.array[size_t(index)];
        } else if(parameter.type==PsbValue::Object) {
            p=&parameter;  // inline parameter (motion-level only in practice)
        } else throw std::runtime_error("invalid E-mote parameterize");
        const auto found=variables.find(p->At("id").string);
        double value=found==variables.end()?0:found->second;
        if(!std::isfinite(value))throw std::runtime_error("non-finite E-mote variable");
        const double lo=Num(p->At("rangeBegin")),hi=Num(p->At("rangeEnd")),division=Num(p->At("division"));
        if(Num(p->At("discretization"))!=0)value=std::trunc(value);
        value=std::clamp(value,std::min(lo,hi),std::max(lo,hi));
        if(Num(p->At("enabled"),1)==0)return fallback;
        return hi!=lo?(value-lo)*division/(hi-lo):0;
    }
    static int SubtreeNodes(const PsbValue& layer) {
        int n=1;
        for(const auto& c:layer.At("children").array)n+=SubtreeNodes(c);
        return n;
    }
    // motion.priority frames list the flattened subtree indices in reverse draw
    // order (reference host: rank = position from the end; unknown entries get
    // the following ranks). Reorders this motion's direct children stably.
    void ApplyPriority(const PsbValue& motion,double time,const PsbValue& layers,
                       std::vector<size_t>* order) const {
        const auto& prio=motion.At("priority");
        if(prio.type!=PsbValue::Array || prio.array.empty())return;
        const PsbValue* chosen=nullptr;
        for(const auto& p:prio.array) if(Num(p.At("time"))<=time) chosen=&p;
        if(!chosen)chosen=&prio.array.front();
        const auto& content=chosen->At("content");
        if(content.type!=PsbValue::Array || content.array.empty())return;
        std::vector<int> base(layers.array.size());
        int total=0;
        for(size_t i=0;i<layers.array.size();++i) {
            base[i]=total;
            total+=SubtreeNodes(layers.array[i]);
        }
        std::vector<int> ranks(size_t(total),-1);
        int rank=0;
        for(auto it=content.array.rbegin();it!=content.array.rend();++it,++rank) {
            const int idx=int(it->number);
            if(idx>=0 && idx<total && ranks[size_t(idx)]<0)ranks[size_t(idx)]=rank;
        }
        int next=rank;
        for(auto& r:ranks) if(r<0)r=next++;
        std::stable_sort(order->begin(),order->end(),[&](size_t a,size_t b) {
            return ranks[size_t(base[a])]<ranks[size_t(base[b])];
        });
    }
    void Motion(const std::string& chara,const std::string& name,const std::string& key,double frame,size_t depth) {
        if(depth>64 || !active.emplace(chara,name).second)throw std::runtime_error("recursive E-mote child motion");
        const auto& motion=model.Document().root.At("object").At(chara).At("motion").At(name);
        if(motion.type!=PsbValue::Object)throw std::runtime_error("missing E-mote child motion: "+chara+"/"+name);
        const double end=Num(motion.At("lastTime")),loop=Num(motion.At("loopTime"),-1);
        if(end>0 && frame>=end)frame=loop>=0 && loop<end?loop+std::fmod(frame-loop,end-loop):std::nextafter(end,0.0);
        // Motion-level parameterize selects the whole nested motion's tick from
        // a variable (the shipped models bind mouth/eye variants this way).
        frame=ParameterTime(motion.At("parameterize"),motion.At("parameter"),frame);
        Nodes(motion.At("layer"),motion,key,frame,depth+1,chara,name);
        active.erase({chara,name});
    }
    void Nodes(const PsbValue& nodes,const PsbValue& motion,const std::string& parent,double frame,size_t depth,
               const std::string& chara,const std::string& motion_name) {
        if(depth>64)throw std::runtime_error("E-mote scene nesting too deep");
        std::vector<size_t> order(nodes.array.size());
        for(size_t i=0;i<order.size();++i)order[i]=i;
        ApplyPriority(motion,frame,nodes,&order);
        for(size_t oi=0;oi<order.size();++oi) {
            const size_t i=order[oi];
            const auto& node=nodes.array[i];double time=frame;
            if(removed.count(RemovalKey(chara,motion_name,node.At("label").string)))continue;
            const bool stencil_node=Num(node.At("type"))==12;
            if(stencil_node) {
                static std::set<std::string> reported;
                if(reported.insert("stencil").second)
                    Log(kLogInfo,"emote: stencil/mask composites render unmasked (masks not implemented)");
            }
            const auto& parameter=node.At("parameterize");
            if(parameter.type!=PsbValue::Null)
                time=ParameterTime(parameter,motion.At("parameter"),time);
            const auto sampled=Sample(node.At("frameList"),time);
            EmoteSceneLayer out;out.key=Child(parent,i);
            out.label=node.At("label").string;
            // Containers always keep their transform so children stay placed;
            // only a live frame contributes this node's own drawable content
            // (HOLD frames keep the previous state for transforms only).
            out.visible=true;
            if(sampled.left) {
                out.x=Coordinate(sampled,0);out.y=Coordinate(sampled,1);
                out.angle=Value(sampled,"angle",0);out.scale_x=Value(sampled,"zx",1);out.scale_y=Value(sampled,"zy",1);
                out.opacity=std::clamp(Value(sampled,"opa",255)/255.0,0.0,1.0);
                out.origin_x=Value(sampled,"ox",0);out.origin_y=Value(sampled,"oy",0);
                const auto& content=sampled.left->At("content");const auto& src=content.At("src").string;
                const auto& color_field=content.At("color");
                if(color_field.type==PsbValue::Number) out.color=uint32_t(int64_t(color_field.number));
                else if(color_field.type==PsbValue::Array && !color_field.array.empty() &&
                        color_field.array[0].type==PsbValue::Number)
                    out.color=uint32_t(int64_t(color_field.array[0].number));
                // `bm` low nibble selects the blend mode; the remaining bits
                // are native flags the compositor does not need (reference:
                // emote_blend masks 0x0f only).
                const auto& bm_field=content.At("bm");
                if(bm_field.type==PsbValue::Number) {
                    switch(int64_t(bm_field.number)&0xF) {
                        case 1: out.blend="add";break;
                        case 2: case 5: out.blend="subtract";break;
                        case 3: out.blend="multiply";break;
                        case 4: out.blend="screen";break;
                        default: break;
                    }
                }
                // Per-icon Bezier mesh warp: warp the icon's normalized grid.
                const auto& mesh=content.At("mesh");
                if(!stencil_node && mesh.type==PsbValue::Object) {
                    std::vector<float> bp;
                    for(const auto& x:mesh.At("bp").array)bp.push_back(float(x.number));
                    // Empty bp/cc placeholders mean "no deformation"; only a
                    // non-empty blend-point grid is sampled.
                    if(!bp.empty()) {
                        int side=0;
                        if(!ParseMeshPatch(bp,&side) || !BuildWarpedMesh(bp,side,mesh_side,&out.mesh))
                            throw std::runtime_error("invalid E-mote mesh patch");
                    }
                }
                const double node_type=Num(node.At("type"));
                bool nested_motion=false;
                std::string nested_chara,nested_motion_name;
                if(src.rfind("src/",0)==0) {const auto path=Path(src,"src/");out.source=path.first;out.icon=path.second;}
                else if(src.rfind("motion/",0)==0) { /* legacy child motion; recursion below */ }
                else if(src=="blank" || src.rfind("blank:",0)==0) {
                    // blank frames carry no texture; a blank descriptor (on src
                    // or icon, "w:h:ox:oy") gives the layout extent/origin.
                    const std::string descriptor=src.rfind("blank:",0)==0?src:content.At("icon").string;
                    if(!descriptor.empty()) {
                        double ox=0,oy=0;
                        if(!ParseBlankDescriptor(descriptor,&ox,&oy))
                            throw std::runtime_error("invalid E-mote blank source");
                        out.origin_x=ox;out.origin_y=oy;
                    }
                } else if(src.rfind("shape/",0)==0 || node_type==1) {
                    out.shape=ParseShape(src);
                    if(out.shape==EmoteSceneLayer::Shape::None)out.shape=EmoteSceneLayer::Shape::Rect;
                    out.origin_x=8;out.origin_y=8;  // 16x16 unit figure (SDK)
                } else if(src=="clip" || src=="layout") {
                } else if(node_type==3) {
                    // Split content model (win exports): src names the nested
                    // character and icon its motion. Frames missing either are
                    // layout-only (the reference host skips them).
                    const auto motion_name=content.At("icon").string;
                    if(!src.empty() && !motion_name.empty()) {
                        nested_motion=true;nested_chara=src;nested_motion_name=motion_name;
                    }
                } else {
                    // Split content model: src names the texture source and icon
                    // the entry inside it; a missing pair is layout-only.
                    const auto icon_name=content.At("icon").string;
                    if(!src.empty() && !icon_name.empty()) {out.source=src;out.icon=icon_name;}
                }
                if(stencil_node || !sampled.visible) {
                    // Mask groups and HOLD frames keep the transform for the
                    // children but never draw their own content.
                    out.source.clear();out.icon.clear();out.mesh.clear();
                    out.shape=EmoteSceneLayer::Shape::None;out.blend.clear();
                    nested_motion=false;
                }
                if(layers.size()>=4096)throw std::runtime_error("too many E-mote scene layers");
                layers.push_back(out);
                if(sampled.visible && src.rfind("motion/",0)==0) {
                    const auto path=Path(src,"motion/");
                    Motion(path.first,path.second,out.key+".000000",time-sampled.time+Num(content.At("motion").At("timeOffset")),depth+1);
                } else if(nested_motion) {
                    Motion(nested_chara,nested_motion_name,out.key+".000000",
                           time-sampled.time+Num(content.At("motion").At("timeOffset")),depth+1);
                }
                Nodes(node.At("children"),motion,out.key,frame,depth+1,chara,motion_name);
            } else {
                if(layers.size()>=4096)throw std::runtime_error("too many E-mote scene layers");
                layers.push_back(out);
                Nodes(node.At("children"),motion,out.key,frame,depth+1,chara,motion_name);
            }
        }
    }
};
// Inspect every reachable frame, not merely frame zero: a later mesh keyframe
// must fail loading rather than suddenly corrupting a character during playback.
void Validate(const EmoteModel& model,const std::string& chara,const std::string& motion,
              std::set<std::pair<std::string,std::string>>& active,
              const std::set<std::string>& removed,
              std::map<std::pair<std::string,std::string>,EmoteImage>& images,size_t& count,size_t depth) {
    if(depth>64 || !active.emplace(chara,motion).second)throw std::runtime_error("recursive E-mote motion graph");
    const auto& m=model.Document().root.At("object").At(chara).At("motion").At(motion);
    if(m.type!=PsbValue::Object)throw std::runtime_error("missing E-mote motion");
    Num(m.At("lastTime"));Num(m.At("loopTime"),-1);
    // Motion-level `variable` is an authoring declaration the reference host
    // ignores; motion-level parameterize selects the whole motion's tick.
    const auto& motion_parameterize=m.At("parameterize");
    if(motion_parameterize.type==PsbValue::Number) {
        const double index=Num(motion_parameterize);
        if(index<0 || index!=std::floor(index) || index>=m.At("parameter").array.size())
            throw std::runtime_error("invalid E-mote motion parameter index");
    } else if(motion_parameterize.type==PsbValue::Object) {
        Num(motion_parameterize.At("rangeBegin"));Num(motion_parameterize.At("rangeEnd"));
        Num(motion_parameterize.At("division"));Num(motion_parameterize.At("discretization"));
    } else if(motion_parameterize.type!=PsbValue::Null) {
        throw std::runtime_error("invalid E-mote motion parameterize");
    }
    const auto& motion_priority=m.At("priority");
    if(motion_priority.type==PsbValue::Array) {
        for(const auto& p:motion_priority.array) {
            Num(p.At("time"));
            const auto& content=p.At("content");
            if(content.type!=PsbValue::Null && content.type!=PsbValue::Array)
                throw std::runtime_error("invalid E-mote priority");
            for(const auto& c:content.array)Num(c);
        }
    } else if(motion_priority.type!=PsbValue::Null) {
        throw std::runtime_error("invalid E-mote priority");
    }
    std::function<void(const PsbValue&,size_t)> nodes=[&](const PsbValue& list,size_t level) {
        if(level>64)throw std::runtime_error("E-mote scene nesting too deep");
        for(const auto& n:list.array) {
            if(removed.count(chara+'\n'+motion+'\n'+n.At("label").string))continue;
            if(++count>4096)throw std::runtime_error("too many E-mote nodes");
            const auto type=Num(n.At("type"));
            if(type!=0 && type!=1 && type!=2 && type!=3 && type!=10 && type!=12)
                throw std::runtime_error("unsupported E-mote node type at "+n.At("label").string);
            const bool stencil_node=type==12 || Num(n.At("stencilType"))!=0;
            if(stencil_node) {
                // Mask groups do not fail the model: their children still
                // validate and render (unmasked until compositing exists).
                static std::set<std::string> reported;
                if(reported.insert("stencil").second)
                    Log(kLogInfo,"emote: stencil/mask composites render unmasked (masks not implemented)");
                nodes(n.At("children"),level+1);
                continue;
            }
            if(Num(n.At("coordinate")) || Num(n.At("groundCorrection")))
                throw std::runtime_error("unsupported E-mote coordinate/ground correction");
            const auto& inherit=n.At("inheritMask");
            if(inherit.type!=PsbValue::Null && inherit.type!=PsbValue::Number)
                throw std::runtime_error("invalid E-mote transform inheritance");
            if(inherit.type==PsbValue::Number && (int64_t(inherit.number)&0x1FC)!=0x1FC) {
                // Partial-inherit masks (e.g. bust centering helpers) remove
                // selected transform channels relative to the motion root. The
                // evaluated transform still inherits fully for now.
                static std::set<std::string> reported;
                if(reported.insert("partial-inherit").second)
                    Log(kLogInfo,"emote: partial transform inheritance (inheritMask) treated as full");
            }
            const auto& order=n.At("transformOrder").array;
            if(!order.empty() && (order.size()!=4 || Num(order[0])!=0 || Num(order[1])!=3 || Num(order[2])!=2 || Num(order[3])!=1))
                throw std::runtime_error("unsupported E-mote transform order");
            double previous=-1;
            for(const auto& f:n.At("frameList").array) {
                const double time=Num(f.At("time")),ft=Num(f.At("type"));
                if(time<previous || time<0 || (ft!=0 && ft!=2 && ft!=3))throw std::runtime_error("unsupported E-mote scene keyframe");
                previous=time;if(ft==0)continue;
                const auto& c=f.At("content");
                const std::set<std::string> supported={"mask","src","coord","angle","zx","zy","opa","ox","oy","bm","motion","mesh",
                    "icon","zcc","ccc","cc","color"};
                for(const auto& field:c.object)if(!supported.count(field.first))throw std::runtime_error("unsupported E-mote content: "+field.first);
                for(const char* field:{"mask","angle","zx","zy","opa","ox","oy","bm"})Num(c.At(field));
                // Split content model (win exports): src names a texture source
                // (or blank/a nested character) and icon selects within it; the
                // motion object only contributes mask/timeOffset (the rest is
                // authored nested-motion structure the reference host ignores).
                const auto& motion_field=c.At("motion");
                if(motion_field.type==PsbValue::Object) {
                    if(Num(motion_field.At("mask"))!=0)
                        throw std::runtime_error("unsupported E-mote child motion flags");
                    Num(motion_field.At("timeOffset"));
                } else if(motion_field.type!=PsbValue::Null) {
                    throw std::runtime_error("invalid E-mote child motion");
                }
                const auto& color=c.At("color");
                if(color.type!=PsbValue::Null) {
                    // Packed 0xRRGGBBAA (a scalar or per-corner array); it
                    // legitimately exceeds the generic 1e9 scene-number limit.
                    const auto valid=[&](const PsbValue& x) {
                        return x.type==PsbValue::Number && std::isfinite(x.number) &&
                               x.number>=0 && x.number<=4294967295.0;
                    };
                    if(color.type==PsbValue::Array) {
                        for(const auto& x:color.array) if(!valid(x))
                            throw std::runtime_error("invalid E-mote content color");
                    } else if(!valid(color)) {
                        throw std::runtime_error("invalid E-mote content color");
                    }
                }
                for(const char* field:{"zcc","ccc","cc"}) {
                    const auto& curve=c.At(field);
                    if(curve.type==PsbValue::Null)continue;
                    if(curve.type!=PsbValue::Object)throw std::runtime_error("invalid E-mote content curve");
                    for(const auto& part:curve.object) {
                        for(const auto& x:part.second.array)Num(x);
                        if(part.second.type!=PsbValue::Array)Num(part.second);
                    }
                    static std::set<std::string> reported;
                    if(reported.insert(field).second)
                        Log(kLogInfo,std::string("emote: content curve evaluated as none yet: ")+field);
                }
                for(const auto& coordinate:c.At("coord").array)Num(coordinate);
                Num(c.At("bm"));
                if(c.At("coord").array.size()>2 && Num(c.At("coord").array[2])!=0) {
                    // Depth/z drives the native 3D/stereovision planes; the 2D
                    // compositor ignores it (like setCoord's z argument).
                    static std::set<std::string> reported;
                    if(reported.insert("depth").second)
                        Log(kLogInfo,"emote: depth coordinate ignored by the 2D compositor");
                }
                const auto& src=c.At("src").string;
                // A content mesh patch must be a well-formed square grid.
                const auto& mesh=c.At("mesh");
                if(mesh.type==PsbValue::Object) {
                    const auto& bp=mesh.At("bp");
                    if(bp.type==PsbValue::Array && !bp.array.empty()) {
                        std::vector<float> points;
                        for(const auto& x:bp.array) { if(x.type!=PsbValue::Number) throw std::runtime_error("invalid E-mote mesh point"); points.push_back(float(x.number)); }
                        int side=0;if(!ParseMeshPatch(points,&side))throw std::runtime_error("invalid E-mote mesh patch shape");
                    } else if(mesh.At("cc").type==PsbValue::Array && !mesh.At("cc").array.empty()) {
                        static std::set<std::string> reported;
                        if(reported.insert("cc").second)
                            Log(kLogInfo,"emote: control-coordinate mesh (cc) drawn affine; deformation unsupported");
                    }
                } else if(mesh.type!=PsbValue::Null) throw std::runtime_error("invalid E-mote mesh");
                if(src.rfind("src/",0)==0) {
                    auto key=Path(src,"src/");
                    if(!images.count(key)) {EmoteImage image;std::string error;
                        if(!model.Image(key.first,key.second,image,error))throw std::runtime_error(error);
                        size_t bytes=image.rgba.size();for(const auto& old:images)bytes+=old.second.rgba.size();
                        if(bytes>128*1024*1024)throw std::runtime_error("E-mote decoded images exceed memory limit");
                        images.emplace(std::move(key),std::move(image));}
                } else if(src.rfind("motion/",0)==0) {
                    const auto path=Path(src,"motion/");Validate(model,path.first,path.second,active,removed,images,count,level+1);
                } else if(src=="blank" || src.rfind("blank:",0)==0) {
                    const std::string descriptor=src.rfind("blank:",0)==0?src:c.At("icon").string;
                    if(!descriptor.empty() && !ParseBlankDescriptor(descriptor,nullptr,nullptr))
                        throw std::runtime_error("invalid E-mote blank source");
                } else if(src.rfind("shape/",0)==0 || type==1) {
                    ParseShape(src);  // validates rect/circle/point/quad
                } else if(src=="clip" || src=="layout") {
                } else if(type==3) {
                    const auto motion_name=c.At("icon").string;
                    if(!src.empty() && !motion_name.empty())
                        Validate(model,src,motion_name,active,removed,images,count,level+1);
                } else {
                    // Split content model: src names the texture source, icon the
                    // entry inside it (win exports share one atlas per source).
                    // A missing pair is a layout-only frame (not drawn).
                    const auto icon_name=c.At("icon").string;
                    if(!src.empty() && !icon_name.empty()) {
                        auto key=std::make_pair(src,icon_name);
                        if(!images.count(key)) {EmoteImage image;std::string error;
                            if(!model.Image(key.first,key.second,image,error))throw std::runtime_error(error);
                            size_t bytes=image.rgba.size();for(const auto& old:images)bytes+=old.second.rgba.size();
                            if(bytes>128*1024*1024)throw std::runtime_error("E-mote decoded images exceed memory limit");
                            images.emplace(std::move(key),std::move(image));}
                    }
                }
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
        for(const auto& removal:next.model_->Removals())
            next.removed_.insert(RemovalKey(removal.chara,removal.motion,removal.layer));
        std::set<std::pair<std::string,std::string>> active;size_t count=0;
        Validate(*next.model_,base.At("chara").string,base.At("motion").string,active,next.removed_,
                 next.images_,count,0);
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
                          std::vector<EmoteSceneLayer>& output,std::string& error,
                          int mesh_side) const {
    try {
        if(!model_ || !std::isfinite(frame) || frame<0 || frame>1e9)throw std::runtime_error("invalid E-mote scene time");
        mesh_side=std::clamp(mesh_side,2,32);
        const auto& base=model_->Document().root.At("metadata").At("base");
        Evaluator evaluate{*model_,variables,removed_,mesh_side};
        evaluate.Motion(base.At("chara").string,base.At("motion").string,"",frame,0);
        output=std::move(evaluate.layers);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool EmoteScene::Render(Compositor& c,const std::string& id,double frame,
                        const std::map<std::string,double>& variables,std::string& error,
                        double grayscale,uint32_t multiply,double mesh_division) {
    if(id.empty()){error="missing E-mote layer id";return false;}
    if(!std::isfinite(grayscale) || grayscale<0)grayscale=0;
    const bool gray=grayscale>0;
    const std::string gray_value=gray?std::to_string(std::clamp(grayscale,0.0,1.0)):std::string();
    // setMeshDivisionRatio: coarser grids keep the deformed silhouette but
    // submit fewer vertices (scripts use it as a per-device performance knob).
    const int mesh_side=std::clamp(
        int(std::lround(kEmoteMeshSide*std::clamp(mesh_division,0.05,1.0))),2,kEmoteMeshSide);
    std::vector<EmoteSceneLayer> layers;if(!Evaluate(frame,variables,layers,error,mesh_side))return false;
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
            std::map<std::string,std::string> part_props{
                {"left",std::to_string(-image.origin_x-l.origin_x)},
                {"top",std::to_string(-image.origin_y-l.origin_y)}};
            if(gray)part_props["grayscale"]=gray_value;
            if(!l.blend.empty())part_props["layermode"]=l.blend;
            // Player tint times the frame color (both MODULATE2X space).
            const uint32_t part_multiply=l.color!=0?MultiplyColor(multiply,FrameTint(l.color)):multiply;
            if(part_multiply!=0xFFFFFFu) {
                char tint_buf[16]={};
                std::snprintf(tint_buf,sizeof(tint_buf),"0x%06X",part_multiply&0xFFFFFFu);
                part_props["colormultiply"]=tint_buf;
            }
            c.SetProps(part,part_props);
            if(!l.mesh.empty()) {
                // Scale the normalized warped grid into icon-local pixels; uv
                // stays the identity grid the icon texture is sampled through.
                std::vector<float> verts;verts.reserve(l.mesh.size());
                for(size_t i=0;i+3<l.mesh.size();i+=4) {
                    verts.push_back(l.mesh[i]*image.width);
                    verts.push_back(l.mesh[i+1]*image.height);
                    verts.push_back(l.mesh[i+2]);
                    verts.push_back(l.mesh[i+3]);
                }
                c.SetLayerMesh(part,verts);
            } else c.SetLayerMesh(part,{});
        }
    }
    installed_shapes_.clear();
    for(const auto& l:layers) if(l.shape!=EmoteSceneLayer::Shape::None)
        installed_shapes_.push_back({id+l.key,l.label,l.shape});
    // One-shot placement diagnostic: the first few frames report how many
    // textured parts are effectively visible and where they land on the stage.
    static std::map<std::string,int> render_counts;
    const int rendered=render_counts[id]++;
    if(rendered==1) {
        size_t parts=0;float minx=1e9f,miny=1e9f,maxx=-1e9f,maxy=-1e9f;
        std::string sample;
        for(const auto& l:layers) {
            if(l.source.empty())continue;
            const std::string part=id+l.key+".000000";
            const Layer* cl=nullptr;
            for(const auto& pl:c.Layers()) if(pl.id==part){cl=&pl;break;}
            if(!cl)continue;
            float ex=0,ey=0,ew=0,eh=0,ea=0;bool ev=false;
            c.EffectiveRect(*cl,&ex,&ey,&ew,&eh,&ea,&ev);
            if(!ev||ea<=0)continue;
            ++parts;
            if(sample.empty()) sample=l.label;
            minx=std::min(minx,ex);miny=std::min(miny,ey);
            maxx=std::max(maxx,ex+ew);maxy=std::max(maxy,ey+eh);
        }
        Log(kLogInfo,"emote: render "+id+" frame="+std::to_string(frame)+
            " parts="+std::to_string(parts)+(parts?" bbox=("+std::to_string(int(minx))+","+
            std::to_string(int(miny))+")-("+std::to_string(int(maxx))+","+std::to_string(int(maxy))+") sample="+sample:""));
    }
    installed_layers_=std::move(current);return true;
}

bool EmoteScene::HitTest(Compositor& c,const std::string& id,const std::string& label,
                         double x,double y) const {
    if(id.empty())return false;
    for(const auto& shape:installed_shapes_) {
        if(!label.empty() && shape.label!=label)continue;
        const auto* layer=static_cast<const Layer*>(nullptr);
        for(const auto& l:c.Layers()) if(l.id==shape.id){layer=&l;break;}
        if(!layer || !layer->visible || layer->alpha<=0)continue;
        const auto m=c.EffectiveTransform(*layer);
        const double det=m.a*m.d-m.b*m.c;
        if(std::abs(det)<1e-12)continue;
        const double dx=x-m.tx, dy=y-m.ty;
        const double px=( m.d*dx-m.c*dy)/det;
        const double py=(-m.b*dx+m.a*dy)/det;
        // The SDK's shape figure is a 16x16 unit square; the layer transform
        // already carries the node scale/rotation.
        switch(shape.shape) {
        case EmoteSceneLayer::Shape::Rect:
            if(std::abs(px)<=8 && std::abs(py)<=8)return true;break;
        case EmoteSceneLayer::Shape::Circle:
            if(px*px+py*py<=64)return true;break;
        case EmoteSceneLayer::Shape::Point:
            if(std::abs(px)<=1 && std::abs(py)<=1)return true;break;
        case EmoteSceneLayer::Shape::Quad:
            if(std::abs(px)+std::abs(py)<=8)return true;break;
        default: break;
        }
    }
    return false;
}
void EmoteScene::Remove(Compositor& c,const std::string& id) {
    // DeleteLayer on a dotted child cascades to its own descendants; keys here
    // are already full dotted paths, but the loop stays O(installed).
    for(const auto& key:installed_layers_)c.DeleteLayer(key);
    installed_layers_.clear();installed_.clear();installed_shapes_.clear();
}
}
