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
// Full blank descriptor including the extent size (E-mote mesh domains).
bool ParseBlankExtent(const std::string& s,double* w,double* h,double* ox,double* oy) {
    int wi=0,hi=0;double x=0,y=0;
    if(s.rfind("blank:",0)==0) {
        if(std::sscanf(s.c_str(),"blank:%d:%d:%lf:%lf",&wi,&hi,&x,&y)!=4)return false;
    } else {
        if(std::sscanf(s.c_str(),"%d:%d:%lf:%lf",&wi,&hi,&x,&y)!=4)return false;
    }
    if(w)*w=wi;if(h)*h=hi;if(ox)*ox=x;if(oy)*oy=y;return true;
}
// 2D affine mirroring the compositor's transform composition
// (T(x,y) T(anchor) R(angle) S(scale) about the anchor; E-mote nodes anchor at
// their own origin, so the plain T R S form is used here).
struct Affine2 {
    double m11=1,m12=0,m21=0,m22=1,tx=0,ty=0;
    static Affine2 TRS(double x,double y,double angle,double sx,double sy) {
        const double r=angle*3.14159265358979323846/180.0,c=std::cos(r),s=std::sin(r);
        Affine2 a;
        a.m11=c*sx;a.m12=-s*sy;a.m21=s*sx;a.m22=c*sy;a.tx=x;a.ty=y;
        return a;
    }
    std::pair<double,double> Apply(double x,double y) const {
        return {m11*x+m12*y+tx,m21*x+m22*y+ty};
    }
    std::pair<double,double> InverseApply(double x,double y) const {
        const double dx=x-tx,dy=y-ty,det=m11*m22-m12*m21;
        if(!std::isfinite(det)||std::abs(det)<=1e-12)return {dx,dy};
        const double inv=1.0/det;
        return {(m22*dx-m12*dy)*inv,(-m21*dx+m11*dy)*inv};
    }
    Affine2 Then(const Affine2& rhs) const {
        Affine2 o;
        o.m11=m11*rhs.m11+m12*rhs.m21;o.m12=m11*rhs.m12+m12*rhs.m22;
        o.m21=m21*rhs.m11+m22*rhs.m21;o.m22=m21*rhs.m12+m22*rhs.m22;
        o.tx=m11*rhs.tx+m12*rhs.ty+tx;o.ty=m21*rhs.tx+m22*rhs.ty+ty;
        return o;
    }
    Affine2 Inverse() const {
        Affine2 o;
        const double det=m11*m22-m12*m21;
        if(!std::isfinite(det)||std::abs(det)<=1e-12)return o;
        const double inv=1.0/det;
        o.m11=m22*inv;o.m12=-m12*inv;o.m21=-m21*inv;o.m22=m11*inv;
        o.tx=-(o.m11*tx+o.m12*ty);o.ty=-(o.m21*tx+o.m22*ty);
        return o;
    }
};
double LerpD(double a,double b,double t){return a+(b-a)*t;}
// Bezier (4x4) / bilinear (NxN) sampling of an E-mote normalized control grid.
std::pair<double,double> SampleGrid(const std::vector<double>& points,int side,double nx,double ny) {
    if(side==4) {
        const auto basis=[](double v){
            v=std::clamp(v,0.0,1.0);const double u=1.0-v;
            return std::array<double,4>{u*u*u,3*u*u*v,3*u*v*v,v*v*v};
        };
        const auto bx=basis(nx),by=basis(ny);
        double rx=0,ry=0;
        for(int y=0;y<4;++y)for(int x=0;x<4;++x){
            const double wgt=bx[size_t(x)]*by[size_t(y)];
            const size_t index=(size_t(y)*4+x)*2;
            rx+=points[index]*wgt;ry+=points[index+1]*wgt;
        }
        return {rx,ry};
    }
    auto axis=[](double v,int n){const double scaled=std::clamp(v,0.0,1.0)*(n-1);const int cell=std::min(int(scaled),n-2);return std::pair<int,double>{cell,scaled-cell};};
    const auto ax=axis(nx,side),ay=axis(ny,side);
    const auto pt=[&](int x,int y){const size_t i=(size_t(y)*side+x)*2;return std::pair<double,double>{points[i],points[i+1]};};
    const auto top=pt(ax.first,ay.first),bottom=pt(ax.first,ay.first+1);
    const auto top2=pt(ax.first+1,ay.first),bottom2=pt(ax.first+1,ay.first+1);
    return {LerpD(LerpD(top.first,top2.first,ax.second),LerpD(bottom.first,bottom2.first,ax.second),ay.second),
            LerpD(LerpD(top.second,top2.second,ax.second),LerpD(bottom.second,bottom2.second,ax.second),ay.second)};
}
// E-mote per-layer linear state (flip/rotate/scale/shear) with the official
// partial inheritMask semantics: a child only takes the parent channels its
// mask selects (reference host: inherit_linear_state / remove_motion_root).
struct LinearState {
    bool fx=false,fy=false;
    double rot=0,sx=1,sy=1,shx=0,shy=0;
};
LinearState InheritLinear(const LinearState& own,const LinearState& parent,int64_t mask) {
    LinearState out=own;
    out.fx=own.fx ^ ((mask&0x4)!=0 && parent.fx);
    out.fy=own.fy ^ ((mask&0x8)!=0 && parent.fy);
    if(mask&0x10)out.rot+=parent.rot;
    if(mask&0x20)out.sx*=parent.sx;
    if(mask&0x40)out.sy*=parent.sy;
    if(mask&0x80)out.shx+=parent.shx;
    if(mask&0x100)out.shy+=parent.shy;
    return out;
}
LinearState RemoveRootLinear(LinearState st,const LinearState& root,int64_t mask) {
    if(mask&0x4)st.fx=st.fx!=root.fx;
    if(mask&0x8)st.fy=st.fy!=root.fy;
    if(mask&0x10)st.rot-=root.rot;
    if(mask&0x20)if(std::abs(root.sx)>1e-12)st.sx/=root.sx;
    if(mask&0x40)if(std::abs(root.sy)>1e-12)st.sy/=root.sy;
    if(mask&0x80)st.shx-=root.shx;
    if(mask&0x100)st.shy-=root.shy;
    return st;
}
Affine2 BuildLinear(const std::vector<double>& order,const LinearState& st) {
    // Native files overwhelmingly omit transformOrder: flip, shear, scale,
    // rotate is the default stage order.
    std::array<int,4> stages{0,3,2,1};
    if(order.size()==4) {
        bool seen[4]={false,false,false,false};bool ok=true;
        for(size_t i=0;i<4;++i) {
            const int value=int(std::lround(order[i]));
            if(value<0||value>3||seen[value]){ok=false;break;}
            seen[value]=true;stages[i]=value;
        }
        if(!ok)stages={0,3,2,1};
    }
    Affine2 linear;
    for(const int stage:stages) {
        Affine2 op;
        switch(stage) {
            case 0: op.m11=st.fx?-1:1;op.m22=st.fy?-1:1;break;
            case 1: {const double r=st.rot*3.14159265358979323846/180.0;
                     const double c=std::cos(r),s=std::sin(r);
                     op.m11=c;op.m12=-s;op.m21=s;op.m22=c;break;}
            case 2: op.m11=st.sx;op.m22=st.sy;break;
            case 3: op.m12=st.shx;op.m21=st.shy;break;
        }
        linear=linear.Then(op);
    }
    return linear;
}
// Absolute per-node transform context. Children inherit the parent layer's
// affine unless the parent carries the transparent marker (0x400000); the
// linear itself composes per inheritMask, and partial masks rebuild relative
// to the enclosing motion root.
struct NodeSource {
    Affine2 linear;          // source linear (no translation)
    double lx=0,ly=0;        // source location
    double opacity=1;
    LinearState state;       // source state (inheritance input)
    int64_t coordinate=0;
};
struct TransformCtx {
    NodeSource source;       // inheritance source for the current layer
    Affine2 root_linear;     // enclosing motion root (for partial masks)
    double root_lx=0,root_ly=0,root_opacity=1;
    LinearState root_state;
    bool independent=false;
};
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
    // Nested-motion clocks subtract keyframe times/offsets and accumulate
    // float error; a frame at t must not be missed by a hair (device bug:
    // a HOLD at t=61 was never reached at local 60.999, so the back hair
    // stayed drawn forever). 1 ms at 60 fps is far below one frame.
    const double frame_epsilon=1e-3;
    const PsbValue* last_live=nullptr;
    for(const auto& f:list.array) {
        const auto at=Num(f.At("time"));
        if(at<=time+frame_epsilon){result.left=&f;result.time=at;if(Num(f.At("type"))!=0)last_live=&f;}
        else {result.right=&f;break;}
    }
    if(!result.left || Num(result.left->At("type"))==0) {
        // Trailing HOLD frame: the official shell keeps drawing the last live
        // frame's authored pose after the motion ends (device evidence: a 立绘
        // stays on screen minutes past lastTime), so hold its content instead
        // of hiding the layer. The held state also drives descendants (the
        // reference host visits children regardless of the live-frame cursor).
        if(last_live) {
            result.left=last_live;
            result.visible=true;
        } else {
            result.visible=false;
        }
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
    // Active E-mote stencil composite mask layer labels. A node with
    // stencilType bit 0x4 replaces the inherited list; descendants inherit it
    // (reference host: stencilCompositeMaskLayerList propagation).
    std::vector<std::string> masks;
    // Shape-sync deformation scopes (meshTransform + shape bit + control grid)
    // and the accumulated affine of the current node in the model space.
    struct DeformScope {
        Affine2 acc;                 // scope node's accumulated transform
        std::vector<double> points;  // normalized control grid
        int side=0;
        double width=0,height=0,origin_x=0,origin_y=0;
        double off_x=0,off_y=0;      // content ox/oy
        std::string source,icon;     // icon extent fallback (non-blank nodes)
    };
    std::vector<DeformScope> scopes;
    bool under_hair=false;    // an ancestor label marks a hair layer
    Affine2 acc;              // current node's absolute affine (deformers)
    TransformCtx ctx;         // inheritance source / motion root
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
            const auto parent_acc=acc;
            const auto parent_scopes=scopes;
            const auto parent_ctx=ctx;
            const bool parent_hair=under_hair;
            if(removed.count(RemovalKey(chara,motion_name,node.At("label").string)))continue;
            const bool stencil_node=Num(node.At("type"))==12;
            const auto parent_masks=masks;
            if((int64_t(Num(node.At("stencilType")))&0x4)!=0) {
                const auto& list=node.At("stencilCompositeMaskLayerList");
                if(list.type==PsbValue::Array && !list.array.empty()) {
                    masks.clear();
                    for(const auto& m:list.array)
                        if(m.type==PsbValue::String)masks.push_back(m.string);
                }
            }
            const auto& parameter=node.At("parameterize");
            if(parameter.type!=PsbValue::Null)
                time=ParameterTime(parameter,motion.At("parameter"),time);
            const auto sampled=Sample(node.At("frameList"),time);
            EmoteSceneLayer out;out.key=Child(parent,i);
            out.label=node.At("label").string;
            out.masks=masks;
            if(out.label.find("髪")!=std::string::npos)under_hair=true;
            const bool hair_layer=under_hair;
            if(out.label.find("後髪")!=std::string::npos ||
               out.label.find("後ろ髪")!=std::string::npos)
                out.paint_hint=-1;
            else if(hair_layer && out.label.find("追加パーツ")!=std::string::npos)
                out.paint_hint=-2;
            else if(out.label.rfind("■首",0)==0)
                out.paint_hint=-3;
            // Containers always keep their transform so children stay placed;
            // only a live frame contributes this node's own drawable content
            // (HOLD frames keep the previous state for transforms only).
            out.visible=true;
            if(sampled.left) {
                out.x=Coordinate(sampled,0);out.y=Coordinate(sampled,1);
                out.angle=Value(sampled,"angle",0);out.scale_x=Value(sampled,"zx",1);out.scale_y=Value(sampled,"zy",1);
                out.opacity=std::clamp(Value(sampled,"opa",255)/255.0,0.0,1.0);
                out.origin_x=Value(sampled,"ox",0);out.origin_y=Value(sampled,"oy",0);
                // Native placement: the parent layer's affine is the child's
                // inheritance source (prepare_child_inherit_source), the linear
                // composes per inheritMask, and partial masks rebuild relative
                // to the enclosing motion root.
                LinearState own;
                own.fx=Value(sampled,"fx",0)!=0;own.fy=Value(sampled,"fy",0)!=0;
                own.rot=Value(sampled,"angle",0);
                own.sx=Value(sampled,"zx",1);own.sy=Value(sampled,"zy",1);
                own.shx=Value(sampled,"sx",0);own.shy=Value(sampled,"sy",0);
                const int64_t inherit_mask=node.At("inheritMask").type==PsbValue::Number
                    ?int64_t(node.At("inheritMask").number):int64_t(0x1fc);
                out.transform_order.clear();
                for(const auto& v:node.At("transformOrder").array)out.transform_order.push_back(v.number);
                const LinearState inherited=InheritLinear(own,ctx.source.state,inherit_mask);
                const Affine2 own_linear=BuildLinear(out.transform_order,own);
                Affine2 linear;
                if((inherit_mask&0x1fc)==0x1fc)
                    linear=ctx.source.linear.Then(own_linear);
                else if(!ctx.independent)
                    linear=ctx.root_linear.Then(BuildLinear(out.transform_order,
                        RemoveRootLinear(inherited,ctx.root_state,inherit_mask)));
                else
                    linear=BuildLinear(out.transform_order,inherited);
                const auto anchor=ctx.source.linear.Apply(Coordinate(sampled,0),Coordinate(sampled,1));
                out.abs_m11=linear.m11;out.abs_m12=linear.m12;
                out.abs_m21=linear.m21;out.abs_m22=linear.m22;
                out.abs_tx=ctx.source.lx+anchor.first;out.abs_ty=ctx.source.ly+anchor.second;
                out.st_fx=inherited.fx;out.st_fy=inherited.fy;
                out.st_rot=inherited.rot;out.st_sx=inherited.sx;out.st_sy=inherited.sy;
                out.st_shx=inherited.shx;out.st_shy=inherited.shy;
                out.abs_opa=((inherit_mask&0x400)!=0?ctx.source.opacity
                             :(!ctx.independent?ctx.root_opacity:1.0))*
                            std::clamp(Value(sampled,"opa",255)/255.0,0.0,1.0);
                acc=Affine2{linear.m11,linear.m12,linear.m21,linear.m22,out.abs_tx,out.abs_ty};
                // prepare_child_inherit_source: descendants inherit this layer's
                // affine unless it is marked transparent (0x400000).
                if((inherit_mask&0x400000)==0) {
                    ctx.source.linear=Affine2{linear.m11,linear.m12,linear.m21,linear.m22,0,0};
                    ctx.source.lx=out.abs_tx;ctx.source.ly=out.abs_ty;
                    ctx.source.opacity=out.abs_opa;
                    ctx.source.state=inherited;
                }
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
                // Shape-sync (meshSyncChild): a node with meshTransform and the
                // shape bit propagates its control-grid deformation to
                // descendants. The deformers use the node's absolute affine,
                // so every layer keeps its own authored domain.
                const auto& inherit_field=node.At("inheritMask");
                const bool inherit_shape=inherit_field.type!=PsbValue::Number ||
                    (int64_t(inherit_field.number)&0x02000000)!=0;
                if(!inherit_shape)scopes.clear();
                if(!stencil_node && Num(node.At("meshTransform"))==1 &&
                   (int64_t(Num(node.At("meshSyncChildMask")))&0x8)!=0) {
                    const auto& mesh=content.At("mesh");
                    std::vector<double> points;
                    for(const auto& x:mesh.At("bp").array)points.push_back(x.number);
                    const size_t point_count=points.size()/2;
                    const int side=point_count>0?int(std::lround(std::sqrt(double(point_count)))):0;
                    if(points.size()%2==0 && side>=2 && size_t(side)*size_t(side)==point_count) {
                        double w=0,h=0,ox=0,oy=0;bool blank_extent=false;
                        if(src=="blank" || src.rfind("blank:",0)==0) {
                            const std::string descriptor=src.rfind("blank:",0)==0?src:content.At("icon").string;
                            blank_extent=!descriptor.empty()&&ParseBlankExtent(descriptor,&w,&h,&ox,&oy);
                        }
                        DeformScope scope;
                        scope.acc=acc;scope.points=std::move(points);scope.side=side;
                        if(blank_extent) {
                            scope.width=w;scope.height=h;scope.origin_x=ox;scope.origin_y=oy;
                        } else if(!out.source.empty()) {
                            // Non-blank sync nodes (eyelids etc.) use their own
                            // icon extent, resolved at render time.
                            scope.source=out.source;scope.icon=out.icon;
                        } else {
                            continue;
                        }
                        scope.off_x=out.origin_x;scope.off_y=out.origin_y;
                        if(getenv("ARTC_EMOTE_DUMP")) {
                            static std::set<std::string> rep;
                            if(rep.insert(node.At("label").string).second)
                                Log(kLogInfo,"emote-scope pushed at '"+node.At("label").string+"' side="+std::to_string(side)+
                                    (blank_extent?" blank "+std::to_string(int(w))+"x"+std::to_string(int(h)):" icon "+out.source+"/"+out.icon));
                        }
                        scopes.push_back(std::move(scope));
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
                if(!out.source.empty() && !scopes.empty()) {
                    // Nearest scope first: the reference applies the innermost
                    // patch before walking outward through the ancestor chain.
                    for(auto scope=scopes.rbegin();scope!=scopes.rend();++scope) {
                        const Affine2 chain=scope->acc.Inverse().Then(acc);
                        EmoteSceneLayer::Deformer d;
                        d.m11=chain.m11;d.m12=chain.m12;d.m21=chain.m21;
                        d.m22=chain.m22;d.tx=chain.tx;d.ty=chain.ty;
                        d.points=scope->points;d.side=scope->side;
                        d.width=scope->width;d.height=scope->height;
                        d.origin_x=scope->origin_x;d.origin_y=scope->origin_y;
                        d.off_x=scope->off_x;d.off_y=scope->off_y;
                        d.source=scope->source;d.icon=scope->icon;
                        out.deformers.push_back(std::move(d));
                    }
                    if(getenv("ARTC_EMOTE_DUMP") && !out.deformers.empty()) {
                        static std::set<std::string> rep;
                        if(rep.insert(out.label).second)
                            Log(kLogInfo,"emote-def attached to '"+out.label+"' count="+std::to_string(out.deformers.size()));
                    }
                }
                if(layers.size()>=4096)throw std::runtime_error("too many E-mote scene layers");
                layers.push_back(out);
                if((sampled.visible && src.rfind("motion/",0)==0) || nested_motion) {
                    // A nested motion is a new local player: its inheritance
                    // source and motion root become the entering node's context.
                    const auto saved_ctx=ctx;
                    TransformCtx child;
                    child.source.linear=Affine2{out.abs_m11,out.abs_m12,out.abs_m21,out.abs_m22,0,0};
                    child.source.lx=out.abs_tx;child.source.ly=out.abs_ty;
                    child.source.opacity=out.abs_opa;
                    child.source.state=LinearState{out.st_fx,out.st_fy,out.st_rot,
                                                   out.st_sx,out.st_sy,out.st_shx,out.st_shy};
                    child.root_linear=child.source.linear;
                    child.root_lx=child.source.lx;child.root_ly=child.source.ly;
                    child.root_opacity=child.source.opacity;
                    child.root_state=child.source.state;
                    ctx=child;
                    if(sampled.visible && src.rfind("motion/",0)==0) {
                        const auto path=Path(src,"motion/");
                        Motion(path.first,path.second,out.key+".000000",time-sampled.time+Num(content.At("motion").At("timeOffset")),depth+1);
                    } else {
                        Motion(nested_chara,nested_motion_name,out.key+".000000",
                               time-sampled.time+Num(content.At("motion").At("timeOffset")),depth+1);
                    }
                    ctx=saved_ctx;
                }
                Nodes(node.At("children"),motion,out.key,frame,depth+1,chara,motion_name);
            } else {
                if(layers.size()>=4096)throw std::runtime_error("too many E-mote scene layers");
                layers.push_back(out);
                Nodes(node.At("children"),motion,out.key,frame,depth+1,chara,motion_name);
            }
            masks=parent_masks;
            acc=parent_acc;
            scopes=parent_scopes;
            ctx=parent_ctx;
            under_hair=parent_hair;
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
        // Layout nodes only carry alpha/visibility: visual layers below carry
        // absolute transforms (flat native placement, no chain accumulation).
        c.SetProps(key,{{"left","0"},{"top","0"},{"rotate","0"},
            {"xscale","100"},{"yscale","100"},
            {"alpha","255"},
            {"visible",l.visible?"1":"0"}});
        if(l.shape!=EmoteSceneLayer::Shape::None) {
            const double a=l.abs_m11,b=l.abs_m12,c2=l.abs_m21,d=l.abs_m22;
            const double sx=std::hypot(a,c2);
            const double rot=std::atan2(c2,a)*180.0/3.14159265358979323846;
            const double sy=sx>1e-9?(a*d-b*c2)/sx:1.0;
            c.SetProps(key,{{"left",std::to_string(l.abs_tx)},{"top",std::to_string(l.abs_ty)},
                {"rotate",std::to_string(rot)},{"xscale",std::to_string(sx*100)},
                {"yscale",std::to_string(sy*100)},{"alpha",std::to_string(l.abs_opa*255)}});
        }
        if(!l.source.empty()) {
            const auto image_key=std::make_pair(l.source,l.icon);const auto& image=images_.at(image_key);
            const auto part=key+".000000";current.insert(part);
            if(installed_[part]!=image_key || !c.GetLayerInfo(part).found) {
                if(!c.SetPixels(part,image.rgba.data(),image.width,image.height)){error="E-mote texture upload failed";return false;}
                installed_[part]=image_key;
            }
            std::map<std::string,std::string> part_props;
            {
                // The icon lives at offset (-icon.origin, -content.origin) in
                // the node's local space; the node's absolute affine places it.
                const double off_x=-image.origin_x-l.origin_x;
                const double off_y=-image.origin_y-l.origin_y;
                const double a=l.abs_m11,b=l.abs_m12,c2=l.abs_m21,d=l.abs_m22;
                const double sx=std::hypot(a,c2);
                const double rot=std::atan2(c2,a)*180.0/3.14159265358979323846;
                const double sy=sx>1e-9?(a*d-b*c2)/sx:1.0;
                part_props["left"]=std::to_string(a*off_x+b*off_y+l.abs_tx);
                part_props["top"]=std::to_string(c2*off_x+d*off_y+l.abs_ty);
                part_props["rotate"]=std::to_string(rot);
                part_props["xscale"]=std::to_string(sx*100);
                part_props["yscale"]=std::to_string(sy*100);
                part_props["alpha"]=std::to_string(l.abs_opa*255);
            }
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
            // Rear layers (back hair) must draw behind the portrait: the
            // official shell keeps them behind while our depth-first id order
            // would paint them last. Provisional, authored-label based rule;
            // revisit once the official sort rule is fully recovered.
            c.SetLayerPaint(part,getenv("ARTC_EMOTE_NO_HINTS")?0:l.paint_hint);
            if(getenv("ARTC_EMOTE_DUMP")) {
                float ex=0,ey=0,ew=0,eh=0,ea=0;bool ev=false;
                for(const auto& pl:c.Layers()) if(pl.id==part){c.EffectiveRect(pl,&ex,&ey,&ew,&eh,&ea,&ev);break;}
                Log(kLogInfo,"emote-dump part="+part+" label='"+l.label+"' src='"+l.source+"' icon='"+l.icon+
                    "' rect="+std::to_string(int(ex))+","+std::to_string(int(ey))+" "+
                    std::to_string(int(ew))+"x"+std::to_string(int(eh))+
                    " def="+std::to_string(l.deformers.size())+
                    " masks="+std::to_string(l.masks.size())+
                    (l.masks.empty()?"":" ["+[&]{std::string j;for(const auto& m:l.masks)j+=m+"|";return j;}()+"]"));
            }
            if(!l.deformers.empty()) {
                // Shape-sync deformation: sample an 8x8 grid over the icon rect,
                // warp it through the ancestor control patches (nearest first),
                // then emit the triangle list in icon-local pixels.
                const int SIDE=8;
                const double off_x=double(image.origin_x)+l.origin_x;
                const double off_y=double(image.origin_y)+l.origin_y;
                auto warped=[&](double u,double v,double* rx,double* ry) {
                    double px=u*double(image.width)-off_x,py=v*double(image.height)-off_y;
                    for(const auto& d:l.deformers) {
                        double dw=d.width,dh=d.height,dox=d.origin_x,doy=d.origin_y;
                        if(!(dw>0)||!(dh>0)) {
                            const auto image=images_.find({d.source,d.icon});
                            if(image==images_.end())continue;
                            dw=image->second.width;dh=image->second.height;
                            dox=image->second.origin_x;doy=image->second.origin_y;
                        }
                        if(d.side<2)continue;
                        const double dx=px-d.tx,dy=py-d.ty;
                        const double det=d.m11*d.m22-d.m12*d.m21;
                        if(!std::isfinite(det)||std::abs(det)<=1e-12)continue;
                        const double inv=1.0/det;
                        const double lx=(d.m22*dx-d.m12*dy)*inv,ly=(-d.m21*dx+d.m11*dy)*inv;
                        const double nx=(lx+dox+d.off_x)/dw,ny=(ly+doy+d.off_y)/dh;
                        if(!(nx>=0&&nx<=1&&ny>=0&&ny<=1))continue;
                        const auto gp=SampleGrid(d.points,d.side,nx,ny);
                        const double wx=gp.first*dw-dox-d.off_x;
                        const double wy=gp.second*dh-doy-d.off_y;
                        px=d.m11*wx+d.m12*wy+d.tx;py=d.m21*wx+d.m22*wy+d.ty;
                    }
                    *rx=px+off_x;*ry=py+off_y;
                };
                std::vector<float> verts;
                verts.reserve(size_t(SIDE-1)*(SIDE-1)*24);
                auto push=[&](int gx,int gy){
                    const double u=double(gx)/double(SIDE-1),v=double(gy)/double(SIDE-1);
                    double x=0,y=0;warped(u,v,&x,&y);
                    verts.push_back(float(x));verts.push_back(float(y));
                    verts.push_back(float(u));verts.push_back(float(v));
                };
                for(int gy=0;gy<SIDE-1;++gy)
                    for(int gx=0;gx<SIDE-1;++gx) {
                        push(gx,gy);push(gx+1,gy);push(gx,gy+1);
                        push(gx+1,gy);push(gx+1,gy+1);push(gx,gy+1);
                    }
                c.SetLayerMesh(part,verts);
            } else if(!l.mesh.empty()) {
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
            // Parts without a mask drop any mask attached in a previous frame.
            if(l.masks.empty())c.SetLayerStageMask(part,0);
        }
    }
    // E-mote stencil composite: resolve each masked part's labels to the
    // closest mask source part and attach the shared stage mask. All layer
    // centers are computed once per frame — EffectiveRect walks the whole
    // ancestor chain, so a per-candidate call made this quadratic (device:
    // two 立绘 with masks dropped the frame loop to ~3 fps).
    {
        std::map<std::string,std::pair<float,float>> centers;
        std::map<std::string,std::vector<std::string>> sources_by_label;
        std::map<std::string,std::string> container_keys;
        for(const auto& pl:c.Layers()) {
            if(pl.id.size()<7 || pl.id.compare(pl.id.size()-7,7,".000000")!=0)continue;
            float ex=0,ey=0,ew=0,eh=0,ea=0;bool ev=false;
            c.EffectiveRect(pl,&ex,&ey,&ew,&eh,&ea,&ev);
            if(!ev)continue;
            centers[pl.id]={ex+ew*0.5f,ey+eh*0.5f};
        }
        for(const auto& l:layers) {
            if(l.label.empty() || !current.count(id+l.key))continue;
            container_keys.emplace(l.label,l.key);
            if(l.source.empty())continue;
            sources_by_label[l.label].push_back(id+l.key+".000000");
        }
        std::map<std::vector<std::string>,uint32_t> mask_cache;
        for(const auto& l:layers) {
            if(l.source.empty()||l.masks.empty())continue;
            const auto part=id+l.key+".000000";
            float px=0,py=0;
            if(const auto center=centers.find(part);center!=centers.end()){px=center->second.first;py=center->second.second;}
            std::vector<std::string> sources;
            for(const auto& label:l.masks) {
                const auto candidates=sources_by_label.find(label);
                if(candidates!=sources_by_label.end()) {
                    std::string best;float best_distance=0;
                    for(const auto& candidate_id:candidates->second) {
                        const auto center=centers.find(candidate_id);
                        if(center==centers.end())continue;
                        const float distance=(center->second.first-px)*(center->second.first-px)+
                                             (center->second.second-py)*(center->second.second-py);
                        if(best.empty()||distance<best_distance){best=candidate_id;best_distance=distance;}
                    }
                    if(!best.empty())sources.push_back(best);
                    continue;
                }
                // Mask labels may name a container (no drawable content of its
                // own, e.g. 下半身変形基礎/頭部変形基礎): the mask is then the
                // union of that subtree's drawable parts.
                const auto container=container_keys.find(label);
                if(container==container_keys.end())continue;
                const std::string prefix=container->second+".";
                for(const auto& l2:layers) {
                    if(l2.source.empty()||l2.key.rfind(prefix,0)!=0)continue;
                    if(!current.count(id+l2.key))continue;
                    sources.push_back(id+l2.key+".000000");
                }
            }
            if(sources.empty()){
                static std::set<std::string> reported;
                if(getenv("ARTC_EMOTE_DUMP")&&reported.insert(l.label).second)
                    Log(kLogInfo,"emote-mask unresolved for '"+l.label+"' labels="+[&]{std::string j;for(const auto& m:l.masks)j+=m+"|";return j;}());
                c.SetLayerStageMask(part,0);continue;}
            auto cached=mask_cache.find(l.masks);
            if(cached==mask_cache.end()) {
                std::string joined;
                for(const auto& m:l.masks)joined+=m+'\n';
                const int key=static_cast<int>(std::hash<std::string>{}(joined));
                cached=mask_cache.emplace(l.masks,c.RenderStageMask(key,sources)).first;
            }
            // A mask whose sources did not draw leaves the part unmasked
            // (reference: a stencil without drawn mask layers is disabled).
            if(cached->second==0){c.SetLayerStageMask(part,0);continue;}
            c.SetLayerStageMask(part,cached->second);
        }
    }
    // Pixel provenance probe (ARTC_EMOTE_PIXEL=x,y): report the topmost part
    // whose texture is actually opaque at the stage point in our draw order.
    // Used to name the layer behind a visual artifact without guessing.
    if(const char* probe=getenv("ARTC_EMOTE_PIXEL")) {
        std::vector<std::pair<float,float>> probe_points;
        {
            const char* cursor=probe;
            float px=0,py=0;int consumed=0;
            while(std::sscanf(cursor,"%f,%f%n",&px,&py,&consumed)==2) {
                probe_points.push_back({px,py});
                cursor+=consumed;
                if(*cursor==';')++cursor;
                else break;
            }
        }
        for(const auto& probe_point:probe_points) {
        float qx=probe_point.first,qy=probe_point.second;
        {
            struct Candidate{std::string id,label,icon;const EmoteImage* image;int paint;};
            std::vector<Candidate> candidates;
            for(const auto& l:layers) {
                if(l.source.empty())continue;
                const auto image=images_.find({l.source,l.icon});
                if(image==images_.end())continue;
                const bool rear=l.label.find("後髪")!=std::string::npos ||
                                l.label.find("後ろ髪")!=std::string::npos;
                candidates.push_back({id+l.key+".000000",l.label,l.icon,&image->second,rear?-1:0});
            }
            std::stable_sort(candidates.begin(),candidates.end(),
                             [](const Candidate& a,const Candidate& b){
                                 if(a.paint!=b.paint)return a.paint<b.paint;
                                 return a.id<b.id;
                             });
            std::vector<std::string> hits;
            for(const auto& cand:candidates) {
                const Layer* cl=nullptr;
                for(const auto& pl:c.Layers()) if(pl.id==cand.id){cl=&pl;break;}
                if(!cl||!cl->visible)continue;
                const auto m=c.EffectiveTransform(*cl);
                const double det=m.a*m.d-m.b*m.c;
                if(std::abs(det)<1e-12)continue;
                const double dx=qx-m.tx,dy=qy-m.ty;
                const double lx=(m.d*dx-m.c*dy)/det;
                const double ly=(-m.b*dx+m.a*dy)/det;
                const int ix=int(lx),iy=int(ly);
                if(ix<0||iy<0||ix>=cand.image->width||iy>=cand.image->height)continue;
                const int alpha=cand.image->rgba[(size_t(iy)*cand.image->width+ix)*4+3];
                const int ea=int(m.alpha*255);
                if(alpha<=8||ea<=8)continue;
                hits.push_back(cand.label+"["+cand.icon+"]a="+std::to_string(alpha)+
                               "ea="+std::to_string(ea));
            }
            std::string report="emote-pixel ("+std::to_string(int(qx))+","+std::to_string(int(qy))+")";
            if(hits.empty())report+=" top=none";
            else {
                report+=" top="+hits.back();
                if(hits.size()>1)report+=" under="+hits[hits.size()-2];
            }
            Log(kLogInfo,report);
        }
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
