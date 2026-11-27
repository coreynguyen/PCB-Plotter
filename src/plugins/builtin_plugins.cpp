// ============================================================================
// builtin_plugins.cpp — built-in importers / exporters (portable, no windows.h).
//
// Importers
//   circuitcam.cpf  CircuitCAM project / library (.cpf / .mdb, Jet database)
//   gencad          GenCAD 1.4                      ipc356     IPC-D-356(A)
//   centroid        ASCII centroid / XY             bom        Bill of Materials
//   gerber          RS-274X / RS-274D layer         excellon   Excellon drill
//   pcbp.json       native project
// Exporters
//   machine jobs:   juki.h8h/h7h/h5h/h4h, juki.e41..e46/e51, generic.csv, generic.xy
//   project:        gencad, bom.csv, pcbp.json
//
// CircuitCAM import follows specs/CPF_FORMAT.md (units: mils; LayerID 1 top /
// 2 bottom; package graphics from Packages.Rendering; component -> model point
// assignments from the Assembly.ComponentAssignments blob, §5 — best effort,
// no sample data exists: decoded records are applied only when the CompID /
// ModelAssignID resolve to an imported location / model point).
// ============================================================================
#include "plugin.h"
#include "../core/fileio.h"
#include "../core/json.h"
#include "../core/project_io.h"
#include "../io/bom_import.h"
#include "../io/cc_archive.h"
#include "../io/centroid_import.h"
#include "../io/gencad.h"
#include "../io/gerber.h"
#include "../io/io_util.h"
#include "../io/ipc356.h"
#include "../io/jet_mdb.h"
#include "../io/juki_writer.h"
#include "../io/text_table.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <iomanip>
#include <locale>
#include <sstream>
#include <set>

