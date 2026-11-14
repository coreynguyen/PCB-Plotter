// ============================================================================
// dialogs.cpp — PCB Plotter modal dialogs (all built on ui::Form).
//
// Layouts follow the CircuitCAM Express dialogs listed in specs/UI_INVENTORY.md
// §3 (Options / Library Connection / Default Directories, Grid, Assembly Info,
// Single Location Edit, BOM item, Fiducial Properties, Panelization + Step and
// Repeat, Global Offset/Rotation, Add Board Frame, Model Point Properties,
// Factory Model, ASCII Import, BOM Import) re-expressed as declarative forms.
// ============================================================================
#include "app.h"
#include "../core/fileio.h"
#include "../core/plot_align.h"
#include "../core/arrays.h"   // R22
#include "../io/bom_import.h"
#include "../io/centroid_import.h"
#include "../io/text_table.h"
#include "../ui/d2d.h"
#include "../ui/form.h"
#include "../ui/theme.h"
#include "../gui/gapp.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <sstream>
#include <cstdlib>
#include <cctype>
#include <utility>

namespace pcbp::app {
using namespace pcbp::ui;

namespace {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
std::wstring ulen() { return L" (" + units_label() + L")"; }

Form& len_field(Form& f, const std::string& key, const std::wstring& label, double mm, const std::wstring& page = L"") {
    return f.text(key, label + ulen(), fmt_len(mm, false), page);
}
double get_len(const Form& f, const std::string& key, double def_mm) { return parse_len(f.str(key), def_mm); }

std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
bool ieq(const std::string& a, const std::string& b) { return upper(a) == upper(b); }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ';') { cur = trim(cur); if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    cur = trim(cur);
    if (!cur.empty()) out.push_back(cur);
    return out;
}
std::string join_list(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) { if (!s.empty()) s += ", "; s += x; }
    return s;
}

const std::vector<std::wstring>& side_names() { static std::vector<std::wstring> v{L"Top", L"Bottom"}; return v; }
const std::vector<std::wstring>& kind_names() { static std::vector<std::wstring> v{L"Placement", L"Manual", L"Inspection", L"Process", L"Test"}; return v; }
const std::vector<std::wstring>& feature_names() { static std::vector<std::wstring> v{L"Component", L"Fiducial", L"Bad Mark", L"Hole"}; return v; }
const std::vector<std::wstring>& rot_names() { static std::vector<std::wstring> v{L"0", L"90", L"180", L"270"}; return v; }

std::vector<std::wstring> part_choices(const Project& p) {
    std::vector<std::wstring> v;
    for (auto& kv : p.parts) v.push_back(W(kv.first));
    return v;
}
std::vector<std::wstring> package_choices(const Project& p) {
    std::vector<std::wstring> v;
    for (auto& kv : p.packages) v.push_back(W(kv.first));
    return v;
}
std::vector<std::wstring> process_choices(const Project& p) {
    std::set<std::string> s{"SMT", "AUTO", "SLIDE", "HAND", "REFLOW", "WAVE", "AOI", "TEST", "WASH", "PASTE"};
    for (auto& mp : p.model.points) if (!mp.process_type.empty()) s.insert(mp.process_type);
    for (auto& kv : p.parts) if (!kv.second.default_process.empty()) s.insert(kv.second.default_process);
    std::vector<std::wstring> v;
    for (auto& x : s) v.push_back(W(x));
    return v;
}
std::vector<std::wstring> ref_choices(const Project& p, bool components_only) {
    std::vector<std::string> r;
    for (auto& c : p.comps) if (!c.ref.empty() && (!components_only || c.feature == Feature::Component)) r.push_back(c.ref);
    std::sort(r.begin(), r.end(), ref_less);
    std::vector<std::wstring> v;
    for (auto& x : r) v.push_back(W(x));
    return v;
}

std::vector<Vec2> rect_poly(const BBox& b) {
    return {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}, {b.x0, b.y0}};
}

[[maybe_unused]] int round_i(double v) { return (int)std::lround(v); }

std::wstring ini_file_path() {
    std::wstring p = exe_dir() + L"\\PCBPlotter.ini";
    DWORD a = GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return p;
    wchar_t app[MAX_PATH] = L"";
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, app))) {
        std::wstring q = std::wstring(app) + L"\\PCBPlotter\\PCBPlotter.ini";
        if (GetFileAttributesW(q.c_str()) != INVALID_FILE_ATTRIBUTES) return q;
    }
    return p + L"  (not created yet)";
}

// machine exporters (accept MachineJob input)
std::vector<Exporter*> machine_exporters() {
    std::vector<Exporter*> v;
    for (auto& e : PluginRegistry::get().exporters())
        if (e->info().export_input == ExportInput::MachineJob || e->info().category == "Machine") v.push_back(e.get());
    return v;
}

// ---------------------------------------------------------------------------
// Model point editor (shared by dlg_model_point and dlg_factory_model)
// ---------------------------------------------------------------------------
bool edit_model_point(HWND owner, const Project& prj, ModelPoint& mp, const std::vector<ModelPoint>& all, int self) {
    auto exps = machine_exporters();
    std::vector<std::wstring> exp_names{L"(none)"};
    std::wstring exp_sel = L"(none)";
    for (auto* e : exps) {
        std::wstring n = W(e->info().name) + L"  [" + W(e->info().id) + L"]";
        exp_names.push_back(n);
        if (e->info().id == mp.machine.exporter_id) exp_sel = n;
    }
    Options mopts = mp.machine.options;
    const std::wstring G = L"General", F = L"Filters", M = L"Machine", N = L"Notes";

    Form f(L"Model Point Properties", 560);
    f.icon((int)Icon::FactoryModel);
    f.two_columns();
    f.header(L"Model point: " + W(mp.name));
    f.text("name", L"Name", W(mp.name), G).wide();
    f.combo("ptype", L"Process type", process_choices(prj), W(mp.process_type), true, G);
    f.combo("kind", L"Kind", kind_names(), kind_names()[(int)mp.kind], false, G);
    f.combo("side", L"Preferred side", side_names(), side_names()[(int)mp.side], false, G);
    f.color("color", L"Colour", mp.color, G);
    f.text("line", L"Line", W(mp.line), G);
    f.text("vatitle", L"Visual aid title", W(mp.visual_aid_title), G);
    f.check("wip", L"WIP point", mp.wip, G);
    f.check("repair", L"Repair point", mp.repair, G);
    f.check("insp", L"Inspection point", mp.inspection, G);

    f.label(L"Components are auto-assigned to this point when their BOM process, part number\n"
            L"or package matches one of these comma-separated lists.", F);
    f.text("fproc", L"Process types", W(join_list(mp.filter_process)), F).wide();
    f.text("fnum", L"Part numbers", W(join_list(mp.filter_numbers)), F).wide();
    f.text("fpkg", L"Packages", W(join_list(mp.filter_packages)), F).wide();

    const MachineSetup& ms = mp.machine;
    f.check("hasm", L"This point programs a machine", mp.has_machine, M).wide();
    f.combo("exp", L"Machine type", exp_names, exp_sel, false, M).wide();
    f.button("mopts", L"Machine options...", [&](Form& ff) {
        int i = ff.index("exp");
        if (i <= 0 || i > (int)exps.size()) { message(owner, L"Machine Options", L"Choose a machine type first."); return; }
        const PluginInfo& info = exps[i - 1]->info();
        if (info.options.empty()) { message(owner, L"Machine Options", W(info.name) + L" has no options."); return; }
        Options o = default_options(info);
        for (auto& kv : mopts) o[kv.first] = kv.second;
        if (dlg_plugin_options(owner, info, o)) mopts = o;
    }, M);
    f.text("tfile", L"Program file", W(ms.target_file), M).wide();
    f.combo("orient", L"Board orientation", rot_names(), std::to_wstring(ms.orientation), false, M);
    f.combo("mside", L"Machine side", side_names(), side_names()[(int)ms.side], false, M);
    len_field(f, "hox", L"Hard offset X", ms.hard_offset.x, M);
    len_field(f, "hoy", L"Hard offset Y", ms.hard_offset.y, M);
    f.check("flip", L"Board flipped", ms.flipped, M);
    f.check("fac", L"Force angle calculation", ms.force_angle_calc, M);
    f.check("single", L"Treat panel as single board", ms.single_board, M);
    f.check("hasorg", L"Machine-specific origin", ms.has_origin, M);
    f.separator(M);
    len_field(f, "ox", L"Origin X", ms.origin.x, M);
    len_field(f, "oy", L"Origin Y", ms.origin.y, M);

    f.multiline("notes", L"Notes", W(mp.notes), 14, N);

    f.validate([&](Form& ff, std::wstring& err) {
        std::string n = trim(ff.u8("name"));
        if (n.empty()) { err = L"The model point needs a name."; return false; }
        for (int i = 0; i < (int)all.size(); ++i)
            if (i != self && ieq(all[i].name, n)) { err = L"Another model point is already called '" + W(n) + L"'."; return false; }
        if (trim(ff.u8("ptype")).empty()) { err = L"A process type is required."; return false; }
        return true;
    });
    if (!f.run(owner)) return false;

    mp.name = trim(f.u8("name"));
    mp.process_type = upper(trim(f.u8("ptype")));
    mp.kind = (PointKind)std::max(0, f.index("kind"));
    mp.side = (Side)std::max(0, f.index("side"));
    mp.color = f.color_value("color");
    mp.line = f.u8("line");
    mp.visual_aid_title = f.u8("vatitle");
    mp.wip = f.checked("wip"); mp.repair = f.checked("repair"); mp.inspection = f.checked("insp");
    mp.filter_process = split_list(f.u8("fproc"));
    mp.filter_numbers = split_list(f.u8("fnum"));
    mp.filter_packages = split_list(f.u8("fpkg"));
    mp.has_machine = f.checked("hasm");
    MachineSetup& m = mp.machine;
    int ei = f.index("exp");
    m.exporter_id = (ei > 0 && ei <= (int)exps.size()) ? exps[ei - 1]->info().id : std::string();
    if (m.exporter_id.empty()) mp.has_machine = false;
    m.options = mopts;
    m.target_file = f.u8("tfile");
    m.orientation = std::max(0, f.index("orient")) * 90;
    m.side = (Side)std::max(0, f.index("mside"));
    m.hard_offset = {get_len(f, "hox", m.hard_offset.x), get_len(f, "hoy", m.hard_offset.y)};
    m.flipped = f.checked("flip");
    m.force_angle_calc = f.checked("fac");
    m.single_board = f.checked("single");
    m.has_origin = f.checked("hasorg");
    m.origin = {get_len(f, "ox", m.origin.x), get_len(f, "oy", m.origin.y)};
    std::string notes = f.u8("notes");
    notes.erase(std::remove(notes.begin(), notes.end(), '\r'), notes.end());
    mp.notes = notes;
    return true;
}

