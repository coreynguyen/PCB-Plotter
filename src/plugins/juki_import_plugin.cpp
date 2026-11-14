// ============================================================================
// juki_import_plugin.cpp — "juki.hlc" importer: JUKI machine program
// (H8H/H7H/H5H/H4H, E41..E46, E51, ISS XML) -> new Project (io/juki_import).
// ============================================================================
#include "plugin.h"
#include "../core/fileio.h"
#include "../io/juki_import.h"

#include <algorithm>

namespace pcbp {

namespace {

class JukiImporter final : public Importer {
public:
    JukiImporter() {
        inf_.id = "juki.hlc";
        inf_.name = "Pick-and-place program (HLC / ISS)";
        inf_.vendor = "PCB Plotter";
        inf_.kind = PluginKind::Importer;
        inf_.extensions = {"h8h", "h7h", "h5h", "h4h", "e41", "e42", "e43", "e44", "e45", "e46", "e51", "iss"};
        inf_.default_ext = "h8h";
        inf_.import_mode = ImportMode::Project;
        inf_.category = "Machine";
        inf_.builtin = true;
        OptionDef p; p.key = "point"; p.label = "Model point name"; p.def = "Top SMT";
        OptionDef k; k.key = "packages"; k.label = "Reconstruct packages from component records";
        k.type = OptionDef::Type::Bool; k.def = "1";
        inf_.options = {p, k};
    }
    const PluginInfo& info() const override { return inf_; }
    int sniff(const std::string& path, const std::string& head) const override {
        const int s = juki::sniff_juki(std::vector<uint8_t>(head.begin(), head.end()));
        const int e = Importer::sniff(path, head);
        return std::max(s, e);
    }
    bool run(const std::string& path, const Options& o, Project& prj, ImportReport& rep, std::string& err) override {
        std::vector<uint8_t> data;
        if (!read_file(path, data, err)) return false;
        juki::ImportOptions io;
        io.point_name = opt(o, "point", "Top SMT");
        if (io.point_name.empty()) io.point_name = "Top SMT";
        io.reconstruct_packages = opt_bool(o, "packages", true);
        juki::ImportResult r;
        if (!juki::import_juki(data, prj, io, r, err)) return false;
        prj.info.original_filename = path;
        rep.components_added = r.placements + r.fiducials;
        rep.parts_added = r.parts;
        rep.packages_added = r.packages;
        rep.messages.push_back(r.format + " program -> machine '" + r.exporter_id + "', " + std::to_string(r.images) +
                               " image(s)" + (r.step_repeat ? " (step & repeat)" : ""));
        for (auto& w : r.warnings) rep.messages.push_back(w);
        return true;
    }
private:
    PluginInfo inf_;
};

} // namespace

void register_juki_import(PluginRegistry& r) { r.add(std::make_unique<JukiImporter>()); }

} // namespace pcbp