namespace pcbp {
// defined in cad_formats_v2.cpp / viewmate_plugin.cpp. Declared here, at namespace scope: declared inside the
// function that calls them, MSVC 14.51 looked for ::register_* at link time (LNK2001 in PcbpThumb).
void register_cad_formats_v2(PluginRegistry& r);
void register_viewmate_import(PluginRegistry& r);
namespace {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
OptionDef opt_bool_def(const std::string& key, const std::string& label, bool def, const std::string& tip = {}) {
    OptionDef d; d.key = key; d.label = label; d.type = OptionDef::Type::Bool; d.def = def ? "1" : "0"; d.tooltip = tip;
    return d;
}
OptionDef opt_enum_def(const std::string& key, const std::string& label, std::vector<std::string> choices,
                       const std::string& def, const std::string& tip = {}) {
    OptionDef d; d.key = key; d.label = label; d.type = OptionDef::Type::Enum; d.choices = std::move(choices);
    d.def = def; d.tooltip = tip;
    return d;
}
OptionDef opt_num_def(const std::string& key, const std::string& label, const std::string& def,
                      bool integer = false, const std::string& tip = {}) {
    OptionDef d; d.key = key; d.label = label; d.type = integer ? OptionDef::Type::Int : OptionDef::Type::Double;
    d.def = def; d.tooltip = tip;
    return d;
}
OptionDef opt_str_def(const std::string& key, const std::string& label, const std::string& def = {},
                      const std::string& tip = {}) {
    OptionDef d; d.key = key; d.label = label; d.type = OptionDef::Type::String; d.def = def; d.tooltip = tip;
    return d;
}
OptionDef opt_path_def(const std::string& key, const std::string& label, const std::string& tip = {}) {
    OptionDef d; d.key = key; d.label = label; d.type = OptionDef::Type::Path; d.tooltip = tip;
    return d;
}

std::string ext_of(const std::string& path) {
    std::string e = file_ext_lower(path);
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return e;
}
bool has_ext(const PluginInfo& inf, const std::string& path) {
    const std::string e = ext_of(path);
    for (auto& x : inf.extensions) if (x == e) return true;
    return false;
}
std::string lower(std::string s) { return io::to_lower(s); }

void put(std::vector<uint8_t>& out, const std::string& s) { out.insert(out.end(), s.begin(), s.end()); }

std::string fmtd(double v, int dec) { return io::fmt_fixed(v, dec); }

// Generic function-backed importer / exporter (keeps registration compact).
class FnImporter final : public Importer {
public:
    using SniffFn = std::function<int(const PluginInfo&, const std::string&, const std::string&)>;
    using RunFn = std::function<bool(const std::string&, const Options&, Project&, ImportReport&, std::string&)>;
    FnImporter(PluginInfo i, SniffFn s, RunFn r) : inf_(std::move(i)), sniff_(std::move(s)), run_(std::move(r)) {}
    const PluginInfo& info() const override { return inf_; }
    int sniff(const std::string& path, const std::string& head) const override {
        return sniff_ ? sniff_(inf_, path, head) : Importer::sniff(path, head);
    }
    bool run(const std::string& path, const Options& o, Project& prj, ImportReport& rep, std::string& err) override {
        Options full = default_options(inf_);
        for (auto& kv : o) full[kv.first] = kv.second;
        return run_(path, full, prj, rep, err);
    }
private:
    PluginInfo inf_;
    SniffFn sniff_;
    RunFn run_;
};

class FnExporter final : public Exporter {
public:
    using JobFn = std::function<bool(const MachineJob&, const Options&, std::vector<uint8_t>&, std::string&)>;
    using PrjFn = std::function<bool(const Project&, const Options&, std::vector<uint8_t>&, std::string&)>;
    FnExporter(PluginInfo i, JobFn j) : inf_(std::move(i)), job_(std::move(j)) {}
    FnExporter(PluginInfo i, PrjFn p) : inf_(std::move(i)), prj_(std::move(p)) {}
    const PluginInfo& info() const override { return inf_; }
    bool export_job(const MachineJob& j, const Options& o, std::vector<uint8_t>& out, std::string& err) override {
        if (!job_) return Exporter::export_job(j, o, out, err);
        return job_(j, merged(o), out, err);
    }
    bool export_project(const Project& p, const Options& o, std::vector<uint8_t>& out, std::string& err) override {
        if (!prj_) return Exporter::export_project(p, o, out, err);
        return prj_(p, merged(o), out, err);
    }
private:
    Options merged(const Options& o) const {
        Options full = default_options(inf_);
        for (auto& kv : o) full[kv.first] = kv.second;
        return full;
    }
    PluginInfo inf_;
    JobFn job_;
    PrjFn prj_;
};

PluginInfo make_info(const std::string& id, const std::string& name, PluginKind kind,
                     std::vector<std::string> exts, const std::string& category) {
    PluginInfo i;
    i.id = id; i.name = name; i.vendor = "PCB Plotter"; i.kind = kind;
    i.extensions = std::move(exts);
    i.default_ext = i.extensions.empty() ? std::string() : i.extensions[0];
    i.category = category;
    i.builtin = true;
    return i;
}

// ===========================================================================
// CircuitCAM .cpf / cclib.mdb
// ===========================================================================
constexpr double kMil = 0.0254;

struct Tab {
    jet::Table t;
    bool ok = false;
    size_t size() const { return ok ? t.rows.size() : 0; }
    const jet::Value* v(size_t r, const char* c) const {
        const jet::Value* x = ok ? t.get(r, c) : nullptr;
        return (x && !x->null) ? x : nullptr;
    }
    std::string s(size_t r, const char* c) const { auto x = v(r, c); return x ? io::trim(x->as_string()) : std::string(); }
    double d(size_t r, const char* c, double def = 0) const { auto x = v(r, c); return x ? x->as_double() : def; }
    int64_t i(size_t r, const char* c, int64_t def = 0) const { auto x = v(r, c); return x ? x->as_int() : def; }
    bool b(size_t r, const char* c) const { auto x = v(r, c); return x && x->as_bool(); }
    bool has(size_t r, const char* c) const { return v(r, c) != nullptr; }
    const std::vector<uint8_t>* blob(size_t r, const char* c) const {
        auto x = v(r, c);
        return (x && !x->bin.empty()) ? &x->bin : nullptr;
    }
};

bool load_tab(const jet::Database& db, const char* name, Tab& t, ImportReport& rep) {
    if (!db.has_table(name)) return false;
    std::string err;
    t.ok = db.read_table(name, t.t, err);
    if (!t.ok) rep.messages.push_back(std::string("Table ") + name + ": " + err);
    return t.ok;
}

// GPrims -> polylines (mm). Text is skipped; circles / arcs are segmented.
void gprims_to_polys(const std::vector<GPrim>& v, std::vector<std::vector<Vec2>>& out) {
    for (const GPrim& g : v) {
        switch (g.kind) {
        case GKind::Group: gprims_to_polys(g.children, out); break;
        case GKind::Line:
        case GKind::Polyline:
            if (g.pts.size() >= 2) out.push_back(g.pts);
            break;
        case GKind::Polygon:
            if (g.pts.size() >= 2) { auto p = g.pts; p.push_back(p.front()); out.push_back(p); }
            break;
        case GKind::Rect:
            if (g.pts.size() >= 2) {
                const Vec2 a = g.pts[0], b = g.pts[1], c = (a + b) * 0.5;
                std::vector<Vec2> p = {{a.x, a.y}, {b.x, a.y}, {b.x, b.y}, {a.x, b.y}, {a.x, a.y}};
                if (g.rot != 0) for (auto& q : p) q = rotate_about(q, c, g.rot);
                out.push_back(p);
            }
            break;
        case GKind::Circle:
        case GKind::Arc: {
            if (g.pts.empty() || g.radius <= 0) break;
            const double a0 = g.kind == GKind::Circle ? 0 : g.a0;
            const double sw = g.kind == GKind::Circle ? 360 : (g.a1 == 0 ? 360 : g.a1);
            const int n = std::max(8, (int)std::ceil(std::fabs(sw) / 5.0));
            std::vector<Vec2> p;
            for (int k = 0; k <= n; ++k) {
                const double a = deg2rad(a0 + sw * k / n);
                p.push_back({g.pts[0].x + g.radius * std::cos(a), g.pts[0].y + g.radius * std::sin(a)});
            }
            out.push_back(p);
            break;
        }
        default: break;
        }
    }
}

bool blob_polys(const std::vector<uint8_t>* b, std::vector<std::vector<Vec2>>& out, std::string& err) {
    if (!b) return false;
    std::vector<GPrim> g;
    if (!cc::decode_graphics(b->data(), b->size(), g, err)) return false;
    const size_t before = out.size();
    gprims_to_polys(g, out);
    return out.size() > before;
}

// ---- Assembly.ComponentAssignments (spec §5) -------------------------------
struct BlobReader {
    const uint8_t* p; size_t n, pos = 0; bool ok = true;
    bool need(size_t k) { if (!ok || n - pos < k) ok = false; return ok; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = p[pos] | (p[pos + 1] << 8) | (p[pos + 2] << 16) | ((uint32_t)p[pos + 3] << 24); pos += 4; return v; }
    int32_t i32() { return (int32_t)u32(); }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = uint16_t(p[pos] | (p[pos + 1] << 8)); pos += 2; return v; }
    uint8_t u8() { if (!need(1)) return 0; return p[pos++]; }
    double f64() { if (!need(8)) return 0; double d; std::memcpy(&d, p + pos, 8); pos += 8; return d; }
    std::string cstr() {
        std::string s;
        bool wide = false;
        uint32_t len = u8();
        if (len == 0xFF) {
            uint16_t w = u16(); len = w;
            if (w == 0xFFFE) {
                wide = true;
                len = u8();
                if (len == 0xFF) { w = u16(); len = w; if (w == 0xFFFF) len = u32(); }
            } else if (w == 0xFFFF) len = u32();
        }
        if (!ok) return s;
        if (!wide) {
            if (!need(len)) return s;
            for (uint32_t k = 0; k < len; ++k) utf8_append(s, p[pos + k]);
            pos += len;
        } else {
            if (len > (n - pos) / 2) { ok = false; return s; }
            for (uint32_t k = 0; k < len; ++k) { utf8_append(s, uint32_t(p[pos] | (p[pos + 1] << 8))); pos += 2; }
        }
        return s;
    }
    bool count(uint32_t& c, uint32_t limit = 1000000) { c = u32(); if (c > limit) ok = false; return ok; }
};

struct CompAssignRec { int32_t comp_id = 0, model_assign_id = 0, point_key = 0; double mach_rot = 0; int32_t mach_rot_sts = 0; };

bool decode_comp_assignments(const std::vector<uint8_t>& blob, std::vector<CompAssignRec>& recs, std::string& err) {
    std::vector<uint8_t> pl;
    if (!cc::unwrap_blob(blob.data(), blob.size(), pl, err)) return false;
    BlobReader r{pl.data(), pl.size()};
    if (r.u32() != 0x551F4FD1u) { err = "bad ComponentAssignments magic"; return false; }
    r.u32();            // version
    r.cstr();           // name
    uint32_t np = 0;
    if (!r.count(np, 100000)) { err = "bad point count"; return false; }
    for (uint32_t k = 0; k < np && r.ok; ++k) {
        const uint32_t ver = r.u32();
        const int32_t key = r.i32();
        if (ver >= 2) { r.u32(); r.u32(); }
        if (ver >= 3) r.u32();
        r.cstr();
        r.u32();
        for (int list = 0; list < 4 && r.ok; ++list) {      // id lists A/B, string lists A/B
            uint32_t c = 0;
            if (!r.count(c)) break;
            for (uint32_t j = 0; j < c && r.ok; ++j) {
                if (list < 2) r.u32();
                else r.cstr();
            }
        }
        if (!r.ok) break;
        uint32_t na = 0;
        if (!r.count(na)) break;
        for (uint32_t j = 0; j < na && r.ok; ++j) {
            CompAssignRec a;
            int32_t f[11];
            for (int q = 0; q < 11; ++q) f[q] = r.i32();
            a.comp_id = f[1]; a.model_assign_id = f[3]; a.point_key = key;
            r.u16(); r.i32();
            a.mach_rot = r.f64();
            r.i32();
            for (int q = 0; q < 4; ++q) r.f64();
            if (ver >= 4) { a.mach_rot_sts = r.i32(); r.i32(); }
            if (r.ok) recs.push_back(a);
        }
    }
    if (!r.ok) { err = "ComponentAssignments truncated after " + std::to_string(recs.size()) + " records"; return !recs.empty(); }
    return true;
}

uint32_t point_palette(int i) {
    static const uint32_t pal[] = {0xFF3C8CFF, 0xFFE8590C, 0xFF7048E8, 0xFF2F9E44, 0xFFF59F00,
                                   0xFFC2255C, 0xFF1098AD, 0xFF5C940D, 0xFF862E9C, 0xFFD9480F};
    return pal[(unsigned)i % (sizeof(pal) / sizeof(pal[0]))];
}

PointKind point_kind_for(const std::string& pt, const std::string& ptype, bool population) {
    const std::string s = lower(pt + " " + ptype);
    if (population) {
        if (s.find("hand") != std::string::npos || s.find("manual") != std::string::npos ||
            s.find("slide") != std::string::npos)
            return PointKind::Manual;
        return PointKind::Placement;
    }
    if (s.find("inspect") != std::string::npos || s.find("aoi") != std::string::npos || s.find("x-ray") != std::string::npos)
        return PointKind::Inspection;
    if (s.find("test") != std::string::npos || s.find("ict") != std::string::npos) return PointKind::Test;
    return PointKind::Process;
}

// CircuitCAM machine-type ids (ProcessPts.MachineType / MachineSetups.MachType),
// from the AisMachineManager type-name table.  Only the JUKI ones have exporters here.
std::string cc_machine_name(int64_t t) {
    switch (t) {
    case 1: return "Custom Machine";
    case 12: return "MYDATA Basic Interface";
    case 25: return "MYDATA Advanced Interface";
    case 22: return "Fuji CP Series";
    case 33: return "Siemens SIPLACE Interface";
    case 57: return "HLC interface";
    case 96: return "IS interface";
    case 92: return "Mirtec AOI Interface";
    default: return t ? "Legacy machine type " + std::to_string(t) : "NONE";
    }
}

// MachType 57 (Juki HLC): the HLC program flavour is chosen inside the
// machine plug-in; its default target file name is JUKI.H5H / .H4H / .E41..E48 / .E51 /
// .H7H, so the TargetFile extension identifies the format.
std::string cc_machine_exporter(int64_t mtype, const std::string& target_file) {
    if (mtype != 57) return "generic.csv";
    const std::string ext = lower(file_ext_lower(target_file));
    static const char* known[] = {".h8h", ".h7h", ".h5h", ".h4h", ".e41", ".e42", ".e43", ".e44", ".e45", ".e46", ".e51"};
    for (const char* k : known) if (ext == k) return std::string("juki.") + (k + 1);
    return "juki.h7h";      // plug-in default case
}

bool import_cpf(const std::string& path, const Options& o, Project& prj, ImportReport& rep, std::string& err) {
    jet::Database db;
    if (!db.open(path, err)) return false;
    const bool lib_only = opt_bool(o, "library_only");
    if (!db.has_table("Packages") && !db.has_table("Numbers")) {
        err = "not a legacy CAM database (no Packages / Numbers tables)";
        return false;
    }
    rep.messages.push_back("Jet " + std::to_string(db.version()) + " database, " +
                           std::to_string(db.tables().size()) + " tables");
    if (prj.info.name.empty()) prj.info.name = file_stem(path);
    prj.info.original_filename = path;

    // ---- Packages ----
    Tab pk;
    int render_ok = 0, render_fail = 0, lead_mismatch = 0;
    if (load_tab(db, "Packages", pk, rep)) {
        for (size_t r = 0; r < pk.size(); ++r) {
            Package p;
            p.name = pk.s(r, "Package");
            if (p.name.empty()) continue;
            p.description = pk.s(r, "Description");
            p.leads = (int)pk.i(r, "nLeads");
            p.j_lead = pk.b(r, "IsJLead"); p.radial = pk.b(r, "IsRadial"); p.notched = pk.b(r, "IsNotched");
            p.pin_marked = pk.b(r, "IsPinMarked"); p.shape_confirmed = pk.b(r, "ShapeConfirmed");
            p.default_process = pk.s(r, "DefaultProcessPt");
            bool decoded = false;
            if (auto b = pk.blob(r, "Rendering")) {
                std::string e;
                if (cc::decode_package_rendering(b->data(), b->size(), p, e)) { decoded = true; ++render_ok; }
                else { ++render_fail; rep.messages.push_back("Package " + p.name + ": rendering not decoded (" + e + ")"); }
            }
            // IsSMT is 0 for every library row, so it cannot be trusted alone:
            // a rendering with round pads (CGrEllipse) is through-hole.
            bool round = false;
            for (auto& d : p.pads) if (d.shape == PadShape::Round) round = true;
            p.smt = pk.b(r, "IsSMT") || !round;
            if (!p.leads) p.leads = (int)p.pads.size();
            if (decoded && (int)p.pads.size() != p.leads) ++lead_mismatch;
            if (p.body.empty()) p.auto_body();
            const bool existed = prj.packages.count(p.name) != 0;
            prj.packages[p.name] = std::move(p);
            if (!existed) ++rep.packages_added;
        }
    }
    rep.messages.push_back("Packages: " + std::to_string(pk.size()) + " rows, " + std::to_string(render_ok) +
                           " renderings decoded" + (render_fail ? ", " + std::to_string(render_fail) + " failed" : "") +
                           (lead_mismatch ? ", " + std::to_string(lead_mismatch) + " with pad count != nLeads" : ""));

    // ---- Numbers ----
    Tab nu;
    if (load_tab(db, "Numbers", nu, rep)) {
        for (size_t r = 0; r < nu.size(); ++r) {
            PartNumber n;
            n.number = nu.s(r, "Number");
            if (n.number.empty()) continue;
            n.description = nu.s(r, "Description"); n.package = nu.s(r, "Package");
            n.polarized = nu.b(r, "Polarized"); n.socket = nu.b(r, "Socket");
            n.cust_pn = nu.s(r, "CustPN"); n.vend_pn = nu.s(r, "VendPN"); n.manf_pn = nu.s(r, "ManfPN");
            n.part_rev = nu.s(r, "PartRev"); n.soft_ver = nu.s(r, "SoftVer"); n.stock_code = nu.s(r, "StockCode");
            n.bin = nu.s(r, "Bin"); n.line_item = nu.s(r, "LineItem"); n.default_process = nu.s(r, "DefaultProcessPt");
            n.pop_type = nu.s(r, "PopType"); n.part_marking = nu.s(r, "PartMarking");
            n.height = nu.d(r, "Height") * kMil;      // mils [I]
            n.cost = nu.d(r, "Cost");
            const bool existed = prj.parts.count(n.number) != 0;
            prj.parts[n.number] = std::move(n);
            if (!existed) ++rep.parts_added;
        }
    }
    rep.messages.push_back("Numbers: " + std::to_string(nu.size()) + " rows");
    if (lib_only) {
        rep.messages.push_back("Library mode: only Packages and Numbers imported");
        return true;
    }

    // ---- Assembly ----
    Tab as;
    const std::vector<uint8_t>* assign_blob = nullptr;
    const std::vector<uint8_t>* asm_outline = nullptr;
    std::string model_name;
    if (load_tab(db, "Assembly", as, rep) && as.size() > 0) {
        AssemblyInfo& a = prj.info;
        auto set = [&](std::string& dst, const char* c) { std::string v = as.s(0, c); if (!v.empty()) dst = v; };
        set(a.name, "Name"); set(a.number, "Number"); set(a.description, "Description"); set(a.originator, "Originator");
        set(a.bom_number, "BOMNumber"); set(a.assembly_rev, "AssemblyRev"); set(a.bom_rev, "BOMRev");
        set(a.manf_rev, "ManfRev"); set(a.standard, "Standard"); set(a.customer, "Customer"); set(a.url, "AssemblyURL");
        set(a.custom[0], "Custom1"); set(a.custom[1], "Custom2"); set(a.custom[2], "Custom3"); set(a.custom[3], "Custom4");
        set(a.created, "Created"); set(a.modified, "Modified"); set(a.original_filename, "OriginalFilename");
        prj.virtual_origin = {as.d(0, "OrgX") * kMil, as.d(0, "OrgY") * kMil};
        model_name = as.s(0, "Model");
        assign_blob = as.blob(0, "ComponentAssignments");
        asm_outline = as.blob(0, "Outline");
        if (as.size() > 1) rep.messages.push_back("Assembly: " + std::to_string(as.size()) + " rows, first used");
    } else {
        rep.messages.push_back("Assembly: no row (library database?)");
    }

    // ---- Images / ImageLocs ----
    Tab im, il;
    int64_t root_image = -1;
    const std::vector<uint8_t>* img_outline = nullptr;
    if (load_tab(db, "Images", im, rep)) {
        for (size_t r = 0; r < im.size(); ++r)
            if (im.b(r, "IsRoot") || root_image < 0) {
                if (root_image >= 0 && !im.b(r, "IsRoot")) continue;
                root_image = im.i(r, "ImageID"); img_outline = im.blob(r, "ImageOutline");
            }
        if (im.size() > 1)
            rep.messages.push_back("Images: " + std::to_string(im.size()) +
                                   " distinct board images; all locations imported into the root image");
    }
    if (load_tab(db, "ImageLocs", il, rep) && il.size() > 0) {
        std::vector<size_t> order;
        for (size_t r = 0; r < il.size(); ++r) order.push_back(r);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return il.b(a, "IsRoot") > il.b(b, "IsRoot");
        });
        const Vec2 root_at{il.d(order[0], "X") * kMil, il.d(order[0], "Y") * kMil};
        prj.images.clear();
        int k = 0;
        for (size_t r : order) {
            PanelImage pi;
            pi.name = std::to_string(++k);
            pi.offset = Vec2{il.d(r, "X") * kMil, il.d(r, "Y") * kMil} - root_at;
            pi.rot = il.d(r, "Rot");
            pi.root = il.b(r, "IsRoot");
            pi.x_out = il.b(r, "XOut");
            pi.x_index = (int)il.i(r, "XIndex"); pi.y_index = (int)il.i(r, "YIndex");
            prj.images.push_back(pi);
        }
        rep.messages.push_back("ImageLocs: " + std::to_string(prj.images.size()) + " panel images");
    }

    // ---- board / panel outline ----
    {
        std::string e;
        prj.board_frame.clear();
        if (img_outline && !blob_polys(img_outline, prj.board_frame, e) && !e.empty())
            rep.messages.push_back("Images.ImageOutline: " + e);
        if (asm_outline) {
            auto& dst = prj.board_frame.empty() ? prj.board_frame : prj.panel_frame;
            e.clear();
            if (!blob_polys(asm_outline, dst, e) && !e.empty()) rep.messages.push_back("Assembly.Outline: " + e);
        }
        // CircuitCAM draws frames as separate CGrLine segments: chain them into closed outlines
        prj.board_frame = join_frame_segments(prj.board_frame);
        prj.panel_frame = join_frame_segments(prj.panel_frame);
        if (!prj.board_frame.empty()) prj.board_outline_src = OutlineSource::Imported;   // R16 provenance (CircuitCAM outline)
        if (!prj.panel_frame.empty()) prj.panel_outline_src = OutlineSource::Imported;
        if (!prj.board_frame.empty()) {
            const BBox cb = prj.circuit_frame_bbox();
            rep.messages.push_back("Circuit frame: " + std::to_string(prj.board_frame.size()) + " outline(s), " +
                                   io::fmt_fixed(cb.w(), 2) + " x " + io::fmt_fixed(cb.h(), 2) + " mm");
        }
        if (!prj.panel_frame.empty()) {
            const BBox pb = prj.panel_frame_bbox();
            rep.messages.push_back("Panel frame: " + io::fmt_fixed(pb.w(), 2) + " x " + io::fmt_fixed(pb.h(), 2) + " mm");
        }
    }

    // ---- Locations ----
    Tab lo;
    std::map<int64_t, uint32_t> loc_to_comp;
    if (load_tab(db, "Locations", lo, rep)) {
        int fids = 0, feats = 0, bottom = 0;
        std::vector<std::pair<size_t, int64_t>> local_owner;   // comp index -> LocalFidID
        std::set<uint32_t> used_ids;
        for (const auto& c : prj.comps) used_ids.insert(c.id);
        for (size_t r = 0; r < lo.size(); ++r) {
            Component c;
            c.ref = lo.s(r, "Ref");
            c.pos = {lo.d(r, "X") * kMil, lo.d(r, "Y") * kMil};
            // Locations.Rot is exactly what CircuitCAM's Board View shows (and what
            // Set Neutralized Angles copies into the machine angle) -> the one rotation.
            // Locations.MachRot (0 in every real file) is kept as legacy comparison only.
            c.rot = norm_deg(lo.d(r, "Rot"));
            if (lo.has(r, "MachRot") && lo.d(r, "MachRot") != 0) {
                c.has_legacy_mach_rot = true; c.legacy_mach_rot = norm_deg(lo.d(r, "MachRot"));
            }
            c.side = lo.i(r, "LayerID", 1) == 2 ? Side::Bottom : Side::Top;
            if (c.side == Side::Bottom) ++bottom;
            c.number = lo.s(r, "Number");
            c.package = lo.s(r, "Package");
            c.url = lo.s(r, "URL");
            c.cad_pos = (lo.has(r, "XC") && lo.has(r, "YC")) ? Vec2{lo.d(r, "XC") * kMil, lo.d(r, "YC") * kMil} : c.pos;
            c.rot_imported = c.rot;
            const int ft = (int)lo.i(r, "FiducialType");
            const int feat = (int)lo.i(r, "FeatureType");
            if (ft > 0) {
                c.feature = Feature::Fiducial;
                c.fid_scope = fid_scope_from_cpf(ft);
                c.fid_number = (int)lo.i(r, "FiducialNumber");
                ++fids;
            } else if (feat != 0) {
                c.feature = Feature::Hole;     // non-component feature class [I]
                ++feats;
            }
            // label box (Long, mils). Reference frame is unknown (spec open question 7):
            // treat as absolute when its centre is closer to the centroid than to (0,0).
            const double L = (double)lo.i(r, "LabelLeft"), R = (double)lo.i(r, "LabelRight");
            const double T = (double)lo.i(r, "LabelTop"), B = (double)lo.i(r, "LabelBottom");
            if (L != 0 || R != 0 || T != 0 || B != 0) {
                Vec2 ctr{(L + R) * 0.5 * kMil, (T + B) * 0.5 * kMil};
                // Confirmed on Project1.cpf: values are small (e.g. -25..25 / 16..40) => centroid-relative mils.
                // Keep a fallback for absolute boxes from other writers.
                if (dist(ctr, c.pos) < ctr.len()) ctr = ctr - c.pos;
                c.label_off = ctr;
                c.label_rot = (double)lo.i(r, "LabelRot");
                c.label_moved = true;
            }
            const int64_t lid = lo.i(r, "LocationID");
            if (lid > 0 && lid < 0x7FFFFFFF && !used_ids.count((uint32_t)lid)) c.id = (uint32_t)lid;
            Component& added = prj.add_component(c);
            used_ids.insert(added.id);
            if (lid > 0) loc_to_comp[lid] = added.id;
            if (c.feature == Feature::Fiducial && c.fid_scope == FiducialScope::Local && lo.has(r, "LocalFidID"))
                local_owner.push_back({prj.comps.size() - 1, lo.i(r, "LocalFidID")});
            ++rep.components_added;
        }
        for (auto& lw : local_owner) {
            auto it = loc_to_comp.find(lw.second);
            if (it != loc_to_comp.end()) prj.comps[lw.first].local_fid_owner = (int)it->second;
        }
        rep.messages.push_back("Locations: " + std::to_string(lo.size()) + " rows (" + std::to_string(fids) +
                               " fiducials, " + std::to_string(feats) + " other features, " +
                               std::to_string(bottom) + " bottom side)");
        if (bottom) rep.messages.push_back("Bottom-side coordinates are top-view board coordinates (not mirrored)");
    }
    prj.link_bom();

    // ---- Factory model ----
    Tab mo, pp, pt, ma, msu;
    load_tab(db, "Models", mo, rep);
    load_tab(db, "ProcessPts", pp, rep);
    load_tab(db, "ProcessTypes", pt, rep);
    load_tab(db, "ModelAssignments", ma, rep);
    load_tab(db, "MachineSetups", msu, rep);
    std::map<int64_t, int> assign_to_point;
    if (ma.size() == 0) prj.model = FactoryModel::make_legacy_default();   // legacy .cpf: unchanged default
    if (ma.size() > 0) {
        if (model_name.empty())
            for (size_t r = 0; r < mo.size(); ++r) if (mo.b(r, "IsDefault")) { model_name = mo.s(r, "Model"); break; }
        if (model_name.empty() && mo.size() > 0) model_name = mo.s(0, "Model");
        std::string model_desc;
        for (size_t r = 0; r < mo.size(); ++r) if (io::ieq(mo.s(r, "Model"), model_name)) model_desc = mo.s(r, "Description");
        std::map<std::string, bool> ptype_pop;
        for (size_t r = 0; r < pt.size(); ++r) ptype_pop[lower(pt.s(r, "ProcessType"))] = pt.b(r, "IsPopulation");
        struct PPInfo { std::string type; int64_t mtype = 0; std::string desc; };
        std::map<std::string, PPInfo> ppinfo;
        for (size_t r = 0; r < pp.size(); ++r)
            ppinfo[lower(pp.s(r, "ProcessPt"))] = {pp.s(r, "ProcessType"), pp.i(r, "MachineType"), pp.s(r, "Description")};
        std::map<int64_t, size_t> setup_row;
        for (size_t r = 0; r < msu.size(); ++r) setup_row[msu.i(r, "ModelAssignID")] = r;

        std::vector<size_t> rows;
        for (size_t r = 0; r < ma.size(); ++r)
            if (model_name.empty() || ma.s(r, "Model").empty() || io::ieq(ma.s(r, "Model"), model_name)) rows.push_back(r);
        std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
            const int64_t sa = ma.i(a, "Sequence"), sb = ma.i(b, "Sequence");
            if ((sa > 0) != (sb > 0)) return sa > 0;
            if (sa != sb) return sa < sb;
            return ma.i(a, "ModelAssignID") < ma.i(b, "ModelAssignID");
        });
        if (!rows.empty()) {
            FactoryModel fm;
            fm.name = model_name.empty() ? "Imported" : model_name;
            fm.description = model_desc;
            int machines = 0;
            for (size_t r : rows) {
                ModelPoint m;
                const std::string ptname = ma.s(r, "ProcessPt");
                m.name = ma.s(r, "Name");
                if (m.name.empty()) m.name = ptname.empty() ? "Point " + std::to_string(fm.points.size() + 1) : ptname;
                const PPInfo info = ppinfo.count(lower(ptname)) ? ppinfo[lower(ptname)] : PPInfo{};
                bool pop = ptype_pop.count(lower(info.type)) ? ptype_pop[lower(info.type)] : (info.mtype != 0);
                m.process_type = info.type.empty() ? ptname : info.type;
                m.kind = point_kind_for(ptname, info.type, pop);
                m.inspection = m.kind == PointKind::Inspection;
                m.color = point_palette((int)fm.points.size());
                m.side = ma.i(r, "PreferredLayer") == 2 ? Side::Bottom : Side::Top;
                if (!ptname.empty()) m.filter_process.push_back(ptname);
                m.notes = ma.s(r, "Description");
                const int64_t aid = ma.i(r, "ModelAssignID");
                auto su = setup_row.find(aid);
                if (su != setup_row.end()) {
                    const size_t q = su->second;
                    m.has_machine = true;
                    MachineSetup& s = m.machine;
                    s.exporter_id = cc_machine_exporter(msu.i(q, "MachType"), msu.s(q, "TargetFile"));
                    s.options["cc.mach_type"] = std::to_string(msu.i(q, "MachType"));
                    s.options["cc.mach_name"] = cc_machine_name(msu.i(q, "MachType"));
                    s.flipped = msu.b(q, "Flipped");
                    const double ox = msu.d(q, "XOrg"), oy = msu.d(q, "YOrg");
                    if (ox != 0 || oy != 0) { s.has_origin = true; s.origin = {ox * kMil, oy * kMil}; }
                    {   // "Board Orientation" combo index 0..3 = 0/90/180/270 deg CCW (real panel sample: 1);
                        // tolerate degree values from other writers
                        const int ov = (int)msu.i(q, "Orientation");
                        s.orientation = (ov >= 0 && ov <= 3) ? ov * 90 : ((ov / 90 * 90) % 360 + 360) % 360;
                    }
                    s.force_angle_calc = msu.has(q, "CalcAngles") ? msu.b(q, "CalcAngles") : true;
                    s.hard_offset = {msu.d(q, "XOffset") * kMil, msu.d(q, "YOffset") * kMil};   // OffUnits assumed mils [I]
                    if (msu.i(q, "OffUnits")) s.options["cc.off_units"] = std::to_string(msu.i(q, "OffUnits"));
                    s.single_board = msu.b(q, "SingleImage");
                    s.side = msu.i(q, "ProgSide") == 2 ? Side::Bottom : Side::Top;
                    s.target_dir = msu.s(q, "TargetDir"); s.target_file = msu.s(q, "TargetFile");
                    s.use_cad_coords = msu.b(q, "UseCadCentroids");
                    ++machines;
                } else if (info.mtype != 0 && m.kind == PointKind::Placement) {
                    m.has_machine = true;
                    m.machine.exporter_id = cc_machine_exporter(info.mtype, "");
                    m.machine.options["cc.mach_type"] = std::to_string(info.mtype);
                    m.machine.options["cc.mach_name"] = cc_machine_name(info.mtype);
                    ++machines;
                }
                assign_to_point[aid] = (int)fm.points.size();
                fm.points.push_back(std::move(m));
            }
            // ModelFilters rows whose ModelAssignID has no ModelAssignments row are stale
            // template leftovers (Project1.cpf: 33 rows, none for its only point).
            Tab mf;
            if (load_tab(db, "ModelFilters", mf, rep) && mf.size() > 0) {
                int live = 0;
                for (size_t r = 0; r < mf.size(); ++r) if (assign_to_point.count(mf.i(r, "ModelAssignID"))) ++live;
                rep.messages.push_back("ModelFilters: " + std::to_string(mf.size()) + " rows, " + std::to_string(live) +
                                       " for existing points (filter codes not mapped)");
            }
            prj.model = std::move(fm);
            prj.model.migrate_export_dir();   // R19a: legacy TargetDir -> project output folder
            rep.messages.push_back("Factory model '" + prj.model.name + "': " + std::to_string(prj.model.points.size()) +
                                   " points, " + std::to_string(machines) + " with machine setups");
        }
    }

    // ---- component assignments ----
    if (assign_blob) {
        std::vector<CompAssignRec> recs;
        std::string e;
        const bool ok = decode_comp_assignments(*assign_blob, recs, e);
        if (!e.empty()) rep.messages.push_back("ComponentAssignments: " + e);
        int applied = 0, legacy_diff = 0;
        if (ok) {
            std::map<uint32_t, size_t> id_to_idx;
            for (size_t k = 0; k < prj.comps.size(); ++k) id_to_idx[prj.comps[k].id] = k;
            for (auto& a : recs) {
                auto lc = loc_to_comp.find(a.comp_id);
                if (lc == loc_to_comp.end()) continue;
                auto pi = assign_to_point.find(a.model_assign_id);
                if (pi == assign_to_point.end()) pi = assign_to_point.find(a.point_key);
                if (pi == assign_to_point.end()) continue;
                Component& c = prj.comps[id_to_idx[lc->second]];
                const PointKind nk = prj.model.points[pi->second].kind;
                const bool new_pop = nk == PointKind::Placement || nk == PointKind::Manual;
                bool cur_pop = false;
                if (c.assigned_point >= 0) {
                    const PointKind ck = prj.model.points[c.assigned_point].kind;
                    cur_pop = ck == PointKind::Placement || ck == PointKind::Manual;
                }
                if (c.assigned_point < 0 || (new_pop && !cur_pop)) {
                    if (c.assigned_point < 0) ++applied;
                    c.assigned_point = pi->second;
                    // per-point CircuitCAM machine angle (written by Calculate/Adjust/
                    // Neutralize; exported verbatim when CalcAngles=0).
                    // Comparison only: rot stays the Board View angle (ANGLES.md §4).
                    c.has_legacy_mach_rot = true;
                    c.legacy_mach_rot = norm_deg(a.mach_rot);
                    if (std::fabs(norm_deg(a.mach_rot - c.rot + 1e-9)) > 1e-3 &&
                        std::fabs(norm_deg(a.mach_rot - c.rot + 1e-9) - 360.0) > 1e-3) ++legacy_diff;
                }
            }
        }
        rep.messages.push_back("ComponentAssignments: " + std::to_string(recs.size()) + " records, " +
                               std::to_string(applied) + " components assigned");
        if (legacy_diff)
            rep.messages.push_back("Machine angles: " + std::to_string(legacy_diff) +
                                   " placement(s) had a CircuitCAM machine angle different from the Board View "
                                   "angle; PCB Plotter uses the Board View angle (see Preview machine angles)");
    }
    return true;
}

