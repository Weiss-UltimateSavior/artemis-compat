#include "script/save_metadata.h"
extern "C" {
#include "lua.h"
}
#include <ctime>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace artc {
bool RepairCompatibilitySaveDates(lua_State* L,VariableBank& bank,const std::string& directory) {
    auto entry=bank.find("g.system");if(entry==bank.end())return false;
    const int top=lua_gettop(L);
    lua_getglobal(L,"pluto");lua_getfield(L,-1,"unpersist");lua_newtable(L);
    lua_pushlstring(L,entry->second.data(),entry->second.size());
    if(lua_pcall(L,2,1,0) || !lua_istable(L,-1)){lua_settop(L,top);return false;}
    const int root=lua_gettop(L);lua_getfield(L,root,"saveslot");
    if(!lua_istable(L,-1)){lua_settop(L,top);return false;}
    const int slots=lua_gettop(L);bool changed=false;lua_pushnil(L);
    while(lua_next(L,slots)) {
        const int slot=lua_gettop(L);
        if(lua_istable(L,slot)) {
            lua_getfield(L,slot,"date");
            bool empty=lua_isnil(L,-1) || (lua_istable(L,-1) && lua_objlen(L,-1)==0);
            lua_pop(L,1);
            if(empty) {
                lua_getfield(L,slot,"file");const char* name=lua_tostring(L,-1);
                const auto path=name?SavePath(directory,std::string(name)+".dat"):std::string{};lua_pop(L,1);
                char magic[4]{};std::ifstream file(path,std::ios::binary);file.read(magic,4);
                struct stat st{};std::tm date{};
                if(file.gcount()==4 && !std::memcmp(magic,"ARCV",4) && stat(path.c_str(),&st)==0) {
#if defined(_WIN32)
                    const bool valid=localtime_s(&date,&st.st_mtime)==0;
#else
                    const bool valid=localtime_r(&st.st_mtime,&date)!=nullptr;
#endif
                    if(valid) {
                        lua_newtable(L);const int fields[]={date.tm_year+1900,date.tm_mon+1,date.tm_mday,date.tm_hour,date.tm_min,date.tm_sec};
                        for(int i=0;i<6;++i){lua_pushinteger(L,fields[i]);lua_rawseti(L,-2,i+1);}
                        lua_setfield(L,slot,"date");changed=true;
                    }
                }
            }
        }
        lua_pop(L,1);
    }
    if(changed) {
        lua_getglobal(L,"pluto");lua_getfield(L,-1,"persist");lua_newtable(L);lua_pushvalue(L,root);
        if(lua_pcall(L,2,1,0) || !lua_isstring(L,-1))changed=false;
        else {size_t size;const char* value=lua_tolstring(L,-1,&size);entry->second.assign(value,size);}
    }
    lua_settop(L,top);return changed;
}
}