DocPtr find_docptr(const Doc& d) {
    for (auto& p : open_docs()) if (p.get() == &d) return p;
    return nullptr;
}


} // namespace

// ===========================================================================
// Options (Global Settings)
// ===========================================================================
bool dlg_options(HWND owner) {
    Settings& s = settings();
    const std::wstring P1 = L"Options", P2 = L"Library Connection", P3 = L"Default Directories";
    Form f(L"Global Settings", 560);
    f.icon((int)Icon::Settings).header(L"Global Settings").subtitle(L"Preferences shared by every project (stored in PCBPlotter.ini)");
    f.heading(L"Company", P1);
    f.text("company", L"Company name", s.company, P1);
    f.multiline("addr", L"Address (printed in visual aid headers)", s.company_addr, 3, P1);
    f.heading(L"Display", P1);
    f.combo("units", L"Units", {L"Millimetres (mm)", L"Inches (in)", L"Mils (1/1000 in)"}, s.units == 0 ? L"Millimetres (mm)" : s.units == 2 ? L"Mils (1/1000 in)" : L"Inches (in)", false, P1);
    f.combo("theme", L"Theme", {L"Follow Windows", L"Light", L"Dark"}, s.theme == 1 ? L"Light" : s.theme == 2 ? L"Dark" : L"Follow Windows", false, P1);
    f.color("frame", L"Default outline colour", s.frame_color, P1);
    f.heading(L"Printing", P1);
    f.number("psx", L"Print scale X", s.print_scale_x, 4, P1).tip(L"Correction factor applied to 1:1 prints in X (measure a printed board and enter actual/expected).");
    f.number("psy", L"Print scale Y", s.print_scale_y, 4, P1);
    f.heading(L"Import", P1);
    f.check("netlist", L"Import net list information from CAD files", s.import_netlist, P1);

    f.label(L"Package and part-number library used by Library \x2192 Import Library and Update Packages.\n"
            L"Either a legacy library database (.mdb) or a PCB Plotter project used as a library (.pcbp).", P2);
    f.file("lib", L"Library file", s.library_path, {{L"Libraries", L"cclib.mdb;*.mdb;*.pcbp"}, {L"All files", L"*.*"}}, false, P2);

    f.folder("dproj", L"Projects", s.dir_projects, P3);
    f.folder("dgerb", L"Gerber files", s.dir_gerber, P3);
    f.folder("dcad", L"CAD import", s.dir_cad, P3);
    f.folder("dbom", L"BOM files", s.dir_bom, P3);
    f.folder("dcent", L"ASCII / centroid", s.dir_centroid, P3);
    f.folder("dprog", L"Machine programs", s.dir_programs, P3);
    f.folder("dpic", L"Pictures", s.dir_pictures, P3);
    if (!f.run(owner)) return false;

    s.company = f.str("company");
    std::wstring a = f.str("addr");
    a.erase(std::remove(a.begin(), a.end(), L'\r'), a.end());
    s.company_addr = a;
    s.units = std::clamp(f.index("units"), 0, 2);
    int theme = std::clamp(f.index("theme"), 0, 2);
    s.frame_color = f.color_value("frame");
    s.print_scale_x = f.num("psx", s.print_scale_x);
    s.print_scale_y = f.num("psy", s.print_scale_y);
    if (s.print_scale_x <= 0) s.print_scale_x = 1;
    if (s.print_scale_y <= 0) s.print_scale_y = 1;
    s.import_netlist = f.checked("netlist");
    s.library_path = f.str("lib");
    s.dir_projects = f.str("dproj"); s.dir_gerber = f.str("dgerb"); s.dir_cad = f.str("dcad"); s.dir_bom = f.str("dbom");
    s.dir_centroid = f.str("dcent"); s.dir_programs = f.str("dprog"); s.dir_pictures = f.str("dpic");
    if (theme != s.theme) {
        s.theme = theme;
        // the shell re-applies the theme after dlg_options returns (ID_EDIT_OPTIONS)
    }
    s.save();
    return true;
}

// ===========================================================================
// Grid spacing
// ===========================================================================
bool dlg_grid_spacing(HWND owner) {
    Settings& s = settings();
    Form f(L"Set Grid Spacing", 340);
    f.icon((int)Icon::Grid);
    len_field(f, "sp", L"Spacing", s.grid_mm);
    f.check("vis", L"Show grid", s.grid_visible);
    f.check("snap", L"Snap to grid", s.snap);
    f.validate([](Form& ff, std::wstring& err) {
        if (parse_len(ff.str("sp"), -1) <= 0) { err = L"Enter a positive grid spacing."; return false; }
        return true;
    });
    if (!f.run(owner)) return false;
    s.grid_mm = get_len(f, "sp", s.grid_mm);
    s.grid_visible = f.checked("vis");
    s.snap = f.checked("snap");
    s.save();
    return true;
}

// ===========================================================================
// Assembly information
// ===========================================================================
bool dlg_assembly_info(HWND owner, Doc& d) {
    AssemblyInfo& a = d.prj.info;
    Form f(L"Edit Assembly Information", 560);
    f.icon((int)Icon::Board);
    f.two_columns();
    f.heading(L"Assembly level");
    f.text("name", L"Assembly name", W(a.name)).wide();
    f.text("desc", L"Description", W(a.description)).wide();
    f.text("num", L"Assembly number", W(a.number));
    f.text("rev", L"Assembly revision", W(a.assembly_rev));
    f.heading(L"Bill of materials level");
    f.text("pcbid", L"PCB / PWB ID", W(a.pcb_id));
    f.text("bom", L"BOM number", W(a.bom_number));
    f.text("bomrev", L"BOM revision", W(a.bom_rev));
    f.heading(L"Process level");
    f.text("manf", L"Manufacturing rev.", W(a.manf_rev));
    f.combo("std", L"Standard", {L"IPC-A-610 Class 1", L"IPC-A-610 Class 2", L"IPC-A-610 Class 3", L"J-STD-001"}, W(a.standard), true);
    f.heading(L"General");
    f.text("cust", L"Customer", W(a.customer));
    f.text("orig", L"Originator", W(a.originator));
    f.text("url", L"URL", W(a.url)).wide();
    f.text("c1", L"Custom 1", W(a.custom[0]));
    f.text("c2", L"Custom 2", W(a.custom[1]));
    f.text("c3", L"Custom 3", W(a.custom[2]));
    f.text("c4", L"Custom 4", W(a.custom[3]));
    f.heading(L"Details");
    f.text("mod", L"Last modified", W(a.modified)).readonly();
    f.text("cre", L"Created", W(a.created)).readonly();
    f.text("ofn", L"Original file", W(a.original_filename)).readonly().wide();
    if (!f.run(owner)) return false;
    d.checkpoint("Assembly info");
    a.name = f.u8("name"); a.description = f.u8("desc"); a.number = f.u8("num"); a.assembly_rev = f.u8("rev");
    a.pcb_id = f.u8("pcbid"); a.bom_number = f.u8("bom"); a.bom_rev = f.u8("bomrev"); a.manf_rev = f.u8("manf"); a.standard = f.u8("std");
    a.customer = f.u8("cust"); a.originator = f.u8("orig"); a.url = f.u8("url");
    a.custom[0] = f.u8("c1"); a.custom[1] = f.u8("c2"); a.custom[2] = f.u8("c3"); a.custom[3] = f.u8("c4");
    d.changed(CH_INFO);
    return true;
}

