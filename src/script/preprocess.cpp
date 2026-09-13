#include "script/preprocess.h"

#include <cctype>
#include <cstdlib>
#include <memory>
#include <utility>

namespace artc {
namespace {

std::unique_ptr<TagIni> g_tag_ini;

std::string Trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// One bracket group inside a line: either literal text or "[ ... ]" inner body.
struct Segment {
    bool is_tag = false;
    std::string text; // tag inner body when is_tag, otherwise literal text
};

std::vector<Segment> SplitLineSegments(const std::string &line) {
    std::vector<Segment> segments;
    size_t i = 0;
    while (i < line.size()) {
        if (line[i] != '[') {
            size_t start = i;
            while (i < line.size() && line[i] != '[') ++i;
            segments.push_back({false, line.substr(start, i - start)});
            continue;
        }
        size_t depth = 0, j = i;
        bool in_quote = false;
        for (; j < line.size(); ++j) {
            const char c = line[j];
            if (in_quote) { if (c == '"') in_quote = false; continue; }
            if (c == '"') { in_quote = true; continue; }
            if (c == '[') ++depth;
            if (c == ']') { --depth; if (depth == 0) break; }
        }
        if (j >= line.size()) break;
        segments.push_back({true, line.substr(i + 1, j - i - 1)});
        i = j + 1;
    }
    return segments;
}

// Parse `name key="value" flag k=v` into a name + ordered attributes.
void ParseInstruction(const std::string &body, std::string *tag,
                      std::vector<std::pair<std::string, std::string>> *attrs) {
    size_t i = 0;
    auto token = [&](std::string &tok) {
        tok.clear();
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
        const bool quoted = i < body.size() && body[i] == '"';
        if (quoted) ++i;
        while (i < body.size()) {
            const char c = body[i];
            if (quoted) {
                if (c == '"') { ++i; break; }
                tok += c; ++i;
            } else {
                if (std::isspace(static_cast<unsigned char>(c)) || c == '=') break;
                tok += c; ++i;
            }
        }
    };
    std::string first;
    token(first);
    *tag = first;
    while (i < body.size()) {
        std::string key;
        token(key);
        if (key.empty()) { ++i; continue; }
        if (i < body.size() && body[i] == '=') {
            ++i;
            std::string val;
            const bool quoted = i < body.size() && body[i] == '"';
            if (quoted) ++i;
            while (i < body.size()) {
                const char c = body[i];
                if (quoted) { if (c == '"') { ++i; break; } val += c; ++i; }
                else { if (std::isspace(static_cast<unsigned char>(c))) break; val += c; ++i; }
            }
            attrs->emplace_back(key, val);
        } else {
            attrs->emplace_back(key, "");
        }
    }
}

std::string Attr(const std::vector<std::pair<std::string, std::string>> &attrs,
                 const char *name) {
    for (const auto &kv : attrs)
        if (kv.first == name) return kv.second;
    return std::string();
}

struct ScpRule {
    std::string suffix; // empty = newline rule
    std::string command;
};

struct Preprocessor {
    bool blankline_set = false, linehead_set = false, lineend_set = false;
    std::string blankline, linehead, lineend;
    bool linetag_allow = false;
    bool linetag_has_prefix = false;
    std::string linetag_prefix;
    std::vector<ScpRule> scp_rules;
    bool in_lua = false;

