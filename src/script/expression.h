#pragma once
#include <functional>
#include <string>

namespace artc {
// Evaluate the expression after an Artemis '$' prefix. Values remain strings
// at the bridge boundary; comparisons and logical operators return 0 or 1.
bool EvaluateExpression(const std::string& expression,
                        const std::function<std::string(const std::string&)>& variable,
                        std::string& result);
} // namespace artc