// ===========================================================================
// Single location edit / Add new component
// ===========================================================================
bool dlg_location(HWND owner, Doc& d, Component& c) {
    Project& p = d.prj;
    const bool in_project = !p.comps.empty() && &c >= p.comps.data() && &c < p.comps.data() + p.comps.size();
    Form f(in_project ? L"Edit Component" : L"Add New Component", 520);
    f.icon((int)Icon::Locations);
    f.two_columns();
    f.text("ref", L"Reference", W(c.ref)).wide();
    const Vec2 up = p.user_pos(c);   // relative to the virtual origin, like every other X/Y in the UI
    len_field(f, "x", L"X", up.x);
    len_field(f, "y", L"Y", up.y);
    f.number("rot", L"Rotation (\x00B0 CCW)", c.rot, 3);
    f.combo("side", L"Side", side_names(), side_names()[(int)c.side]);
    std::vector<std::wstring> pn = part_choices(p), pk = package_choices(p);
    f.combo("pn", L"Part number", pn, W(c.number), true).wide();
    f.combo("pkg", L"Package type", pk, W(c.package), true).wide();
    f.combo("feat", L"Feature type", feature_names(), feature_names()[(int)c.feature]);
    f.check("dnp", L"Do not populate (DNP)", c.dnp);
    f.text("url", L"URL / note", W(c.url));
    if (const PartNumber* part = p.part_of(c)) f.label(L"Description: " + W(part->description));
    f.validate([&](Form& ff, std::wstring& err) {
        std::string r = trim(ff.u8("ref"));
        if (!r.empty()) {
            for (auto& o : p.comps)
                if (&o != &c && ieq(o.ref, r)) {
                    if (!message(owner, L"Duplicate Reference", L"'" + W(r) + L"' is already used by another location. Keep the duplicate?", true)) { err.clear(); return false; }
                    break;
                }
        }
        if (parse_len(ff.str("x"), std::nan("")) != parse_len(ff.str("x"), std::nan(""))) { err = L"X is not a number."; return false; }
        if (parse_len(ff.str("y"), std::nan("")) != parse_len(ff.str("y"), std::nan(""))) { err = L"Y is not a number."; return false; }
        return true;
    });
    if (!f.run(owner)) return false;
    if (in_project) d.checkpoint("Edit location " + c.ref);
    c.ref = trim(f.u8("ref"));
    c.pos = p.from_user({get_len(f, "x", up.x), get_len(f, "y", up.y)});
    c.rot = norm_deg(f.num("rot", c.rot));
    c.side = (Side)std::max(0, f.index("side"));
    c.number = trim(f.u8("pn"));
    c.package = trim(f.u8("pkg"));
    if (c.package.empty()) if (const PartNumber* part = p.part_of(c)) c.package = part->package;
    c.feature = (Feature)std::max(0, f.index("feat"));
    c.dnp = f.checked("dnp");
    c.url = f.u8("url");
    if (in_project) d.changed(CH_GEOMETRY);
    return true;
}

// ===========================================================================
// Part number (BOM item)
// ===========================================================================
bool dlg_part_number(HWND owner, Doc& d, const std::string& number) {
    Project& p = d.prj;
    auto it = p.parts.find(number);
    const bool exists = it != p.parts.end();
    PartNumber pn;
    if (exists) pn = it->second;
    else pn.number = number;
    int used = 0;
    for (auto& c : p.comps) if (!number.empty() && c.number == number) ++used;

    Form f(exists ? L"Part Number Information" : L"Add Part Number", 600);
    f.icon((int)Icon::Bom);
    f.two_columns();
    f.header(exists ? L"Part number " + W(number) + L"  \x2014  used by " + std::to_wstring(used) + L" location(s)"
                    : std::wstring(L"New BOM item"));
    f.text("num", L"Part number", W(pn.number));
    f.combo("pkg", L"Package", package_choices(p), W(pn.package), true);
    f.text("desc", L"Description", W(pn.description)).wide();
    f.text("cust", L"Customer P/N", W(pn.cust_pn));
    f.text("vend", L"Vendor P/N", W(pn.vend_pn));
    f.text("manf", L"Manufacturer P/N", W(pn.manf_pn));
    f.text("stock", L"Stock code", W(pn.stock_code));
    f.text("prev", L"Part revision", W(pn.part_rev));
    f.text("soft", L"Software ver.", W(pn.soft_ver));
    f.text("bin", L"Bin", W(pn.bin));
    f.text("line", L"Line item", W(pn.line_item));
    f.combo("proc", L"Process", process_choices(p), W(pn.default_process), true);
    f.text("pop", L"Population type", W(pn.pop_type));
    f.text("mark", L"Marking", W(pn.part_marking));
    len_field(f, "h", L"Height", pn.height);
    f.number("cost", L"Cost", pn.cost, 4);
    f.check("pol", L"Polarized", pn.polarized);
    f.check("sock", L"Socket", pn.socket);
    f.validate([&](Form& ff, std::wstring& err) {
        std::string n = trim(ff.u8("num"));
        if (n.empty()) { err = L"Enter a part number."; return false; }
        if (n != number && p.parts.count(n)) { err = L"Part number '" + W(n) + L"' already exists."; return false; }
        return true;
    });
    if (!f.run(owner)) return false;

    d.checkpoint(exists ? "Edit part number " + number : "Add part number");
    std::string nn = trim(f.u8("num"));
    pn.number = nn;
    pn.package = trim(f.u8("pkg"));
    pn.description = f.u8("desc");
    pn.cust_pn = f.u8("cust"); pn.vend_pn = f.u8("vend"); pn.manf_pn = f.u8("manf");
    pn.stock_code = f.u8("stock"); pn.part_rev = f.u8("prev"); pn.soft_ver = f.u8("soft");
    pn.bin = f.u8("bin"); pn.line_item = f.u8("line");
    pn.default_process = upper(trim(f.u8("proc")));
    pn.pop_type = f.u8("pop"); pn.part_marking = f.u8("mark");
    pn.height = get_len(f, "h", pn.height);
    pn.cost = f.num("cost", pn.cost);
    pn.polarized = f.checked("pol");
    pn.socket = f.checked("sock");
    if (exists && nn != number) {
        p.parts.erase(number);
        for (auto& c : p.comps) if (c.number == number) c.number = nn;
        for (auto& mp : p.model.points) for (auto& x : mp.filter_numbers) if (x == number) x = nn;
    }
    p.parts[nn] = pn;
    // components with this number but no package pick up the BOM package
    for (auto& c : p.comps) if (c.number == nn && c.package.empty()) c.package = pn.package;
    d.changed(CH_GEOMETRY);
    return true;
}

// ===========================================================================
// unit-aware coordinate fields (dialog-local Units combo, like CircuitCAM)
// ===========================================================================
static const std::vector<std::wstring>& unit_names() { static std::vector<std::wstring> v{L"mm", L"inch", L"mil"}; return v; }
static double unit_mm(int u) { return u == 1 ? 25.4 : u == 2 ? 0.0254 : 1.0; }
static std::wstring fmt_u(double mm, int u) { return fmt_num(mm / unit_mm(u), u == 0 ? 4 : u == 1 ? 5 : 2); }
static double rd_u(const Form& f, const std::string& k, int u, double def_mm) {
    std::wstring t = f.str(k);
    wchar_t* e = nullptr;
    double v = std::wcstod(t.c_str(), &e);
    if (e == t.c_str()) return def_mm;
    return v * unit_mm(u);
}
// adds a Units combo that converts the listed fields when changed; returns the live unit index
static int* add_units_combo(Form& f, std::vector<std::string> keys) {
    static int cur = 0;
    cur = std::clamp(settings().units, 0, 2);
    f.combo("units", L"Units", unit_names(), unit_names()[cur]);
    f.on_change([keys](Form& ff, const std::string&) {
        int nu = std::max(0, ff.index("units"));
        if (nu == cur) return;
        for (auto& k : keys) ff.set(k, fmt_u(rd_u(ff, k, cur, 0), nu));
        cur = nu;
    });
    return &cur;
}