    bool ApplyDirective(const std::string &inner) {
        std::string tag;
        std::vector<std::pair<std::string, std::string>> attrs;
        ParseInstruction(inner, &tag, &attrs);
        if (tag == "&autoinsert") {
            const std::string cmd = Attr(attrs, "command");
            const std::string target = Attr(attrs, "target");
            if (target == "blankline") { blankline = cmd; blankline_set = !cmd.empty(); }
            else if (target == "linehead") { linehead = cmd; linehead_set = !cmd.empty(); }
            else if (target == "lineend") { lineend = cmd; lineend_set = !cmd.empty(); }
            return true;
        }
        if (tag == "&linetag") {
            const std::string allow = Attr(attrs, "allow");
            if (!allow.empty()) linetag_allow = Trim(allow) == "1";
            const std::string prefix = Attr(attrs, "prefix");
            linetag_has_prefix = !prefix.empty();
            linetag_prefix = prefix;
            return true;
        }
        if (tag == "&scpsupport") {
            const std::string mode = Attr(attrs, "mode");
            if (mode == "init") scp_rules.clear();
            else if (mode == "add" || mode == "addsp") {
                const std::string cmd = Attr(attrs, "command");
                if (!cmd.empty() && cmd.find('[') == std::string::npos &&
                    cmd.find(']') == std::string::npos)
                    scp_rules.push_back({Attr(attrs, "char"), cmd});
            }
            return true;
        }
        return false;
    }

    // Strip [&...] directives from a line and update state. Returns the
    // remainder and whether any directive was consumed.
    std::pair<std::string, bool> StripDirectives(const std::string &raw) {
        if (raw.find("[&") == std::string::npos) return {raw, false};
        bool had = false;
        std::string rebuilt;
        for (const Segment &seg : SplitLineSegments(raw)) {
            if (seg.is_tag && !seg.text.empty() && seg.text[0] == '&' &&
                ApplyDirective(seg.text)) {
                had = true;
                continue;
            }
            if (seg.is_tag) { rebuilt += '['; rebuilt += seg.text; rebuilt += ']'; }
            else rebuilt += seg.text;
        }
        return {rebuilt, had};
    }

    std::string FormatScp(const std::string &command) const {
        return command == "@" ? std::string("@") : "[" + command + "]";
    }

    void ApplyScpRules(std::string *out) const {
        bool char_hit = false;
        for (const ScpRule &r : scp_rules) {
            if (!r.suffix.empty() && out->size() >= r.suffix.size() &&
                out->compare(out->size() - r.suffix.size(), r.suffix.size(), r.suffix) == 0) {
                *out += FormatScp(r.command);
                char_hit = true;
            }
        }
        if (char_hit) return;
        for (const ScpRule &r : scp_rules)
            if (r.suffix.empty()) *out += FormatScp(r.command);
    }

    bool TryConvertLinetag(const std::string &trimmed, std::string *out) const {
        if (!linetag_allow) return false;
        std::string body = trimmed;
        if (linetag_has_prefix) {
            if (body.rfind(linetag_prefix, 0) != 0) return false;
            body = Trim(body.substr(linetag_prefix.size()));
        }
        if (body.empty()) return false;
        const unsigned char first = static_cast<unsigned char>(body[0]);
        // ASCII-head rule only without a prefix; a prefix permits any macro name.
        if (body[0] == '[') return false;
        if (!linetag_has_prefix && !std::isalnum(first)) return false;

        size_t sp = 0;
        while (sp < body.size() && !std::isspace(static_cast<unsigned char>(body[sp]))) ++sp;
        const std::string name = body.substr(0, sp);
        std::string rest = Trim(body.substr(sp));

        std::string built = "[" + name;
        if (!rest.empty()) {
            const std::vector<std::string> *names =
                g_tag_ini ? g_tag_ini->ParamNames(name) : nullptr;
            size_t i = 0;
            size_t start = 0;
            auto emit = [&](const std::string &raw_arg) {
                std::string value = Trim(raw_arg);
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                    value = value.substr(1, value.size() - 2);
                std::string key =
                    names && i < names->size() && !(*names)[i].empty()
                        ? (*names)[i] : std::to_string(i);
                built += " " + key + "=\"" + value + "\"";
                ++i;
            };
            for (size_t k = 0; k <= rest.size(); ++k) {
                if (k == rest.size() || rest[k] == ',') {
                    emit(rest.substr(start, k - start));
                    start = k + 1;
                }
            }
        }
        built += "]";
        *out = built;
        return true;
    }