// ===========================================================================
// text-format sniffers
// ===========================================================================
int sniff_gencad(const PluginInfo& inf, const std::string& path, const std::string& head) {
    if (head.find("$HEADER") != std::string::npos && io::icontains(head, "GENCAD")) return 95;
    if (head.find("$HEADER") != std::string::npos && head.find("$BOARD") != std::string::npos) return 80;
    return has_ext(inf, path) ? 30 : 0;
}

int sniff_ipc356(const PluginInfo& inf, const std::string& path, const std::string& head) {
    int recs = 0;
    for (auto& l : io::split_lines(head)) {
        if (l.size() >= 3 && (l.compare(0, 3, "317") == 0 || l.compare(0, 3, "327") == 0 ||
                              l.compare(0, 3, "017") == 0 || l.compare(0, 3, "027") == 0))
            ++recs;
        if (l.compare(0, 3, "P  ") == 0 && (l.find("JOB") != std::string::npos || l.find("UNITS") != std::string::npos))
            recs += 3;
    }
    if (recs >= 3) return 90;
    return has_ext(inf, path) ? 40 : 0;
}

int sniff_centroid(const PluginInfo& inf, const std::string& path, const std::string& head) {
    const bool ext = has_ext(inf, path);
    if (head.empty()) return ext ? 20 : 0;
    io::CentroidOptions co;
    io::CentroidReport cr;
    std::string err;
    // Drop a possibly truncated last line.
    std::string h = head;
    if (h.size() >= 8192) { auto p = h.find_last_of('\n'); if (p != std::string::npos) h.resize(p); }
    if (!io::parse_centroid_text(h, co, cr, err) || !cr.map.valid() || cr.rows_parsed.size() < 2) return ext ? 15 : 0;
    // BOM consolidated refs ("C1-C5, C7") are not centroid rows.
    return ext ? 60 : 35;
}

int sniff_bom(const PluginInfo& inf, const std::string& path, const std::string& head) {
    const bool ext = has_ext(inf, path);
    io::TextTable t;
    std::string err, h = head;
    if (h.size() >= 8192) { auto p = h.find_last_of('\n'); if (p != std::string::npos) h.resize(p); }
    if (!io::read_text_table(h, {}, t, err) || !t.has_header) return ext ? 10 : 0;
    io::BomMapping m = io::auto_map_bom(t);
    bool has_xy = false;
    for (auto& c : t.columns) {
        const std::string k = io::norm_key(c);
        if (k == "x" || k == "y" || k == "posx" || k == "posy" || k == "centerx" || k == "centery" ||
            k.rfind("midx", 0) == 0 || k.rfind("midy", 0) == 0)
            has_xy = true;
    }
    if (m.valid() && !has_xy) return ext ? 70 : 45;
    return ext ? 10 : 0;
}

bool has_dcode_pattern(const std::string& h) {
    int hits = 0;
    for (size_t i = 0; i + 3 < h.size() && hits < 4; ++i) {
        if (h[i] == 'D' && (h[i + 1] == '0' || h[i + 1] == '1' || h[i + 1] == '2' || h[i + 1] == '3')) {
            size_t j = i + 1;
            while (j < h.size() && std::isdigit((unsigned char)h[j])) ++j;
            if (j < h.size() && h[j] == '*' && j - i <= 4) ++hits;
        }
    }
    return hits >= 2;
}

int sniff_gerber(const PluginInfo& inf, const std::string& path, const std::string& head) {
    if (gerber::looks_like_excellon(head)) return 0;
    if (gerber::looks_like_rs274x(head)) return 85;
    const bool ext = has_ext(inf, path);
    if (has_dcode_pattern(head) && (head.find('X') != std::string::npos || head.find('Y') != std::string::npos))
        return ext ? 75 : 45;
    return ext ? 20 : 0;
}

int sniff_excellon(const PluginInfo& inf, const std::string& path, const std::string& head) {
    if (gerber::looks_like_excellon(head)) return has_ext(inf, path) ? 90 : 70;
    const std::string e = ext_of(path);
    return (e == "drl" || e == "xln") ? 30 : 0;
}

// ===========================================================================
// Gerber layer helpers
// ===========================================================================
void guess_layer_role(const std::string& path, GerberLayerRef& L) {
    const std::string e = ext_of(path);
    const std::string n = lower(file_stem(path));
    auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
    LayerRole role = LayerRole::Other;
    bool bottom = false;
    if (e == "gtl") role = LayerRole::Copper;
    else if (e == "gbl") { role = LayerRole::Copper; bottom = true; }
    else if (e == "gts") role = LayerRole::Mask;
    else if (e == "gbs") { role = LayerRole::Mask; bottom = true; }
    else if (e == "gto") role = LayerRole::Silk;
    else if (e == "gbo") { role = LayerRole::Silk; bottom = true; }
    else if (e == "gtp") role = LayerRole::Paste;
    else if (e == "gbp") { role = LayerRole::Paste; bottom = true; }
    else if (e == "gko" || e == "gm1") role = LayerRole::Outline;
    else if (e == "drl" || e == "xln") role = LayerRole::Drill;
    if (role == LayerRole::Other) {
        if (has("silk") || has("legend") || has("overlay")) role = LayerRole::Silk;
        else if (has("mask")) role = LayerRole::Mask;
        else if (has("paste") || has("stencil") || has("cream")) role = LayerRole::Paste;
        else if (has("assembly") || has("assy")) role = LayerRole::Assembly;
        else if (has("drill") || has("ncdrill")) role = LayerRole::Drill;
        else if (has("outline") || has("profile") || has("edge") || has("contour")) role = LayerRole::Outline;
        else if (has("trace") || has("copper") || has("signal") || has("cu") || has("layer")) role = LayerRole::Copper;
    }
    if (has("bot") || has("bottom") || has("solder side") || has("b.cu") || has("back")) bottom = true;
    L.role = role;
    L.side = bottom ? Side::Bottom : Side::Top;
    switch (role) {
    case LayerRole::Copper: L.color = bottom ? 0xFF3C78D8 : 0xFFC08040; break;
    case LayerRole::Paste: L.color = 0xFFA0A0A0; break;
    case LayerRole::Mask: L.color = 0xFF2F9E44; break;
    case LayerRole::Silk: L.color = 0xFFE0E0E0; break;
    case LayerRole::Assembly: L.color = 0xFFF59F00; break;
    case LayerRole::Drill: L.color = 0xFF808080; break;
    case LayerRole::Outline: L.color = 0xFFFFD43B; break;
    default: L.color = 0xFFC08040; break;
    }
}

bool import_gerber_layer(const std::string& path, const Options& o, Project& prj, ImportReport& rep,
                         std::string& err, bool excellon) {
    std::string text;
    if (!io::read_file(path, text, err)) return false;
    GerberLayerRef L;
    L.name = file_stem(path);
    L.path = path;
    L.content = text;
    guess_layer_role(path, L);
    if (excellon) { L.role = LayerRole::Drill; L.color = 0xFF808080; }
    gerber::ParseOptions po;
    po.kind = excellon ? gerber::ParseOptions::Kind::Excellon : gerber::ParseOptions::Kind::Auto;
    if (!excellon && !gerber::looks_like_rs274x(text)) {
        // RS-274D: needs an aperture list
        std::string apath = opt(o, "aperture_list");
        std::string atext;
        if (!apath.empty()) {
            if (!io::read_file(apath, atext, err)) return false;
        } else if (opt_bool(o, "auto_aperture_list", true)) {
            const std::string dir = file_dir(path);
            const char* names[] = {"Aperture List", "Aperture List.txt", "Aperture List/Aperture List.txt",
                                   "apertures.txt", "aperture.lst", "apertures.lst"};
            for (const char* nm : names) {
                std::string e2, cand = dir.empty() ? std::string(nm) : dir + "/" + nm;
                if (io::read_file(cand, atext, e2) && !atext.empty()) { apath = cand; break; }
                atext.clear();
            }
        }
        if (!atext.empty()) {
            const std::string u = opt(o, "aperture_units", "auto");
            gerber::ListUnits lu = u == "mil" ? gerber::ListUnits::Mil : u == "inch" ? gerber::ListUnits::Inch
                                 : u == "mm" ? gerber::ListUnits::MM : gerber::ListUnits::Auto;
            auto al = gerber::parse_aperture_list(atext, lu);
            po.apertures = al.aps;
            L.aperture_list = atext;
            if (u != "auto") L.units_hint = u;
            rep.messages.push_back("Aperture list " + file_stem(apath) + ": " + std::to_string(al.aps.size()) + " apertures");
            auto cands = gerber::guess_format(text, al.aps);
            if (!cands.empty()) {
                const gerber::Format& f = cands[0].fmt;
                po.fmt = f;
                L.format = std::string(f.inch ? "in " : "mm ") + std::to_string(f.int_digits) + "." +
                           std::to_string(f.frac_digits) + (f.lead_supp ? " L" : " T");
                rep.messages.push_back("RS-274D format guess: " + f.describe() + " -> \"" + L.format + "\"");
            }
        } else {
            rep.messages.push_back("RS-274D file without aperture list (assign one in the layer properties)");
        }
    }
    gerber::Layer gl;
    std::string perr;
    if (gerber::parse(text, po, gl, perr)) {
        rep.messages.push_back(L.name + ": " + std::to_string(gl.flash_count()) + " flashes, " +
                               std::to_string(gl.draw_count()) + " draws, " + std::to_string(gl.region_count()) +
                               " regions, " + std::to_string(gl.aps.size()) + " apertures (" + gl.fmt.describe() + ")");
        if (!gl.warnings.empty()) rep.messages.push_back(std::to_string(gl.warnings.size()) + " parser warnings, first: " + gl.warnings[0]);
    } else {
        rep.messages.push_back(L.name + ": parse failed (" + perr + "), layer kept for manual setup");
    }
    if (!opt(o, "side", "auto").empty() && opt(o, "side", "auto") != "auto")
        L.side = opt(o, "side") == "bottom" ? Side::Bottom : Side::Top;
    bool any_digitize = false;
    for (auto& x : prj.layers) any_digitize |= x.digitize;
    if (!any_digitize && (L.role == LayerRole::Paste || L.role == LayerRole::Copper) && L.side == Side::Top) L.digitize = true;
    prj.layers.push_back(std::move(L));
    rep.layers_added = 1;
    return true;
}

