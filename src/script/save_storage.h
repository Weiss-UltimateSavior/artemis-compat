#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace artc {
using VariableBank=std::map<std::string,std::string>;
std::string SavePath(const std::string& directory,const std::string& file);
bool ReadSaveFile(const std::string& path,std::vector<uint8_t>& bytes);
bool WriteSaveFile(const std::string& path,const std::vector<uint8_t>& bytes);
// ARCV is explicitly a compatibility checkpoint (onLoad reconstructs the
// scenario). It is not presented to original engines as a complete BOWS file.
bool EncodeVariableBank(const VariableBank& vars,bool checkpoint,std::vector<uint8_t>& out);
bool DecodeVariableBank(const std::vector<uint8_t>& bytes,bool checkpoint,VariableBank& out);
}
