#include "script/pluto_codec.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>

namespace artc {
namespace {
// Pluto 2.x: LE int32 first/ref/type, IEEE double, native-size string lengths.
// Reference: hoelzro/pluto pluto.c, cross-checked against Artemis ARM64 and
// original BOWS saves. No VM pointers/bytecode are imported by this value codec.
constexpr size_t kMaxBytes=16*1024*1024, kMaxObjects=262144, kMaxDepth=128;
struct Codec {
    lua_State* L;
    std::string bytes;
    size_t pos=0, calls=0;
    unsigned word=8;
    std::set<uint32_t> refs;
    void Guard(size_t depth) {
        if(depth>kMaxDepth || ++calls>kMaxObjects || !lua_checkstack(L,8))
            throw std::runtime_error("Pluto value graph exceeds limits");
    }
    void Put(uint64_t v,unsigned n=4) {
        if(bytes.size()+n>kMaxBytes) throw std::runtime_error("Pluto data too large");
        for(unsigned i=0;i<n;++i) {bytes.push_back(char(v));v>>=8;}
    }
    uint64_t Get(unsigned n=4) {
        if(n>bytes.size()-pos) throw std::runtime_error("truncated Pluto value");
        uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(uint8_t(bytes[pos++]))<<(8*i);
        return v;
    }
    void Read(size_t depth=0) {
        Guard(depth);
        auto first=Get(), ref=Get();
        if(first==0) {
            if(!ref) lua_pushnil(L);
            else {
                if(!refs.count(ref)) throw std::runtime_error("invalid Pluto reference");
                lua_rawgeti(L,3,int(ref));
            }
            return;
        }
        if(first!=1 || !ref || ref>kMaxObjects || !refs.insert(ref).second)
            throw std::runtime_error("invalid Pluto object header");
        const auto type=Get();
        switch(type) {
        case LUA_TBOOLEAN: {auto b=Get();if(b>1)throw std::runtime_error("invalid Pluto boolean");lua_pushboolean(L,b);break;}
        case LUA_TNUMBER: {uint64_t v=Get(8);double n;std::memcpy(&n,&v,8);lua_pushnumber(L,n);break;}
        case LUA_TSTRING: {
            auto n=Get(word);
            if(n>bytes.size()-pos)throw std::runtime_error("invalid Pluto string length");
            lua_pushlstring(L,bytes.data()+pos,n);pos+=n;break;
        }
        case LUA_TTABLE: {
            if(Get()!=0)throw std::runtime_error("Pluto special persistence requires a VM closure");
            lua_newtable(L);const int table=lua_gettop(L);
            lua_pushvalue(L,table);lua_rawseti(L,3,int(ref)); // before children: cycles
            Read(depth+1);
            if(!lua_isnil(L,-1) && !lua_istable(L,-1))throw std::runtime_error("invalid Pluto metatable");
            lua_setmetatable(L,table);
            while(true) {
                Read(depth+1);if(lua_isnil(L,-1)){lua_pop(L,1);break;}
                if(lua_type(L,-1)==LUA_TNUMBER && std::isnan(lua_tonumber(L,-1)))
                    throw std::runtime_error("NaN Pluto table key");
                Read(depth+1);lua_rawset(L,table);
            }
            break;
        }
        case 101: // permanent object; the supplied inverse table owns its lifetime
            Read(depth+1);lua_rawget(L,1);
            if(lua_isnil(L,-1))throw std::runtime_error("missing Pluto permanent object");
            break;
        default: throw std::runtime_error("unsupported Pluto VM type "+std::to_string(type));
        }
        lua_pushvalue(L,-1);lua_rawseti(L,3,int(ref));
    }
    void Write(int index,size_t depth=0) {
        Guard(depth);
        const int type=lua_type(L,index);
        if(type==LUA_TNIL) {Put(0);Put(0);return;}
        lua_pushvalue(L,index);lua_rawget(L,3);
        if(!lua_isnil(L,-1)){auto ref=lua_tointeger(L,-1);lua_pop(L,1);Put(0);Put(ref);return;}
        lua_pop(L,1);
        const auto ref=calls;
        // NaN is a value, but cannot be used as a reference-table key.
        if(type!=LUA_TNUMBER || !std::isnan(lua_tonumber(L,index))) {
            lua_pushvalue(L,index);lua_pushinteger(L,ref);lua_rawset(L,3);
        }
        Put(1);Put(ref);
        lua_pushvalue(L,index);lua_rawget(L,1);
        if(!lua_isnil(L,-1)){Put(101);Write(lua_gettop(L),depth+1);lua_pop(L,1);return;}
        lua_pop(L,1);Put(type);
        switch(type) {
        case LUA_TBOOLEAN: Put(lua_toboolean(L,index));break;
        case LUA_TNUMBER: {double n=lua_tonumber(L,index);uint64_t v;std::memcpy(&v,&n,8);Put(v,8);break;}
        case LUA_TSTRING: {
            size_t n;const auto* s=lua_tolstring(L,index,&n);Put(n,word);
            if(n>kMaxBytes-bytes.size())throw std::runtime_error("Pluto data too large");
            bytes.append(s,n);break;
        }
        case LUA_TTABLE: {
            Put(0);
            if(!lua_getmetatable(L,index))lua_pushnil(L);
            if(lua_istable(L,-1)) {
                lua_pushliteral(L,"__persist");lua_rawget(L,-2);
                const bool allowed=lua_isnil(L,-1) || (lua_isboolean(L,-1)&&lua_toboolean(L,-1));
                lua_pop(L,1);
                if(!allowed)throw std::runtime_error("unsupported Pluto special persistence");
            }
            Write(lua_gettop(L),depth+1);lua_pop(L,1);
            lua_pushnil(L);
            while(lua_next(L,index)) {
                Write(lua_gettop(L)-1,depth+1);Write(lua_gettop(L),depth+1);lua_pop(L,1);
            }
            Put(0);Put(0);break;
        }
        default: throw std::runtime_error("cannot persist Pluto VM type "+std::to_string(type));
        }
    }
};
int Persist(lua_State* L) {
    luaL_checktype(L,1,LUA_TTABLE);lua_settop(L,2);lua_newtable(L);
    try {Codec c{L};c.Write(2);lua_pushlstring(L,c.bytes.data(),c.bytes.size());return 1;}
    catch(const std::exception& e){lua_pushstring(L,e.what());}
    return lua_error(L); // C++ temporaries have been destroyed before Lua longjmp
}
int Unpersist(lua_State* L) {
    luaL_checktype(L,1,LUA_TTABLE);size_t n;const char* s=luaL_checklstring(L,2,&n);
    if(!n){lua_pushnil(L);return 1;}
    if(n>kMaxBytes)return luaL_error(L,"Pluto data too large");
    if(n && (s[0]=='t' || s[0]=='\n')) {
        lua_pushvalue(L,lua_upvalueindex(1));lua_pushvalue(L,1);lua_pushvalue(L,2);
        lua_call(L,2,1);return 1;
    }
    for(unsigned word:{8u,4u}) {
        lua_settop(L,2);lua_newtable(L);
        try {
            Codec c{L,std::string(s,n)};c.word=word;c.Read();
            if(c.pos!=n)throw std::runtime_error("trailing Pluto data");
            return 1;
        } catch(const std::exception& e) {if(word==4)lua_pushstring(L,e.what());}
    }
    return lua_error(L);
}
}
void RegisterPlutoCodec(lua_State* L) {
    lua_getglobal(L,"pluto");
    lua_getfield(L,-1,"unpersist");lua_pushcclosure(L,Unpersist,1);lua_setfield(L,-2,"unpersist");
    lua_pushcfunction(L,Persist);lua_setfield(L,-2,"persist");lua_pop(L,1);
}
}
