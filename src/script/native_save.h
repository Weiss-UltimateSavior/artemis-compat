#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace artc {
struct SavedCommand {
    std::string name;
    std::map<std::string,std::string> attrs;
};
struct NativeSave {
    std::map<std::string,std::string> variables;
    std::vector<SavedCommand> layers;
    std::map<std::string,std::vector<SavedCommand>> text;
};
struct NativeGlobals {
    std::map<std::string,std::string> variables;
    std::map<std::string,std::vector<uint32_t>> read_lines;
};
// BOWS v1003: zlib envelope around CSerializer data plus its field directory.
// This is an importer. It does not claim that a compatibility save can be
// exported to an original engine, nor discard/overwrite unknown native fields.
// Outputs are replaced only after the entire supported structure validates.
bool DecodeNativeSave(const std::vector<uint8_t>& file, NativeSave& out, std::string& error);
// BOWG uses the same envelope/directory, with read-line sets in field 1 and
// global variables in field 2. Parsing does not alter the original file.
bool DecodeNativeGlobals(const std::vector<uint8_t>& file, NativeGlobals& out, std::string& error);
}
