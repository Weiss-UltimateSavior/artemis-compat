#include "script/save_storage.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <zlib.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif
namespace artc {
namespace {constexpr size_t max_bytes=64*1024*1024;}
std::string SavePath(const std::string& directory,const std::string& file) {
    if(directory.empty()||file.empty())return {};
    const auto base=std::filesystem::path(directory).lexically_normal();
    const auto path=(base/std::filesystem::path(file)).lexically_normal();
    const auto relative=path.lexically_relative(base);
    if(relative.empty() || relative=="." || *relative.begin()=="..")return {};
    return path.string();
}
bool ReadSaveFile(const std::string& path,std::vector<uint8_t>& out) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    if(!in || in.tellg()<0 || in.tellg()>max_bytes)return false;
    std::vector<uint8_t> next(static_cast<size_t>(in.tellg()));in.seekg(0);
    if(!next.empty() && !in.read(reinterpret_cast<char*>(next.data()),next.size()))return false;
    out=std::move(next);return true;
}
bool WriteSaveFile(const std::string& path,const std::vector<uint8_t>& bytes) {
    if(path.empty() || bytes.size()>max_bytes)return false;
    static std::atomic<uint64_t> serial{0};
    const auto tmp=path+".tmp."+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
        "."+std::to_string(++serial);
    bool ok=false;
#if !defined(_WIN32)
    const int fd=open(tmp.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);
    if(fd<0)return false;
    size_t done=0;
    while(done<bytes.size()) {
        const auto n=write(fd,bytes.data()+done,bytes.size()-done);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)break;done+=n;
    }
    ok=done==bytes.size() && fsync(fd)==0;
    if(close(fd)!=0)ok=false;
#else
    {std::ofstream out(std::filesystem::path(tmp),std::ios::binary|std::ios::trunc);
     if(out){out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());out.flush();ok=bool(out);}}
#endif
    if(ok)ok=std::rename(tmp.c_str(),path.c_str())==0;
    if(!ok)std::remove(tmp.c_str());
    return ok;
}
bool EncodeVariableBank(const VariableBank& vars,bool checkpoint,std::vector<uint8_t>& out) {
    if(vars.size()>65536)return false;
    std::vector<uint8_t> bytes;
    auto put=[&](uint32_t n){for(int i=0;i<4;++i)bytes.push_back(uint8_t(n>>(i*8)));};
    if(checkpoint){bytes={'A','R','C','V'};put(1);}
    put(vars.size());
    for(const auto& v:vars) {
        if(v.first.size()>1024*1024 || v.second.size()>16*1024*1024 ||
           bytes.size()+v.first.size()+v.second.size()+12>max_bytes)return false;
        put(v.first.size());bytes.insert(bytes.end(),v.first.begin(),v.first.end());
        put(v.second.size());bytes.insert(bytes.end(),v.second.begin(),v.second.end());
    }
    if(checkpoint)put(crc32(0,bytes.data(),bytes.size()));
    out=std::move(bytes);return true;
}
bool DecodeVariableBank(const std::vector<uint8_t>& bytes,bool checkpoint,VariableBank& out) {
    if(bytes.size()<4 || bytes.size()>max_bytes)return false;
    size_t pos=0,end=bytes.size();bool valid=true;
    auto get=[&]()->uint32_t {
        if(end-pos<4){valid=false;return 0;}
        uint32_t n=0;for(int i=0;i<4;++i)n|=uint32_t(bytes[pos++])<<(i*8);return n;
    };
    if(checkpoint) {
        if(bytes.size()<16 || std::string(bytes.begin(),bytes.begin()+4)!="ARCV")return false;
        pos=4;if(get()!=1)return false;
        pos=bytes.size()-4;const auto expected=get();
        if(crc32(0,bytes.data(),bytes.size()-4)!=expected)return false;
        pos=8;end=bytes.size()-4;
    }
    const auto count=get();if(count>65536)return false;VariableBank next;
    auto string=[&](size_t max)->std::string {
        const auto n=get();if(!valid||n>max||n>end-pos){valid=false;return {};}
        std::string s(reinterpret_cast<const char*>(bytes.data()+pos),n);pos+=n;return s;
    };
    for(uint32_t i=0;i<count;++i){auto k=string(1024*1024),v=string(16*1024*1024);
        if(!valid || !next.emplace(std::move(k),std::move(v)).second)return false;}
    if(!valid||pos!=end)return false;out=std::move(next);return true;
}
}
