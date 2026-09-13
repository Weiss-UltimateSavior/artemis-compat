// preprocess.h — Artemis script preprocessor (&autoinsert / &linetag /
// &scpsupport) plus the optional tag.ini positional-parameter table.
//
// Text .iet/.ast scripts run through this before parsing; compiled .asb
// records are already preprocessed by the original compiler and skip it.
// The transform is line-for-line, so source line numbers are preserved.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace artc {

// tag.ini: tag name -> ordered positional parameter names.
class TagIni {
public:
    void Parse(const std::string &content);
    bool Empty() const { return params_.empty(); }
    // nullptr when the tag has no entry.
    const std::vector<std::string> *ParamNames(const std::string &tag) const {
        const auto it = params_.find(tag);
        return it == params_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, std::vector<std::string>> params_;
};

// Process-global tag.ini installed at startup (nullptr when absent).
void InstallTagIniFromText(const std::string &content);
void ClearTagIni();

// Apply the preprocessor to a full script text. Line count is preserved.
std::string PreprocessScript(const std::string &content);

} // namespace artc
