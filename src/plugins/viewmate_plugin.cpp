// ============================================================================
// viewmate_plugin.cpp — "viewmate.bin" importer: ViewMate job database (*.bin)
// -> new Project with one Gerber layer per job layer (io/viewmate_bin).
//
// Jobs are self-contained (geometry is embedded, not referenced), so no
// "locate missing files" step is needed. Each layer is rendered to RS-274X in
// the job's own integer grid and stored as embedded layer content; name,
// colour (job palette), visibility, negative polarity (%IPNEG) and the layer
// type (-> role / side) are carried over. Detection is by signature, so the
// generic .bin extension never claims unrelated files.
// ============================================================================
#include "plugin.h"
#include "../core/fileio.h"
#include "../io/gerber.h"
#include "../io/viewmate_bin.h"

#include <algorithm>

namespace pcbp {

namespace {

void map_role(int type, const std::string& name, const std::string& content, GerberLayerRef& r) {
    using R = LayerRole;
    auto set = [&](R role, Side s) { r.role = role; r.side = s; };
    switch (type) {
    case 5: set(R::Copper, Side::Top); return;
    case 6: set(R::Copper, Side::Bottom); return;
    case 7: set(R::Silk, Side::Top); return;
    case 8: set(R::Silk, Side::Bottom); return;
    case 9: set(R::Mask, Side::Top); return;
    case 10: set(R::Mask, Side::Bottom); return;
    case 11: case 146: set(R::Paste, Side::Top); return;
    case 12: case 147: set(R::Paste, Side::Bottom); return;
    case 2: case 3: case 4: case 135: case 136: case 137: case 138: case 150: case 151: set(R::Drill, Side::Top); return;
    case 34: case 139: case 140: case 141: case 142: case 143: set(R::Outline, Side::Top); return;
    case 148: set(R::Assembly, Side::Top); return;
    case 149: set(R::Assembly, Side::Bottom); return;
    default: break;
    }
    if (type == 0 || (type >= 13 && type <= 32) || (type >= 36 && type <= 134)) { set(R::Copper, Side::Top); return; }
    // OTH / NFL / PAD / SCG / PLA: fall back to the file-name guess
    gerber::LayerGuess g = gerber::guess_layer_function(name, content);
    r.role = (LayerRole)std::clamp(g.role, 0, 7);
    r.side = g.side == 1 ? Side::Bottom : Side::Top;
}

class ViewMateImporter final : public Importer {
public:
    ViewMateImporter() {
        inf_.id = "viewmate.bin";
        inf_.name = "ViewMate job (.bin)";
        inf_.vendor = "PCB Plotter";
        inf_.kind = PluginKind::Importer;
        inf_.extensions = {"bin"};
        inf_.default_ext = "bin";
        inf_.import_mode = ImportMode::Project;
        inf_.category = "Gerber";
        inf_.builtin = true;
        OptionDef a; a.key = "skip_empty"; a.label = "Skip layers without geometry"; a.type = OptionDef::Type::Bool; a.def = "1";
        OptionDef b; b.key = "job_colors"; b.label = "Use the job's layer colours"; b.type = OptionDef::Type::Bool; b.def = "1";
        inf_.options = {a, b};
    }
    const PluginInfo& info() const override { return inf_; }
    int sniff(const std::string&, const std::string& head) const override {
        return io::vm::is_job((const uint8_t*)head.data(), head.size()) ? 95 : 0;   // signature only: .bin is generic
    }
    bool run(const std::string& path, const Options& o, Project& prj, ImportReport& rep, std::string& err) override {
        io::vm::Job job;
        if (!io::vm::read_job_file(path, job, err)) return false;
        const bool skip_empty = opt_bool(o, "skip_empty", true), colors = opt_bool(o, "job_colors", true);
        if (prj.info.name.empty()) prj.info.name = file_stem(path);
        prj.info.original_filename = path;
        prj.display_units = job.metric() ? Units::MM : Units::Inch;
        char b[256];
        std::snprintf(b, sizeof b, "ViewMate job V%d, %s database, resolution %s, %zu layers, %zu D-codes, %zu elements",
                      job.version, job.metric() ? "metric" : "imperial",
                      job.metric() ? (job.resolution == 1000 ? "1 nm" : "10 nm") : (job.resolution == 1000 ? "0.1 uin" : "1 uin"),
                      job.layers.size(), job.apertures.size(), job.elements.size());
        rep.messages.push_back(b);
        rep.messages.push_back("Geometry is embedded in the job (no external Gerber files are referenced).");
        const std::string dir = file_dir(path);
        int skipped = 0;
        for (auto& L : job.layers) {
            int np = 0, nt = 0, na = 0, ng = 0, nr = 0;
            for (auto& e : job.elements) {
                if (e.layer != L.index) continue;
                switch (e.kind) {
                case io::vm::Element::Pad: ++np; break;
                case io::vm::Element::Trace: ++nt; break;
                case io::vm::Element::Arc: ++na; break;
                case io::vm::Element::Polygon: ++ng; break;
                case io::vm::Element::Route: ++nr; break;
                }
            }
            const int total = np + nt + na + ng + nr;
            if (total == 0 && (skip_empty || L.name.empty())) { ++skipped; continue; }
            GerberLayerRef r;
            r.name = L.name.empty() ? "Layer " + std::to_string(L.index) : L.name;
            r.path = (dir.empty() ? "" : dir + "/") + r.name;
            std::vector<std::string> warns;
            r.content = io::vm::to_gerber(job, L.index, &warns);
            map_role(L.type, r.name, r.content, r);
            r.visible = L.visible();
            if (colors && L.color >= 1 && L.color <= 15) r.color = job.palette[L.color];
            prj.layers.push_back(std::move(r));
            ++rep.layers_added;
            std::snprintf(b, sizeof b, "Layer %d '%s' (%s%s): %d pads, %d traces, %d arcs, %d polygons, %d routes", L.index,
                          L.name.c_str(), io::vm::type_code(L.type).c_str(), L.negative() ? ", negative" : "", np, nt, na, ng, nr);
            rep.messages.push_back(b);
            for (auto& w : warns) rep.messages.push_back("  " + w);
        }
        if (skipped) rep.messages.push_back(std::to_string(skipped) + " empty layer(s) skipped");
        for (auto& w : job.warnings) rep.messages.push_back(w);
        if (!job.unknown_records.empty()) {
            std::string t = "Preference / view records not imported (tag x count):";
            for (auto& [tag, n] : job.unknown_records) t += " " + std::to_string(tag) + "x" + std::to_string(n);
            rep.messages.push_back(t);
        }
        if (prj.layers.empty()) { err = "the job contains no layer geometry"; return false; }
        return true;
    }
private:
    PluginInfo inf_;
};

} // namespace

void register_viewmate_import(PluginRegistry& r) { r.add(std::make_unique<ViewMateImporter>()); }

} // namespace pcbp
