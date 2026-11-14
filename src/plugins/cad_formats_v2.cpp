// ============================================================================
// cad_formats_v2.cpp — registration of the "v2" CAD importers (portable).
//
//   odbpp            ODB++ job (directory / .tgz / .tar.gz / .tar / .zip)
//   ipc2581          IPC-2581 XML
//   kicad.pcb        KiCad .kicad_pcb
//   eagle.brd        EAGLE 6+ XML board
//   pads.asc         PADS ASCII
//   allegro.extract  Cadence Allegro extract text
//   allegro.brd      Cadence Allegro 17.x binary board (.brd)
//   mentor.neutral   Mentor Boardstation / Expedition neutral file
//   fabmaster        FABmaster FATF
//   dxf.outline      DXF board outline (category "Outline")
//
// Called from register_builtin_plugins(). All importers share
// io::CadImportOptions (see src/io/cad_common.h).
// ============================================================================
#include "plugin.h"
#include "../io/allegro_brd.h"
#include "../io/allegro_extract.h"
#include "../io/cad_common.h"
#include "../io/dxf_outline.h"
#include "../io/eagle_brd.h"
#include "../io/fabmaster.h"
#include "../io/io_util.h"
#include "../io/ipc2581.h"
#include "../io/kicad_pcb.h"
#include "../io/mentor_neutral.h"
#include "../io/odbpp.h"
#include "../io/pads_ascii.h"

#include <functional>