// ===========================================================================
// machine exporters
// ===========================================================================
std::vector<OptionDef> juki_options(bool h4h, bool h8h) {
    std::vector<OptionDef> v;
    v.push_back(opt_enum_def("juki.transport", "Board transport", {"LR", "RL"}, "LR", "LR = left to right"));
    v.push_back(opt_enum_def("juki.fixed_rail", "Fixed rail", {"front", "back"}, "front"));
    v.push_back(opt_bool_def("juki.sort_by_part", "Sort placements by part number", false));
    v.push_back(opt_str_def("juki.comment", "Comment (line info, 32 chars)", ""));
    // juki.board_x0/_y0 are not offered: the job already carries the board corner and a
    // "0" default here shifted the board away from the placements (HLC_FORMAT.md §1).
    v.push_back(opt_enum_def("juki.circuit_mode", "Circuit configuration", {"auto", "single", "matrix", "nonmatrix"}, "auto",
                             "auto = Single PWB for one circuit, Matrix for a regular array, else Non-Matrix"));
    v.push_back(opt_num_def("juki.circuit_w", "Circuit width (mm, blank = auto)", ""));
    v.push_back(opt_num_def("juki.circuit_h", "Circuit height (mm, blank = auto)", ""));
    v.push_back(opt_num_def("juki.circuit_x", "Circuit origin X (mm)", ""));
    v.push_back(opt_num_def("juki.circuit_y", "Circuit origin Y (mm)", ""));
    v.push_back(opt_str_def("juki.global_marks", "Global mark order (fiducial names, comma separated)"));
    v.push_back(opt_str_def("juki.timestamp", "Timestamp (unix seconds, blank = now)"));
    if (h4h) v.push_back(opt_bool_def("juki.h4h_hlc5", "Write HLC 5.x layout (default HLC 6.x)", false));
    if (h8h) v.push_back(opt_str_def("juki.h8h.version", "H8H header version", "C00A"));
    return v;
}

MachineJob job_with_options(const MachineJob& j, const Options& o) {
    MachineJob job = j;
    for (auto& kv : o)
        if (!kv.second.empty()) job.options[kv.first] = kv.second;     // exporter options override setup options
    return job;
}

double unit_scale(const std::string& u) {             // mm -> unit
    if (u == "inch") return 1.0 / 25.4;
    if (u == "mil") return 1000.0 / 25.4;
    return 1.0;
}
int unit_decimals(const std::string& u, const Options& o) {
    const int d = (int)opt_num(o, "decimals", -1);
    if (d >= 0) return std::min(d, 8);
    return u == "inch" ? 4 : u == "mil" ? 1 : 3;
}

std::string csv_cell(const std::string& s, char delim) {
    if (s.find(delim) == std::string::npos && s.find('"') == std::string::npos && s.find('\n') == std::string::npos)
        return s;
    std::string r = "\"";
    for (char c : s) { if (c == '"') r += '"'; r += c; }
    return r + '"';
}

char delim_char(const std::string& d) {
    if (d == "semicolon") return ';';
    if (d == "tab") return '\t';
    if (d == "space") return ' ';
    return ',';
}

// Third-party placement formats: flat-file and vendor interchange writers.
// These writers consume the already transformed MachineJob coordinate frame.
enum class VendorFormat { Flat, Janets, Samsung, Directline, TPSys, Dp, Ftip, Quadra, Dima, Tdk, Blakell };

bool vendor_field(const std::string& value, const char* delimiters) {
    return value.find_first_of(delimiters) == std::string::npos &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
}

// Library fields are supplied by the user per part (no inference from geometry).
bool export_mydata_cmp(const MachineJob& job, const Options& opts,
                       std::vector<uint8_t>& out, std::string& err) {
    const std::string part = opt(opts, "cmp.part", "");
    const bool placed = std::any_of(job.placements.begin(), job.placements.end(),
        [&](const JobPlacement& p) { return !p.skip && p.part == part; });
    if (part.empty() || !vendor_field(part, "") || !placed) {
        err = "Enter the exact part number of an active placement in this model point.";
        return false;
    }
    std::string text = "#\r\nC00 " + part + "\r\n";
    for (const char* record : {"C01", "C02", "C03", "C05", "C07"}) {
        const std::string payload = opt(opts, std::string("cmp.") + record, "");
        if (payload.empty() || !vendor_field(payload, "")) {
            err = std::string("Enter the printable Mydata library value for ") + record + '.';
            return false;
        }
        if (std::string(record) == "C03") {
            std::istringstream fields(payload);
            std::string a, b, c, extra;
            if (!(fields >> a >> b >> c) || (fields >> extra)) {
                err = "C03 requires exactly three space-separated fields.";
                return false;
            }
            text += std::string(record) + ' ' + a + ' ' + b + ' ' + c + "\r\n";
        } else {
            text += std::string(record) + ' ' + payload + "\r\n";
        }
    }
    put(out, text);
    return true;
}

// One semicolon-delimited row per component. Reuse the single-record writer so
// an invalid row fails the entire export before any bytes are published.
bool export_mydata_cmp_batch(const MachineJob& job, const Options& opts,
                             std::vector<uint8_t>& out, std::string& err) {
    const std::string gen_path = opt(opts, "cmp.gen_file", "");
    std::set<std::string> requested;
    if (!gen_path.empty()) {
        std::vector<uint8_t> data;
        if (!read_file(gen_path, data, err)) return false;
        std::istringstream gen(std::string(data.begin(), data.end()));
        std::string line;
        const std::string prefix = "# Unknown item \"";
        while (std::getline(gen, line)) {
            if (line.compare(0, prefix.size(), prefix) != 0) continue;
            const size_t end = line.find('"', prefix.size());
            const std::string part = line.substr(prefix.size(), end - prefix.size());
            if (part.empty() || end == std::string::npos || !vendor_field(part, "")) {
                err = "Malformed unknown-item record in the GEN file.";
                return false;
            }
            requested.insert(part);
        }
        if (requested.empty()) {
            err = "The GEN file contains no unknown-item records.";
            return false;
        }
    }
    std::istringstream input(opt(opts, "cmp.rows", ""));
    std::set<std::string> parts;
    std::vector<uint8_t> bytes;
    std::string row;
    size_t line = 0;
    while (std::getline(input, row, '|')) {
        ++line;
        if (!row.empty() && row.back() == '\r') row.pop_back();
        if (row.empty()) continue;
        std::istringstream fields(row);
        Options entry;
        for (const char* key : {"cmp.part", "cmp.C01", "cmp.C02", "cmp.C03", "cmp.C05", "cmp.C07"}) {
            std::getline(fields, entry[key], ';');
        }
        const std::string& part = entry.at("cmp.part");
        if (!fields.eof() || row.back() == ';' || !parts.insert(io::to_lower(part)).second) {
            err = "CMP row " + std::to_string(line) + " has extra columns or a duplicate part number.";
            return false;
        }
        if (!gen_path.empty() && !requested.count(part)) continue;
        requested.erase(part);
        if (!export_mydata_cmp(job, entry, bytes, err)) {
            err = "CMP row " + std::to_string(line) + ". " + err;
            return false;
        }
    }
    if (!requested.empty()) {
        err = "Missing CMP values for GEN part " + *requested.begin();
        return false;
    }
    if (bytes.empty()) {
        err = "Enter at least one CMP component row.";
        return false;
    }
    out.insert(out.end(), bytes.begin(), bytes.end());
    return true;
}


bool export_universal_hsp(const MachineJob& job, const Options& opts,
                          std::vector<uint8_t>& out, std::string& err) {
    const std::string units = opt(opts, "units", "mm");
    if (units != "mm" && units != "inch") { err = "Invalid HSP units."; return false; }
    std::vector<const JobPlacement*> placements;
    std::set<std::string> refs;
    for (const auto& p : job.placements) {
        if (p.skip) continue;
        if (p.ref.empty() || p.ref.size() > 8 || p.part.empty() || p.part.size() > 32 ||
            !vendor_field(p.ref, ",") || !vendor_field(p.part, ",") ||
            !refs.insert(io::to_lower(p.ref)).second ||
            !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.rot)) {
            err = "Invalid HSP reference, part, or coordinates for " + p.ref;
            return false;
        }
        if (std::abs(std::remainder(p.rot, 90.0)) > 1e-6) {
            err = "HSP requires right-angle placement rotations for " + p.ref;
            return false;
        }
        placements.push_back(&p);
    }
    if (placements.empty()) { err = "No active HSP placements."; return false; }
    std::stable_sort(placements.begin(), placements.end(), [](const auto* a, const auto* b) {
        return a->part == b->part ? ref_less(a->ref, b->ref) : a->part < b->part;
    });
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "XXX,00000000,000,CA,00,000,000,0000,0000,0000,0,0000,0,-,0\r\n:CORE\r\nXXX          ,XXX NO GSM ,--------------------------------,000.00,000.00,0.00,+00.000,+00.000,+000.00,+0.00,3,+000,0,0,0,000.00,000.00,0,1\r\n:RECOG\r\n0,0,0,0,000.000,000.000,00,000.000,000.000,00,000.000,000.000,00\r\n:MARK\r\n00,0.00,0.00,00.0,0,0,00\r\n00,0.00,0.00,00.0,0,0,00\r\n00,0.00,0.00,00.0,0,0,00\r\n00,0.00,0.00,00.0,0,0,00\r\n00,0.00,0.00,00.0,0,0,00\r\n00,0.00,0.00,00.0,0,0,00\r\n00,0.00,0.00,00.0,0,0,00\r\n00,0.00,0.00,00.0,0,0,00\r\n00,0.00,0.00,00.0,0,0,00\r\n00,0.00,0.00,00.0,0,0,00\r\n:SETUP\r\n1,1,1,0,0,000.00\r\n:OPE-C\r\n00.00,0,0,0,0,0,000,0,0,+000\r\n000,-,000,-,000,-,000,-,000,-,000,-,000,-,000,-,000,-,000,-,000,-,000,-,000,-,000,-,000,-,000,-\r\n0,0\r\n:PARTS\r\n:PLT1\r\n";
    std::map<std::string, int> part_ids;
    for (const auto* p : placements) {
        if (part_ids.count(p->part)) continue;
        const int id = 101 + static_cast<int>(part_ids.size());
        if (id > 999) { err = "HSP part index exceeds three digits."; return false; }
        part_ids[p->part] = id;
        s << id << ',' << std::left << std::setw(32) << p->part << std::right << ",0,000,-,\r\n";
    }
    s << ":PLT2\r\n:M-DAT01\r\n-00.00,-00.00,+00.00,000,00,+0.00,-,000.000,000.000,000.000,000.000,00,00,        ,+00,000\r\n";
    for (const auto* p : placements) {
        const double x = p->x * unit_scale(units), y = p->y * unit_scale(units);
        if (x < -99.994 || y < -99.994 || x > 999.994 || y > 999.994) {
            err = "HSP six-character coordinate field overflow for " + p->ref;
            return false;
        }
        const double rotation = norm_deg(opt(opts, "angle", "CCW") == "CW" ? -p->rot : p->rot);
        auto number = [&](double value) {
            s << std::fixed << std::setprecision(2) << std::internal << std::setfill('0')
              << std::setw(6) << value << std::setfill(' ') << std::right;
        };
        number(x); s << ','; number(y); s << ','; number(rotation);
        s << ',' << part_ids.at(p->part)
          << ",00,+0.00,-,000.000,000.000,000.000,000.000,00,00,"
          << std::left << std::setw(8) << p->ref << std::right << ",+00,000\r\n";
    }
    s << "000.00,000.00,000.00,000,00,+0.00,Q,000.000,000.000,000.000,000.000,00,00,E       ,+00,000\r\n:A-DAT01\r\n+00.00,+00.00,+00.00,00,+0.00,-,ASTEPS  ,000.00,000.00\r\n000.00,000.00,000.00,00,+0.00,-,OFFSET  ,000.00,000.00\r\n000.00,000.00,000.00,00,+0.00,-,OFFSET  ,000.00,000.00\r\n000.00,000.00,000.00,00,+0.00,-,OFFSET  ,000.00,000.00\r\n000.00,000.00,000.00,00,+0.00,-,OFFSET  ,000.00,000.00\r\n000.00,000.00,000.00,00,+0.00,-,OFFSET  ,000.00,000.00\r\n000.00,000.00,000.00,00,+0.00,E,ENDATA  ,000.00,000.00\r\n";
    put(out, s.str());
    return true;
}


bool export_mascot(const MachineJob& job, const Options&,
                   std::vector<uint8_t>& out, std::string& err) {
    auto valid = [](double x) { return std::isfinite(x) && std::abs(x) <= 10000; };
    if (!valid(job.board_x0) || !valid(job.board_y0) || !valid(job.board_w) ||
        !valid(job.board_h) || job.board_w <= 0 || job.board_h <= 0) {
        err = "Mascot requires a finite board frame."; return false;
    }
    std::vector<const JobFiducial*> marks;
    for (const auto& f : job.fiducials)
        if (!f.local && f.circuit == 0 && f.owner.empty()) marks.push_back(&f);
    if (marks.size() != 2) { err = "Mascot requires two global alignment marks."; return false; }
    for (const auto* f : marks)
        if (!valid(f->x) || !valid(f->y) || f->name.empty() || !vendor_field(f->name, "")) {
            err = "Invalid Mascot alignment mark."; return false;
        }
    if (marks[0]->x == marks[1]->x && marks[0]->y == marks[1]->y) {
        err = "Mascot alignment marks coincide."; return false;
    }
    std::vector<const JobPlacement*> rows;
    std::set<std::string> refs;
    for (const auto& p : job.placements) {
        if (p.skip) continue;
        if (!p.has_scan_pair || !valid(p.scan1.x) || !valid(p.scan1.y) ||
            !valid(p.scan2.x) || !valid(p.scan2.y) || p.ref.empty() || p.part.empty() ||
            !refs.insert(io::to_lower(p.ref)).second || !vendor_field(p.ref, "") ||
            !vendor_field(p.part, "") || !vendor_field(p.package, "") || !vendor_field(p.description, "")) {
            err = "Mascot needs a valid pin-1 scan pair and component fields for " + p.ref;
            return false;
        }
        rows.push_back(&p);
    }
    if (rows.empty()) { err = "No active Mascot components."; return false; }
    std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) { return ref_less(a->ref, b->ref); });
    auto um = [](double x) { return std::llround(x * 1000); };
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "MAS Assembly File,1.6,,,,,,,,,,,,,,,,,,,,,,,,,,\r\n*BOARDS*,,,,,,,,,,,,,,,,,,,,,,,,,,,\r\n//ID,Align A Ref, Align A X,Align A Y,Align B Ref, Align B X,Align B Y,Extent Left,Extent Bottom,Extent Right,Extent Top,Colour,Theta,,,,,,,,,,,,,,,\r\n";
    s << "1," << csv_cell(marks[0]->name + " PIN 1", ',') << ',' << um(marks[0]->x) << ',' << um(marks[0]->y)
      << ',' << csv_cell(marks[1]->name + " PIN 1", ',') << ',' << um(marks[1]->x) << ',' << um(marks[1]->y)
      << ',' << um(job.board_x0) << ',' << um(job.board_y0) << ',' << um(job.board_x0 + job.board_w)
      << ',' << um(job.board_y0 + job.board_h) << ",0,0,,,,,,,,,,,,,,,\r\n";
    s << "*END*,,,,,,,,,,,,,,,,,,,,,,,,,,,\r\n" << "*COMPONENTS*,,,,,,,,,,,,,,,,,,,,,,,,,,,\r\n//Reference,Part Number,Description,Bin,Alt Bin,Polarity,Profile,Image File,Image Rotation,Scan Left,Scan Bottom,Scan Right,Scan Top,Rotation,Board ID,Colour,Stores,ReOrder,Bulk,Enabled,CC E,CC 1,CC 2,CC 3,CC 4,CC 5,Information,Validate\r\n";
    for (const auto* p : rows)
        s << csv_cell(p->ref, ',') << ',' << csv_cell(p->part, ',') << ',' << csv_cell(p->package, ',')
          << ",0,," << (p->leads >= 3 ? 3 : 1) << ",60,,0," << um(p->scan1.x) << ',' << um(p->scan1.y)
          << ',' << um(p->scan2.x) << ',' << um(p->scan2.y)
          << ",0,1,255,,0,1,1,0,0,0,0,0,0," << csv_cell(p->description, ',') << ",\r\n";
    s << "*END*,,,,,,,,,,,,,,,,,,,,,,,,,\r\n";
    put(out, s.str());
    return true;
}