// ===========================================================================
// Global offset / rotation / mirror (CircuitCAM "Offset/Rotate Board")
// ===========================================================================
bool dlg_offset_rotate(HWND owner, Doc& d) {
    Project& p = d.prj;
    const BBox bb = p.board_bbox();
    const std::vector<std::wstring> ops{L"Offset", L"Rotate", L"Mirror X", L"Mirror Y"};
    const std::vector<std::wstring> rots{L"90", L"180", L"270", L"Custom angle"};
    const std::vector<std::wstring> pivots{L"Origin (0, 0)", L"Board centre", L"Virtual origin"};
    Form f(L"Global Offset/Rotation", 560);
    f.icon((int)Icon::Offset);
    f.subtitle(L"Modifies location coordinates; CAD source coordinates are kept for machine setups that use them.");
    f.two_columns();
    f.heading(L"Side(s) to modify");
    f.combo("sides", L"Side(s) to modify", {L"Both", L"Top", L"Bottom"}, L"Both").wide();
    f.check("pan", L"Include Board Outline (Panel)", false);
    f.check("img", L"Include Circuit Outline", true);
    f.heading(L"Modification");
    f.combo("op", L"Modification type", ops, ops[0]).wide();
    f.text("dx", L"X Offset", L"0");
    f.text("dy", L"Y Offset", L"0");
    f.combo("rot", L"Rotational offset (\x00B0 CCW)", rots, rots[0]);
    f.number("ang", L"Custom angle (\x00B0 CCW)", 0, 3);
    f.combo("pivot", L"Rotate / mirror about", pivots, pivots[0]);
    int* u = add_units_combo(f, {"dx", "dy"});
    f.check("org", L"Move virtual origin too", true);
    f.check("swap", L"Mirror swaps component sides (flip board)", false);
    f.check("copyimg", L"Copy circuit outline (bottom-side mirror)", false);
    f.ok_text(L"Apply");
    if (!f.run(owner)) return false;

    const int op = std::max(0, f.index("op"));
    const int sides = std::max(0, f.index("sides"));
    const int pv = std::max(0, f.index("pivot"));
    const int ri = std::max(0, f.index("rot"));
    const double ang = ri == 0 ? 90 : ri == 1 ? 180 : ri == 2 ? 270 : f.num("ang", 0);
    const Vec2 piv = pv == 0 ? Vec2{} : pv == 1 ? bb.center() : p.virtual_origin;
    Xform T;
    switch (op) {
    case 0: T = Xform::translate(rd_u(f, "dx", *u, 0), rd_u(f, "dy", *u, 0)); break;
    case 1: T = Xform::translate(piv.x, piv.y) * Xform::rotation(ang) * Xform::translate(-piv.x, -piv.y); break;
    case 2: T = Xform::translate(piv.x, piv.y) * Xform::scale(-1, 1) * Xform::translate(-piv.x, -piv.y); break;
    case 3: T = Xform::translate(piv.x, piv.y) * Xform::scale(1, -1) * Xform::translate(-piv.x, -piv.y); break;
    }
    Xform L = T; L.tx = L.ty = 0;   // linear part for relative vectors
    const bool swap = f.checked("swap");
    d.checkpoint("Global offset/rotation");
    int n = 0;
    for (auto& c : p.comps) {
        if (sides == 1 && c.side != Side::Top) continue;
        if (sides == 2 && c.side != Side::Bottom) continue;
        c.pos = T.apply(c.pos);
        c.label_off = L.apply(c.label_off);
        if (op == 1) { c.rot = norm_deg(c.rot + ang); c.label_rot = norm_deg(c.label_rot + ang); }
        else if (op == 2) { c.rot = norm_deg(-c.rot); }
        else if (op == 3) { c.rot = norm_deg(180.0 - c.rot); }
        if ((op == 2 || op == 3) && swap) c.side = c.side == Side::Top ? Side::Bottom : Side::Top;
        ++n;
    }
    auto xf_polys = [&](std::vector<std::vector<Vec2>>& v) { for (auto& poly : v) for (auto& q : poly) q = T.apply(q); };
    const auto img_before = p.board_frame;
    if (f.checked("img")) xf_polys(p.board_frame);
    if (f.checked("pan")) xf_polys(p.panel_frame);
    if ((op == 2 || op == 3) && sides == 2 && f.checked("copyimg") && !img_before.empty()) {   // keep the original outline too
        for (auto& pl : img_before) p.board_frame.push_back(pl);
    }
    if (f.checked("org")) p.virtual_origin = T.apply(p.virtual_origin);
    d.changed(CH_GEOMETRY);
    set_status(std::to_wstring(n) + L" locations modified");
    return true;
}

// ===========================================================================
// Panel frame by coordinates. Safety-critical (sets conveyor width): no loose
// dragging, only typed caliper values or two picked/snapped corners, always
// confirmed with the resulting size. CircuitCAM's CAddBoardFrame likewise took a
// reference + four explicit offsets rather than a mouse rectangle.
// ===========================================================================
static bool panel_frame_form(HWND owner, Doc& d, BBox init, bool picked) {
    Project& p = d.prj;
    const Vec2 o = p.virtual_origin;
    if (init.empty() && !p.panel_frame.empty()) init = p.panel_frame_bbox();   // R22: never pre-filled from placement extents
    if (init.empty()) init = BBox(o.x, o.y, o.x, o.y);
    Form f(picked ? L"Confirm Board Outline (Panel)" : L"Board Outline (Panel)", 520);
    f.icon((int)Icon::Panelize);
    f.subtitle(L"Coordinates relative to the virtual origin. Upper-left = (min X, max Y); lower-right = (max X, min Y).");
    f.two_columns();
    const int u0 = std::clamp(settings().units, 0, 2);
    f.heading(L"Upper-left corner");
    f.text("ulx", L"X", fmt_u(init.x0 - o.x, u0));
    f.text("uly", L"Y", fmt_u(init.y1 - o.y, u0));
    f.heading(L"Lower-right corner");
    f.text("lrx", L"X", fmt_u(init.x1 - o.x, u0));
    f.text("lry", L"Y", fmt_u(init.y0 - o.y, u0));
    int* u = add_units_combo(f, {"ulx", "uly", "lrx", "lry"});
    f.label(L"Check the size against the board / panel drawing: the Y size sets the conveyor width.");
    bool pick = false;
    if (!picked) f.extra_button(L"Pick Corners on Plot...", [&](Form&) { pick = true; return true; });
    BBox r;
    auto box = [&](Form& ff) {
        double ux = rd_u(ff, "ulx", *u, 0), uy = rd_u(ff, "uly", *u, 0), lx = rd_u(ff, "lrx", *u, 0), ly = rd_u(ff, "lry", *u, 0);
        return BBox(std::min(ux, lx) + o.x, std::min(uy, ly) + o.y, std::max(ux, lx) + o.x, std::max(uy, ly) + o.y);
    };
    f.validate([&](Form& ff, std::wstring& err) {
        if (pick) return true;
        BBox b = box(ff);
        if (b.w() < 1 || b.h() < 1) { err = L"The panel must be at least 1 mm in each direction."; return false; }
        return true;
    });
    f.ok_text(L"Next");
    if (!f.run(owner)) return false;
    if (pick) { gui::post_command(ID_TOOL_FRAME_PANEL); return false; }
    r = box(f);
    // explicit confirmation showing the resulting size
    std::wstring wh = fmt_len(r.w()) + L"  \x00D7  " + fmt_len(r.h());
    std::wstring prev = p.panel_frame.empty() ? std::wstring(L"(none)") : fmt_len(p.panel_frame_bbox().w()) + L"  \x00D7  " + fmt_len(p.panel_frame_bbox().h());
    if (!message(owner, L"Apply Board Outline (Panel)",
                 L"New board outline (panel):  " + wh + L"\n\nX size (length along the conveyor):  " + fmt_len(r.w()) +
                 L"\nY size (conveyor width):  " + fmt_len(r.h()) + L"\n\nCurrent board outline (panel):  " + prev + L"\n\nApply?", true)) return false;
    d.checkpoint("Set board outline (panel)");
    p.set_panel_frame_rect(r);
    p.panel_outline_src = OutlineSource::Confirmed;   // R16: typed / picked and confirmed by the user
    d.show_frame = true;
    set_status(L"Board outline (panel) set: " + wh);
    return true;
}
bool dlg_panel_frame(HWND owner, Doc& d) { return panel_frame_form(owner, d, BBox(), false); }
bool dlg_confirm_panel_frame(HWND owner, Doc& d, BBox r) { return panel_frame_form(owner, d, r, true); }

