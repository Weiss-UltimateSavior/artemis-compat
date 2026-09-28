// Shared bracket-tag syntax for the two text IET execution paths.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace artc {
// Unnamed arguments use zero-based string keys; named arguments do not
// consume a positional slot. Quotes preserve whitespace within a value.
void ParseIetInstruction(const std::string &body, std::string &tag,
                         std::vector<std::pair<std::string, std::string>> &attrs);
} // namespace artc
