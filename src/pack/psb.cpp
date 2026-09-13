#include "pack/psb.h"
#include <cstring>
#include <limits>
#include <stdexcept>
#include <zlib.h>

namespace artc {
const PsbValue& PsbValue::At(const std::string& key) const {
    static const PsbValue missing;
    const auto i=object.find(key);return i==object.end()?missing:i->second;
}
bool PsbDocument::ReadResource(const PsbValue& ref,std::vector<uint8_t>& out) const {
    const auto& bank=ref.extra?extra_resources:resources;
    if(ref.type!=PsbValue::Resource || ref.resource>=bank.size())return false;
    const auto r=bank[ref.resource];
    if(r.first>bytes.size() || r.second>bytes.size()-r.first)return false;
    out.assign(bytes.begin()+r.first,bytes.begin()+r.first+r.second);return true;
}
namespace {
constexpr size_t MaxBytes=256*1024*1024, MaxItems=1024*1024, MaxDepth=128;
struct Reader {
    PsbDocument doc;
    std::vector<std::string> names,strings;
    size_t items=0,text=0;
    [[noreturn]] void Fail(const char* why) {throw std::runtime_error(why);}
    uint64_t Int(size_t& p,unsigned width) {
        if(width>8 || p>doc.bytes.size() || width>doc.bytes.size()-p)Fail("truncated PSB integer");
        uint64_t v=0;for(unsigned i=0;i<width;++i)v|=uint64_t(doc.bytes[p++])<<(i*8);return v;
    }
    uint32_t Header(size_t p) {return uint32_t(Int(p,4));}
    size_t Offset(size_t base,uint64_t delta) {
        if(base>doc.bytes.size() || delta>=doc.bytes.size()-base)Fail("PSB offset outside file");
        return base+size_t(delta);
    }
    void Count(size_t n) {if(n>MaxItems-items)Fail("PSB object limit exceeded");items+=n;}
    std::vector<uint64_t> Array(size_t& p) {
        unsigned width=unsigned(Int(p,1));
        if(width<13 || width>20)Fail("invalid PSB packed array");
        const uint64_t count=Int(p,width-12);Count(count);
        width=unsigned(Int(p,1));
        if(width<13 || width>20)Fail("invalid PSB array width");
        std::vector<uint64_t> a;a.reserve(size_t(count));
        for(size_t i=0;i<count;++i)a.push_back(Int(p,width-12));return a;
    }
    std::string Text(size_t p) {
        if(p>=doc.bytes.size())Fail("PSB string outside file");
        const size_t start=p;
        while(p<doc.bytes.size() && doc.bytes[p])++p;
        if(p==doc.bytes.size() || p-start>16*1024*1024 || p-start>MaxBytes-text)Fail("invalid PSB string");
        text+=p-start;return std::string(reinterpret_cast<const char*>(doc.bytes.data()+start),p-start);
    }
    void Names(size_t p) {
        const auto bases=Array(p),parents=Array(p),terminals=Array(p);
        for(auto terminal:terminals) {
            if(terminal>=parents.size())Fail("invalid PSB name terminal");
            uint64_t node=parents[terminal];std::string value;
            while(node) {
                if(node>=parents.size() || value.size()>=4096)Fail("invalid PSB name trie");
                const auto parent=parents[node];
                if(parent>=bases.size() || node<bases[parent] || node-bases[parent]>255)Fail("invalid PSB name character");
                value.push_back(char(node-bases[parent]));node=parent;
            }
            if(value.size()>MaxBytes-text)Fail("PSB name limit exceeded");text+=value.size();
            names.emplace_back(value.rbegin(),value.rend());
        }
    }
    void Chunks(size_t offset,size_t length,size_t data,std::vector<std::pair<size_t,size_t>>& out) {
        const auto offsets=Array(offset),lengths=Array(length);
        if(offsets.size()!=lengths.size())Fail("PSB resource counts differ");
        for(size_t i=0;i<offsets.size();++i) {
            if(data>doc.bytes.size() || offsets[i]>doc.bytes.size()-data ||
               lengths[i]>doc.bytes.size()-data-offsets[i])Fail("PSB resource outside file");
            out.emplace_back(data+size_t(offsets[i]),size_t(lengths[i]));
        }
    }
    PsbValue Value(size_t p,size_t depth=0) {
        if(depth>MaxDepth)Fail("PSB nesting limit exceeded");Count(1);
        const auto tag=Int(p,1);PsbValue v;
        if(tag<=1)return v;
        if(tag<=3){v.type=PsbValue::Boolean;v.number=tag==3;return v;}
        if(tag<=12) {
            v.type=PsbValue::Number;const unsigned width=unsigned(tag-4);
            uint64_t n=Int(p,width);
            if(width && width<8 && (n&(uint64_t(1)<<(width*8-1))))n|=~uint64_t(0)<<(width*8);
            int64_t signed_n;std::memcpy(&signed_n,&n,8);v.number=double(signed_n);return v;
        }
        if(tag<=20) {
            --p;v.type=PsbValue::Array;
            for(auto n:Array(p)){PsbValue x;x.type=PsbValue::Number;x.number=double(n);v.array.push_back(x);}return v;
        }
        if(tag<=24) {
            const auto index=Int(p,unsigned(tag-20));if(index>=strings.size())Fail("invalid PSB string reference");
            v.type=PsbValue::String;v.string=strings[index];
            if(v.string.size()>MaxBytes-text)Fail("PSB expanded text limit exceeded");text+=v.string.size();return v;
        }
        if((tag>=25 && tag<=28) || (tag>=34 && tag<=37)) {
            v.type=PsbValue::Resource;v.extra=tag>=34;
            const auto index=Int(p,unsigned(tag-(v.extra?33:24)));
            if(index>=(v.extra?doc.extra_resources:doc.resources).size())Fail("invalid PSB resource reference");
            v.resource=uint32_t(index);return v;
        }
        if(tag<=31) {
            v.type=PsbValue::Number;
            if(tag==30){uint32_t n=uint32_t(Int(p,4));float f;std::memcpy(&f,&n,4);v.number=f;}
            if(tag==31){uint64_t n=Int(p,8);std::memcpy(&v.number,&n,8);}return v;
        }
        if(tag==32) {
            v.type=PsbValue::Array;const auto offsets=Array(p);
            for(auto n:offsets)v.array.push_back(Value(Offset(p,n),depth+1));return v;
        }
        if(tag==33) {
            v.type=PsbValue::Object;const auto keys=Array(p),offsets=Array(p);
            if(keys.size()!=offsets.size())Fail("PSB dictionary counts differ");
            for(size_t i=0;i<keys.size();++i) {
                if(keys[i]>=names.size())Fail("invalid PSB dictionary key");
                if(!v.object.emplace(names[keys[i]],Value(Offset(p,offsets[i]),depth+1)).second)Fail("duplicate PSB key");
            }return v;
        }
        Fail("unsupported PSB value type");
    }
    void Parse() {
        if(doc.bytes.size()<40 || std::memcmp(doc.bytes.data(),"PSB\0",4))Fail("not a PSB model");
        size_t p=4;doc.version=uint16_t(Int(p,2));
        if(doc.version<2 || doc.version>4)Fail("unsupported PSB version");
        if(Int(p,2))Fail("encrypted PSB requires its original decoder");
        if(doc.bytes.size()<(doc.version==4?56:doc.version==3?44:40))Fail("truncated PSB header");
        Names(Header(12));p=Header(16);const auto offsets=Array(p);const auto base=Header(20);
        for(auto o:offsets)strings.push_back(Text(Offset(base,o)));
        Chunks(Header(24),Header(28),Header(32),doc.resources);
        if(doc.version==4)Chunks(Header(44),Header(48),Header(52),doc.extra_resources);
        doc.root=Value(Header(36));
    }
};
}
bool DecodePsb(const std::vector<uint8_t>& input,PsbDocument& out,std::string& error) {
    try {
        if(input.size()>MaxBytes)throw std::runtime_error("PSB file too large");
        Reader r;
        if(input.size()>=8 && (input[0]|32)=='m' && (input[1]|32)=='d' && (input[2]|32)=='f' && !input[3]) {
            uint32_t size=0;for(int i=0;i<4;++i)size|=uint32_t(input[4+i])<<(8*i);
            if(!size || size>MaxBytes)throw std::runtime_error("invalid MDF size");
            r.doc.bytes.resize(size);z_stream z{};
            z.next_in=const_cast<Bytef*>(input.data()+8);z.avail_in=uInt(input.size()-8);
            z.next_out=r.doc.bytes.data();z.avail_out=size;
            if(inflateInit(&z)!=Z_OK)throw std::runtime_error("MDF inflater failed");
            const int rc=inflate(&z,Z_FINISH);const bool valid=rc==Z_STREAM_END && z.total_out==size && !z.avail_in;
            inflateEnd(&z);if(!valid)throw std::runtime_error("invalid MDF compressed data");
        } else r.doc.bytes=input;
        r.Parse();out=std::move(r.doc);error.clear();return true;
    } catch(const std::exception& e){error=e.what();return false;}
}
bool DecodePsbRl(const std::vector<uint8_t>& bytes,size_t count,unsigned stride,std::vector<uint8_t>& out) {
    if((stride!=1 && stride!=4) || count>MaxBytes/stride)return false;
    const size_t size=count*stride;std::vector<uint8_t> result;result.reserve(size);
    size_t p=0;
    while(p<bytes.size() && result.size()<size) {
        const unsigned marker=bytes[p++];const size_t n=(marker&128)?(marker&127)+3:marker+1;
        if(n>(size-result.size())/stride)return false;
        const size_t length=(marker&128)?stride:n*stride;
        if(length>bytes.size()-p)return false;
        if(marker&128)for(size_t i=0;i<n;++i)result.insert(result.end(),bytes.begin()+p,bytes.begin()+p+stride);
        else result.insert(result.end(),bytes.begin()+p,bytes.begin()+p+length);
        p+=length;
    }
    if(p!=bytes.size() || result.size()!=size)return false;
    out=std::move(result);return true;
}
}