// ===========================================================================
// Circuit (image) frame by coordinates
// ===========================================================================
bool dlg_circuit_frame(HWND owner, Doc& d) {
    Project& p = d.prj;
    const Vec2 o = p.virtual_origin;
    BBox b = p.circuit_frame_bbox().empty() ? BBox(o.x, o.y, o.x, o.y) : p.circuit_frame_bbox();   // R22: no placement-extent pre-fill
    const int u0 = std::clamp(settings().units, 0, 2);
    const std::vector<std::wstring> modes{L"Corners (X1, Y1, X2, Y2)", L"Origin + size (X, Y, W, H)"};
    Form f(L"Circuit Outline", 520);
    f.icon((int)Icon::Board);
    f.subtitle(L"Coordinates relative to the virtual origin.");
    f.two_columns();
    f.combo("mode", L"Entry", modes, modes[0]).wide();
    f.text("x1", L"X1 (left)", fmt_u(b.x0 - o.x, u0));
    f.text("y1", L"Y1 (bottom)", fmt_u(b.y0 - o.y, u0));
    f.text("x2", L"X2 (right)", fmt_u(b.x1 - o.x, u0));
    f.text("y2", L"Y2 (top)", fmt_u(b.y1 - o.y, u0));
    f.text("w", L"Width", fmt_u(b.w(), u0));
    f.text("h", L"Height", fmt_u(b.h(), u0));
    int* u = add_units_combo(f, {"x1", "y1", "x2", "y2", "w", "h"});
    auto box = [&](Form& ff) {
        double x1 = rd_u(ff, "x1", *u, 0) + o.x, y1 = rd_u(ff, "y1", *u, 0) + o.y;
        if (ff.index("mode") == 1) return BBox(x1, y1, x1 + rd_u(ff, "w", *u, 0), y1 + rd_u(ff, "h", *u, 0));
        double x2 = rd_u(ff, "x2", *u, 0) + o.x, y2 = rd_u(ff, "y2", *u, 0) + o.y;
        return BBox(std::min(x1, x2), std::min(y1, y2), std::max(x1, x2), std::max(y1, y2));
    };
    f.validate([&](Form& ff, std::wstring& err) {
        BBox r = box(ff);
        if (r.w() <= 0.1 || r.h() <= 0.1) { err = L"The frame must have a positive width and height."; return false; }
        return true;
    });
    f.ok_text(L"Apply");
    if (!f.run(owner)) return false;
    BBox r = box(f);
    d.checkpoint("Set circuit outline");
    p.set_circuit_frame_rect(r);
    p.board_outline_src = OutlineSource::Confirmed;   // R16
    d.show_frame = true;
    set_status(L"Circuit outline " + fmt_len(r.w()) + L" x " + fmt_len(r.h()));
    return true;
}

// ===========================================================================
// R22 Board > Outline > Add Outline (REVIEW_V21 §3.1, specs/OUTLINES.md)
// ===========================================================================
// Pick WHICH outline with two 4:3 diagram cards, then type the measured corners relative to the virtual origin
// (Upper-left X / Y on one row, Lower-right X / Y on one row) in mm / in / mil. Fields start from the existing
// outline of that kind, else empty: nothing is pre-filled from placement extents. Gerber-based outlines are made
// in the Gerber viewer (select / draw -> Confirm). Replacing an existing outline is confirmed.
bool dlg_add_board_frame(HWND owner, Doc& d) {
    Project& p = d.prj;
    const Vec2 vo = p.virtual_origin;
    int kind = p.panel_frame.empty() && (p.is_panel() || !p.board_frame.empty()) ? 0 : p.board_frame.empty() ? 1 : 0;   // 0 board, 1 circuit
    Units u = (Units)std::clamp(settings().units, 0, 2);
    double v[4] = {0, 0, 0, 0};   // ul x, ul y, lr x, lr y in units u (relative to the virtual origin)
    bool have = false;
    auto load = [&](int k) {
        const bool ex = k == 0 ? !p.panel_frame.empty() : !p.board_frame.empty();
        BBox b;
        if (ex) for (auto& pl : (k == 0 ? p.panel_frame : p.board_frame)) for (auto& q : pl) b.add(q);
        have = ex && !b.empty();
        if (!have) { for (double& x : v) x = 0; return; }
        v[0] = mm_to_units(b.x0 - vo.x, u); v[1] = mm_to_units(b.y1 - vo.y, u);
        v[2] = mm_to_units(b.x1 - vo.x, u); v[3] = mm_to_units(b.y0 - vo.y, u);
    };
    load(kind);
    auto rect = [&](BBox& r) {
        const double ux = units_to_mm(v[0], u) + vo.x, uy = units_to_mm(v[1], u) + vo.y, lx = units_to_mm(v[2], u) + vo.x, ly = units_to_mm(v[3], u) + vo.y;
        r = BBox(std::min(ux, lx), std::min(uy, ly), std::max(ux, lx), std::max(uy, ly));
        return lx > ux && uy > ly && r.w() >= 0.01 && r.h() >= 0.01;
    };
    Form f(L"Add Outline", 520);
    f.icon((int)Icon::Board);
    f.subtitle(L"Measured outline: whole PCB / panel, or one circuit (repeated on every array instance)");
    // graphical Board vs Circuit choice: two 4:3 cards with a high-contrast diagram of the edited boundary
    f.custom("kind", [&](Form&, float w) {
        const gui::GTheme& t = gui::th();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float gap = gui::S(14);
        const float cw = std::min((w - gap) * 0.5f, gui::S(200)), ch = std::floor(cw * 0.75f);
        const float x00 = ImGui::GetCursorScreenPos().x + (w - (2 * cw + gap)) * 0.5f;
        const float y00 = ImGui::GetCursorScreenPos().y;
        for (int k = 0; k < 2; ++k) {
            const ImVec2 o(x00 + k * (cw + gap), y00);
            ImGui::SetCursorScreenPos(o);
            ImGui::PushID(k);
            if (ImGui::InvisibleButton("##card", ImVec2(cw, ch)) && kind != k) { kind = k; load(k); }
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool on = kind == k;
            const ImU32 accent = IM_COL32(64, 220, 96, 255);              // outline green
            const ImU32 ink = t.dark ? IM_COL32(236, 240, 236, 255) : IM_COL32(24, 28, 24, 255);
            dl->AddRectFilled(o, ImVec2(o.x + cw, o.y + ch), t.dark ? IM_COL32(22, 26, 24, 255) : IM_COL32(250, 252, 250, 255), gui::S(6));
            dl->AddRect(o, ImVec2(o.x + cw, o.y + ch), on ? accent : hov ? t.text_dim : t.border, gui::S(6), 0, on ? gui::S(3) : gui::S(1));
            // title + radio
            const ImVec2 rd(o.x + gui::S(14), o.y + gui::S(15));
            dl->AddCircle(rd, gui::S(6), on ? accent : t.text_dim, 16, gui::S(1.6f));
            if (on) dl->AddCircleFilled(rd, gui::S(3.5f), accent);
            dl->AddText(gui::g_font_bold, ImGui::GetFontSize(), ImVec2(o.x + gui::S(26), o.y + gui::S(6)), on ? ink : t.text_dim, k == 0 ? "Board Outline" : "Circuit Outline");
            // diagram: a panel with 2 x 2 circuits; the edited boundary is the thick green line, everything else grey
            const float m0 = gui::S(14), top = gui::S(32);
            const ImVec2 a(o.x + m0, o.y + top), b(o.x + cw - m0, o.y + ch - gui::S(12));
            dl->AddRectFilled(a, b, IM_COL32(18, 84, 46, 255), gui::S(2));
            const float m = gui::S(7), gw = (b.x - a.x - 3 * m) * 0.5f, gh = (b.y - a.y - 3 * m) * 0.5f;
            for (int r = 0; r < 2; ++r) for (int c = 0; c < 2; ++c) {
                const ImVec2 c0(a.x + m + c * (gw + m), a.y + m + r * (gh + m)), c1(c0.x + gw, c0.y + gh);
                const bool hl = k == 1 && r == 1 && c == 0;
                dl->AddRectFilled(c0, c1, IM_COL32(30, 110, 62, 255));
                for (int q = 0; q < 3; ++q) dl->AddRectFilled(ImVec2(c0.x + gw * (0.18f + 0.26f * q), c0.y + gh * 0.42f), ImVec2(c0.x + gw * (0.18f + 0.26f * q) + gui::S(6), c0.y + gh * 0.42f + gui::S(4)), IM_COL32(210, 190, 120, 255));
                dl->AddRect(c0, c1, hl ? accent : IM_COL32(150, 170, 158, 255), 0, 0, hl ? gui::S(3.5f) : gui::S(1));
            }
            dl->AddRect(a, b, k == 0 ? accent : IM_COL32(150, 170, 158, 255), 0, 0, k == 0 ? gui::S(3.5f) : gui::S(1));
        }
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y00 + ch + gui::S(2)));
        ImGui::Dummy(ImVec2(w, 1));
    }, 0).wide();
    // corners: units + UL X/Y on one row + LR X/Y on one row (relative to the virtual origin)
    f.custom("xy", [&](Form&, float w) {
        const gui::GTheme& t = gui::th();
        static const char* un[3] = {"mm", "in", "mil"};
        const float lw = gui::S(96), fw = std::floor((w - lw - gui::S(60)) * 0.5f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "Units");
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(gui::S(90));
        if (ImGui::BeginCombo("##u", un[(int)u])) {
            for (int i = 0; i < 3; ++i) if (ImGui::Selectable(un[i], (int)u == i) && (int)u != i) {
                for (double& x : v) x = mm_to_units(units_to_mm(x, u), (Units)i);
                u = (Units)i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_faint), "relative to the virtual origin (+X right, +Y up)");
        const char* fmt = u == Units::MM ? "%.4f" : u == Units::Inch ? "%.5f" : "%.2f";
        auto row = [&](const char* name, int i0) {
            ImGui::PushID(i0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(name);
            ImGui::SameLine(lw);
            ImGui::TextUnformatted("X"); ImGui::SameLine();
            ImGui::SetNextItemWidth(fw); ImGui::InputDouble("##x", &v[i0], 0, 0, fmt);
            ImGui::SameLine(0, gui::S(12));
            ImGui::TextUnformatted("Y"); ImGui::SameLine();
            ImGui::SetNextItemWidth(fw); ImGui::InputDouble("##y", &v[i0 + 1], 0, 0, fmt);
            ImGui::PopID();
        };
        row("Upper left", 0);
        row("Lower right", 2);
        BBox r;
        if (rect(r)) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t.text_dim), "Size %.4f x %.4f %s (%.3f x %.3f mm)", mm_to_units(r.w(), u), mm_to_units(r.h(), u), un[(int)u], r.w(), r.h());
        else ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "Enter the measured corners: upper left must be left of and above lower right.");
    }, 0).wide();
    f.ok_text(L"Add Outline");
    f.validate([&](Form&, std::wstring& err) {
        BBox r;
        if (!rect(r)) { err = L"Upper left must be left of and above lower right."; return false; }
        return true;
    });
    if (!f.run(owner)) return false;
    BBox r;
    if (!rect(r)) return false;
    const OutlineKind ok = kind == 0 ? OutlineKind::Board : OutlineKind::Circuit;
    const std::wstring nm = kind == 0 ? L"board outline" : L"circuit outline";
    if (outline_exists(p, ok)) {
        BBox cur;
        for (auto& pl : (kind == 0 ? p.panel_frame : p.board_frame)) for (auto& q : pl) cur.add(q);
        if (!message(owner, L"Replace Outline", L"Replace the existing " + nm + L" (" + fmt_len(cur.w()) + L" \x00D7 " + fmt_len(cur.h()) + L") with " +
                     fmt_len(r.w()) + L" \x00D7 " + fmt_len(r.h()) + L"?", true)) return false;
    }
    d.checkpoint(kind == 0 ? "Add board outline" : "Add circuit outline");
    commit_outline_rect(p, ok, r, OutlineSource::Confirmed, false);
    d.show_frame = true;
    d.changed(CH_GEOMETRY);
    std::string issues;
    for (auto& fi : check_outlines(p)) if (fi.error || !fi.ref.empty()) issues += "\n\xE2\x80\xA2 " + (fi.ref.empty() ? fi.msg : fi.ref + " " + fi.msg);
    set_status(W((kind == 0 ? std::string("Board outline set: ") : std::string("Circuit outline set: ")) + gui::fmt_len_u8(r.w(), false) + " x " + gui::fmt_len_u8(r.h())));
    if (!issues.empty()) message(owner, L"Outline check", W("The outline is set (Ctrl+Z undoes). Please check:" + issues));
    return true;
}