bool export_universal_radial(const MachineJob& job, const Options& opts,
                             std::vector<uint8_t>& out, std::string& err, bool style2 = false, bool vcd = false) {
    std::vector<uint8_t> library;
    if (!read_file(opt(opts, "uni.path", ""), library, err)) return false;
    std::map<std::string, int> heads;
    std::map<std::string, char> depth_stops;
    std::istringstream lines(std::string(library.begin(), library.end()));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '\'') continue;
        std::istringstream fields(line);
        std::string part, kind, head;
        std::getline(fields, part, '\t'); std::getline(fields, kind, '\t');
        std::getline(fields, head, '\t');
        if (kind != (vcd ? "V" : "R")) continue;
        if (vcd) {
            std::string depth;
            std::getline(fields, depth, '\t');
            if (depth.size() != 1 || depth[0] < 'A' || depth[0] > 'Z') {
                err = "Invalid VCD UNI depth stop for " + part; return false;
            }
            depth_stops.emplace(part, depth[0]);
        }
        if (part.empty() || head.empty() ||
            !std::all_of(head.begin(), head.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }) ||
            head.size() > 4 || !heads.emplace(part, std::stoi(head)).second) {
            err = "Invalid or duplicate insertion entry in UNI library."; return false;
        }
    }
    std::vector<std::pair<int, const JobPlacement*>> rows;
    for (const auto& p : job.placements) {
        if (p.skip) continue;
        auto head = heads.find(p.part);
        if (head == heads.end()) continue;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x) > 25000 || std::abs(p.y) > 25000) {
            err = "Invalid insertion coordinates for " + p.ref; return false;
        }
        rows.emplace_back(head->second, &p);
    }
    if (rows.empty() || rows.size() > 9997) { err = "Universal insertion export needs 1 to 9997 mapped placements."; return false; }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    auto vertical = [](const JobPlacement& p) {
        return std::abs(p.scan1.x - p.scan2.x) <= 0.0127;
    };
    if (vcd) {
        if (rows.size() > 9996) { err = "VCD needs a step for the re-zero record."; return false; }
        std::stable_partition(rows.begin(), rows.end(), [&](const auto& row) { return !vertical(*row.second); });
    }
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "0001>     0000     0000    00000    00000     0000    00000    00000    A O\r\n"
      << "0002>     0000     0000    00000    00000     0000    00000    00000    A Z\r\n";
    if (style2) {
        s.str("");
        s << "0001> +00000 +00000 +00000 +00000 +00000 +00000 +00000 A O\r\n"
          << "0002> +00000 +00000 +00000 +00000 +00000 +00000 +00000 A Z\r\n";
    }
    int step = 2;
    bool rezero = false;
    auto write_rezero = [&] {
        s << std::setfill('0') << std::setw(4) << ++step << '>' << std::setfill(' ')
          << (style2 ? " +00000 +00000 +00000 +00000 +00000 +00000 +00000 A Z\r\n"
                     : "     0000     0000    00000    00000     0000    00000    00000    A Z\r\n");
        rezero = true;
    };
    for (const auto& row : rows) {
        const auto& p = *row.second;
        const char depth = vcd ? depth_stops.at(p.part) : 'A';
        const double dx = std::abs(p.scan1.x - p.scan2.x);
        const double dy = std::abs(p.scan1.y - p.scan2.y);
        if (vcd && (!p.has_scan_pair || !std::isfinite(dx) || !std::isfinite(dy) ||
                    !(vertical(p) ? dy >= 0.0254 : dy <= 0.0127 && dx >= 0.0254))) {
            err = "VCD requires an axis-aligned lead pair for " + p.ref;
            return false;
        }
        const bool turned = vcd && vertical(p);
        if (turned && !rezero) write_rezero();
        const double x = turned ? -p.y : p.x;
        const double y = turned ? p.x : p.y;
        s << std::setfill('0') << std::setw(4) << ++step << '>' << std::setfill(' ');
        if (style2) {
            const double span = std::max(std::abs(p.scan2.x - p.scan1.x), std::abs(p.scan2.y - p.scan1.y));
            if (!p.has_scan_pair || !std::isfinite(span) || span <= 0 || span > 2539.97 ||
                std::abs(p.x) > 2539.97 || std::abs(p.y) > 2539.97) {
                err = "Style 2 requires valid lead geometry and five-digit coordinates for " + p.ref;
                return false;
            }
            auto signed_field = [&](long long value) {
                s << ' ' << (value < 0 ? '-' : '+') << std::setfill('0') << std::setw(5)
                  << std::abs(value) << std::setfill(' ');
            };
            signed_field(std::llround(x / 0.0254));
            signed_field(std::llround(y / 0.0254));
            signed_field(std::llround(span / 0.0254));
            signed_field(0); signed_field(row.first); signed_field(0); signed_field(0);
            s << ' ' << depth << " I\r\n";
            continue;
        }
        auto field = [&](long long value, int digits) {
            std::string v = std::to_string(std::abs(value));
            if (v.size() < static_cast<size_t>(digits)) v.insert(0, digits - v.size(), '0');
            if (value < 0) v.insert(v.begin(), '-');
            s << std::setw(9) << v;
        };
        field(std::llround(x / 0.0254), 4);
        field(std::llround(y / 0.0254), 4);
        field(0, 5); field(0, 5); field(row.first, 4); field(0, 5); field(0, 5);
        s << "        " << depth << "        I\r\n";
    }
    if (vcd && !rezero) write_rezero();
    put(out, s.str());
    return true;
}

bool export_cs400(const MachineJob& job, const Options& opts,
                  std::vector<uint8_t>& out, std::string& err) {
    struct Part { std::string image, clinch, polarized, angle, length; };
    std::vector<uint8_t> data;
    if (!read_file(opt(opts, "cs4.path", ""), data, err)) return false;
    std::istringstream lines(std::string(data.begin(), data.end()));
    std::map<std::string, Part> parts;
    std::string line;
    while (std::getline(lines, line)) {
        line = io::trim(line);
        if (line.empty() || line.front() == '\'') continue;
        std::istringstream fields(line);
        std::string number;
        Part part;
        if (!std::getline(fields, number, '\t') || !std::getline(fields, part.image, '\t') ||
            !std::getline(fields, part.clinch, '\t') || !std::getline(fields, part.polarized, '\t') ||
            number.empty()) {
            err = "Invalid CS4 row for " + number; return false;
        }
        std::getline(fields, part.angle, '\t');
        std::getline(fields, part.length, '\t');
        if (!parts.emplace(number, part).second) {
            err = "Duplicate CS4 row for " + number; return false;
        }
    }
    const std::string model = opt(opts, "cs4.model", "D/E");
    const double divisor = model == "D/E" ? 2.5 : model == "CS400C" ? 10.0 : 5.0;
    const double unit = 0.0254 * divisor;
    if (!vendor_field(job.program_name, "\"")) { err = "Invalid CS400 program name."; return false; }
    std::map<std::string, std::vector<const JobPlacement*>> groups;
    std::set<std::string> refs;
    for (const auto& p : job.placements) {
        if (p.skip || !parts.count(p.part)) continue;
        if (!p.has_scan_pair || p.ref.empty() || p.ref.size() > 5 || !vendor_field(p.ref, "\\") ||
            !refs.insert(io::to_lower(p.ref)).second) {
            err = "CS400 needs a unique five-character reference and numbered pin 1 for " + p.ref;
            return false;
        }
        groups[p.part].push_back(&p);
    }
    if (groups.empty() || groups.size() > 2499) { err = "Invalid number of CS400 part groups."; return false; }
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "\r\nS206\"" << job.program_name << "\"\r\n032+0000+0000+0000+0000\r\n";
    auto signed_field = [&](int value) {
        s << (value < 0 ? '-' : '+') << std::setfill('0') << std::setw(4) << std::abs(value) << std::setfill(' ');
    };
    int index = 0;
    bool custom_clinch = false;
    for (const auto& group : groups) {
        const auto& part = parts.at(group.first);
        auto setting = [](const std::string& value) {
            if (value.empty() || value == "5") return 5;
            if (value == "10") return 10;
            return value.size() == 1 && value[0] >= '1' && value[0] <= '9' ? value[0] - '0' : 0;
        };
        const int angle = setting(part.angle), length = setting(part.length);
        if (!vendor_field(group.first, "\"") || !vendor_field(part.image, "\"") ||
            (part.clinch != "N" && part.clinch != "NONE" && part.clinch != "IN" && part.clinch != "OUT") ||
            (part.polarized != "N" && part.polarized != "Y") || !angle || !length) {
            err = "Invalid CS4 settings for " + group.first; return false;
        }
        s << "032"; signed_field(4 * ++index); s << "+0000+0000+0000\r\n";
        if (custom_clinch || angle != 5 || length != 5) {
            s << "001"; signed_field(angle * 100); signed_field(length * 100); s << "+0000+0000\r\n";
            custom_clinch = angle != 5 || length != 5;
        }
        if (part.polarized == "Y") s << "126+0000+0000+0000+0000\r\n";
        if (!part.image.empty()) s << "216\"" << part.image << "\"\r\n";
        s << "236\"" << group.first << "\"\r\n";
        for (const auto* p : group.second) {
            const double values[] = {p->scan1.x, p->scan1.y, p->scan2.x - p->scan1.x, p->scan2.y - p->scan1.y};
            s << (part.clinch == "OUT" ? "900" : "000");
            for (double value : values) {
                const double ticks = std::trunc(value / unit);
                if (!std::isfinite(ticks) || std::abs(ticks) > 9999) {
                    err = "CS400 coordinate exceeds its fixed-width field for " + p->ref; return false;
                }
                signed_field(static_cast<int>(ticks));
            }
            s << '\\' << std::left << std::setw(5) << p->ref << std::right << "\\\r\n";
        }
        if (part.polarized == "Y") s << "116+0000+0000+0000+0000\r\n";
    }
    s << "009+0000+0000+0000+0000\r\n";
    put(out, s.str());
    return true;
}

bool export_juki_iss(const MachineJob& job, const Options& opts,
                     std::vector<uint8_t>& out, std::string& err) {
    auto coordinate = [](double value) { return std::isfinite(value) && std::abs(value) <= 10000; };
    if (!coordinate(job.board_w) || !coordinate(job.board_h) ||
        job.board_w <= 0 || job.board_h <= 0) {
        err = "ISS requires a valid board outline."; return false;
    }
    std::vector<const JobPlacement*> placements;
    std::set<std::string> refs, parts;
    for (const auto& p : job.placements) {
        if (p.skip) continue;
        if (p.circuit != 1 || p.ref.empty() || p.part.empty() ||
            !vendor_field(p.ref, "") || !vendor_field(p.part, "") ||
            !refs.insert(io::to_lower(p.ref)).second || !coordinate(p.x) ||
            !coordinate(p.y) || !std::isfinite(p.rot)) {
            err = "ISS requires unique, valid single-circuit placements. Check " + p.ref;
            return false;
        }
        placements.push_back(&p);
        parts.insert(p.part);
    }
    if (placements.empty()) { err = "No active ISS placements."; return false; }
    std::vector<const JobFiducial*> marks;
    for (const auto& f : job.fiducials) {
        if (f.local || f.circuit != 0 || !f.owner.empty()) continue;
        if (f.name.empty() || !vendor_field(f.name, "") || !coordinate(f.x) || !coordinate(f.y)) {
            err = "Invalid ISS board fiducial."; return false;
        }
        marks.push_back(&f);
    }
    if (marks.size() < 2 || marks.size() > 3) { err = "ISS requires two or three board fiducials."; return false; }
    auto escape = [](const std::string& value) {
        std::string xml;
        for (char c : value) {
            switch (c) {
            case '&': xml += "&amp;"; break;
            case '<': xml += "&lt;"; break;
            case '>': xml += "&gt;"; break;
            default: xml += c; break;
            }
        }
        return xml;
    };
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::fixed << std::setprecision(3);
    s << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<productionProgram>\n"
      << "<headerData><targetMachine>ISS</targetMachine><targetVersion revision=\"9\" version=\"5\" level=\"0\" />"
      << "<editMachine>ISS</editMachine><editVersion revision=\"9\" version=\"5\" level=\"0\" />"
      << "<lineConfiguration><lineName id=\"0\" /><configuration /></lineConfiguration>"
      << "<headPwb total=\"1\" completion=\"1\" />"
      << "<headPlacement total=\"" << placements.size() << "\" completion=\"0\" />"
      << "<headComponent total=\"" << parts.size() << "\" completion=\"0\" /></headerData>\n";
    s << "<core><pwbData><pwbId /><pwbBasic><referenceSide>FRONT</referenceSide><transferDirection>"
      << (opt(opts, "iss.transport", "LTOR") == "RTOL" ? "RTOL" : "LTOR")
      << "</transferDirection><referenceType>SHAPE</referenceType><circuitType>STANDARD</circuitType>"
      << "<bocMarkSelect>ALL</bocMarkSelect><markRecognition>GRAYSCALE</markRecognition>"
      << "<badMarkType>STANDARD</badMarkType><badMarkRecognition>POSITIVE</badMarkRecognition></pwbBasic>"
      << "<pwbConfiguration><outline x=\"" << job.board_w << "\" y=\"" << job.board_h << "\" />"
      << "<pwbThick thick=\"1.6\" /><backHeight height=\"37\" /><pwbHeight height=\"0\" /></pwbConfiguration>"
      << "<circuitConfigurationData><circuitConfiguration index=\"0\"><circuitUse>USE</circuitUse>"
      << "<circuitId>A</circuitId><circuitConfiguration>SINGLE</circuitConfiguration>"
      << "<bocMarkType>PWBBOC</bocMarkType><circuitOutline /><referencePosition />"
      << "<badMark><badMarkUse>NOUSE</badMarkUse></badMark></circuitConfiguration></circuitConfigurationData>"
      << "</pwbData>\n<placementData>\n";
    size_t index = 0;
    for (const auto* p : placements) {
        s << "<placement index=\"" << ++index << "\"><placementId>" << escape(p->ref)
          << "</placementId><baseCircuitId>A</baseCircuitId><componentName>" << escape(p->part)
          << "</componentName><placementPosition x=\"" << p->x << "\" y=\"" << p->y
          << "\" rangeOver=\"False\" /><placementAngle angle=\"" << std::setprecision(2) << norm_deg(p->rot)
          << std::setprecision(3) << "\" /><attribute><skip placement=\"NO\" adhesive=\"NO\" /></attribute></placement>\n";
    }
    s << "</placementData><componentData>\n";
    index = 0;
    for (const auto& part : parts)
        s << "<component index=\"" << ++index << "\"><componentBasic><componentName>" << escape(part)
          << "</componentName><comment /><componentSize /><moldline><outline /><offset /></moldline>"
          << "</componentBasic><componentInspection /></component>\n";
    s << "</componentData></core>\n<model><pwbData><pwbId /><pwbBasic><pwbMaterial>EPOXY</pwbMaterial>"
      << "<vacuumTable>NOUSE</vacuumTable><traceability>NOUSE</traceability></pwbBasic>"
      << "<pwbConfiguration><pwbLayoutOffset x=\"0\" y=\"0\" /><referencePosition x=\"0\" y=\"0\" />"
      << "</pwbConfiguration><circuitConfigurationData><circuitConfiguration index=\"0\"><circuitId>A</circuitId>"
      << "<circuitLayoutOffset x=\"0\" y=\"0\" /></circuitConfiguration></circuitConfigurationData></pwbData>"
      << "<placementData></placementData><componentData></componentData></model>\n"
      << "<machine><pickData /><bocMarkData><circuitData index=\"0\"><bocMark no=\"1\"><markType>BOC</markType>"
      << "<fiducialMarkData>\n";
    for (size_t i = 0; i < 3; ++i) {
        s << "<fiducialMark index=\"" << i << "\">";
        if (i < marks.size())
            s << "<markName>" << escape(marks[i]->name) << "</markName><markPosition x=\""
              << marks[i]->x << "\" y=\"" << marks[i]->y << "\" />";
        else s << "<markName /><markPosition />";
        s << "</fiducialMark>\n";
    }
    s << "</fiducialMarkData></bocMark></circuitData></bocMarkData></machine>\n</productionProgram>\n";
    put(out, s.str());
    return true;
}

