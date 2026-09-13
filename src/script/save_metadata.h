#pragma once
#include "script/save_storage.h"
struct lua_State;
namespace artc {
// Recover only empty dates written by early ARCV save implementations. The
// timestamp is taken from the matching compatibility file, never fabricated.
bool RepairCompatibilitySaveDates(lua_State* lua,VariableBank& bank,const std::string& directory);
}