// ===========================================================================
// R22: "Confirm as measured" (inferred-outline banner; the Confirm Board Outline menu item is retired)
// ===========================================================================
bool dlg_confirm_board_outline(HWND owner, Doc& d) {
    Project& p = d.prj;
    if (p.board_outline_confirmed() && p.circuit_outline_source() != OutlineSource::Inferred) {
        message(owner, L"Confirm as Measured", L"The outlines are already confirmed. To change one use Board > Outline > Add Outline.");
        return false;
    }
    const BBox cb = p.panel_frame.empty() || p.panel_outline_src != OutlineSource::Inferred ? p.board_bbox() : p.panel_frame_bbox();
    if (!message(owner, L"Confirm as Measured", L"The inferred outline is " + fmt_len(cb.w()) + L" \x00D7 " + fmt_len(cb.h()) +
                 L" (estimated, not measured).\n\nMeasure the board and compare: confirm only when these dimensions match the board drawing. "
                 L"The machine program will use them.\n\nConfirm as measured?", true)) return false;
    d.checkpoint("Confirm outline as measured");
    if (!confirm_inferred_outline(p)) return false;
    d.show_frame = true;
    d.changed(CH_GEOMETRY);
    set_status(L"Outline confirmed as measured: " + fmt_len(cb.w()) + L" \x00D7 " + fmt_len(cb.h()));
    return true;
}

// ===========================================================================
// Model point properties
// ===========================================================================
bool dlg_model_point(HWND owner, Doc& d, int point) {
    auto& pts = d.prj.model.points;
    if (point < 0 || point >= (int)pts.size()) return false;
    ModelPoint mp = pts[point];
    if (!edit_model_point(owner, d.prj, mp, pts, point)) return false;
    d.checkpoint("Model point properties");
    pts[point] = mp;
    d.changed(CH_MODEL);
    return true;
}

// ===========================================================================
// Factory model (project copy)
// ===========================================================================
bool dlg_factory_model(HWND owner, Doc& d) {
    FactoryModel m = d.prj.model;
    std::vector<int> origin(m.points.size());
    std::iota(origin.begin(), origin.end(), 0);
    std::vector<int> used(d.prj.model.points.size(), 0);
    for (auto& c : d.prj.comps) if (c.assigned_point >= 0 && c.assigned_point < (int)used.size()) used[c.assigned_point]++;

    auto rows = [&]() {
        std::vector<std::vector<std::wstring>> r;
        for (size_t i = 0; i < m.points.size(); ++i) {
            const ModelPoint& mp = m.points[i];
            std::wstring mach = L"";
            if (mp.has_machine) {
                Exporter* e = PluginRegistry::get().exporter(mp.machine.exporter_id);
                mach = e ? W(e->info().name) : W(mp.machine.exporter_id);
            }
            int n = origin[i] >= 0 && origin[i] < (int)used.size() ? used[origin[i]] : 0;
            r.push_back({std::to_wstring(i + 1), W(mp.name), W(mp.process_type), kind_names()[(int)mp.kind],
                         side_names()[(int)mp.side], mach, std::to_wstring(n)});
        }
        return r;
    };
    auto select = [](Form& ff, int i) {
        if (Field* fl = ff.field("pts")) fl->list_sel = i;
    };

    Form f(L"Edit Project Factory Model", 700);
    f.icon((int)Icon::FactoryModel);
    f.two_columns();
    f.text("name", L"Factory model name", W(m.name));
    f.text("desc", L"Description", W(m.description));
    f.list("pts", {L"#", L"Model point", L"Process", L"Kind", L"Side", L"Machine", L"Assigned"}, rows(), 12);
    f.button("add", L"Add point...", [&](Form& ff) {
        ModelPoint mp;
        mp.name = "New Point " + std::to_string(m.points.size() + 1);
        mp.process_type = "SMT";
        if (!edit_model_point(owner, d.prj, mp, m.points, -1)) return;
        int at = ff.list_selection("pts");
        at = at < 0 ? (int)m.points.size() : at + 1;
        m.points.insert(m.points.begin() + at, mp);
        origin.insert(origin.begin() + at, -1);
        ff.set_list("pts", rows());
        select(ff, at);
    });
    f.button("edit", L"Properties...", [&](Form& ff) {
        int i = ff.list_selection("pts");
        if (i < 0 || i >= (int)m.points.size()) { message(owner, L"Factory Model", L"Select a model point."); return; }
        ModelPoint mp = m.points[i];
        if (!edit_model_point(owner, d.prj, mp, m.points, i)) return;
        m.points[i] = mp;
        ff.set_list("pts", rows());
        select(ff, i);
    });
    f.button("up", L"Move up", [&](Form& ff) {
        int i = ff.list_selection("pts");
        if (i <= 0 || i >= (int)m.points.size()) return;
        std::swap(m.points[i], m.points[i - 1]);
        std::swap(origin[i], origin[i - 1]);
        ff.set_list("pts", rows());
        select(ff, i - 1);
    });
    f.button("down", L"Move down", [&](Form& ff) {
        int i = ff.list_selection("pts");
        if (i < 0 || i + 1 >= (int)m.points.size()) return;
        std::swap(m.points[i], m.points[i + 1]);
        std::swap(origin[i], origin[i + 1]);
        ff.set_list("pts", rows());
        select(ff, i + 1);
    });
    f.button("del", L"Delete point", [&](Form& ff) {
        int i = ff.list_selection("pts");
        if (i < 0 || i >= (int)m.points.size()) return;
        int n = origin[i] >= 0 && origin[i] < (int)used.size() ? used[origin[i]] : 0;
        if (!message(owner, L"Delete Model Point", L"Delete '" + W(m.points[i].name) + L"'?" +
                     (n ? L"\n" + std::to_wstring(n) + L" assigned component(s) become unassigned." : std::wstring()), true)) return;
        m.points.erase(m.points.begin() + i);
        origin.erase(origin.begin() + i);
        ff.set_list("pts", rows());
        select(ff, std::min(i, (int)m.points.size() - 1));
    });
    f.button("reset", L"Reset to default model", [&](Form& ff) {
        if (!message(owner, L"Reset Factory Model", L"Replace the model points with the default factory model?\n"
                     L"Assignments are kept for points whose names match.", true)) return;
        FactoryModel def = FactoryModel::make_default();
        m.points = def.points;
        origin.assign(m.points.size(), -1);
        for (size_t i = 0; i < m.points.size(); ++i)
            for (size_t j = 0; j < d.prj.model.points.size(); ++j)
                if (ieq(m.points[i].name, d.prj.model.points[j].name)) { origin[i] = (int)j; break; }
        ff.set("name", W(def.name));
        ff.set("desc", W(def.description));
        ff.set_list("pts", rows());
    });
    f.validate([&](Form&, std::wstring& err) {
        if (m.points.empty()) { err = L"The factory model needs at least one model point."; return false; }
        return true;
    });
    if (!f.run(owner)) return false;

    d.checkpoint("Edit factory model");
    m.name = f.u8("name");
    m.description = f.u8("desc");
    std::vector<int> remap(d.prj.model.points.size(), -1);
    for (size_t i = 0; i < origin.size(); ++i) if (origin[i] >= 0 && origin[i] < (int)remap.size()) remap[origin[i]] = (int)i;
    for (auto& c : d.prj.comps)
        if (c.assigned_point >= 0) c.assigned_point = c.assigned_point < (int)remap.size() ? remap[c.assigned_point] : -1;
    int ap = d.active_point >= 0 && d.active_point < (int)remap.size() ? remap[d.active_point] : -1;
    d.prj.model = m;
    d.active_point = ap >= 0 ? ap : (m.points.empty() ? -1 : 0);
    d.changed(CH_MODEL | CH_ASSIGN);
    return true;
}