bool export_vendor(const MachineJob& job, const Options& opts, VendorFormat format,
                   std::vector<uint8_t>& out, std::string& err) {
    const bool blakell = format == VendorFormat::Blakell;
    const bool tdk = format == VendorFormat::Tdk;
    const bool quadra = format == VendorFormat::Quadra;
    const bool dima = format == VendorFormat::Dima;
    const bool dp = format == VendorFormat::Dp;
    const bool ftip = format == VendorFormat::Ftip;
    const bool flat = format == VendorFormat::Flat;
    const bool samsung = format == VendorFormat::Samsung;
    const bool directline = format == VendorFormat::Directline;
    const bool mydata = directline || format == VendorFormat::TPSys;
    const std::string units = samsung ? "inch" : opt(opts, "units", "mm");
    const double scale = unit_scale(units);
    if (units != "mm" && units != "inch" && !(flat && units == "mil")) {
        err = "Unsupported output units.";
        return false;
    }
    const bool cw = opt(opts, "angle", "CCW") == "CW";
    const int decimals = units == "mm" && flat ? 4 : units == "inch" && !flat && !samsung ? 4 : 3;
    const char* delimiters = samsung ? "\"" : flat ? "" : blakell ? "" : tdk ? " :;" : mydata ? " " : dp ? ";" : quadra ? "#," : dima ? "|" : ftip ? ",()=" : ",";
    auto fail = [&](std::string message) { err = std::move(message); return false; };
    auto valid_xy = [](double x, double y) {
        return std::isfinite(x) && std::isfinite(y) && std::abs(x) <= 10000 && std::abs(y) <= 10000;
    };

    std::vector<const JobPlacement*> rows;
    std::set<std::string> refs;
    for (const auto& p : job.placements) {
        if (p.skip || (tdk && p.smt)) continue;
        if (!refs.insert(io::to_lower(p.ref)).second)
            return fail("Duplicate reference " + p.ref + ". Export a single circuit or use unique panel references.");
        if (!valid_xy(p.x, p.y) || !std::isfinite(p.rot))
            return fail("Invalid placement coordinates or rotation for " + p.ref);
        const char* bad = nullptr; const std::string* val = nullptr;
        if (p.ref.empty() || !vendor_field(p.ref, delimiters)) { bad = "reference"; val = &p.ref; }
        else if (!vendor_field(p.part, delimiters) || (!flat && p.part.empty())) { bad = "part number"; val = &p.part; }
        else if ((flat || ftip || quadra || blakell) && !vendor_field(p.package, flat ? "" : delimiters)) { bad = "package"; val = &p.package; }
        else if ((samsung || flat) && !vendor_field(p.description, delimiters)) { bad = "description"; val = &p.description; }
        if (bad && val->empty())
            return fail(p.ref.empty() ? std::string("A placement has no reference.") : p.ref + " has no " + bad + "; this format requires one.");
        if (bad)
            return fail("Unsupported " + std::string(bad) + " text for " + p.ref + ": \"" + *val +
                        "\". This format allows printable ASCII only, without its field delimiters.");
        if (dima && (p.ref.size() > 8 || p.part.size() > 32 ||
                     std::abs(p.x) >= 999.9995 || std::abs(p.y) >= 999.9995))
            return fail("Dima field width exceeded for " + p.ref);
        rows.push_back(&p);
    }
    if (rows.empty()) return fail(tdk ? "No active through-hole placements to export." : "No active placements to export.");
    // These formats sort by reference, except Samsung and Mydata,
    // whose placement tables are grouped by part number.
    std::stable_sort(rows.begin(), rows.end(), [&](const JobPlacement* a, const JobPlacement* b) {
        if ((samsung || mydata || tdk) && a->part != b->part) return a->part < b->part;
        return ref_less(a->ref, b->ref);
    });

    std::vector<const JobFiducial*> marks;
    if (flat || mydata) {
        for (const auto& f : job.fiducials)
            if (!f.local && f.circuit == 0 && f.owner.empty()) marks.push_back(&f);
        if (mydata && marks.size() != 2)
            return fail("Mydata export requires exactly two global fiducials in reference order.");
        if (marks.size() > 2) marks.resize(2);
        for (const auto* f : marks)
            if (f->name.empty() || !vendor_field(f->name, delimiters) || !valid_xy(f->x, f->y))
                return fail("Invalid reference mark " + f->name);
        if (mydata && (marks[0]->name == marks[1]->name ||
                      (marks[0]->x == marks[1]->x && marks[0]->y == marks[1]->y)))
            return fail("Mydata reference marks must have distinct names and positions.");
    }

    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::fixed << std::setprecision(decimals);
    constexpr const char* nl = "\r\n";
    const char* layer = job.bottom_side ? "BOTTOM" : "TOP";
    auto tenth_mil = [](double mm) { return std::llround(mm * 10000 / 25.4); };
    auto micron = [](double mm) { return std::llround(mm * 1000); };
    double origin_x = 0, origin_y = 0;
    if (mydata) {
        if (job.program_name.empty() || !vendor_field(job.program_name, directline ? "" : " "))
            return fail("Mydata requires a printable board name. Use a single token for TPSys.");
        if (directline) {
            origin_x = marks[0]->x;
            origin_y = marks[0]->y;
            auto fits = [&](double x, double y) {
                return tenth_mil(x - origin_x) >= -9999999 && tenth_mil(x - origin_x) <= 99999999 &&
                       tenth_mil(y - origin_y) >= -9999999 && tenth_mil(y - origin_y) <= 99999999;
            };
            if (!fits(marks[1]->x, marks[1]->y) ||
                std::any_of(rows.begin(), rows.end(), [&](const auto* p) { return !fits(p->x, p->y); }))
                return fail("Directline coordinates exceed the eight-character field width.");
            s << "B " << job.program_name << nl
              << "B Ref1 = " << marks[0]->name << nl << "B Ref2 = " << marks[1]->name << nl
              << "U  0.1" << nl << "C1" << std::setw(8) << 0 << std::setw(8) << 0
              << std::setw(8) << tenth_mil(marks[1]->x - origin_x)
              << std::setw(8) << tenth_mil(marks[1]->y - origin_y) << nl;
        } else {
            s << "F1 " << job.program_name << nl << "U 1" << nl;
            for (const auto* f : marks)
                s << "F3 " << micron(f->x) << ' ' << micron(f->y) << ' ' << f->name << nl;
            s << "F6 0 0" << nl << "F7 0 0" << nl;
        }
    } else if (tdk) {
        for (const char* line : {
                 "PN=", "LN=", "FC=", "UT=1", "UC=1", "DM=1", "RP=0", "FX=", "FY=",
                 "BX=99999", "BY=99999", "B1=0", "B2=0", "XL=-99999", "YL=-99999",
                 "XH=99999", "YH=99999", "DC=0", "SC=",
                 "FN=1 S:1 P:0 L:0 X: Y: H:0 D:,,0,0,0,0 C: AT:0 ;FIDUCIAL"
             }) s << line << nl;
    } else if (dima) {
        s << "[FileID 1.00]" << nl << nl << "[General]" << nl
          << "Pcb name          : ..\\xxx.pcb" << nl << "Barcode           :" << nl
          << "Pcb units         : mm" << nl << "Pcb length        : ###.###" << nl
          << "Pcb width         : ###.###" << nl << "Active side       : xxx" << nl
          << "Pcb type          : xxx" << nl << "Pcb orientation   : 0" << nl
          << "Badmark support   : xx" << nl << "Component library : .\\xxx_xxx.clb" << nl
          << "Fiducial library  : .\\xxx xxx.flb" << nl << "Pcb comment       : #-#-20## ##:##x" << nl << nl
          << "[Top side]" << nl << "Use variants only          : xxx" << nl << nl
          << "[Fiducials]" << nl << "Use reference points       : xxx" << nl
          << "Check all panel refs       : xxx" << nl << "Check refs with both heads : xxx" << nl
          << "Double-check refs          : xx" << nl << nl;
        constexpr const char* fid_header =
            ";Fiducial name------------------|----X---|----Y---|-Comment------------";
        s << "[Global Reference Points]" << nl << fid_header << nl << nl
          << "[Reference Panel Reference Points]" << nl << fid_header << nl
          << "xxx             | ###.###| ###.###| " << nl
          << "xxx             | ###.###| ###.###| " << nl << nl
          << "[Panel Reference Points]" << nl << ";--X1---|---Y1---|---X2---|---Y2---" << nl << nl
          << "[Components]" << nl
          << ";Ref----|-Component name-----------------|----X---|----Y---|-Angle-|Prio|--ID" << nl;
    } else if (dp || ftip) {
        if (job.program_name.empty() || !vendor_field(job.program_name, delimiters))
            return fail("A printable board name is required.");
        const std::time_t now = std::time(nullptr);
        std::tm date{};
#ifdef _WIN32
        if (localtime_s(&date, &now) != 0) return fail("Unable to format the export date.");
#else
        if (!localtime_r(&now, &date)) return fail("Unable to format the export date.");
#endif
        if (dp) {
            const char* machine = opt(opts, "target", "Europlacer") == "Multroniks" ? "Multroniks" : "Europlacer";
            s << "#P" << nl << job.program_name << nl << job.program_name << nl << "--" << nl
              << (units == "inch" ? "1.000" : "25.400") << nl << "--" << nl << "--" << nl
              << std::put_time(&date, "%d/%m/%y") << nl << "--" << nl << nl;
            for (int i = 0; i < 2; ++i)
                s << machine << nl << std::put_time(&date, "%d/%m/%y") << nl
                  << std::put_time(&date, "%H:%M") << nl;
            s << "0.0" << nl << "0.0" << nl << nl << "#M ROOT ; 0.0; 0.0; 0.0" << nl;
        } else {
            s << "# FTIP file is derived from CAD File:" << nl << "# " << job.program_name << nl << nl
              << "*DESIGN" << nl << "FORMATVS = 2" << nl << "BOARDNAME = " << job.program_name << nl
              << "IDENTCODE = IMAGE1" << nl << "MASKCNTR = MaskCenter" << nl
              << "ISSUEDATE = " << std::put_time(&date, "%y-%m-%d") << nl
              << "REVISDATE = " << std::put_time(&date, "%y-%m-%d") << nl
              << "USERID = user" << nl << "UNIT = MM" << nl << nl
              << "*BOARD_DEFINITION" << nl << "(" << nl << "   *CIRCUIT_ARRANGEMENT" << nl
              << "   (" << nl << "      (IMAGE1,0,0,0)" << nl << "   )" << nl << nl
              << "   *BOARDFRAME" << nl << "   (" << nl;
            for (int i = 0; i < 4; ++i) s << "      (0.00,0.00,)" << nl;
            s << "   )" << nl << ")" << nl << nl << "*CIRCUIT_DEFINITION" << nl << "(" << nl
              << "   *CIRCUIT_DESCRIPTION" << nl << "   (" << nl << "      CIRCUIT_ID = IMAGE1" << nl
              << "      IDENTCODE = " << job.program_name << nl << "      *CIRCUIT_COMPONENTS" << nl
              << "      (" << nl;
        }
        s << std::setprecision(dp ? 3 : 2);
    } else if (samsung) {
        s << "[VERSION]" << nl << "Name =" << nl << "ID =" << nl << nl << "[PCB]" << nl
          << "Unit System = INCHES" << nl << "Coordinate = "
          << (opt(opts, "coordinate", "blank") == "lower-left" ? "LOWER LEFT" : "")
          << nl << nl << "[PLACEMENTS]" << nl;
    } else if (flat) {
        s << "Units\t=\t" << (units == "inch" ? "Inches" : units == "mil" ? "Mils" : "MM") << nl << nl;
        s << std::setprecision(4);
        for (size_t i = 0; i < 2; ++i) {
            s << "Ref" << i + 1 << "\t=\t";
            if (i < marks.size())
                s << marks[i]->name << '\t' << marks[i]->x * scale << '\t' << marks[i]->y * scale;
            else
                s << '\t' << 0.0 << '\t' << 0.0;
            s << nl;
        }
        s << nl << "RefDes\tPartNumber\tShape\tX\tY\tRotation\tLayer\tSpan\tOperations Field" << nl
          << std::setprecision(decimals);
    }

    size_t part_width = 0;
    if (dp) for (const auto* p : rows) part_width = std::max(part_width, p->part.size());
    size_t line_number = 10;
    for (const auto* p : rows) {
        const int rotation = static_cast<int>(std::lround(norm_deg(cw ? -p->rot : p->rot))) % 360;
        if (samsung && std::abs(std::remainder(p->rot, 90.0)) > 1e-6)
            return fail("Samsung CP SSA supports right-angle placements only. Check " + p->ref);
        switch (format) {
        case VendorFormat::Blakell: {
            if (!p->has_ordered_pin_pair || !valid_xy(p->pin1.x, p->pin1.y) ||
                !valid_xy(p->pin2.x, p->pin2.y))
                return fail("Blakell Robin needs two pads explicitly numbered 1 and 2 for " + p->ref);
            const auto x1 = std::llround(p->pin1.x / 0.0254);
            const auto y1 = std::llround(p->pin1.y / 0.0254);
            const auto x2 = std::llround(p->pin2.x / 0.0254);
            const auto y2 = std::llround(p->pin2.y / 0.0254);
            const auto dx = std::abs(x1 - x2);
            const auto dy = std::abs(y1 - y2);
            if (dx == 0 && dy == 0) return fail("Pin coordinates coincide in mils for " + p->ref);
            const int quadrant = dx > dy ? (x1 >= x2 ? 3 : 1) : (y1 >= y2 ? 0 : 2);
            s << p->part << '\t' << p->ref << '\t' << x1 << '\t' << y1 << '\t' << x2 << '\t'
              << y2 << '\t' << std::max(dx, dy) << '\t' << quadrant << '\t' << p->package << nl;
            break;
        }
        case VendorFormat::Tdk: {
            const auto x = std::llround(p->x * 100);
            const auto y = std::llround(p->y * 100);
            if (std::abs(x) > 99999 || std::abs(y) > 99999)
                return fail("TDK coordinate exceeds the NCP limits for " + p->ref);
            s << "N:" << line_number << " X:" << x << " Y:" << y << " P:" << p->part
              << " M:1 C:" << rotation << " H:0 B:0 D:0 Q:0 OPT:        ;" << p->ref << nl;
            line_number += 10;
            break;
        }
        case VendorFormat::Quadra:
            s << '#' << p->ref << "#," << std::llround(p->x / 0.0254) << ',' << std::llround(p->y / 0.0254)
              << ',' << rotation << ",#PXY#,#" << p->package << "#,#" << (job.bottom_side ? "BOT" : "TOP")
              << "#,1," << (job.bottom_side ? 'B' : 'T') << ",#1#,0,F,#TAPE#,#X#,#A#,##,##,F" << nl;
            break;
        case VendorFormat::Dima:
            s << std::left << std::setw(8) << p->ref << '|' << std::setw(32) << p->part << '|'
              << std::right << std::setprecision(3) << std::setw(8) << p->x << '|' << std::setw(8)
              << p->y << '|' << std::setprecision(2) << std::setw(7) << static_cast<double>(rotation) << '|' << nl;
            break;
        case VendorFormat::Dp:
            s << " #C " << std::left << std::setw(static_cast<int>(part_width)) << p->part
              << " ; " << std::setw(6) << p->ref << std::right << " ; " << static_cast<double>(rotation)
              << "; " << p->x * scale << "; " << p->y * scale << nl;
            break;
        case VendorFormat::Ftip:
            s << p->ref << "= ( " << p->part << ',' << p->package << ",,," << p->x << ',' << p->y
              << ',' << rotation << ',' << layer << ')' << nl;
            break;
        case VendorFormat::Flat:
            s << p->ref << '\t' << p->part << '\t' << p->package << '\t' << p->x * scale << '\t'
              << p->y * scale << '\t' << rotation << '\t' << layer << "\t\t" << p->description << nl;
            break;
        case VendorFormat::Janets:
            s << p->ref << ',' << p->x * scale << ',' << p->y * scale << ',' << rotation << ',' << p->part << nl;
            break;
        case VendorFormat::Samsung:
            s << '"' << p->ref << "\"\t" << p->x * scale << '\t' << p->y * scale
              << "\t0\t" << rotation << "\tNONE\t0\t0\t0\t0\t1\t0\t\"" << p->part
              << "\"\t\"\"\t\"" << p->description << '"' << nl;
            break;
        case VendorFormat::Directline:
            s << "M1" << std::setw(8) << tenth_mil(p->x - origin_x) << std::setw(8)
              << tenth_mil(p->y - origin_y) << ' ' << std::setw(3) << rotation << ' ' << p->part << nl
              << "M2" << p->ref << nl;
            break;
        case VendorFormat::TPSys:
            s << "F8 " << micron(p->x) << ' ' << micron(p->y) << ' ' << rotation * 1000
              << " 0 N N " << p->part << nl << "F9 " << p->ref << nl;
            break;
        }
    }
    if (directline) s << 'E' << nl;
    if (dima) {
        s << nl << "[Variant]" << nl << "Variant name      : smd" << nl << "Barcode           : " << nl
          << "Pcb orientation   : Original" << nl << "Badmark support   : Original" << nl
          << "Variant comment   : " << nl << nl << "[xxx side]" << nl << nl
          << "[Reference list]" << nl << ";Ref----|-Component name-----------------" << nl << nl
          << "[Component list]" << nl << ";Original name------------------|-New name-----------------------" << nl;
        for (int i = 0; i < 5; ++i) s << std::string(43, ' ') << '|' << std::string(32, ' ') << nl;
    }
    if (ftip) s << "      )" << nl << "   )" << nl << ')' << nl;
    put(out, s.str());
    return true;
}

