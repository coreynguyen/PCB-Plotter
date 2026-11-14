#pragma once
// ============================================================================
// selection_sets.h — named selection sets (3ds Max style) stored in a Project.
// Additive: Project::selection_sets + two hooks in project_io.cpp.
// ============================================================================
#include "json.h"
#include <string>
#include <vector>

namespace pcbp {

struct SelectionSet {
    std::string name;
    std::vector<std::string> refs;     // component references (case-sensitive)
};

inline json::Value selection_sets_to_json(const std::vector<SelectionSet>& v) {
    json::Value a = json::Value::array();
    for (auto& s : v) {
        json::Value o = json::Value::object();
        o.set("name", s.name);
        json::Value r = json::Value::array();
        for (auto& x : s.refs) r.push(x);
        o.set("refs", r);
        a.push(o);
    }
    return a;
}

inline std::vector<SelectionSet> selection_sets_from_json(const json::Value& a) {
    std::vector<SelectionSet> v;
    for (auto& o : a.items()) {
        SelectionSet s;
        s.name = o["name"].str();
        for (auto& r : o["refs"].items()) s.refs.push_back(r.str());
        if (!s.name.empty()) v.push_back(std::move(s));
    }
    return v;
}

} // namespace pcbp
