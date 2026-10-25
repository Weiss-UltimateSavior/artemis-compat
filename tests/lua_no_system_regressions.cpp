// lua_no_system_regressions.cpp — unsupported process execution reports failure.
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}
#include <iostream>

int main() {
    lua_State *L = luaL_newstate();
    if (!L) return 1;
    luaL_openlibs(L);
    const char *script =
        "local ok, err = os.execute('echo definitely-not-run')\n"
        "assert(ok == nil, 'unsupported process execution returned success')\n"
        "assert(type(err) == 'string' and #err > 0, 'missing execution error')\n";
    const int status = luaL_dostring(L, script);
    if (status != 0) {
        std::cerr << "FAIL: " << lua_tostring(L, -1) << "\n";
        lua_close(L);
        return 1;
    }
    lua_close(L);
    std::cout << "Lua no-system regression passed\n";
    return 0;
}