bool export_generic_csv(const MachineJob& j, const Options& o, std::vector<uint8_t>& out, std::string& err) {
    const std::string units = opt(o, "units", "mm");
    const double k = unit_scale(units);
    const int dec = unit_decimals(units, o);
    const char dl = delim_char(opt(o, "delimiter", "comma"));
    const bool cw = opt(o, "angle", "CCW") == "CW";
    std::vector<std::string> cols;
    {
        std::string s = opt(o, "columns", "ref,x,y,rot,part,package,side"), cur;
        for (char ch : s + ",") {
            if (ch == ',' || ch == ';' || ch == ' ') { cur = io::to_lower(io::trim(cur)); if (!cur.empty()) cols.push_back(cur); cur.clear(); }
            else cur += ch;
        }
    }
    static const std::set<std::string> known = {"ref", "part", "package", "desc", "x", "y", "rot", "side", "circuit",
                                                "skip", "polarized", "smt", "leads", "height"};
    for (auto& c : cols) if (!known.count(c)) { err = "unknown column '" + c + "'"; return false; }
    if (cols.empty()) { err = "no columns selected"; return false; }
    const std::string nl = opt_bool(o, "crlf", true) ? "\r\n" : "\n";
    std::string s;
    if (opt_bool(o, "header", true)) {
        for (size_t i = 0; i < cols.size(); ++i) {
            std::string h = cols[i];
            if (h == "x" || h == "y" || h == "height") h += "(" + units + ")";
            if (i) s += dl;
            s += csv_cell(h, dl);
        }
        s += nl;
    }
    auto row = [&](const JobPlacement& p) {
        for (size_t i = 0; i < cols.size(); ++i) {
            const std::string& c = cols[i];
            std::string v;
            if (c == "ref") v = p.ref;
            else if (c == "part") v = p.part;
            else if (c == "package") v = p.package;
            else if (c == "desc") v = p.description;
            else if (c == "x") v = fmtd(p.x * k, dec);
            else if (c == "y") v = fmtd(p.y * k, dec);
            else if (c == "rot") { double a = norm_deg(cw ? -p.rot : p.rot); v = fmtd(a, 2); }
            else if (c == "side") v = j.bottom_side ? "B" : "T";
            else if (c == "circuit") v = std::to_string(p.circuit);
            else if (c == "skip") v = p.skip ? "1" : "0";
            else if (c == "polarized") v = p.polarized ? "1" : "0";
            else if (c == "smt") v = p.smt ? "1" : "0";
            else if (c == "leads") v = std::to_string(p.leads);
            else if (c == "height") v = fmtd(p.height * k, dec);
            if (i) s += dl;
            s += csv_cell(v, dl);
        }
        s += nl;
    };
    const bool inc_skip = opt_bool(o, "include_skipped");
    for (auto& p : j.placements) if (inc_skip || !p.skip) row(p);
    if (opt_bool(o, "fiducials")) {
        for (auto& f : j.fiducials) {
            JobPlacement p;
            p.ref = f.name; p.part = "FIDUCIAL"; p.package = "FIDUCIAL"; p.x = f.x; p.y = f.y;
            p.circuit = f.circuit; p.height = 0;
            row(p);
        }
    }
    put(out, s);
    return true;
}

std::string pad_right(const std::string& s, size_t w) {
    if (s.size() >= w) return s + " ";
    return s + std::string(w - s.size(), ' ');
}

bool export_generic_xy(const MachineJob& j, const Options& o, std::vector<uint8_t>& out, std::string&) {
    const std::string units = opt(o, "units", "mil");
    const double k = unit_scale(units);
    const int dflt = units == "mil" ? 0 : units == "inch" ? 4 : 3;
    const int d0 = (int)opt_num(o, "decimals", -1);
    const int dec = d0 >= 0 ? std::min(d0, 8) : dflt;
    const bool cw = opt(o, "angle", "CCW") == "CW";
    const bool part = opt_bool(o, "part"), pkg = opt_bool(o, "package");
    const bool inc_skip = opt_bool(o, "include_skipped");
    std::string s;
    if (opt_bool(o, "header")) {
        s += ";" + pad_right("Reference", 23) + pad_right("X(" + units + ")", 10) + pad_right("Y(" + units + ")", 10) +
             pad_right("Rotation", 10);
        if (part) s += pad_right("Part", 26);
        if (pkg) s += pad_right("Package", 16);
        s += "\r\n";
    }
    for (auto& p : j.placements) {
        if (p.skip && !inc_skip) continue;
        s += pad_right(p.ref, 24) + pad_right(fmtd(p.x * k, dec), 10) + pad_right(fmtd(p.y * k, dec), 10) +
             pad_right(fmtd(norm_deg(cw ? -p.rot : p.rot), dec == 0 ? 0 : 2), 10);
        if (part) s += pad_right(p.part, 26);
        if (pkg) s += pad_right(p.package, 16);
        s += "\r\n";
    }
    put(out, s);
    return true;
}

// ===========================================================================
// project exporters
// ===========================================================================
bool export_bom_csv(const Project& p, const Options& o, std::vector<uint8_t>& out, std::string&) {
    const char dl = delim_char(opt(o, "delimiter", "comma"));
    const bool inc_dnp = opt_bool(o, "include_dnp");
    const std::string side = opt(o, "side", "both");
    struct Row { std::string number, package; std::vector<std::string> refs; int dnp = 0; };
    std::map<std::string, Row> rows;
    for (const Component& c : p.comps) {
        if (c.feature != Feature::Component) continue;
        if (c.dnp && !inc_dnp) continue;
        if (side == "top" && c.side != Side::Top) continue;
        if (side == "bottom" && c.side != Side::Bottom) continue;
        const std::string key = c.number.empty() ? "\x01" + c.package : c.number;
        Row& r = rows[key];
        r.number = c.number; r.package = c.package;
        if (r.package.empty()) { const PartNumber* pn = p.part_of(c); if (pn) r.package = pn->package; }
        r.refs.push_back(c.ref);
        if (c.dnp) ++r.dnp;
    }
    const std::string nl = "\r\n";
    std::string s;
    const char* hdr[] = {"Item", "Qty", "Part Number", "Package", "Description", "References", "Manufacturer PN",
                         "Vendor PN", "Customer PN", "Process", "Polarized"};
    for (size_t i = 0; i < sizeof(hdr) / sizeof(hdr[0]); ++i) { if (i) s += dl; s += hdr[i]; }
    s += nl;
    int item = 0;
    for (auto& kv : rows) {
        Row& r = kv.second;
        std::sort(r.refs.begin(), r.refs.end(), ref_less);
        std::string refs;
        for (size_t i = 0; i < r.refs.size(); ++i) { if (i) refs += ", "; refs += r.refs[i]; }
        auto it = p.parts.find(r.number);
        const PartNumber* pn = it == p.parts.end() ? nullptr : &it->second;
        std::vector<std::string> cells = {
            std::to_string(++item), std::to_string(r.refs.size() - (inc_dnp ? 0 : r.dnp)),
            r.number.empty() ? "(no part number)" : r.number, r.package, pn ? pn->description : "", refs,
            pn ? pn->manf_pn : "", pn ? pn->vend_pn : "", pn ? pn->cust_pn : "", pn ? pn->default_process : "",
            pn && pn->polarized ? "Y" : "N"};
        for (size_t i = 0; i < cells.size(); ++i) { if (i) s += dl; s += csv_cell(cells[i], dl); }
        s += nl;
    }
    put(out, s);
    return true;
}

} // namespace

// ===========================================================================
// registration
// ===========================================================================
void register_juki_import(PluginRegistry& r);   // juki_import_plugin.cpp