// ===========================================================================
// Find reference
// ===========================================================================
bool dlg_find_ref(HWND owner, Doc& d) {
    static std::wstring last;
    std::wstring v = last;
    if (!prompt_text(owner, L"Find Reference", L"Reference designator", v)) return false;
    last = v;
    Component* c = d.prj.find_ref(trim(U(v)));
    if (!c) { message(owner, L"Find Reference", L"Reference '" + v + L"' was not found."); return false; }
    d.select_none();
    c->selected = true;
    if (c->assigned_point >= 0) d.active_point = c->assigned_point;
    d.changed(CH_SELECTION);
    if (DocPtr dp = find_docptr(d)) {
        std::vector<int> idx;
        for (int i = 0; i < (int)d.prj.comps.size(); ++i) if (&d.prj.comps[i] == c) idx.push_back(i);
        gui::focus_components(dp, idx, true);
    }
    set_status(L"Found " + W(c->ref) + L" at " + fmt_len(d.prj.user_pos(*c).x) + L", " + fmt_len(d.prj.user_pos(*c).y));
    return true;
}

// ===========================================================================
// Apply offset file (CSV: part-or-package, rotation)
// ===========================================================================
bool dlg_apply_offset_file(HWND owner, Doc& d) {
    std::wstring path = settings().dir_programs;
    if (!open_file_dialog(owner, L"Apply Offset File (part or package, rotation)", {{L"CSV / text", L"*.csv;*.txt"}, {L"All files", L"*.*"}}, path)) return false;
    std::string text, err;
    if (!read_text_file(U(path), text, err)) { message(owner, L"Apply Offset File", W(err)); return false; }
    std::map<std::string, double> offs;
    std::istringstream in(text);
    std::string line;
    int bad = 0;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        auto parts = io::split_delimited(t, t.find(',') != std::string::npos ? ',' : t.find('\t') != std::string::npos ? '\t' : ';');
        if (parts.size() < 2) { ++bad; continue; }
        char* end = nullptr;
        std::string rv = trim(parts[1]);
        double r = std::strtod(rv.c_str(), &end);
        if (rv.empty() || (end && *end)) { ++bad; continue; }   // header line etc.
        offs[upper(trim(parts[0]))] = r;
    }
    if (offs.empty()) { message(owner, L"Apply Offset File", L"No 'name,rotation' lines found in the file."); return false; }
    int n_pn = 0, n_pk = 0;
    std::vector<std::pair<Component*, double>> todo;
    for (auto& c : d.prj.comps) {
        if (c.feature != Feature::Component) continue;
        auto it = offs.find(upper(c.number));
        if (!c.number.empty() && it != offs.end()) { todo.push_back({&c, it->second}); ++n_pn; continue; }
        it = offs.find(upper(c.package));
        if (!c.package.empty() && it != offs.end()) { todo.push_back({&c, it->second}); ++n_pk; }
    }
    if (todo.empty()) { message(owner, L"Apply Offset File", std::to_wstring(offs.size()) + L" entries read, but no component matched a part number or package."); return false; }
    if (!message(owner, L"Apply Offset File", L"Add the rotation offsets to " + std::to_wstring(todo.size()) + L" components?\n(" +
                 std::to_wstring(n_pn) + L" by part number, " + std::to_wstring(n_pk) + L" by package" +
                 (bad ? L"; " + std::to_wstring(bad) + L" lines ignored" : std::wstring()) + L")", true)) return false;
    d.checkpoint("Apply offset file");
    for (auto& t : todo) t.first->rot = norm_deg(t.first->rot + t.second);
    d.changed(CH_GEOMETRY);
    set_status(std::to_wstring(todo.size()) + L" rotations offset");
    return true;
}

// ===========================================================================
// Plugin options
// ===========================================================================
bool dlg_plugin_options(HWND owner, const PluginInfo& info, Options& opts) {
    if (info.options.empty()) return true;
    Form f(W(info.name) + L" Options", 520);
    f.icon((int)Icon::Plugin);
    if (!info.vendor.empty()) f.header(W(info.name) + L"  \x2014  " + W(info.vendor));
    for (auto& o : info.options) {
        const std::string v = opt(opts, o.key, o.def);
        const std::wstring lbl = W(o.label.empty() ? o.key : o.label);
        switch (o.type) {
        case OptionDef::Type::Bool: f.check(o.key, lbl, opt_bool(opts, o.key, opt_bool({{o.key, o.def}}, o.key, false))); break;
        case OptionDef::Type::Int: f.number(o.key, lbl, std::strtod(v.c_str(), nullptr), 0); break;
        case OptionDef::Type::Double: f.number(o.key, lbl, std::strtod(v.c_str(), nullptr), 4); break;
        case OptionDef::Type::Enum: {
            std::vector<std::wstring> ch;
            for (auto& c : o.choices) ch.push_back(W(c));
            f.combo(o.key, lbl, ch, W(v));
            break;
        }
        case OptionDef::Type::Path: f.file(o.key, lbl, W(v), {{L"All files", L"*.*"}}); break;
        case OptionDef::Type::String: f.text(o.key, lbl, W(v)); break;
        }
        if (!o.tooltip.empty()) f.tip(W(o.tooltip));
    }
    if (!f.run(owner)) return false;
    for (auto& o : info.options) {
        switch (o.type) {
        case OptionDef::Type::Bool: opts[o.key] = f.checked(o.key) ? "1" : "0"; break;
        case OptionDef::Type::Int: opts[o.key] = std::to_string(std::llround(f.num(o.key, 0))); break;
        case OptionDef::Type::Double: opts[o.key] = U(fmt_num(f.num(o.key, 0), 6)); break;
        default: opts[o.key] = f.u8(o.key); break;
        }
    }
    return true;
}

// ===========================================================================
// About / plugins / help
// ===========================================================================
void dlg_about(HWND owner) {
    Form f(L"About PCB Plotter", 440);
    f.icon((int)Icon::Board);
    f.header(L"PCB Plotter  1.0");
    f.label(L"SMT CAM: Gerber / CAD / centroid / BOM import, factory model,\nvisual aids and machine program generation.");
    f.label(L"Independent software. Not affiliated with any CAM software or equipment vendor.");
    f.separator();
    f.label(L"Win32 + Direct2D, no external libraries.");
    f.label(L"Plugins: " + std::to_wstring(PluginRegistry::get().importers().size()) + L" importers, " +
            std::to_wstring(PluginRegistry::get().exporters().size()) + L" exporters.");
    f.no_cancel().ok_text(L"Close");
    f.run(owner);
}