    std::string ProcessLine(const std::string &raw) {
        if (in_lua) {
            if (Trim(raw) == "[/lua]") in_lua = false;
            return raw;
        }
        if (raw.find('[') != std::string::npos) {
            for (const Segment &seg : SplitLineSegments(raw))
                if (seg.is_tag && Trim(seg.text) == "lua") { in_lua = true; return raw; }
        }
        auto stripped = StripDirectives(raw);
        const std::string remainder = stripped.first;
        const bool had_directive = stripped.second;
        const std::string trimmed = Trim(remainder);
        if (trimmed.empty()) {
            if (had_directive) return std::string();
            return blankline_set ? blankline : remainder;
        }
        if (trimmed.rfind("//", 0) == 0 || trimmed[0] == ';' || trimmed[0] == '*')
            return remainder;
        std::string converted;
        if (TryConvertLinetag(trimmed, &converted)) return converted;

        std::string out = trimmed;
        const bool head_is_text = !(out[0] == '[' || out[0] == '@');
        auto end_is_text = [](const std::string &s) {
            if (s.empty()) return false;
            const char c = s.back();
            return c != ']' && c != '@';
        };
        if (end_is_text(out) && !scp_rules.empty()) ApplyScpRules(&out);
        if (lineend_set && end_is_text(out)) out += lineend;
        if (head_is_text && linehead_set) out = linehead + out;
        return out;
    }
};

} // namespace

void TagIni::Parse(const std::string &content) {
    params_.clear();
    std::string section;
    bool has_section = false;
    size_t pos = 0;
    while (pos <= content.size()) {
        size_t eol = content.find('\n', pos);
        std::string raw = content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        if (eol == std::string::npos) pos = content.size() + 1; else pos = eol + 1;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = Trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#' ||
            line.rfind("//", 0) == 0)
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            has_section = true;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = Trim(line.substr(0, eq));
        const std::string rest = Trim(line.substr(eq + 1));
        if (name.empty()) continue;
        // Numbered entries under a [section] extend that section's table.
        bool numeric = !name.empty();
        for (char c : name) if (!std::isdigit(static_cast<unsigned char>(c))) { numeric = false; break; }
        if (has_section && numeric) {
            const unsigned long index = std::strtoul(name.c_str(), nullptr, 10);
            if (index < 4096) {
                auto &names = params_[section];
                if (names.size() <= index) names.resize(index + 1);
                names[index] = rest;
            }
            continue;
        }
        std::vector<std::string> names;
        size_t s = 0;
        while (s <= rest.size()) {
            size_t comma = rest.find(',', s);
            std::string part = Trim(rest.substr(s, comma == std::string::npos ? std::string::npos : comma - s));
            if (!part.empty()) names.push_back(part);
            if (comma == std::string::npos) break;
            s = comma + 1;
        }
        params_[name] = std::move(names);
    }
}

void InstallTagIniFromText(const std::string &content) {
    auto ini = std::make_unique<TagIni>();
    ini->Parse(content);
    if (ini->Empty()) { g_tag_ini.reset(); return; }
    g_tag_ini = std::move(ini);
}

void ClearTagIni() { g_tag_ini.reset(); }

std::string PreprocessScript(const std::string &content) {
    Preprocessor pp;
    std::string out;
    out.reserve(content.size());
    size_t pos = 0;
    bool first = true;
    while (pos <= content.size()) {
        size_t eol = content.find('\n', pos);
        const bool last = eol == std::string::npos;
        std::string line = content.substr(pos, last ? std::string::npos : eol - pos);
        if (!last && !line.empty() && line.back() == '\r') line.pop_back();
        if (!first) out += '\n';
        first = false;
        out += pp.ProcessLine(line);
        if (last) break;
        pos = eol + 1;
    }
    return out;
}

} // namespace artc