void register_builtin_plugins(PluginRegistry& r) {
    using K = PluginKind;

    // ---- CircuitCAM ----
    {
        PluginInfo i = make_info("circuitcam.cpf", "Legacy CAM project / library (migration)", K::Importer, {"cpf", "mdb"}, "Project");
        i.import_mode = ImportMode::Project;
        i.options.push_back(opt_bool_def("library_only", "Library only (packages + part numbers)", false,
                                         "Used by Library > Import Legacy Package Library (.mdb)"));
        r.add(std::make_unique<FnImporter>(
            i,
            [](const PluginInfo& inf, const std::string& path, const std::string& head) {
                if (head.size() >= 19 && head.compare(4, 15, "Standard Jet DB") == 0) return 90;
                return has_ext(inf, path) ? 20 : 0;
            },
            import_cpf));
    }

    // ---- GenCAD ----
    {
        PluginInfo i = make_info("gencad", "GenCAD", K::Importer, {"cad", "gcd", "gen"}, "CAD");
        i.options = {opt_bool_def("replace", "Replace existing components / packages", true),
                     opt_bool_def("import_board", "Import board outline", true),
                     opt_bool_def("import_packages", "Import shapes as packages", true),
                     opt_bool_def("fiducial_by_name", "Refs FID* are fiducials", true)};
        r.add(std::make_unique<FnImporter>(i, sniff_gencad,
            [](const std::string& path, const Options& o, Project& prj, ImportReport& rep, std::string& err) {
                io::GencadOptions go;
                go.replace = opt_bool(o, "replace", true);
                go.import_board = opt_bool(o, "import_board", true);
                go.import_packages = opt_bool(o, "import_packages", true);
                go.fiducial_by_name = opt_bool(o, "fiducial_by_name", true);
                io::GencadResult res;
                if (!io::import_gencad_file(path, prj, go, res, err)) return false;
                rep.components_added = res.components;
                rep.packages_added = res.shapes;
                rep.parts_added = res.devices;
                rep.messages.push_back(res.describe());
                for (auto& w : res.warnings) rep.messages.push_back(w);
                if (!res.nets.empty()) rep.messages.push_back(std::to_string(res.nets.size()) + " nets read");
                return true;
            }));
    }

    // ---- IPC-D-356 ----
    {
        PluginInfo i = make_info("ipc356", "IPC-D-356 netlist", K::Importer, {"ipc", "356", "d356"}, "CAD");
        i.options = {opt_bool_def("replace", "Replace existing components", true),
                     opt_bool_def("include_vias", "Import vias", false),
                     opt_bool_def("normalise_rotation", "Recover part rotation (axis-aligned packages)", true)};
        r.add(std::make_unique<FnImporter>(i, sniff_ipc356,
            [](const std::string& path, const Options& o, Project& prj, ImportReport& rep, std::string& err) {
                io::Ipc356Options io_;
                io_.replace = opt_bool(o, "replace", true);
                io_.include_vias = opt_bool(o, "include_vias");
                io_.normalise_rotation = opt_bool(o, "normalise_rotation", true);
                io::Ipc356Result res;
                if (!io::import_ipc356_file(path, prj, io_, res, err)) return false;
                rep.components_added = res.components;
                rep.packages_added = res.packages;
                rep.messages.push_back(res.describe());
                for (auto& w : res.warnings) rep.messages.push_back(w);
                return true;
            }));
    }

    // ---- Centroid ----
    {
        PluginInfo i = make_info("centroid", "ASCII centroid (XY)", K::Importer, {"txt", "csv", "pos", "xy", "mnt"}, "Centroid");
        i.options = {opt_enum_def("units", "Units", {"auto", "mil", "inch", "mm"}, "auto"),
                     opt_enum_def("mode", "Mode", {"replace", "merge", "fusion"}, "merge",
                                  "fusion = attach references to already digitized components"),
                     opt_num_def("rot_offset", "Rotation offset (deg)", "0"),
                     opt_num_def("factor", "Unit factor", "1"),
                     opt_enum_def("sides", "Sides", {"both", "top", "bottom"}, "both"),
                     opt_bool_def("bottom_mirror_x", "Bottom rows viewed from bottom (mirror X)", false),
                     opt_bool_def("bottom_negate_rot", "Bottom rows: negate rotation", false),
                     opt_num_def("fusion_tol", "Fusion tolerance (mm)", "0.5"),
                     opt_str_def("key_ref", "Fusion key component"),
                     opt_bool_def("clear_refs", "Fusion: clear references first", false)};
        r.add(std::make_unique<FnImporter>(i, sniff_centroid,
            [](const std::string& path, const Options& o, Project& prj, ImportReport& rep, std::string& err) {
                io::CentroidOptions co;
                const std::string u = opt(o, "units", "auto");
                co.units = u == "mil" ? io::LenUnit::Mil : u == "inch" ? io::LenUnit::Inch : u == "mm" ? io::LenUnit::MM : io::LenUnit::Auto;
                const std::string m = opt(o, "mode", "merge");
                co.mode = m == "replace" ? io::CentroidMode::Replace : m == "fusion" ? io::CentroidMode::Fusion : io::CentroidMode::Merge;
                co.rot_offset = opt_num(o, "rot_offset", 0);
                co.factor = opt_num(o, "factor", 1);
                if (co.factor == 0) co.factor = 1;
                const std::string sd = opt(o, "sides", "both");
                co.side_select = sd == "top" ? io::SideSelect::Top : sd == "bottom" ? io::SideSelect::Bottom : io::SideSelect::Both;
                co.bottom_mirror_x = opt_bool(o, "bottom_mirror_x");
                co.bottom_negate_rot = opt_bool(o, "bottom_negate_rot");
                co.fusion_tol_mm = opt_num(o, "fusion_tol", 0.5);
                co.key_ref = opt(o, "key_ref");
                co.clear_refs = opt_bool(o, "clear_refs");
                io::CentroidReport cr;
                if (!io::import_centroid_file(path, prj, co, cr, err)) return false;
                rep.components_added = cr.added;
                rep.components_updated = cr.updated + cr.fused;
                rep.messages.push_back(cr.describe());
                for (auto& s : cr.messages) rep.messages.push_back(s);
                return true;
            }));
    }

    // ---- BOM ----
    {
        PluginInfo i = make_info("bom", "Bill of Materials", K::Importer, {"prn", "bom", "csv", "txt"}, "BOM");
        i.options = {opt_bool_def("replace_parts", "Replace part number library", false),
                     opt_bool_def("clear_comp_numbers", "Clear component part numbers first", false),
                     opt_bool_def("overwrite_package", "BOM package overrides component package", false),
                     opt_bool_def("multiple_pn_check", "Multiple PN check (keep first)", true),
                     opt_bool_def("dnp_unlisted", "Mark components not in BOM as DNP", false)};
        r.add(std::make_unique<FnImporter>(i, sniff_bom,
            [](const std::string& path, const Options& o, Project& prj, ImportReport& rep, std::string& err) {
                io::BomOptions bo;
                bo.replace_parts = opt_bool(o, "replace_parts");
                bo.clear_comp_numbers = opt_bool(o, "clear_comp_numbers");
                bo.overwrite_package = opt_bool(o, "overwrite_package");
                bo.multiple_pn_check = opt_bool(o, "multiple_pn_check", true);
                bo.dnp_unlisted = opt_bool(o, "dnp_unlisted");
                io::BomReport br;
                if (!io::import_bom_file(path, prj, bo, br, err)) return false;
                rep.parts_added = br.parts_added;
                rep.components_updated = br.linked;
                rep.messages.push_back(br.describe());
                for (auto& s : br.messages) rep.messages.push_back(s);
                return true;
            }));
    }

    // ---- Gerber / Excellon ----
    {
        PluginInfo i = make_info("gerber", "Gerber layer", K::Importer,
                                 {"gbr", "ger", "gtl", "gbl", "gts", "gbs", "gto", "gbo", "gtp", "gbp", "spl", "pho", "274", "art"},
                                 "Gerber");
        i.options = {opt_enum_def("side", "Side", {"auto", "top", "bottom"}, "auto"),
                     opt_path_def("aperture_list", "Aperture list (RS-274D)"),
                     opt_bool_def("auto_aperture_list", "Look for an aperture list next to the file", true),
                     opt_enum_def("aperture_units", "Aperture list units", {"auto", "mil", "inch", "mm"}, "auto")};
        r.add(std::make_unique<FnImporter>(i, sniff_gerber,
            [](const std::string& path, const Options& o, Project& prj, ImportReport& rep, std::string& err) {
                return import_gerber_layer(path, o, prj, rep, err, false);
            }));
    }
    {
        PluginInfo i = make_info("excellon", "Excellon drill", K::Importer, {"drl", "xln", "txt"}, "Gerber");
        i.options = {opt_enum_def("side", "Side", {"auto", "top", "bottom"}, "auto")};
        r.add(std::make_unique<FnImporter>(i, sniff_excellon,
            [](const std::string& path, const Options& o, Project& prj, ImportReport& rep, std::string& err) {
                return import_gerber_layer(path, o, prj, rep, err, true);
            }));
    }

    // ---- native project ----
    {
        PluginInfo i = make_info("pcbp.json", "PCB Plotter project", K::Importer, {"pcbp"}, "Project");
        i.import_mode = ImportMode::Project;
        r.add(std::make_unique<FnImporter>(
            i,
            [](const PluginInfo& inf, const std::string& path, const std::string& head) {
                if (head.find("\"PCBPlotter.Project\"") != std::string::npos) return 100;
                return has_ext(inf, path) ? 50 : 0;
            },
            [](const std::string& path, const Options&, Project& prj, ImportReport& rep, std::string& err) {
                if (!load_project(path, prj, err)) return false;
                rep.components_added = (int)prj.comps.size();
                rep.packages_added = (int)prj.packages.size();
                rep.parts_added = (int)prj.parts.size();
                rep.layers_added = (int)prj.layers.size();
                return true;
            }));
    }

    // ======================= exporters =======================
    struct JukiDef { const char* id; const char* name; bool variant; juki::HlcFormat fmt; juki::HlcVariant var; };
    const JukiDef jd[] = {
        {"juki.h8h", "HLC 10+ program (H8H)", false, juki::HlcFormat::H8H, juki::HlcVariant::H8H},
        {"juki.h7h", "HLC 9.x program (H7H)", false, juki::HlcFormat::H7H, juki::HlcVariant::H7H},
        {"juki.h5h", "HLC 7.x program (H5H)", false, juki::HlcFormat::H5H, juki::HlcVariant::H5H},
        {"juki.h4h", "HLC 5.x/6.x program (H4H)", false, juki::HlcFormat::H4H, juki::HlcVariant::H4H_v6},
        {"juki.e41", "Per-machine program (E41)", true, juki::HlcFormat::H4H, juki::HlcVariant::E41},
        {"juki.e42", "Per-machine program (E42)", true, juki::HlcFormat::H4H, juki::HlcVariant::E42},
        {"juki.e43", "Per-machine program (E43)", true, juki::HlcFormat::H4H, juki::HlcVariant::E43},
        {"juki.e44", "Per-machine program (E44)", true, juki::HlcFormat::H4H, juki::HlcVariant::E44},
        {"juki.e45", "Per-machine program (E45)", true, juki::HlcFormat::H4H, juki::HlcVariant::E45},
        {"juki.e46", "Per-machine program (E46)", true, juki::HlcFormat::H4H, juki::HlcVariant::E46},
        {"juki.e51", "Per-machine program (E51)", true, juki::HlcFormat::H4H, juki::HlcVariant::E51},
    };
    for (const JukiDef& d : jd) {
        const std::string ext = lower(juki::hlc_extension(d.var));
        PluginInfo i = make_info(d.id, d.name, K::Exporter, {ext}, "Machine");
        i.vendor = "PCB Plotter";
        i.export_input = ExportInput::MachineJob;
        i.options = juki_options(!d.variant && d.fmt == juki::HlcFormat::H4H, !d.variant && d.fmt == juki::HlcFormat::H8H);
        const JukiDef dd = d;
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn(
            [dd](const MachineJob& j, const Options& o, std::vector<uint8_t>& out, std::string& err) {
                const MachineJob job = job_with_options(j, o);
                return dd.variant ? juki::write_hlc_variant(job, dd.var, out, err) : juki::write_hlc(job, dd.fmt, out, err);
            })));
    }
    {
        PluginInfo i = make_info("generic.csv", "Generic placement CSV", K::Exporter, {"csv", "txt"}, "Machine");
        i.export_input = ExportInput::MachineJob;
        i.options = {opt_str_def("columns", "Columns", "ref,x,y,rot,part,package,side",
                                 "ref part package desc x y rot side circuit skip polarized smt leads height"),
                     opt_enum_def("units", "Units", {"mm", "inch", "mil"}, "mm"),
                     opt_num_def("decimals", "Decimals (-1 = auto)", "-1", true),
                     opt_enum_def("delimiter", "Delimiter", {"comma", "semicolon", "tab", "space"}, "comma"),
                     opt_bool_def("header", "Header row", true),
                     opt_enum_def("angle", "Angle convention", {"CCW", "CW"}, "CCW"),
                     opt_bool_def("include_skipped", "Include skipped (DNP / x-out)", false),
                     opt_bool_def("fiducials", "Append fiducials", false),
                     opt_bool_def("crlf", "CRLF line endings", true)};
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn(export_generic_csv)));
    }
    {
        PluginInfo i = make_info("generic.xy", "Generic XY text (centroid style)", K::Exporter, {"txt", "xy"}, "Machine");
        i.export_input = ExportInput::MachineJob;
        i.options = {opt_enum_def("units", "Units", {"mil", "inch", "mm"}, "mil"),
                     opt_num_def("decimals", "Decimals (-1 = auto)", "-1", true),
                     opt_enum_def("angle", "Angle convention", {"CCW", "CW"}, "CCW"),
                     opt_bool_def("part", "Part number column", false),
                     opt_bool_def("package", "Package column", false),
                     opt_bool_def("header", "Commented header line", false),
                     opt_bool_def("include_skipped", "Include skipped (DNP / x-out)", false)};
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn(export_generic_xy)));
    }

    struct VendorDef {
        const char* id;
        const char* name;
        const char* extension;
        const char* vendor;
        VendorFormat format;
    };
    for (const auto& d : {
             VendorDef{"europlacer.bla", "Europlacer Blakell Robin", "bla", "Europlacer", VendorFormat::Blakell},
             VendorDef{"tdk.rx11", "TDK RX-11 insertion template", "ncp", "TDK", VendorFormat::Tdk},
             VendorDef{"tws.quadra", "TWS Quadra placement import", "asq", "TWS Automation", VendorFormat::Quadra},
             VendorDef{"dima.pcb", "Dima PCB placement template", "pcb", "Dima", VendorFormat::Dima},
             VendorDef{"europlacer.dp", "Europlacer / Multroniks placement import", "dp", "Europlacer / Multroniks", VendorFormat::Dp},
             VendorDef{"assembleon.ftip", "Assembleon / Philips FTIP placement import", "ftp", "Assembleon / Philips", VendorFormat::Ftip},
             VendorDef{"generic.flat", "Generic placement flat file", "txt", "Generic", VendorFormat::Flat},
             VendorDef{"janets.juk", "Juki JaNets placement import", "juk", "Juki", VendorFormat::Janets},
             VendorDef{"samsung.cp.ssa", "Samsung CP placement import", "ssa", "Samsung", VendorFormat::Samsung},
             VendorDef{"mydata.directline", "Mydata Directline mount file", "mnt", "Mydata / Mycronic", VendorFormat::Directline},
             VendorDef{"mydata.tpsys", "Mydata TPSys placement import", "pcb", "Mydata / Mycronic", VendorFormat::TPSys},
         }) {
        PluginInfo i = make_info(d.id, d.name, K::Exporter, {d.extension}, "Machine");
        i.vendor = d.vendor;
        i.export_input = ExportInput::MachineJob;
        if (d.format == VendorFormat::Flat)
            i.options.push_back(opt_enum_def("units", "Units", {"mm", "inch", "mil"}, "mm"));
        if (d.format == VendorFormat::Janets || d.format == VendorFormat::Dp)
            i.options.push_back(opt_enum_def("units", "Units", {"mm", "inch"}, "mm"));
        if (d.format == VendorFormat::Samsung)
            i.options.push_back(opt_enum_def("coordinate", "Coordinate header", {"blank", "lower-left"}, "blank",
                                             "Header label only. Set the origin in the machine setup."));
        if (d.format == VendorFormat::Dp)
            i.options.push_back(opt_enum_def("target", "Machine family", {"Europlacer", "Multroniks"}, "Europlacer"));
        if (d.format != VendorFormat::Blakell)
            i.options.push_back(opt_enum_def("angle", "Angle convention", {"CCW", "CW"}, "CCW"));
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn(
            [format = d.format](const MachineJob& j, const Options& o, std::vector<uint8_t>& out, std::string& err) {
                return export_vendor(j, o, format, out, err);
            })));
    }

    {
        PluginInfo i = make_info("mydata.cmp", "Mydata component library (one part)", K::Exporter, {"cmp"}, "Machine");
        i.vendor = "Mydata / Mycronic";
        i.export_input = ExportInput::MachineJob;
        i.options = {opt_str_def("cmp.part", "Part number", "", "Select a new part number. Import can overwrite an existing library entry."),
                     opt_str_def("cmp.C01", "C01 library value"),
                     opt_str_def("cmp.C02", "C02 library value"),
                     opt_str_def("cmp.C03", "C03 values (three fields)"),
                     opt_str_def("cmp.C05", "C05 library value"),
                     opt_str_def("cmp.C07", "C07 library value")};
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn(export_mydata_cmp)));
    }

    {
        PluginInfo i = make_info("mydata.cmp.batch", "Mydata component library (batch)", K::Exporter, {"cmp"}, "Machine");
        i.vendor = "Mydata / Mycronic";
        i.export_input = ExportInput::MachineJob;
        i.options = {opt_str_def("cmp.gen_file", "GEN file path (optional)", "", "Select only parts reported as unknown by Mydata."),
                     opt_str_def("cmp.rows", "Component rows", "",
                                 "part;C01;C02;C03;C05;C07. Separate rows with |. Existing library entries may be overwritten.")};
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn(export_mydata_cmp_batch)));
    }

    {
        PluginInfo i = make_info("universal.hsp", "Universal HSP placement template", K::Exporter, {"ncz"}, "Machine");
        i.vendor = "Universal";
        i.export_input = ExportInput::MachineJob;
        i.options = {opt_enum_def("units", "Units", {"mm", "inch"}, "mm"),
                     opt_enum_def("angle", "Angle convention", {"CCW", "CW"}, "CCW")};
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn(export_universal_hsp)));
    }

    {
        PluginInfo i = make_info("robotas.mascot", "Robotas Mascot assembly (MAS 1.6)", K::Exporter, {"asy"}, "Machine");
        i.vendor = "Robotas";
        i.export_input = ExportInput::MachineJob;
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn(export_mascot)));
    }

    {
        PluginInfo i = make_info("universal.radial", "Universal Radial PUT", K::Exporter, {"put"}, "Machine");
        i.vendor = "Universal";
        i.export_input = ExportInput::MachineJob;
        i.options = {opt_str_def("uni.path", "UNI library path", "", "Tab-separated part number, R, feeder head. Set the machine origin to reference point 1.")};
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn([](const MachineJob& j, const Options& o, std::vector<uint8_t>& out, std::string& err) {
            return export_universal_radial(j, o, out, err);
        })));
    }

    {
        PluginInfo i = make_info("universal.radial2", "Universal Radial Style 2", K::Exporter, {"txt"}, "Machine");
        i.vendor = "Universal";
        i.export_input = ExportInput::MachineJob;
        i.options = {opt_str_def("uni.path", "UNI library path", "", "Set the machine origin to reference point 1.")};
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn(
            [](const MachineJob& j, const Options& o, std::vector<uint8_t>& out, std::string& err) {
                return export_universal_radial(j, o, out, err, true);
            })));
    }

    for (bool style2 : {false, true}) {
        PluginInfo i = make_info(style2 ? "universal.vcd2" : "universal.vcd",
                                 style2 ? "Universal VCD Style 2" : "Universal VCD PUT",
                                 K::Exporter, {style2 ? "txt" : "put"}, "Machine");
        i.vendor = "Universal";
        i.export_input = ExportInput::MachineJob;
        i.options = {opt_str_def("uni.path", "UNI library path", "", "Set the machine origin to reference point 1.")};
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn(
            [style2](const MachineJob& j, const Options& o, std::vector<uint8_t>& out, std::string& err) {
                return export_universal_radial(j, o, out, err, style2, true);
            })));
    }

    {
        PluginInfo i = make_info("contact.cs400", "Contact Systems CS400 DNL (basic)", K::Exporter, {"dnl"}, "Machine");
        i.vendor = "Contact Systems";
        i.export_input = ExportInput::MachineJob;
        i.options = {opt_str_def("cs4.path", "CS4 library path"),
                     opt_enum_def("cs4.model", "Controller model", {"D/E", "CS400C", "SM-CS400C"}, "D/E")};
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn(export_cs400)));
    }

    {
        PluginInfo i = make_info("iss.xml", "Juki ISS production program", K::Exporter, {"iss"}, "Machine");
        i.vendor = "Juki";
        i.export_input = ExportInput::MachineJob;
        i.options = {opt_enum_def("iss.transport", "Board transport", {"LTOR", "RTOL"}, "LTOR")};
        r.add(std::make_unique<FnExporter>(i, FnExporter::JobFn(export_juki_iss)));
    }

    // ---- project exporters ----
    {
        PluginInfo i = make_info("gencad", "GenCAD", K::Exporter, {"cad"}, "CAD");
        i.export_input = ExportInput::Project;
        i.options = {opt_enum_def("side", "Side", {"both", "top", "bottom"}, "both"),
                     opt_bool_def("include_board", "Board outline", true),
                     opt_bool_def("include_fiducials", "Fiducials", true),
                     opt_num_def("decimals", "Decimals", "4", true)};
        r.add(std::make_unique<FnExporter>(i, FnExporter::PrjFn(
            [](const Project& p, const Options& o, std::vector<uint8_t>& out, std::string& err) {
                io::GencadExportOptions go;
                const std::string s = opt(o, "side", "both");
                go.side = s == "top" ? io::SideFilter::Top : s == "bottom" ? io::SideFilter::Bottom : io::SideFilter::Both;
                go.include_board = opt_bool(o, "include_board", true);
                go.include_fiducials = opt_bool(o, "include_fiducials", true);
                go.decimals = std::clamp((int)opt_num(o, "decimals", 4), 1, 8);
                std::string text;
                if (!io::export_gencad(p, go, text, err)) return false;
                put(out, text);
                return true;
            })));
    }
    {
        PluginInfo i = make_info("bom.csv", "Bill of Materials (CSV)", K::Exporter, {"csv"}, "Report");
        i.export_input = ExportInput::Project;
        i.options = {opt_enum_def("delimiter", "Delimiter", {"comma", "semicolon", "tab"}, "comma"),
                     opt_enum_def("side", "Side", {"both", "top", "bottom"}, "both"),
                     opt_bool_def("include_dnp", "Include DNP components", false)};
        r.add(std::make_unique<FnExporter>(i, FnExporter::PrjFn(export_bom_csv)));
    }
    {
        PluginInfo i = make_info("pcbp.json", "PCB Plotter project JSON", K::Exporter, {"json", "pcbp"}, "Project");
        i.export_input = ExportInput::Project;
        i.options = {opt_bool_def("layer_content", "Embed Gerber layer content", true),
                     opt_bool_def("pretty", "Indented", true)};
        r.add(std::make_unique<FnExporter>(i, FnExporter::PrjFn(
            [](const Project& p, const Options& o, std::vector<uint8_t>& out, std::string&) {
                put(out, project_to_json(p, opt_bool(o, "layer_content", true)).dump(opt_bool(o, "pretty", true) ? 1 : -1));
                return true;
            })));
    }
    register_juki_import(r);
    register_cad_formats_v2(r);   // v2 CAD importers (cad_formats_v2.cpp)
    register_viewmate_import(r); // ViewMate .bin jobs (viewmate_plugin.cpp)
}

} // namespace pcbp