void dlg_plugins(HWND owner) {
    auto& r = PluginRegistry::get();
    std::vector<std::vector<std::wstring>> rows;
    auto exts = [](const PluginInfo& i) {
        std::wstring s;
        for (auto& e : i.extensions) { if (!s.empty()) s += L" "; s += L"." + W(e); }
        return s;
    };
    auto src = [](const PluginInfo& i) { return i.builtin ? std::wstring(L"built-in") : W(i.module_path); };
    for (auto& im : r.importers()) {
        const PluginInfo& i = im->info();
        rows.push_back({L"Import", W(i.id), W(i.name), W(i.category), exts(i), src(i)});
    }
    for (auto& ex : r.exporters()) {
        const PluginInfo& i = ex->info();
        std::wstring kind = i.export_input == ExportInput::MachineJob ? L"Machine" : L"Export";
        rows.push_back({kind, W(i.id), W(i.name), W(i.category), exts(i), src(i)});
    }
    std::wstring log;
    for (auto& l : r.load_log) log += W(l) + L"\r\n";
    if (log.empty()) log = L"(no plugin DLLs were loaded from " + exe_dir() + L"\\plugins)";
    Form f(L"Plugins", 820);
    f.icon((int)Icon::Plugin);
    f.header(std::to_wstring(r.importers().size()) + L" importers, " + std::to_wstring(r.exporters().size()) + L" exporters");
    f.list("pl", {L"Kind", L"ID", L"Name", L"Category", L"Extensions", L"Source"}, rows, 16);
    f.multiline("log", L"DLL load log", log, 5).readonly();
    f.no_cancel().ok_text(L"Close");
    f.run(owner);
}

void dlg_help(HWND owner, int topic) {
    std::wstring t;
    if (topic == 0) {
        t =
L"PCB PLOTTER \x2014 QUICK GUIDE\n"
L"=============================\n\n"
L"PCB Plotter turns board data (Gerber, CAD, centroid and BOM files) into assembly documents and\n"
L"machine programs. A typical job runs through these steps:\n\n"
L"1. Prepare the factory model\n"
L"   Library > Factory Model lists the model points of this project: the stations a board passes\n"
L"   through (SMT placement, reflow, hand load, wave, inspection...). Give each point a process type,\n"
L"   a side and, for automated stations, a machine type. Filters (process types, part numbers,\n"
L"   packages) let Manufacturing > Auto Assign route components automatically.\n\n"
L"2. Start a project\n"
L"   File > New opens an empty Universal window. The project carries its own copy of the factory model.\n\n"
L"3. Bring in placement data\n"
L"   - CAD: Import > CAD File (Auto) recognises GenCAD, IPC-356 and other supported formats.\n"
L"   - Centroid: Import > ASCII Centroid File; tag each column (Reference, X, Y, Rotation, Side,\n"
L"     Part Number, Package) and check the live preview before pressing OK.\n"
L"   - Gerber: Import > Gerber Files, then digitise components on the paste/copper layer by dragging\n"
L"     a box around each part and naming it.\n"
L"   - Reference Fusion attaches reference designators from a centroid file to components digitised\n"
L"     from Gerber, matching them by position.\n\n"
L"4. Import the bill of materials\n"
L"   BOM > Import BOM links part numbers, descriptions and processes to components by reference.\n"
L"   Ranges such as C1-C25 are expanded; the report lists references missing on either side.\n\n"
L"5. Review and correct\n"
L"   Board > Edit Locations and BOM > Edit BOM open spreadsheet views (double-click a cell or press F2\n"
L"   to edit). Board > Edit Packages opens the Package Type Editor for pads, pin 1 and body artwork.\n"
L"   Right-click a component in the Universal window for quick edits.\n\n"
L"6. Assign components to model points\n"
L"   Select components, pick a model point in the Factory Model bar, press Assign (or use Auto Assign).\n"
L"   Show Unassigned highlights anything still missing.\n\n"
L"7. Fiducials\n"
L"   Right-click a mark > Turn Component into Fiducial, choose master/image/local and its number.\n\n"
L"8. Panelize (once the single board is correct)\n"
L"   Board > Panelize > Create Linear Array (X/Y counts and pitch) for regular arrays, or Create\n"
L"   Nonlinear Array for rotated / custom positions. Instances are linked to the reference circuit.\n"
L"   The board outline is never generated: enter the measured board in Board > Outline > Add Outline.\n\n"
L"9. Documents and programs\n"
L"   Double-click a model point to open its visual aid (colour-coded drawing + BOM block) and notes.\n"
L"   Points with a machine get a program page: set up the machine, then Generate (Ctrl+G).\n\n"
L"Save with File > Save (.pcbp). Legacy .cpf projects can be migrated with File > Migrate Legacy Project.\n";
    } else if (topic == 1) {
        t =
L"KEYBOARD AND MOUSE\n"
L"==================\n\n"
L"Graphics windows\n"
L"  Mouse wheel ............ zoom at the cursor\n"
L"  Shift + wheel .......... pan left / right\n"
L"  Ctrl + wheel ........... pan up / down\n"
L"  Middle-drag ............ pan\n"
L"  Space + left-drag ...... pan\n"
L"  Left-drag (empty area) . rubber-band select\n"
L"  Home ................... zoom all\n"
L"  + / - .................. zoom in / out\n"
L"  Esc .................... cancel the current drag / zoom rectangle\n"
L"  Right-click ............ context menu\n\n"
L"Editing\n"
L"  Del .................... delete selection\n"
L"  Ctrl+Z / Ctrl+Y ........ undo / redo   (Alt+Backspace also undoes)\n"
L"  Ctrl+A ................. select all\n"
L"  Ctrl+C ................. copy (grids copy the selected rows as text)\n"
L"  Ctrl+F ................. find reference\n"
L"  Ctrl+G ................. generate the program of the selected model point\n\n"
L"Packages workspace\n"
L"  N ...................... show / hide the Packages list (left panel)\n"
L"  Ctrl / Shift + click ... multi-select packages in the list; Ctrl+A selects all shown\n"
L"  Up / Down .............. previous / next package\n"
L"  P / Shift+P ............ polarity marker on / off (all selected) / Dot <-> Arrow\n"
L"  R / Shift+R ............ rotate package 90\xB0 CCW / CW\n"
L"  B ...................... suggest a body outline from the leads\n"
L"  Ctrl+Space ............. maximize the drawing\n\n"
L"Grids (Edit Locations / Edit BOM / pad table)\n"
L"  Double-click or F2 ..... edit cell\n"
L"  Enter / Esc ............ commit / cancel the cell edit\n"
L"  Click a header ......... sort (click again to reverse)\n\n"
L"File\n"
L"  Ctrl+N / Ctrl+O / Ctrl+S  new / open / save\n"
L"  Ctrl+P ................. print\n"
L"  F1 ..................... quick guide\n";
    } else {
        auto& r = PluginRegistry::get();
        int dll = 0;
        for (auto& im : r.importers()) if (!im->info().builtin) ++dll;
        for (auto& ex : r.exporters()) if (!ex->info().builtin) ++dll;
        std::wstring pdir = exe_dir() + L"\\plugins";
        DWORD pa = GetFileAttributesW(pdir.c_str());
        bool d2d = d2d_factory() != nullptr, dw = dwrite_factory() != nullptr;
        OSVERSIONINFOEXW os{};
        os.dwOSVersionInfoSize = sizeof os;
        typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW*);
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
            if (auto fn = (RtlGetVersionFn)(void*)GetProcAddress(nt, "RtlGetVersion")) fn(&os);
        t = L"VERIFY INSTALLATION\n===================\n\n";
        t += L"Program folder ......... " + exe_dir() + L"\n";
        t += L"Settings file .......... " + ini_file_path() + L"\n";
        t += L"Plugin folder .......... " + pdir + (pa != INVALID_FILE_ATTRIBUTES && (pa & FILE_ATTRIBUTE_DIRECTORY) ? L"" : L"  (not present)") + L"\n";
        t += L"Importers .............. " + std::to_wstring(r.importers().size()) + L"\n";
        t += L"Exporters .............. " + std::to_wstring(r.exporters().size()) + L"\n";
        t += L"DLL plugins loaded ..... " + std::to_wstring(dll) + L"\n";
        t += L"Direct2D ............... " + std::wstring(d2d ? L"available" : L"NOT AVAILABLE") + L"\n";
        t += L"DirectWrite ............ " + std::wstring(dw ? L"available" : L"NOT AVAILABLE") + L"\n";
        t += L"Windows ................ " + std::to_wstring(os.dwMajorVersion) + L"." + std::to_wstring(os.dwMinorVersion) + L" build " + std::to_wstring(os.dwBuildNumber) + L"\n";
        t += L"Theme .................. " + std::wstring(theme_dark() ? L"dark" : L"light") + L"\n";
        t += L"Display units .......... " + units_label() + L"\n";
        t += L"Library ................ " + (settings().library_path.empty() ? std::wstring(L"(none)") : settings().library_path) + L"\n";
        t += L"Open documents ......... " + std::to_wstring(open_docs().size()) + L"\n";
        if (!r.load_log.empty()) {
            t += L"\nPlugin load log:\n";
            for (auto& l : r.load_log) t += L"  " + W(l) + L"\n";
        }
    }
    show_text(owner, topic == 0 ? L"Quick Guide" : topic == 1 ? L"Keyboard and Mouse" : L"Verify Installation", t);
}

} // namespace pcbp::app
