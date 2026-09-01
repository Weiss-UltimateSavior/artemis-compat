// iet_interpreter.h — interpreter for the Artemis .iet boot/scenario scripts.
//
// Line syntax spec (from decrypted first.iet samples):
//   // ...                 line comment (skipped)
//   *label                 jump label (recorded; linear exec ignores)
//   [lua] ... [/lua]       multi-line Lua chunk (loaded and executed)
//   [calllua function="X"] call global Lua function X (engine bridge passed as arg 1)
//   [wt]                   wait one frame (no-op in linear exec)
//   [stop] / [return]      stop the script
//   [tag key="v" ...]      engine tag dispatch (routed through the e bridge)
//   anything else          scenario text (M1: logged; M2: message layer)
#pragma once
#include "pack/pack_manager.h"
#include "script/lua_engine.h"

#include <map>
#include <string>
#include <vector>

namespace artc {

class IetRunner {
public:
    IetRunner(PackManager *packs, LuaEngine *lua);

    // Execute the script linearly. Returns false only if the file cannot be read.
    bool Run(const std::string &path);

    bool Stopped() const { return stopped_; }

private:
    void ExecLine(const std::string &line);
    void ExecBrackets(const std::string &line);   // [tag ...] groups on one line
    void ExecBracket(const std::string &inner);   // inner text without brackets
    void ExecLuaChunk(const std::string &code, const std::string &chunk);

    static std::vector<std::string> SplitLines(const std::string &s);

    PackManager *packs_;
    LuaEngine *lua_;
    bool stopped_ = false;
    std::map<std::string, size_t> labels_;
    std::string current_path_;
};

} // namespace artc
