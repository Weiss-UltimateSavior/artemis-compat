#include "script/native_save.h"
#include <zlib.h>
#include <cstring>
#include <stdexcept>
#include <utility>
namespace artc {
namespace {
constexpr size_t kMaxBytes=64*1024*1024,kMaxCount=262144;
struct Reader {
    const std::vector<uint8_t>& data;
    size_t pos,end;
    uint32_t U32() {
        if(end-pos<4)throw std::runtime_error("truncated native save");
        uint32_t n=0;for(unsigned i=0;i<4;++i)n|=uint32_t(data[pos++])<<(i*8);return n;
    }
    uint32_t Count() {auto n=U32();if(n>kMaxCount)throw std::runtime_error("native save count too large");return n;}
    std::string String() {
        const auto n=U32();if(n>end-pos)throw std::runtime_error("truncated native save string");
        std::string s(reinterpret_cast<const char*>(data.data()+pos),n);pos+=n;return s;
    }
    std::map<std::string,std::string> Strings() {
        std::map<std::string,std::string> r;const auto n=Count();
        for(uint32_t i=0;i<n;++i){auto key=String();auto value=String();
            if(!r.emplace(std::move(key),std::move(value)).second)throw std::runtime_error("duplicate native save key");}
        return r;
    }
    SavedCommand Command() {
        U32(); // source line; no terminating NULs in CSerializer strings
        SavedCommand c;c.name=String();c.attrs=Strings();return c;
    }
    void End() {if(pos!=end)throw std::runtime_error("native save field length mismatch");}
};
struct Archive {
    std::vector<uint8_t> data;
    std::map<uint32_t,std::map<uint32_t,uint32_t>> objects;
    uint32_t directory=0;
    explicit Archive(const std::vector<uint8_t>& file,const char* magic="BOWS") {
        if(file.size()<16 || file.size()>kMaxBytes || std::memcmp(file.data(),magic,4))
            throw std::runtime_error("wrong native save signature");
        Reader h{file,4,file.size()};
        if(h.U32()!=1003)throw std::runtime_error("unsupported BOWS version");
        const auto n=h.U32();if(n<8 || n>kMaxBytes)throw std::runtime_error("invalid BOWS expanded size");
        data.resize(n);
        z_stream z{};z.next_in=const_cast<Bytef*>(file.data()+12);z.avail_in=file.size()-12;
        z.next_out=data.data();z.avail_out=data.size();
        if(inflateInit(&z)!=Z_OK)throw std::runtime_error("BOWS inflate initialization failed");
        const int result=inflate(&z,Z_FINISH);
        const bool valid=result==Z_STREAM_END && z.total_out==n && z.avail_in==0;
        inflateEnd(&z);
        if(!valid)throw std::runtime_error("invalid BOWS zlib stream");
        Reader tail{data,data.size()-4,data.size()};directory=tail.U32();
        if(directory>data.size()-8)throw std::runtime_error("invalid BOWS directory offset");
        Reader r{data,directory,data.size()-4};const auto count=r.Count();
        for(uint32_t i=0;i<count;++i) {
            const auto start=r.U32(),nfields=r.Count();
            if(start>directory || objects.count(start))throw std::runtime_error("invalid BOWS object offset");
            auto& fields=objects[start];
            for(uint32_t j=0;j<nfields;++j) {
                const auto id=r.U32(),offset=r.U32();
                if(offset<start || offset>directory || !fields.emplace(id,offset).second)
                    throw std::runtime_error("invalid BOWS field offset");
            }
        }
        r.End();if(!objects.count(0))throw std::runtime_error("BOWS root missing");
    }
    Reader Field(uint32_t object,uint32_t id,size_t limit) const {
        auto obj=objects.find(object);
        if(obj==objects.end() || !obj->second.count(id))throw std::runtime_error("required BOWS field missing");
        const auto begin=obj->second.at(id);
        size_t end=limit;
        for(const auto& p:obj->second)if(p.second>begin && p.second<end)end=p.second;
        if(begin>end)throw std::runtime_error("BOWS field outside object");
        return {data,begin,end};
    }
    std::map<std::string,std::vector<SavedCommand>> Commands(uint32_t field) const {
        auto r=Field(0,field,directory);std::map<std::string,std::vector<SavedCommand>> result;
        const auto n=r.Count();size_t total=0;
        for(uint32_t i=0;i<n;++i) {
            auto id=r.String();if(result.count(id))throw std::runtime_error("duplicate saved layer");
            auto& commands=result[id];auto count=r.Count();total+=count;
            if(total>kMaxCount)throw std::runtime_error("too many saved commands");
            for(uint32_t j=0;j<count;++j)commands.push_back(r.Command());
        }
        r.End();return result;
    }
};
}
bool DecodeNativeSave(const std::vector<uint8_t>& file,NativeSave& out,std::string& error) {
    try {
        Archive a(file);NativeSave next;
        auto bank=a.Field(0,30,a.directory);
        auto vars=a.Field(bank.pos,2,bank.end);next.variables=vars.Strings();vars.End();
        for(auto& entry:a.Commands(26))
            for(auto& command:entry.second)next.layers.push_back(std::move(command));
        next.text=a.Commands(24);
        out=std::move(next);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool DecodeNativeGlobals(const std::vector<uint8_t>& file,NativeGlobals& out,std::string& error) {
    try {
        Archive a(file,"BOWG");NativeGlobals next;
        auto vars=a.Field(0,2,a.directory);next.variables=vars.Strings();vars.End();
        for(const auto& v:next.variables)
            if(v.first.rfind("g.",0)!=0)throw std::runtime_error("non-global variable in BOWG bank");
        auto lines=a.Field(0,1,a.directory);const auto count=lines.Count();size_t total=0;
        for(uint32_t i=0;i<count;++i) {
            auto file=lines.String();const auto n=lines.Count();total+=n;
            if(total>kMaxCount || next.read_lines.count(file))throw std::runtime_error("invalid BOWG read-line set");
            auto& entries=next.read_lines[file];entries.reserve(n);
            for(uint32_t j=0;j<n;++j)entries.push_back(lines.U32());
        }
        lines.End();out=std::move(next);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
}
