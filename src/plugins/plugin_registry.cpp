// ============================================================================
// plugin_registry.cpp — registry, option helpers, MachineJob JSON.
// ============================================================================
#include "plugin.h"
#include "../core/fileio.h"
#include "../core/json.h"
#include <algorithm>
#include <cstdlib>

namespace pcbp {

double opt_num(const Options& o, const std::string& k, double def) {
    auto it = o.find(k);
    if (it == o.end() || it->second.empty()) return def;
    char* end = nullptr;
    double v = std::strtod(it->second.c_str(), &end);
    return end == it->second.c_str() ? def : v;
}

std::string ImportReport::summary() const {
    std::string s;
    auto add = [&](int n, const char* what) {
        if (!n) return;
        if (!s.empty()) s += ", ";
        s += std::to_string(n) + " " + what;
    };
    add(components_added, "components added");
    add(components_updated, "components updated");
    add(packages_added, "packages");
    add(parts_added, "part numbers");
    add(layers_added, "layers");
    if (s.empty()) s = "nothing imported";
    return s;
}

int Importer::sniff(const std::string& path, const std::string& head) const {
    (void)head;
    std::string ext = file_ext_lower(path);
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    for (auto& e : info().extensions) if (e == ext) return 50;
    return 0;
}

PluginRegistry& PluginRegistry::get() {
    static PluginRegistry r;
    return r;
}

void PluginRegistry::add(std::unique_ptr<Importer> p) {
    for (auto& x : imp_) if (x->info().id == p->info().id) { x = std::move(p); return; }
    imp_.push_back(std::move(p));
}
void PluginRegistry::add(std::unique_ptr<Exporter> p) {
    for (auto& x : exp_) if (x->info().id == p->info().id) { x = std::move(p); return; }
    exp_.push_back(std::move(p));
}

Importer* PluginRegistry::importer(const std::string& id) const {
    for (auto& x : imp_) if (x->info().id == id) return x.get();
    return nullptr;
}
Exporter* PluginRegistry::exporter(const std::string& id) const {
    for (auto& x : exp_) if (x->info().id == id) return x.get();
    return nullptr;
}

Importer* PluginRegistry::detect(const std::string& path) const {
    std::vector<uint8_t> data;
    std::string err, head;
    if (read_file(path, data, err)) head.assign((const char*)data.data(), std::min<size_t>(data.size(), 8192));
    Importer* best = nullptr;
    int best_score = 0;
    for (auto& x : imp_) {
        int s = x->sniff(path, head);
        if (s > best_score) { best_score = s; best = x.get(); }
    }
    return best;
}

Options default_options(const PluginInfo& info) {
    Options o;
    for (auto& d : info.options) o[d.key] = d.def;
    return o;
}

std::string machine_job_to_json(const MachineJob& j) {
    using json::Value;
    Value o = Value::object();
    o.set("format", "PCBPlotter.MachineJob");
    o.set("version", 1);
    o.set("program_name", j.program_name); o.set("assembly", j.assembly); o.set("assembly_rev", j.assembly_rev);
    o.set("customer", j.customer); o.set("point_name", j.point_name); o.set("machine", j.machine);
    o.set("board_w", j.board_w); o.set("board_h", j.board_h); o.set("board_t", j.board_t);
    o.set("bottom_side", j.bottom_side); o.set("single_board", j.single_board);
    Value pl = Value::array();
    for (auto& p : j.placements) {
        Value q = Value::object();
        q.set("ref", p.ref); q.set("part", p.part); q.set("package", p.package); q.set("desc", p.description);
        q.set("x", p.x); q.set("y", p.y); q.set("rot", p.rot); q.set("circuit", p.circuit);
        q.set("skip", p.skip); q.set("polarized", p.polarized); q.set("smt", p.smt);
        q.set("leads", p.leads); q.set("height", p.height);
        pl.push(q);
    }
    o.set("placements", pl);
    Value pa = Value::array();
    for (auto& p : j.parts) {
        Value q = Value::object();
        q.set("part", p.part); q.set("package", p.package); q.set("desc", p.description);
        q.set("length", p.length); q.set("width", p.width); q.set("height", p.height);
        q.set("leads", p.leads); q.set("polarized", p.polarized); q.set("smt", p.smt); q.set("count", p.count);
        pa.push(q);
    }
    o.set("parts", pa);
    Value fi = Value::array();
    for (auto& f : j.fiducials) {
        Value q = Value::object();
        q.set("name", f.name); q.set("x", f.x); q.set("y", f.y); q.set("diameter", f.diameter);
        q.set("circuit", f.circuit); q.set("local", f.local);
        fi.push(q);
    }
    o.set("fiducials", fi);
    Value ci = Value::array();
    for (auto& c : j.circuits) {
        Value q = Value::object();
        q.set("index", c.index); q.set("name", c.name); q.set("x", c.x); q.set("y", c.y);
        q.set("rot", c.rot); q.set("skip", c.skip);
        ci.push(q);
    }
    o.set("circuits", ci);
    Value op = Value::object();
    for (auto& kv : j.options) op.set(kv.first, kv.second);
    o.set("options", op);
    return o.dump(1);
}

} // namespace pcbp