namespace pcbp {
namespace {

using SniffFn = std::function<int(const std::string& path, const std::string& head)>;
using RunFn = std::function<bool(const std::string&, Project&, const io::CadImportOptions&, io::CadImportResult&, std::string&)>;

std::string ext_lower(const std::string& path) {
    auto s = path.find_last_of("/\\");
    auto d = path.find_last_of('.');
    if (d == std::string::npos || (s != std::string::npos && d < s)) return {};
    return io::to_lower(path.substr(d + 1));
}

class CadV2Importer final : public Importer {
public:
    CadV2Importer(PluginInfo i, SniffFn s, RunFn r) : inf_(std::move(i)), sniff_(std::move(s)), run_(std::move(r)) {}
    const PluginInfo& info() const override { return inf_; }
    int sniff(const std::string& path, const std::string& head) const override {
        int s = sniff_(path, head);
        const std::string e = ext_lower(path);
        bool ext = false;
        for (auto& x : inf_.extensions) if (x == e) ext = true;
        if (s > 0 && ext) s = std::min(99, s + 3);
        return s;
    }
    bool run(const std::string& path, const Options& o0, Project& prj, ImportReport& rep, std::string& err) override {
        Options o = default_options(inf_);
        for (auto& kv : o0) o[kv.first] = kv.second;
        io::CadImportOptions co;
        co.replace = opt_bool(o, "replace", true);
        co.import_board = opt_bool(o, "import_board", true);
        co.import_packages = opt_bool(o, "import_packages", true);
        co.fiducial_by_name = opt_bool(o, "fiducial_by_name", true);
        co.arc_seg_deg = opt_num(o, "arc_seg_deg", 10.0);
        co.step = opt(o, "step");
        co.layer = opt(o, "layer");
        co.units = opt(o, "units");
        co.scale = opt_num(o, "scale", 1.0);
        if (co.units == "auto") co.units.clear();
        io::CadImportResult res;
        if (!run_(path, prj, co, res, err)) return false;
        rep.components_added = res.components;
        rep.packages_added = res.packages;
        rep.parts_added = res.parts;
        rep.messages.push_back(res.describe());
        for (auto& w : res.warnings) rep.messages.push_back(w);
        return true;
    }
private:
    PluginInfo inf_;
    SniffFn sniff_;
    RunFn run_;
};

OptionDef bdef(const std::string& k, const std::string& l, bool d) {
    OptionDef o; o.key = k; o.label = l; o.type = OptionDef::Type::Bool; o.def = d ? "1" : "0"; return o;
}
OptionDef sdef(const std::string& k, const std::string& l, const std::string& d, const std::string& tip) {
    OptionDef o; o.key = k; o.label = l; o.type = OptionDef::Type::String; o.def = d; o.tooltip = tip; return o;
}
OptionDef udef() {
    OptionDef o; o.key = "units"; o.label = "Units"; o.type = OptionDef::Type::Enum;
    o.choices = {"auto", "mm", "inch", "mil"}; o.def = "auto"; o.tooltip = "Override the units found in the file";
    return o;
}

PluginInfo cad_info(const std::string& id, const std::string& name, std::vector<std::string> ext, const std::string& cat = "CAD") {
    PluginInfo i;
    i.id = id; i.name = name; i.vendor = "PCB Plotter";
    i.kind = PluginKind::Importer;
    i.extensions = std::move(ext);
    i.default_ext = i.extensions.empty() ? "" : i.extensions[0];
    i.category = cat;
    i.import_mode = ImportMode::Merge;
    if (cat == "CAD")
        i.options = {bdef("replace", "Replace existing components / packages", true),
                     bdef("import_board", "Import board outline", true),
                     bdef("import_packages", "Import packages", true),
                     bdef("fiducial_by_name", "Refs FID* / FID packages are fiducials", true)};
    return i;
}

} // namespace

void register_cad_formats_v2(PluginRegistry& r) {
    {
        PluginInfo i = cad_info("odbpp", "ODB++ job", {"tgz", "gz", "tar", "zip", "odb"});
        i.options.push_back(sdef("step", "Step", "", "Step name (empty = 'pcb' or first step)"));
        r.add(std::make_unique<CadV2Importer>(i, io::sniff_odbpp, io::import_odbpp));
    }
    {
        PluginInfo i = cad_info("ipc2581", "IPC-2581", {"xml", "cvg", "ipc2581"});
        i.options.push_back(sdef("step", "Step", "", "Step name (empty = first)"));
        i.options.push_back(udef());
        r.add(std::make_unique<CadV2Importer>(i, io::sniff_ipc2581, io::import_ipc2581_file));
    }
    r.add(std::make_unique<CadV2Importer>(cad_info("kicad.pcb", "KiCad board", {"kicad_pcb"}), io::sniff_kicad_pcb,
                                          io::import_kicad_pcb_file));
    r.add(std::make_unique<CadV2Importer>(cad_info("eagle.brd", "EAGLE board", {"brd"}), io::sniff_eagle_brd,
                                          io::import_eagle_brd_file));
    {
        PluginInfo i = cad_info("pads.asc", "PADS ASCII", {"asc", "txt"});
        i.options.push_back(udef());
        r.add(std::make_unique<CadV2Importer>(i, io::sniff_pads_ascii, io::import_pads_ascii_file));
    }
    {
        PluginInfo i = cad_info("allegro.extract", "Allegro extract", {"txt", "rpt", "extract"});
        i.options.push_back(udef());
        r.add(std::make_unique<CadV2Importer>(i, io::sniff_allegro_extract, io::import_allegro_extract_file));
    }
    {
        PluginInfo i = cad_info("allegro.brd", "Allegro board (binary)", {"brd"});
        i.options.push_back(udef());
        r.add(std::make_unique<CadV2Importer>(i, io::sniff_allegro_brd, io::import_allegro_brd_file));
    }
    {
        PluginInfo i = cad_info("mentor.neutral", "Mentor neutral file", {"neu", "neutral"});
        i.options.push_back(udef());
        r.add(std::make_unique<CadV2Importer>(i, io::sniff_mentor_neutral, io::import_mentor_neutral_file));
    }
    {
        PluginInfo i = cad_info("fabmaster", "FABmaster FATF", {"fab", "fatf", "asc"});
        i.options.push_back(udef());
        r.add(std::make_unique<CadV2Importer>(i, io::sniff_fabmaster, io::import_fabmaster_file));
    }
    {
        PluginInfo i = cad_info("dxf.outline", "DXF board outline", {"dxf"}, "Outline");
        i.options = {bdef("replace", "Replace existing board outline", true),
                     sdef("layer", "Layer(s)", "", "Comma-separated layer names; empty = all layers"),
                     udef()};
        i.options.back().tooltip = "Override $INSUNITS";
        r.add(std::make_unique<CadV2Importer>(i, io::sniff_dxf, io::import_dxf_outline_file));
    }
}

} // namespace pcbp
