#pragma once
#include "render/emote_model.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
namespace emote_fixture {
inline artc::PsbValue N(double n){artc::PsbValue v;v.type=artc::PsbValue::Number;v.number=n;return v;}
inline artc::PsbValue S(const char* s){artc::PsbValue v;v.type=artc::PsbValue::String;v.string=s;return v;}
inline artc::PsbValue A(std::initializer_list<artc::PsbValue> a){artc::PsbValue v;v.type=artc::PsbValue::Array;v.array=a;return v;}
inline artc::PsbValue O(std::initializer_list<std::pair<const std::string,artc::PsbValue>> o){artc::PsbValue v;v.type=artc::PsbValue::Object;v.object=o;return v;}
inline artc::PsbValue Key(double time,int type,artc::PsbValue content={}) {
    return O({{"time",N(time)},{"type",N(type)},{"content",std::move(content)}});
}
inline artc::PsbValue Node(int type,const char* label,artc::PsbValue frames,artc::PsbValue children=A({})) {
    return O({{"type",N(type)},{"label",S(label)},{"frameList",std::move(frames)},{"children",std::move(children)},
        {"inheritMask",N(33556476)},{"transformOrder",A({N(0),N(3),N(2),N(1)})}});
}
inline artc::PsbDocument Scene() {
    artc::PsbDocument doc;
    auto image=[&](int width,int height,int red,int green,double ox,double oy) {
        artc::PsbValue resource;resource.type=artc::PsbValue::Resource;resource.resource=doc.resources.size();
        const auto offset=doc.bytes.size();
        for(int i=0;i<width*height;++i)doc.bytes.insert(doc.bytes.end(),{uint8_t(red),uint8_t(green),0,255});
        doc.resources.push_back({offset,doc.bytes.size()-offset});
        return O({{"width",N(width)},{"height",N(height)},{"originX",N(ox)},{"originY",N(oy)},
            {"type",S("RGBA8")},{"pixel",resource}});
    };
    auto body=Node(0,"body",A({Key(0,2,O({{"src",S("src/images/body")}})),Key(11,0)}));
    auto face=Node(0,"face",A({Key(0,2,O({{"src",S("src/images/face")}})),
        Key(10,2,O({{"src",S("src/images/wide")}})),Key(11,0)}));
    face.object["parameterize"]=N(0);
    auto face_motion=O({{"layer",A({face})},{"lastTime",N(11)},{"loopTime",N(-1)},
        {"parameter",A({O({{"id",S("expression")},{"rangeBegin",N(0)},{"rangeEnd",N(10)},{"division",N(10)}})})}});
    auto child=Node(3,"face child",A({Key(0,2,O({{"src",S("motion/actor/face")},{"coord",A({N(0),N(-1),N(0)})}})),Key(11,0)}));
    auto group=Node(2,"root",A({Key(0,3,O({{"src",S("layout")},{"coord",A({N(8),N(8),N(0)})}})),
        Key(10,3,O({{"src",S("layout")},{"coord",A({N(18),N(8),N(0)})}})),Key(11,0)}),A({body,child}));
    auto motion=O({{"layer",A({group})},{"lastTime",N(11)},{"loopTime",N(0)}});
    doc.root=O({{"spec",S("common")},{"source",O({{"images",O({{"icon",O({
        {"body",image(4,4,255,0,2,2)},{"face",image(2,2,0,255,1,1)},{"wide",image(4,2,0,255,2,1)}})}})}})},
        {"object",O({{"actor",O({{"motion",O({{"idle",motion},{"face",face_motion}})}})}})},
        {"metadata",O({{"base",O({{"chara",S("actor")},{"motion",S("idle")}})},
            {"variableList",A({O({{"label",S("expression")}})})}})}});
    return doc;
}

// A playback-ready document: the Scene() graph plus timelineControl entries
// ("loop" main/infinite, "once" main/finite, "delta" difference/infinite), all
// driving the "expression" variable the face node parameterizes on.
inline artc::PsbDocument PlayerDocument() {
    auto doc = Scene();
    auto key = [](double time, double value) {
        return O({{"time", N(time)}, {"type", N(2)},
                  {"content", O({{"value", N(value)}, {"easing", N(0)}})}});
    };
    auto track = [&](double end) {
        return O({{"label", S("expression")}, {"frameList", A({key(0, 0), key(end, end)})}});
    };
    auto timeline = [](const char* label, double last, double begin, double end,
                       double diff, artc::PsbValue variable_list) {
        return O({{"label", S(label)}, {"lastTime", N(last)},
                  {"loopBegin", N(begin)}, {"loopEnd", N(end)}, {"diff", N(diff)},
                  {"variableList", A({std::move(variable_list)})}});
    };
    doc.root.object["metadata"].object["timelineControl"] = A({
        timeline("delta", -1, 0, 20, 1, track(20)),
        timeline("loop", -1, 0, 20, 0, track(20)),
        timeline("once", 60, -1, -1, 0, track(60)),
    });
    return doc;
}

// Minimal PSB serializer for tests: fixed 4-byte array elements/offsets, a
// sum-of-char-codes name trie (bases[i]==i, parents[child]=parent, terminal =
// last node + 1) and resources copied verbatim from the fixture's byte bank.
// Mirrors exactly what the decoder accepts — not a general writer.
inline std::vector<uint8_t> EncodePsb(const artc::PsbDocument& document) {
    std::vector<std::string> names, strings;
    std::map<std::string, uint32_t> name_ids, string_ids;
    auto intern = [](std::vector<std::string>& list,
                     std::map<std::string, uint32_t>& ids, const std::string& s) {
        const auto found = ids.find(s);
        if (found != ids.end()) return found->second;
        const uint32_t id = uint32_t(list.size());
        list.push_back(s);
        ids.emplace(s, id);
        return id;
    };
    std::function<void(const artc::PsbValue&)> collect = [&](const artc::PsbValue& v) {
        switch (v.type) {
        case artc::PsbValue::Object:
            for (const auto& kv : v.object) { intern(names, name_ids, kv.first); collect(kv.second); }
            break;
        case artc::PsbValue::Array:
            for (const auto& e : v.array) collect(e);
            break;
        case artc::PsbValue::String:
            intern(strings, string_ids, v.string);
            break;
        default:
            break;
        }
    };
    collect(document.root);

    // First-fit trie: the decoder pins every child of a parent at
    // bases[parent]+char, so each parent claims one base whose whole char span
    // is free — naive sum-of-char-codes node ids collide across prefixes
    // (e.g. "spec"'s 'c' node vs "angle"'s 'l' node both land on 418).
    std::map<std::string, std::set<unsigned char>> children_of;  // trie edges
    std::set<std::string> complete;                              // full names
    for (const auto& name : names) {
        if (name.empty()) std::abort();
        std::string prefix;
        for (unsigned char c : name) {
            children_of[prefix].insert(c);
            prefix.push_back(char(c));
        }
        complete.insert(prefix);
    }
    std::map<uint32_t, uint32_t> parent_of;   // node -> parent (root = 0)
    std::map<uint32_t, uint32_t> base_of;     // parent node -> claimed base
    std::vector<uint32_t> terminals;
    std::map<std::string, uint32_t> terminal_index;  // name -> position in terminals
    std::vector<bool> used{true};             // node id 0 is the root
    auto take_free = [&]() {
        size_t id = 1;
        while (id < used.size() && used[id]) ++id;
        if (id >= used.size()) used.push_back(true);
        else used[id] = true;
        return uint32_t(id);
    };
    std::map<std::string, uint32_t> node_of{{"", 0}};
    std::vector<std::string> frontier{""};    // BFS keeps the ids compact
    for (size_t at = 0; at < frontier.size(); ++at) {
        const std::string prefix = frontier[at];  // copy: push_back below reallocates
        const uint32_t id = node_of.at(prefix);
        if (complete.count(prefix)) {         // name ends here: terminal link
            const uint32_t terminal = take_free();
            parent_of[terminal] = id;
            terminal_index.emplace(prefix, uint32_t(terminals.size()));
            terminals.push_back(terminal);
        }
        const auto it = children_of.find(prefix);
        if (it == children_of.end()) continue;
        const std::vector<unsigned char> chars(it->second.begin(), it->second.end());
        uint32_t base = 1;                    // first base where base+char is free for all children
        for (;;) {
            bool fits = true;
            for (unsigned char c : chars) {
                while (used.size() <= base + c) used.push_back(false);
                if (used[base + c]) { fits = false; break; }
            }
            if (fits) break;
            ++base;
        }
        base_of[id] = base;
        for (unsigned char c : chars) {
            const uint32_t child = base + c;
            used[child] = true;
            parent_of[child] = id;
            std::string next = prefix;
            next.push_back(char(c));
            node_of[next] = child;
            frontier.push_back(std::move(next));
        }
    }
    size_t max_node = 0;
    for (const auto& p : parent_of) max_node = std::max(max_node, size_t(p.first));

    std::vector<uint8_t> b(44, 0);  // PSB v3 header
    b[0] = 'P'; b[1] = 'S'; b[2] = 'B'; b[3] = 0;
    b[4] = 3;  // version (tests decode v3)
    auto u32at = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (i * 8)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (i * 8))); };
    auto u32array = [&](const std::vector<uint32_t>& a) {
        b.push_back(16);  // width tag: 4-byte elements
        u32(uint32_t(a.size()));
        b.push_back(16);
        for (uint32_t v : a) u32(v);
    };

    u32at(12, uint32_t(b.size()));  // names: bases, parents, terminals
    { std::vector<uint32_t> bases(max_node + 1);
      for (const auto& kv : base_of) bases[kv.first] = kv.second;
      u32array(bases); }
    { std::vector<uint32_t> parent_list(max_node + 1);
      for (const auto& p : parent_of) parent_list[p.first] = p.second;
      u32array(parent_list); }
    u32array(terminals);

    u32at(16, uint32_t(b.size()));  // string offsets, relative to the data base
    { b.push_back(16); u32(uint32_t(strings.size())); b.push_back(16);
      const size_t data_base = b.size() + strings.size() * 4;
      size_t at = data_base;
      for (const auto& s : strings) { u32(uint32_t(at - data_base)); at += s.size() + 1; } }
    u32at(20, uint32_t(b.size()));  // string data
    for (const auto& s : strings) { b.insert(b.end(), s.begin(), s.end()); b.push_back(0); }

    u32at(24, uint32_t(b.size()));  // resource offsets + lengths
    { std::vector<uint32_t> offsets, lengths;
      for (const auto& r : document.resources) { offsets.push_back(uint32_t(r.first)); lengths.push_back(uint32_t(r.second)); }
      u32array(offsets);
      u32at(28, uint32_t(b.size()));  // resource lengths array
      u32array(lengths); }
    u32at(32, uint32_t(b.size()));  // resource data base
    b.insert(b.end(), document.bytes.begin(), document.bytes.end());

    std::function<void(const artc::PsbValue&)> value = [&](const artc::PsbValue& v) {
        switch (v.type) {
        case artc::PsbValue::Null:
            b.push_back(0);
            break;
        case artc::PsbValue::Boolean:
            b.push_back(v.number ? 3 : 2);
            break;
        case artc::PsbValue::Number: {
            const double d = v.number;
            if (d == std::floor(d) && std::abs(d) < 4e9) {
                const int64_t n = int64_t(d);
                const unsigned width = n == 0 ? 0 :
                    (n >= -128 && n <= 127) ? 1 :
                    (n >= -32768 && n <= 32767) ? 2 :
                    (n >= -2147483648LL && n <= 2147483647LL) ? 4 : 8;
                b.push_back(uint8_t(4 + width));
                const uint64_t bits = uint64_t(n);
                for (unsigned i = 0; i < width; ++i) b.push_back(uint8_t(bits >> (i * 8)));
            } else {
                b.push_back(31);
                uint64_t bits;
                std::memcpy(&bits, &d, 8);
                for (int i = 0; i < 8; ++i) b.push_back(uint8_t(bits >> (i * 8)));
            }
            break;
        }
        case artc::PsbValue::String: {
            const uint32_t id = string_ids.at(v.string);
            const unsigned width = id < 256 ? 1 : id < 65536 ? 2 : id < 16777216 ? 3 : 4;
            b.push_back(uint8_t(20 + width));
            for (unsigned i = 0; i < width; ++i) b.push_back(uint8_t(id >> (i * 8)));
            break;
        }
        case artc::PsbValue::Resource: {
            if (v.extra) std::abort();  // fixture never uses extended resource refs
            const uint32_t id = v.resource;
            const unsigned width = id < 256 ? 1 : id < 65536 ? 2 : id < 16777216 ? 3 : 4;
            b.push_back(uint8_t(24 + width));
            for (unsigned i = 0; i < width; ++i) b.push_back(uint8_t(id >> (i * 8)));
            break;
        }
        case artc::PsbValue::Array: {
            b.push_back(32);
            b.push_back(16); u32(uint32_t(v.array.size())); b.push_back(16);
            const size_t slots = b.size();
            for (size_t i = 0; i < v.array.size(); ++i) u32(0);  // placeholder offsets
            const size_t base = b.size();  // offsets are relative to here
            for (size_t i = 0; i < v.array.size(); ++i) {
                u32at(slots + 4 * i, uint32_t(b.size() - base));
                value(v.array[i]);
            }
            break;
        }
        case artc::PsbValue::Object: {
            b.push_back(33);
            b.push_back(16); u32(uint32_t(v.object.size())); b.push_back(16);
            // keys reference the names table, which the decoder fills in
            // terminals order — the index is the terminal's position.
            for (const auto& kv : v.object) u32(terminal_index.at(kv.first));
            b.push_back(16); u32(uint32_t(v.object.size())); b.push_back(16);
            const size_t slots = b.size();
            for (size_t i = 0; i < v.object.size(); ++i) u32(0);
            const size_t base = b.size();
            size_t i = 0;
            for (const auto& kv : v.object) {
                u32at(slots + 4 * i, uint32_t(b.size() - base));
                value(kv.second);
                ++i;
            }
            break;
        }
        }
    };
    u32at(36, uint32_t(b.size()));  // root value
    value(document.root);
    return b;
}
}
