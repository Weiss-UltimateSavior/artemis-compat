// ini.h — minimal INI parser matching the engine's system.ini / boot.ini dialect.
//
// Dialect spec (official docs + samples):
//   [SECTION]          section header
//   KEY = VALUE        assignment ('=' spacing arbitrary)
//   ; comment          line comment ('#' runs also appear inside comments)
//   values may be empty; duplicate keys: last one wins
//   encoding: bytes are preserved as-is (UTF-8 titles use UTF-8; legacy titles
//   may be Shift_JIS — callers transcode as needed)
#pragma once
#include <map>
#include <string>
#include <vector>

namespace artc {

class Ini {
public:
    bool Parse(const std::string &bytes);      // whole-file parse
    bool ParseFile(const std::string &path);

    bool HasSection(const std::string &section) const;
    std::string Get(const std::string &section, const std::string &key,
                    const std::string &def = "") const;
    int GetInt(const std::string &section, const std::string &key, int def = 0) const;

    const std::vector<std::string> &Sections() const { return order_; }

private:
    std::map<std::string, std::map<std::string, std::string>> sections_;
    std::vector<std::string> order_;
    std::string current_;
};

} // namespace artc
