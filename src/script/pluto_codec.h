#pragma once
struct lua_State;
namespace artc {
// Installs the native Pluto value codec; keeps the previous unpersist function
// for migration of saves made by the early Lua-source compatibility codec.
void RegisterPlutoCodec(lua_State* L);
}
