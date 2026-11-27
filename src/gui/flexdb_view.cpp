// flexdb_view.cpp — "Components" view: JUKI FlexLine component database integration.
//
// Every project part number gets a 20-character machine name, the FlexAssign description
// classification, plus the evidence CircuitCAM has and the H7H export drops: the footprint
// (pad span/pitch/count) and package body. Ranked profile suggestions with confidence,
// tape / nozzle implications and mismatch warnings; assignments are written straight into a
// FlexLine component database (new from the empty template, or an existing one like Parts.mdb).
#include "gapp.h"
#include "../io/flexdb.h"
#include "../io/juki_research.h"
#include "../io/juki_autopart.h"
#include "../io/juki_writer.h"
#include "../core/fileio.h"
#include "../ui/form.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <cctype>

namespace pcbp::gui {

HWND main_hwnd();

namespace {

using namespace pcbp::flexdb;

ImU32 alpha(ImU32 c, int a) { return (c & 0x00FFFFFFu) | ((ImU32)a << 24); }
std::string fmt2(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

// ---- [FlexDB] settings + teaching data store ------------------------------------
// Settings live in %APPDATA%\\PCBPlotter\\PCBPlotter.ini so they survive a new exe in a new folder (the old
// exe-folder ini is still read as a fallback). Copies of the profile library, the learned house conventions and
// the machine-history CSVs are kept in %APPDATA%\\PCBPlotter\\FlexDB and used when the originals are not reachable.
std::wstring exe_dir() {
    wchar_t b[MAX_PATH];
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring p = b;
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring(L".") : p.substr(0, s);
}
std::wstring app_dir() {
    wchar_t b[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", b, MAX_PATH);
    if (!n || n >= MAX_PATH) return exe_dir();
    std::wstring d = std::wstring(b) + L"\\PCBPlotter";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
std::wstring store_dir() {
    std::wstring d = app_dir() + L"\\FlexDB";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
std::wstring ini_file() { return app_dir() + L"\\PCBPlotter.ini"; }
std::string ini_get(const wchar_t* key) {
    wchar_t b[4096] = {0};
    GetPrivateProfileStringW(L"FlexDB", key, L"", b, 4096, ini_file().c_str());
    if (!b[0]) GetPrivateProfileStringW(L"FlexDB", key, L"", b, 4096, (exe_dir() + L"\\PCBPlotter.ini").c_str());
    return ui::U(b);
}
void ini_set(const wchar_t* key, const std::string& v) { WritePrivateProfileStringW(L"FlexDB", key, ui::W(v).c_str(), ini_file().c_str()); }
bool path_exists(const std::string& p) { return !p.empty() && GetFileAttributesW(ui::W(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
std::string base_name(const std::string& p) { size_t s = p.find_last_of("\\/"); return s == std::string::npos ? p : p.substr(s + 1); }
// copy a source file into the store (refreshing it); returns the stored path, "" on failure
std::string store_copy(const std::string& src, const std::string& name) {
    std::wstring dst = store_dir() + L"\\" + ui::W(name);
    if (ui::W(src) == dst) return src;
    return CopyFileW(ui::W(src).c_str(), dst.c_str(), FALSE) ? ui::U(dst) : std::string();
}
std::string stored(const std::string& name) { return ui::U(store_dir() + L"\\" + ui::W(name)); }
bool is_dnp(const std::string& pn) {
    std::string u;
    for (char c : pn) if (c != ' ') u += (char)std::toupper((unsigned char)c);
    if (u.empty()) return false;
    if (u.find_first_not_of('?') == std::string::npos) return true;          // "???": the BOM never assigned the reference
    static const char* k[] = {"DNP", "DNI", "DNF", "NC", "N/C", "NP", "NOPOP", "NOTPLACED", "DONOTPLACE", "NOTFITTED", "NF"};
    for (const char* x : k) if (u == x) return true;
    return false;
}
std::string clean_cell(std::string v) { for (auto& c : v) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return v; }
std::string csv_cell(const std::string& v) {
    if (v.find_first_of(",\"\n\r") == std::string::npos) return v;
    std::string o = "\"";
    for (char c : v) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

struct Row {
    std::string pn, desc, pkg, mname;
    int qty = 0;
    Evaluation ev;
    bool evaluated = false;
    int db_id = 0, db_pid = 0, db_tape = 0;   // state in the target database
    int pid = 0;                               // pending assignment (0 = keep DB state)
    bool auto_set = false;
    int tape = -1;                             // pending carrier: -1 keep, 0 emboss, 1 paper
    Status st = Status::Untaught;
    bool has_mach = false;                     // machine-history proposal (body, carrier, pitch, nozzle)
    history::Proposal mach;
    std::string mach_route;                    // how the taught profile was chosen
};

enum Col { C_Status, C_PN, C_Machine, C_Desc, C_Pkg, C_Body, C_Profile, C_Conf, C_Tape, C_Feeder, C_Count };
enum Filter { F_All, F_Untaught, F_Conflict, F_Assigned, F_Renamed, F_Check, F_Count };

class ComponentsView : public View {
public:
    ViewKind kind() const override { return ViewKind::Components; }
    const char* title() const override { return "Components"; }
    ui::Icon icon() const override { return ui::Icon::SelectPart; }

    std::string status() const override { return status_; }

    void on_activate() override {
        if (!tried_load_) {
            tried_load_ = true;
            lib_path_ = ini_get(L"Library");
            conv_path_ = ini_get(L"Conventions");
            tpl_path_ = ini_get(L"Template");
            hist_paths_ = ini_get(L"History");
            noz_text_ = ini_get(L"Nozzles");
            std::snprintf(noz_buf_, sizeof noz_buf_, "%s", noz_text_.c_str());
            apply_nozzles();
            // the library: the remembered file, else PCB Plotter's own copy
            std::string lp = path_exists(lib_path_) ? lib_path_ : path_exists(stored("profiles.flx")) ? stored("profiles.flx") : std::string();
            if (!lp.empty()) defer([this, lp] { load_library(lp); });
            if (!hist_paths_.empty()) defer([this] { load_history(); });
            machine_dir_ = ini_get(L"Machine");
            if (machine_dir_.empty() && path_exists("C:\\HLC\\SubCompo.ini")) machine_dir_ = "C:\\HLC";
            if (path_exists(conv_path_) || path_exists(stored("house_db.mdb"))) defer([this] { load_research(); });
        }
    }

    // the page's own header carries every action; the toolbar only adds the report
    void draw_toolbar() override {
        ImGui::SameLine(0, S(2));
        if (icon_button("rpt", ui::Icon::Copy, "Export teaching report (CSV)...", false, !rows_.empty(), 0)) defer([this] { export_report(); });
    }

    void draw(ImVec2 size) override {
        ensure_rows();
        const GTheme& t = th();
        if (rows_.empty()) {
            if (empty_state(ui::Icon::Bom, "No part numbers yet", "Import a BOM or CAD data; every part number gets a FlexLine profile here.",
                            "Import BOM...", ui::Icon::ImportBom))
                post_command(app::ID_BOM_IMPORT);
            return;
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) find_next_untaught();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) { msel_.clear(); msel_.insert(view_.begin(), view_.end()); }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                if (io.KeyShift) copy_rows(view_);
                else copy_rows(selection());
            }
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), S(6)));
        draw_header(size.x);
        float side = std::min(S(420), size.x * 0.42f);
        float h = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("##list", ImVec2(size.x - side - S(8), h), ImGuiChildFlags_None);
        draw_filter_bar();
        draw_table(ImGui::GetContentRegionAvail().y);
        ImGui::EndChild();
        ImGui::SameLine(0, S(8));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
        ImGui::BeginChild("##detail", ImVec2(side, h), ImGuiChildFlags_Borders);
        draw_detail(side - S(16));
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    // ---- used by the Export page (output_views.cpp) -------------------------
    void ensure_rows() { if (gen_ != doc->data_gen || rows_.empty()) { gen_ = doc->data_gen; rebuild_rows(); } }
    // readiness of the given part numbers (nullptr = every project part)
    void fill_readiness(const std::set<std::string>* pns, Readiness& o) {
        on_activate();
        ensure_rows();
        o = Readiness{};
        o.library = lib_.is_open();
        o.db = db_.is_open();
        if (o.library) { size_t s = lib_path_.find_last_of("\\/"); o.library_name = s == std::string::npos ? lib_path_ : lib_path_.substr(s + 1); }
        for (auto& r : rows_) {
            if (pns ? !pns->count(r.pn) : r.qty == 0) continue;
            ++o.parts;
            o.renamed += r.mname != r.pn;
            o.pending += (r.pid && r.pid != r.db_pid) || r.tape >= 0 || (o.db && r.db_id == 0);
            if (!o.library) continue;
            switch (r.st) {
            case Status::Taught: ++o.taught; break;
            case Status::Auto: ++o.autoa; break;
            case Status::Conflict: ++o.conflict; break;
            default: ++o.untaught; break;
            }
            if ((r.st == Status::Untaught || r.st == Status::Conflict) && o.attention.size() < 6) o.attention.push_back(r.pn);
            const Profile* p = lib_.find(current_pid(r));
            std::string tape, noz;
            if (p) {
                tape = p->tape_kind == 'P' ? "paper" : p->tape_kind == 'E' ? "emboss" : tape_type_name(p->tape_type);
                if (r.ev.feeder.tape_mm) tape = std::to_string(r.ev.feeder.tape_mm) + " mm " + tape;
                if (p->nozzle) noz = std::to_string(p->nozzle);
            } else if (r.ev.feeder.tape_mm) tape = std::to_string(r.ev.feeder.tape_mm) + " mm (implied)";
            if (noz.empty() && !r.ev.feeder.nozzle.empty()) noz = r.ev.feeder.nozzle;
            if (!tape.empty()) ++o.tape[tape];
            if (!noz.empty()) ++o.nozzle[noz];
        }
    }
    // open on the next part that still needs teaching
    void focus_next() {
        if (status_filter_ != F_All && status_filter_ != F_Untaught && status_filter_ != F_Conflict) { status_filter_ = 0; apply_filter(); }
        sel_ = -1;
        find_next_untaught();
    }

private:
    // ------------------------------------------------------------------ data
    Library lib_;
    Classifier cls_;
    ComponentDB db_;
    std::string lib_path_, conv_path_, tpl_path_, status_ = "No profile library loaded";
    history::History hist_;
    std::string hist_paths_;                   // '|'-separated pnp_harvest / cpf_harvest CSVs
    bool hist_ok_ = false;
    std::string conv_note_;
    std::vector<Row> rows_;
    std::vector<int> view_;      // filtered row indices
    int sel_ = -1, sel_sugg_ = 0;
    uint64_t gen_ = 0;
    bool tried_load_ = false;
    char filter_[128] = {0};
    int status_filter_ = 0;      // Filter
    float threshold_ = 0.75f;    // below this an automatic pick is listed under "Check" (optional review)
    std::set<int> msel_;         // multi-selection (row indices)
    int anchor_ = -1;
    bool ctx_open_ = false;
    int dnp_ = 0;                // DNP / unassigned references skipped
    std::string last_export_;    // last database written by Export
    // H8H research, installed as the H8H writer's research hook: a part in the house component DB gets its DB
    // rows; any other part the package-library profile teaching picks for it, with its own body / carrier / pitch /
    // nozzle (what teaching would write to the DB). Parts neither fills get the writer's built-in rules.
    std::string machine_dir_;      // the HLC software folder (ini "Machine"): SubCompo*.ini acceleration tables
    std::string research_note_;
    size_t research_db_parts_ = 0;
    std::shared_ptr<juki::ResearchDb> house_rdb_, lib_rdb_;   // the house DB / the package library, as research data
    struct TeachSnap { std::shared_ptr<Library> lib; Classifier cls; history::History hist; };
    std::shared_ptr<const TeachSnap> snap_;                   // immutable copy for the hook (export may run any time)
    std::string noz_text_;       // owned nozzles, user list ("" = from machine history)
    char noz_buf_[128] = {0};
    int sort_col_ = C_PN;
    bool sort_asc_ = true;

    const Package* package_for(const std::string& pn) const {
        auto& prj = doc->prj;
        auto it = prj.parts.find(pn);
        if (it != prj.parts.end() && !it->second.package.empty()) {
            auto p = prj.packages.find(it->second.package);
            if (p != prj.packages.end()) return &p->second;
        }
        for (auto& c : prj.comps)
            if (c.number == pn) if (const Package* p = prj.package_of(c)) return p;
        return nullptr;
    }

    void rebuild_rows() {
        std::map<std::string, Row> old;
        for (auto& r : rows_) old[r.pn] = r;
        rows_.clear();
        auto& prj = doc->prj;
        std::map<std::string, int> qty;
        for (auto& c : prj.comps) if (c.feature == Feature::Component && !c.number.empty()) ++qty[c.number];
        std::set<std::string> seen;
        dnp_ = 0;
        auto add = [&](const std::string& pn) {
            if (pn.empty() || !seen.insert(pn).second) return;
            if (is_dnp(pn)) { ++dnp_; return; }   // "???" / DNP: nothing to place, nothing to teach
            Row r;
            auto o = old.find(pn);
            if (o != old.end()) { r.pid = o->second.pid; r.tape = o->second.tape; r.auto_set = o->second.auto_set; }
            r.pn = pn;
            auto it = prj.parts.find(pn);
            if (it != prj.parts.end()) { r.desc = it->second.description; r.pkg = it->second.package; }
            if (r.pkg.empty())
                for (auto& c : prj.comps) if (c.number == pn && !c.package.empty()) { r.pkg = c.package; break; }
            r.qty = qty[pn];
            rows_.push_back(std::move(r));
        };
        for (auto& kv : prj.parts) add(kv.first);
        for (auto& c : prj.comps) if (c.feature == Feature::Component) add(c.number);
        assign_machine_names();
        refresh_db_state();
        evaluate_all();
        apply_filter();
        if (sel_ >= (int)rows_.size()) sel_ = -1;
        msel_.clear();
        anchor_ = -1;
    }

    void assign_machine_names() {
        // names already in the target DB keep their mapping; new ones are made unique
        std::set<std::string> used;
        for (auto& r : rows_) {
            r.mname = machine_name(r.pn, [&](const std::string& c) { return used.count(upper(c)) > 0; });
            used.insert(upper(r.mname));
        }
    }

    void refresh_db_state() {
        std::map<std::string, Part> by;
        if (db_.is_open()) for (auto& p : db_.parts()) by[upper(p.name)] = p;
        for (auto& r : rows_) {
            auto it = by.find(upper(r.mname));
            r.db_id = it == by.end() ? 0 : it->second.id;
            r.db_pid = it == by.end() ? 0 : it->second.pkg_id;
            r.db_tape = it == by.end() ? 0 : it->second.tape_type;
            if (r.desc.empty() && it != by.end()) r.desc = it->second.desc;
        }
    }

    void evaluate_all() {
        if (!lib_.is_open()) return;
        for (auto& r : rows_) evaluate_row(r);
    }
    void evaluate_row(Row& r) {
        r.ev = evaluate(cls_, r.pn, r.desc, package_for(r.pn));
        r.evaluated = true;
        r.has_mach = false;
        if (lib_.is_open()) {
            // teaching: package code / IPC name / footprint / machine history -> profile, body, carrier, pitch, nozzle.
            // Runs with or without machine history (size rules and fallbacks fill in); the pick always leads.
            // machine programs carry the (<= 20 char) machine name; fall back to the full part number
            const std::string& key = !r.mname.empty() && hist_.find(r.mname) ? r.mname : r.pn;
            const Footprint& f = r.ev.fp;
            Teach tc = teach_part(cls_, lib_, hist_, key, r.desc, f.ext_l, f.ext_w, true, r.pkg, f.body_from_art ? f.body_l : 0,
                                  f.body_from_art ? f.body_w : 0);
            r.mach = tc.machine;
            r.mach_route = tc.profile_route;
            r.has_mach = r.mach.tape || r.mach.nozzle || r.mach.l > 0;
            // other suggestions that need a nozzle the shop does not have sink
            const std::set<int>& own = hist_.owned_nozzles();
            if (!own.empty())
                for (auto& sg : r.ev.sugg)
                    if (const Profile* p = lib_.find(sg.pid)) if (p->nozzle && !own.count(p->nozzle)) { sg.conf *= 0.6; sg.why += "; nozzle " + std::to_string(p->nozzle) + " not owned"; }
            if (tc.pid) {
                auto it = std::find_if(r.ev.sugg.begin(), r.ev.sugg.end(), [&](const Suggestion& x) { return x.pid == tc.pid; });
                Suggestion sg;
                if (it != r.ev.sugg.end()) { sg = *it; r.ev.sugg.erase(it); }
                sg.pid = tc.pid;
                sg.conf = std::max(0.05, tc.conf);
                sg.why = "auto: " + tc.profile_route + (sg.why.empty() ? std::string() : " | " + sg.why);
                std::stable_sort(r.ev.sugg.begin(), r.ev.sugg.end(), [](const Suggestion& a, const Suggestion& b) { return a.conf > b.conf; });
                r.ev.sugg.insert(r.ev.sugg.begin(), sg);
                // The footprint body is derived from the pads (SOD123 pads span 3.04 x 0.68); a body from the machine,
                // the .cpf, the package code or the IPC name is better evidence. When the pick fits that body, size
                // conflicts measured against the pads are dropped (lead-count conflicts stay).
                const std::string& why = r.mach.why;
                const bool good_body = r.mach.l > 0 && (r.mach.from_history || why.rfind("body: CAD", 0) == 0 || why.rfind("body: package", 0) == 0 ||
                                                        why.rfind("body: IPC", 0) == 0);
                const Profile* tp = lib_.find(tc.pid);
                if (good_body && tp && tp->l > 0 && tp->w > 0) {
                    const double pl = std::max(tp->l, tp->w), pw = std::min(tp->l, tp->w);
                    const double e = std::fabs(std::log(pl / r.mach.l)) + std::fabs(std::log(pw / std::max(0.05, r.mach.w)));
                    if (e < 0.6) {
                        auto& w = r.ev.warnings;
                        w.erase(std::remove_if(w.begin(), w.end(), [](const std::string& x) {
                                    return x.find("footprint body") != std::string::npos || x.find("body short edge") != std::string::npos ||
                                           x.find("does not fit the footprint") != std::string::npos;
                                }), w.end());
                        r.ev.conflict = false;
                        for (auto& x : w) if (x.find("leads, footprint has") != std::string::npos) r.ev.conflict = true;
                    }
                }
            }
        }
        update_status(r);
    }

    // pnp_harvest *_components.csv (machine programs) and cpf_harvest *_parts.csv (CAD bodies), on a worker
    void load_history() {
        struct Res { history::History h; std::string err; size_t recs = 0, cad = 0; };
        auto res = std::make_shared<Res>();
        std::string paths = hist_paths_;
        app::DocPtr d = doc;
        run_with_progress("Loading machine history", [res, paths](Progress& pr) {
            size_t a = 0;
            while (a <= paths.size()) {
                size_t b = paths.find('|', a);
                std::string f = paths.substr(a, b == std::string::npos ? std::string::npos : b - a);
                a = b == std::string::npos ? paths.size() + 1 : b + 1;
                if (f.empty()) continue;
                // the original when reachable (refreshing PCB Plotter's copy), else the copy
                const std::string nm = "history_" + base_name(f);
                if (path_exists(f)) store_copy(f, nm);
                else if (path_exists(stored(nm))) f = stored(nm);
                pr.set(-1.f, f.c_str());
                std::string e;
                size_t n = res->h.load_csv(f, e);
                if (n) res->recs += n;
                else if (size_t c = res->h.load_cpf_parts(f, e)) res->cad += c;
                else if (res->err.empty()) res->err = f + ": " + e;
            }
            pr.set(-1.f, "Building size tables...");
            res->h.build();
        }, [this, res, d](bool ok) {
            if (!ok || !d || !docui_of(d.get())) return;
            hist_ = std::move(res->h);
            hist_ok_ = hist_.parts() > 0;
            install_research();
            apply_nozzles();
            evaluate_all();
            apply_filter();
            std::string m = "Machine history: " + std::to_string(res->recs) + " program records, " + std::to_string(hist_.parts()) + " parts" +
                            (res->cad ? ", " + std::to_string(res->cad) + " CAD bodies" : std::string());
            if (!res->err.empty()) toast(m + " (" + res->err + ")", ToastKind::Warning, 8.f);
            else toast(m, ToastKind::Success);
        }, ProgressOpts{true, false, 0.2f});
    }
    void pick_history() {
        std::wstring p;
        if (!ui::open_file_dialog(main_hwnd(), L"Add machine history (pnp_harvest *_components.csv or cpf_harvest *_parts.csv)",
                                  {{L"Harvest CSV (*.csv)", L"*.csv"}, {L"All files", L"*.*"}}, p)) return;
        std::string f = ui::U(p);
        if (("|" + hist_paths_ + "|").find("|" + f + "|") == std::string::npos) hist_paths_ += (hist_paths_.empty() ? "" : "|") + f;
        ini_set(L"History", hist_paths_);
        load_history();
    }
    // H8H research: the house component DB (+ the HLC folder's acceleration tables). Every H8H export then fills each
    // part's machine data from its DB rows (io/juki_research), like the editor's Database -> Research.
    void load_research() {
        struct Res { std::shared_ptr<juki::ResearchDb> db = std::make_shared<juki::ResearchDb>(); std::string dberr; int tables = 0; };
        auto res = std::make_shared<Res>();
        const std::string conv = conv_path_, machine = machine_dir_;
        app::DocPtr d = doc;
        run_with_progress("Loading research data", [res, conv, machine](Progress& pr) {
            // the house DB: the original when reachable (refreshing the copy), else PCB Plotter's copy
            std::string db = conv;
            if (path_exists(db)) store_copy(db, "house_db.mdb");
            else db = stored("house_db.mdb");
            pr.set(-1.f, "Reading the component database...");
            if (path_exists(db)) { if (!res->db->open(db, res->dberr)) res->db.reset(); }
            else { res->dberr = "no house conventions DB set"; res->db.reset(); }
            // the machine software's default acceleration tables (a copy is kept for when the folder is not reachable)
            if (res->db) {
                for (const char* nm : {"SubCompo.ini", "SubCompoFx1r.ini", "SubCompoFx2.ini"}) {
                    const std::string src = machine + "\\" + nm;
                    if (!machine.empty() && path_exists(src)) store_copy(src, nm);
                }
                std::string dir = stored("SubCompo.ini");
                const size_t sl = dir.find_last_of("\\/");
                if (sl != std::string::npos) dir.resize(sl);
                res->tables = res->db->load_machine_folder(dir);
            }
        }, [this, res, d](bool ok) {
            if (!ok || !d || !docui_of(d.get())) return;
            research_db_parts_ = res->db ? res->db->parts() : 0;
            house_rdb_ = res->db;
            install_research();
            if (res->db) {
                research_note_ = "H8H export fills machine data for the " + std::to_string(research_db_parts_) + " parts of the component DB" +
                                 (res->tables == 3 ? std::string()
                                                   : "\nHLC software folder: " + std::to_string(res->tables) +
                                                         " of 3 SubCompo*.ini found; accelerations the DB leaves at 0 stay 0");
                toast("H8H research ready: " + std::to_string(research_db_parts_) + " parts", ToastKind::Success);
            } else {
                research_note_ = "Component DB off (" + res->dberr + "): parts get machine data from the package library or the built-in rules";
                toast(research_note_, ToastKind::Warning, 8.f);
            }
        }, ProgressOpts{true, false, 0.2f});
    }

    // The H8H writer's research hook: house DB part -> its rows; else a package-library profile taught for the part
    // with the part's machine data over it. Reinstalled when the DB, the library or the machine history changes.
    void install_research() {
        if (lib_.is_open()) {
            auto sn = std::make_shared<TeachSnap>();
            sn->lib = std::make_shared<Library>(lib_);
            sn->cls = cls_;
            sn->cls.set_library(*sn->lib);
            sn->hist = hist_;
            snap_ = sn;
        } else snap_.reset();
        auto house = house_rdb_;
        auto libdb = lib_rdb_;
        auto snap = snap_;
        if (!house && !(libdb && snap)) { juki::set_h8h_research_hook(nullptr); return; }
        juki::set_h8h_research_hook([house, libdb, snap](JobPart& p) {
            auto fill = [&](const juki::ResearchResult& rr) {
                p.h8h_b4 = rr.rec.b4; p.h8h_b7 = rr.rec.b7; p.h8h_b18 = rr.rec.b18; p.h8h_b19 = rr.rec.b19; p.h8h_bga = rr.rec.bga;
                p.h8h_groups = rr.rec.groups; p.h8h_elements = rr.rec.elements; p.h8h_arrays = rr.rec.arrays;
            };
            juki::ResearchResult rr;
            std::string e;
            // 1. the house component DB (by name, long part number, or its 20-character machine name)
            if (house) {
                std::string mn;
                if (const long id = house->find_part(p.part, &mn)) {
                    if (!juki::research_rows(*house, id, juki::PartRows(), rr, e)) return false;
                    fill(rr);
                    p.h8h_source = "database";
                    p.h8h_name = mn;
                    return true;
                }
            }
            // 2. the package library: the profile teaching picks, with the part's own body / carrier / pitch / nozzle
            if (!libdb || !snap || !snap->lib) return false;
            const bool body = p.size_src == "body";
            // machine history is keyed by the 20-character machine name
            const std::string key = p.part.size() > 20 && snap->hist.find(p.part.substr(0, 20)) ? p.part.substr(0, 20) : p.part;
            const Teach tc = teach_part(snap->cls, *snap->lib, snap->hist, key, p.description, p.length, p.width, true, p.package,
                                        body ? p.length : 0, body ? p.width : 0);
            const Profile* pf = snap->lib->find(tc.pid);
            if (!pf) return false;
            const long tid = libdb->id_of(pf->name);
            if (!tid) return false;
            const history::Proposal& m = tc.machine;
            double l = m.l, w = m.w, h = m.h;
            if (!(l > 0 && w > 0) && body) { l = p.length; w = p.width; }
            if (!(h > 0)) h = p.height;
            const juki::PartRows ov = juki::template_rows(p.description, l, w, h, m.tape, m.pitch, m.nozzle);
            if (!juki::research_rows(*libdb, tid, ov, rr, e)) return false;
            fill(rr);
            p.h8h_source = "library";
            p.h8h_note = pf->name + (tc.profile_route.empty() ? std::string() : " (" + tc.profile_route + ")");
            return true;
        });
    }

    void clear_history() {
        hist_paths_.clear();
        ini_set(L"History", hist_paths_);
        hist_ = history::History();
        hist_ok_ = false;
        install_research();
        apply_nozzles();
        evaluate_all();
        apply_filter();
    }
    void update_status(Row& r) {
        if (r.pid && r.auto_set) r.st = Status::Auto;
        else if (r.pid || r.db_pid) r.st = Status::Taught;
        else r.st = Status::Untaught;
        if (r.ev.conflict && r.st != Status::Taught) r.st = Status::Conflict;
        if (r.st == Status::Taught && r.ev.fp.valid) {
            // an existing/hand assignment whose body contradicts the footprint is a conflict too
            const Profile* p = lib_.find(r.pid ? r.pid : r.db_pid);
            if (p && p->w > 0 && r.ev.fp.body_l > 0) {
                double pa = std::max(p->w, p->l), pb = std::min(p->w, p->l);
                double e1 = std::hypot((r.ev.fp.body_l - pa) / pa, (r.ev.fp.body_w - pb) / pb);
                double e2 = std::hypot((r.ev.fp.ext_l - pa) / pa, (r.ev.fp.ext_w - pb) / pb);
                if (std::min(e1, e2) > 0.45) r.st = Status::Conflict;
            }
        }
    }
    int current_pid(const Row& r) const { return r.pid ? r.pid : r.db_pid; }
    const Suggestion* best(const Row& r) const { return r.ev.sugg.empty() ? nullptr : &r.ev.sugg[0]; }

    // Open the .flx (Jet) on a worker; install it on the UI thread.
    void load_library(const std::string& path) {
        struct Res { Library l; std::string err; bool ok = false; std::shared_ptr<juki::ResearchDb> rdb = std::make_shared<juki::ResearchDb>(); };
        auto res = std::make_shared<Res>();
        app::DocPtr d = doc;
        run_with_progress("Opening FlexLine library", [res, path](Progress& p) {
            p.set(-1.f, "Reading package profiles...");
            res->ok = res->l.open(path, res->err);
            std::string e;
            if (res->ok) {
                p.set(-1.f, "Reading profile machine data...");
                if (!res->rdb->open(path, e)) res->rdb.reset();
                else {   // the acceleration tables kept from the HLC software folder (load_research)
                    std::string dir = stored("SubCompo.ini");
                    const size_t sl = dir.find_last_of("\\/");
                    if (sl != std::string::npos) dir.resize(sl);
                    res->rdb->load_machine_folder(dir);
                }
            }
        }, [this, res, path, d](bool ok) {
            if (!ok || !d || !docui_of(d.get())) return;
            if (!res->ok) { toast("Cannot open library: " + res->err, ToastKind::Error, 6.f); return; }
            lib_rdb_ = res->rdb;
            install_library(std::move(res->l), path);
        }, ProgressOpts{true, false, 0.2f});
    }
    void install_library(Library&& l, const std::string& path) {
        lib_ = std::move(l);
        cls_ = Classifier{};
        cls_.set_library(lib_);
        if (path != stored("profiles.flx")) {      // keep PCB Plotter's own copy current
            lib_path_ = path;
            ini_set(L"Library", path);
            store_copy(path, "profiles.flx");
        } else if (lib_path_.empty()) lib_path_ = path;
        learn();
        install_research();
        status_ = "Library: " + std::to_string(lib_.profiles.size()) + " profiles; " + conv_note_;
        rebuild_rows();
        if (sel_ < 0) find_next_untaught();   // land on the first part that needs attention
    }
    void learn() {
        std::vector<Classifier::Row> rows;
        conv_note_.clear();
        std::string err;
        const std::string snap = stored("conventions.tsv");
        if (!conv_path_.empty()) {
            ComponentDB c;
            if (path_exists(conv_path_) && c.open(conv_path_, err)) {
                std::string tsv = "name\tdescription\tprofile\n";
                for (auto& p : c.parts())
                    if (const Profile* pf = lib_.find(p.pkg_id)) {
                        rows.push_back({p.name, p.desc, p.pkg_id});
                        tsv += clean_cell(p.name) + "\t" + clean_cell(p.desc) + "\t" + clean_cell(pf->name) + "\n";
                    }
                conv_note_ = "conventions from " + std::to_string(rows.size()) + " assigned parts";
                std::string e2;
                if (!rows.empty()) write_file(snap, tsv.data(), tsv.size(), e2);   // PCB Plotter keeps what it learned
            } else if (!path_exists(conv_path_)) conv_note_ = "conventions DB not reachable";
            else conv_note_ = "conventions DB not readable (" + err + ")";
        }
        if (rows.empty()) {
            // what was learned last time (profile names map onto this library)
            std::string text, e2;
            if (read_text_file(snap, text, e2)) {
                std::map<std::string, int> by_name;
                for (auto& p : lib_.profiles) by_name[p.name] = p.id;
                size_t a = text.find('\n');
                while (a != std::string::npos && a + 1 < text.size()) {
                    size_t b = text.find('\n', a + 1);
                    std::string ln = text.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
                    a = b;
                    size_t t1 = ln.find('\t'), t2 = t1 == std::string::npos ? t1 : ln.find('\t', t1 + 1);
                    if (t2 == std::string::npos) continue;
                    auto it = by_name.find(ln.substr(t2 + 1));
                    if (it != by_name.end()) rows.push_back({ln.substr(0, t1), ln.substr(t1 + 1, t2 - t1 - 1), it->second});
                }
                if (!rows.empty()) conv_note_ = "conventions from " + std::to_string(rows.size()) + " parts (saved copy" +
                                                (conv_note_.empty() ? std::string(")") : std::string(", ") + conv_note_ + ")");
            }
        }
        if (rows.empty() && db_.is_open())
            for (auto& p : db_.parts()) if (lib_.find(p.pkg_id)) rows.push_back({p.name, p.desc, p.pkg_id});
        if (rows.empty()) {
            // no house history: let the library names stand in for conventions
            for (auto& p : lib_.profiles) rows.push_back({p.name, p.name, p.id});
            conv_note_ = "no conventions DB - library names used as priors";
        } else if (conv_note_.empty()) conv_note_ = "conventions from the target DB (" + std::to_string(rows.size()) + " parts)";
        cls_.learn(rows);
    }

    // ------------------------------------------------------------------ actions
    void apply_filter() {
        view_.clear();
        std::string q = upper(filter_);
        for (int i = 0; i < (int)rows_.size(); ++i) {
            const Row& r = rows_[(size_t)i];
            if (status_filter_ == F_Untaught && r.st != Status::Untaught) continue;
            if (status_filter_ == F_Conflict && r.st != Status::Conflict) continue;
            if (status_filter_ == F_Assigned && !(r.st == Status::Taught || r.st == Status::Auto)) continue;
            if (status_filter_ == F_Renamed && r.mname == r.pn) continue;
            if (status_filter_ == F_Check && !needs_check(r)) continue;
            if (!q.empty()) {
                std::string hay = upper(r.pn + " " + r.mname + " " + r.desc + " " + r.pkg);
                if (hay.find(q) == std::string::npos) continue;
            }
            view_.push_back(i);
        }
        auto key = [&](const Row& r) -> std::string {
            switch (sort_col_) {
            case C_Status: return std::to_string((int)r.st);
            case C_Machine: return upper(r.mname);
            case C_Desc: return upper(r.desc);
            case C_Pkg: return upper(r.pkg);
            case C_Profile: { const Profile* p = lib_.find(current_pid(r) ? current_pid(r) : (best(r) ? best(r)->pid : 0)); return p ? p->name : "~"; }
            case C_Conf: { char b[16]; std::snprintf(b, sizeof b, "%06.3f", best(r) ? best(r)->conf : 0.0); return b; }
            default: return upper(r.pn);
            }
        };
        std::stable_sort(view_.begin(), view_.end(), [&](int a, int b) {
            std::string ka = key(rows_[(size_t)a]), kb = key(rows_[(size_t)b]);
            return sort_asc_ ? ka < kb : ka > kb;
        });
    }

    void find_next_untaught() {
        if (view_.empty()) return;
        int start = 0;
        for (int k = 0; k < (int)view_.size(); ++k) if (view_[(size_t)k] == sel_) start = k + 1;
        for (int n = 0; n < (int)view_.size(); ++n) {
            int i = view_[(size_t)((start + n) % view_.size())];
            if (rows_[(size_t)i].st == Status::Untaught || rows_[(size_t)i].st == Status::Conflict) { select(i); scroll_to_ = true; return; }
        }
        gui::status("No untaught parts left");
    }
    bool scroll_to_ = false;

    void select(int i) {
        sel_ = i;
        sel_sugg_ = 0;
        if (i >= 0) {
            Row& r = rows_[(size_t)i];
            int cur = current_pid(r);
            for (size_t k = 0; k < r.ev.sugg.size(); ++k) if (r.ev.sugg[k].pid == cur) sel_sugg_ = (int)k;
        }
    }

    // every part without a profile gets the best pick (the teaching fallbacks always produce one when any size is
    // known); low-confidence picks and footprint conflicts are listed under "Check" for an optional look
    void auto_assign() {
        int n = 0, chk = 0, none = 0;
        for (auto& r : rows_) {
            if (current_pid(r)) continue;
            const Suggestion* b = best(r);
            if (!b) { ++none; continue; }
            r.pid = b->pid; r.auto_set = true; ++n;
            update_status(r);
            chk += needs_check(r);
            flash(row_key(r));
        }
        apply_filter();
        if (n) toast("Auto-assigned " + std::to_string(n) + " part numbers" + (chk ? "; " + std::to_string(chk) + " listed under Check (optional review)" : std::string()) +
                     (none ? "; " + std::to_string(none) + " with no size or description to go on" : std::string()), ToastKind::Success, 6.f);
        else toast(none ? std::to_string(none) + " parts have no size or description to go on" : std::string("Every part already has a profile"), ToastKind::Info);
    }
    bool needs_check(const Row& r) const {
        if (r.st == Status::Conflict) return true;
        if (r.st == Status::Untaught) return !r.ev.sugg.empty() && r.ev.sugg[0].conf < threshold_;
        return r.auto_set && best(r) && best(r)->pid == r.pid && best(r)->conf < threshold_;
    }
    std::vector<int> selection() const {
        std::vector<int> v;
        for (int i : view_) if (msel_.count(i)) v.push_back(i);
        if (v.empty() && sel_ >= 0) v.push_back(sel_);
        return v;
    }
    void apply_nozzles() {
        std::set<int> s;
        for (const char* p = noz_text_.c_str(); *p;) {
            if (std::isdigit((unsigned char)*p)) { int v = std::atoi(p); if (v >= 400 && v <= 999) s.insert(v); while (std::isdigit((unsigned char)*p)) ++p; }
            else ++p;
        }
        hist_.set_owned_nozzles(s);
    }
    std::string owned_text() const {
        std::string o;
        for (int n : hist_.owned_nozzles()) o += (o.empty() ? "" : " ") + std::to_string(n);
        return o;
    }

    // ---- report: one line per part, every decision and why (TSV for the clipboard, CSV to a file)
    std::vector<std::string> report_row(const Row& r) const {
        const int pid = current_pid(r) ? current_pid(r) : best(r) ? best(r)->pid : 0;
        const Profile* p = lib_.find(pid);
        const Suggestion* b = best(r);
        auto f2 = [](double v) { return v > 0 ? fmt2(v) : std::string(); };
        std::string warn;
        for (auto& w : r.ev.warnings) warn += (warn.empty() ? "" : " | ") + w;
        return {status_name(r.st), r.pn, r.mname, r.desc, r.pkg, std::to_string(r.qty), f2(r.mach.l), f2(r.mach.w), f2(r.mach.h),
                p ? p->name : std::string(), p && p->nozzle ? std::to_string(p->nozzle) : std::string(),
                b ? fmt2(b->conf) : std::string(), r.mach.tape ? tape_type_name(r.mach.tape) : std::string(),
                r.mach.pitch ? std::to_string(r.mach.pitch) : std::string(), r.mach.nozzle ? std::to_string(r.mach.nozzle) : std::string(),
                r.has_mach ? (r.mach.from_history ? "history" : "size rules") : std::string(), r.mach_route, r.mach.why, warn,
                needs_check(r) ? "yes" : ""};
    }
    static std::vector<std::string> report_head() {
        return {"Status", "PartNumber", "MachineName", "Description", "Package", "Qty", "BodyL", "BodyW", "BodyH", "Profile", "ProfileNozzle",
                "Conf", "Tape", "Pitch", "Nozzle", "Source", "ProfileReason", "MachineReason", "Warnings", "Check"};
    }
    void copy_rows(const std::vector<int>& idx) {
        if (idx.empty()) return;
        std::string o;
        auto line = [&](const std::vector<std::string>& c) { for (size_t k = 0; k < c.size(); ++k) o += (k ? "\t" : "") + clean_cell(c[k]); o += "\r\n"; };
        line(report_head());
        for (int i : idx) line(report_row(rows_[(size_t)i]));
        ImGui::SetClipboardText(o.c_str());
        gui::status("Copied " + std::to_string(idx.size()) + " rows (tab-separated)");
    }
    void export_report() {
        std::wstring out = L"teaching_report.csv";
        if (!ui::save_file_dialog(main_hwnd(), L"Export teaching report", {{L"CSV (*.csv)", L"*.csv"}}, L"csv", out)) return;
        std::string o = "\xEF\xBB\xBF";
        auto line = [&](const std::vector<std::string>& c) { for (size_t k = 0; k < c.size(); ++k) o += (k ? "," : "") + csv_cell(c[k]); o += "\r\n"; };
        line(report_head());
        for (int i : view_) line(report_row(rows_[(size_t)i]));
        std::string err;
        if (!write_file(ui::U(out), o.data(), o.size(), err)) { toast("Report not written: " + err, ToastKind::Error, 6.f); return; }
        toast("Report written: " + std::to_string(view_.size()) + " rows", ToastKind::Success);
    }

    static ImGuiID row_key(const Row& r) { return ImHashStr(r.pn.c_str()) ^ 0xC0FFEEu; }
    void assign(Row& r, int pid) {
        r.pid = pid; r.auto_set = false;
        flash(row_key(r));
        update_status(r);
        apply_filter();
    }

    void set_tape(Row& r, bool paper) {
        int pid = current_pid(r);
        const Profile* p = lib_.find(pid);
        // switch to the paper/emboss twin profile when the library has one (FlexAssign forceTape)
        if (p && p->sibling && ((paper && p->tape_kind == 'E') || (!paper && p->tape_kind == 'P'))) { r.pid = p->sibling; r.tape = -1; }
        else r.tape = paper ? 1 : 0;
        update_status(r);
    }

    void open_digikey(const std::string& pn) {
        std::string q;
        for (unsigned char c : pn) {
            if (std::isalnum(c) || c == '-' || c == '.' || c == '_') q += (char)c;
            else { char b[8]; std::snprintf(b, sizeof b, "%%%02X", c); q += b; }
        }
        std::wstring url = ui::W("https://www.digikey.com/en/products/result?keywords=" + q);
        defer([url] { ShellExecuteW(main_hwnd(), L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    }

    void pick_library() {
        std::wstring p = ui::W(lib_path_);
        if (!ui::open_file_dialog(main_hwnd(), L"Import profile library", {{L"FlexLine package library (*.flx;*.mdb)", L"*.flx;*.mdb"}, {L"All files", L"*.*"}}, p)) return;
        load_library(ui::U(p));
    }
    void pick_conventions() {
        std::wstring p = ui::W(conv_path_);
        if (!ui::open_file_dialog(main_hwnd(), L"Learn house conventions from (read only)", {{L"FlexLine component DB (*.mdb)", L"*.mdb"}, {L"All files", L"*.*"}}, p)) return;
        conv_path_ = ui::U(p);
        ini_set(L"Conventions", conv_path_);
        if (lib_.is_open()) { learn(); evaluate_all(); apply_filter(); status_ = "Library: " + std::to_string(lib_.profiles.size()) + " profiles; " + conv_note_; }
    }
    static bool file_exists(const std::string& p) { DWORD a = GetFileAttributesW(ui::W(p).c_str()); return a != INVALID_FILE_ATTRIBUTES; }
    static std::wstring full_path(const std::string& p) {
        if (p.empty()) return L"";
        wchar_t b[4096];
        DWORD n = GetFullPathNameW(ui::W(p).c_str(), 4096, b, nullptr);
        std::wstring w = n && n < 4096 ? std::wstring(b, n) : ui::W(p);
        for (auto& c : w) { if (c == L'/') c = L'\\'; c = (wchar_t)towlower(c); }
        return w;
    }
    static bool same_file(const std::string& a, const std::string& b) { return !a.empty() && !b.empty() && full_path(a) == full_path(b); }

    // where a NEW database gets its structure from (no template needed): PCB Plotter's saved empty copy, the
    // FlexLine empty template if one was ever given, else the house conventions DB (copied and emptied, only read)
    std::string new_db_source() const {
        if (file_exists(stored("flexdb_empty.mdb"))) return stored("flexdb_empty.mdb");
        if (file_exists(tpl_path_)) return tpl_path_;
        if (file_exists(conv_path_)) return conv_path_;
        return std::string();
    }
    std::string default_export_path() const {
        std::string dir = ini_get(L"ExportDir");
        if (dir.empty() || !file_exists(dir)) {
            wchar_t b[MAX_PATH] = {0};
            DWORD n = GetEnvironmentVariableW(L"USERPROFILE", b, MAX_PATH);
            dir = n && n < MAX_PATH ? ui::U(b) + "\\Documents" : ui::U(exe_dir());
        }
        std::string nm = ui::U(doc->display_name());
        size_t dot = nm.find_last_of('.');
        if (dot != std::string::npos) nm.resize(dot);
        for (auto& c : nm) if (std::strchr("\\/:*?\"<>|", c)) c = '_';
        SYSTEMTIME st; GetLocalTime(&st);
        char d[16]; std::snprintf(d, sizeof d, "%04d%02d%02d", st.wYear, st.wMonth, st.wDay);
        std::string base = dir + "\\FlexLine_" + (nm.empty() ? std::string("parts") : nm) + "_" + d;
        std::string out = base + ".mdb";
        for (int k = 2; file_exists(out) && k < 100; ++k) out = base + "_" + std::to_string(k) + ".mdb";
        return out;
    }

    // ---- Export: always writes a NEW file. The house conventions DB and an existing DB picked as the base are
    // only read; the output can never be one of them, and an existing file is never overwritten.
    void export_dialog() {
        if (!lib_.is_open()) return;
        int assigned = 0, picks = 0, none = 0;
        std::wstring undefined;   // parts with no usable name, description, package or size: no pick possible
        for (auto& r : rows_) {
            if (r.qty == 0 && !r.pid) continue;
            if (current_pid(r)) ++assigned; else if (best(r)) ++picks;
            else { if (++none <= 8) undefined += (undefined.empty() ? L"" : L", ") + ui::W(r.pn); }
        }
        if (none > 8) undefined += L", ... (+" + std::to_wstring(none - 8) + L")";
        ui::Form f(L"Export to FlexLine database", 620);
        f.header(L"Export to FlexLine database").icon((int)ui::Icon::Export)
            .subtitle(std::to_wstring(assigned + picks + none) + L" part numbers: " + std::to_wstring(assigned) + L" assigned, " + std::to_wstring(picks) +
                      L" get their best pick, " + std::to_wstring(none) + L" have nothing to go on.");
        f.combo("mode", L"Write", {L"A new database (recommended)", L"A copy of an existing database, plus these parts"}, L"A new database (recommended)");
        f.on_change([](ui::Form& fm, const std::string&) { const bool cp = fm.index("mode") == 1; fm.enable("src", cp); fm.enable("replace", cp); });
        f.file("src", L"Existing database (only read)", L"", {{L"FlexLine component DB (*.mdb)", L"*.mdb"}, {L"All files", L"*.*"}})
            .tip(L"Its components are copied into the new file together with these parts. The file itself is never written.");
        f.check("replace", L"Replace profiles of parts already taught in that database", false)
            .tip(L"Off: parts that already have a profile there keep it; only new or untaught parts are filled.");
        if (picks)
            f.check("best", L"Give the " + std::to_wstring(picks) + L" unassigned part" + (picks > 1 ? L"s" : L"") + L" their best pick", true)
                .tip(L"The same pick Auto-assign makes. Off: only assigned parts get a profile.");
        if (none) {
            f.label(L"Undefined (no usable name, description, package or size, so no profile can be picked): " + undefined);
            f.check("names", L"Write the " + std::to_wstring(none) + L" undefined part" + (none > 1 ? L"s" : L"") + L" anyway (name and description only)", false);
        }
        f.file("out", L"Save as (new file)", ui::W(default_export_path()), {{L"FlexLine component DB (*.mdb)", L"*.mdb"}}, true);
        f.label(L"Export only ever creates a new file; it never overwrites one. Your house conventions DB (" +
                ui::W(conv_path_.empty() ? std::string("not set") : base_name(conv_path_)) +
                L") is only read. Merge the result into it with the FlexLine software.");
        f.ok_text(L"Export");
        f.validate([this](ui::Form& fm, std::wstring& e) {
            const std::string out = fm.u8("out"), src = fm.u8("src");
            if (out.empty()) { e = L"Choose where to save the new database."; return false; }
            if (file_exists(out)) { e = L"That file already exists. Export never overwrites a file; choose a new name."; return false; }
            for (const std::string& prot : {conv_path_, lib_path_, src, tpl_path_, stored("flexdb_empty.mdb"), stored("profiles.flx")})
                if (same_file(out, prot)) { e = L"That is one of your source files. Choose a new file name."; return false; }
            if (fm.index("mode") == 1 && (src.empty() || !file_exists(src))) { e = L"Choose the existing database to copy."; return false; }
            if (fm.index("mode") == 0 && new_db_source().empty()) {
                e = L"No database structure to start from yet: set your house conventions DB under Teaching data, or pick an existing database (it is only read).";
                return false;
            }
            return true;
        });
        f.enable("src", false);
        f.enable("replace", false);
        if (!f.run(main_hwnd())) return;
        ExportJob j;
        j.copy = f.index("mode") == 1;
        j.src = j.copy ? f.u8("src") : new_db_source();
        j.out = f.u8("out");
        j.best = picks ? f.checked("best") : true;
        j.names = none ? f.checked("names") : false;
        j.replace = j.copy && f.checked("replace");
        { size_t sl = j.out.find_last_of("\\/"); if (sl != std::string::npos) ini_set(L"ExportDir", j.out.substr(0, sl)); }
        write_db(j);
    }

    struct ExportJob { bool copy = false, best = true, names = false, replace = false; std::string src, out; };
    // Jet writes are slow: run on a worker, install the written DB on the UI thread.
    void write_db(const ExportJob& j) {
        struct Item { std::string pn, mname, desc; int pid, cls_override, tape; bool explicit_pid, mach; history::Proposal m; };
        struct Res { ComponentDB db; int added = 0, applied = 0, kept = 0, taped = 0, machined = 0, fails = 0; std::string first_err, save_err, open_err; bool saved = false; };
        auto items = std::make_shared<std::vector<Item>>();
        for (auto& r : rows_) {
            if (r.qty == 0 && !r.pid) continue;
            int pid = current_pid(r);
            const bool expl = pid != 0;
            if (!pid && j.best && best(r)) pid = best(r)->pid;
            if (!pid && !j.names) continue;
            items->push_back({r.pn, r.mname, r.desc, pid, r.ev.text.pid == pid ? r.ev.text.cls_override : 0, r.tape, expl, r.has_mach && pid, r.mach});
        }
        auto res = std::make_shared<Res>();
        const Library* lib = &lib_;   // read-only on both threads while the modal job runs
        const std::string empty_copy = stored("flexdb_empty.mdb");
        app::DocPtr d = doc;
        run_with_progress("Writing FlexLine database", [res, items, lib, j, empty_copy](Progress& p) {
            std::string err;
            p.set(0.02f, "Preparing the new database...");
            if (!j.copy) {
                if (!res->db.create_empty_from(j.src, res->open_err)) return;
                // keep an empty copy so later exports start from it even without the source
                if (!same_file(j.src, empty_copy)) { std::string e2; ComponentDB c; if (c.create_empty_from(j.src, e2)) c.save(empty_copy, e2); }
            } else {
                if (!res->db.open(j.src, res->open_err)) return;
            }
            std::map<std::string, std::pair<int, int>> have;   // upper name -> (id, profile)
            for (auto& pt : res->db.parts()) have[upper(pt.name)] = {pt.id, pt.pkg_id};
            const size_t n = items->size();
            for (size_t i = 0; i < n; ++i) {
                if (p.cancelled()) return;
                const Item& it = (*items)[i];
                p.set(0.05f + 0.85f * (float)i / (float)std::max<size_t>(n, 1), it.mname.c_str());
                auto h = have.find(upper(it.mname));
                int id = h == have.end() ? 0 : h->second.first;
                const int had = h == have.end() ? 0 : h->second.second;
                if (!id) {
                    id = res->db.add_part(it.mname, it.desc, it.mname != it.pn ? it.pn : std::string(), err);
                    if (!id) { ++res->fails; if (res->first_err.empty()) res->first_err = it.pn + ": " + err; continue; }
                    ++res->added;
                }
                const bool may = !had || j.replace;
                if (it.pid && it.pid != had && may) {
                    if (res->db.apply_profile(id, *lib, it.pid, it.cls_override, true, err)) {
                        ++res->applied;
                        // machine history over the template: real body, carrier, pitch, nozzle (PkgTypeID stays the profile)
                        if (it.mach) {
                            history::Proposal m = it.m;
                            if (it.tape >= 0) m.tape = 0;            // explicit paper/emboss choice wins
                            if (res->db.apply_machine(id, m, err)) ++res->machined;
                            else { ++res->fails; if (res->first_err.empty()) res->first_err = it.pn + ": " + err; }
                        }
                    } else { ++res->fails; if (res->first_err.empty()) res->first_err = it.pn + ": " + err; }
                } else if (had) ++res->kept;
                if (it.tape >= 0 && (may || !had)) {
                    if (res->db.set_tape(id, it.tape == 1, err)) ++res->taped;
                    else { ++res->fails; if (res->first_err.empty()) res->first_err = it.pn + ": " + err; }
                }
            }
            p.set(0.95f, "Saving the new database file...");
            res->saved = res->db.save(j.out, res->save_err);
            p.set(1.f, "Done");
        }, [this, res, j, d](bool ok) {
            if (!d || !docui_of(d.get())) return;
            if (!ok) return;   // cancelled: nothing written
            if (!res->open_err.empty()) { toast("Cannot read " + base_name(j.src) + ": " + res->open_err, ToastKind::Error, 8.f); return; }
            if (!res->saved) { toast("Export failed: " + res->save_err, ToastKind::Error, 8.f); return; }
            db_ = std::move(res->db);           // statuses now reflect the file just written
            last_export_ = j.out;
            for (auto& r : rows_) { r.pid = 0; r.tape = -1; r.auto_set = false; }
            refresh_db_state();
            for (auto& r : rows_) { update_status(r); flash(row_key(r)); }
            apply_filter();
            std::string m = "Exported " + base_name(j.out) + ": " + std::to_string(res->added) + " parts, " + std::to_string(res->applied) + " profiles" +
                            (res->machined ? ", " + std::to_string(res->machined) + " with machine data" : std::string()) +
                            (res->kept ? ", " + std::to_string(res->kept) + " already taught kept" : std::string());
            std::wstring file = ui::W(j.out);
            auto show = [file] { std::wstring a = L"/select,\"" + file + L"\""; ShellExecuteW(main_hwnd(), L"open", L"explorer.exe", a.c_str(), nullptr, SW_SHOWNORMAL); };
            if (res->fails) toast(m + "; " + std::to_string(res->fails) + " problems (first: " + res->first_err + ")", ToastKind::Warning, 10.f, "Show file", show);
            else toast(m, ToastKind::Success, 8.f, "Show file", show);
            gui::status("FlexLine DB written: " + j.out, ToastKind::Success);
        });
    }

    // ---- header: data sources (persistent, one line of chips) + progress + the two actions
    void draw_header(float width) {
        const GTheme& t = th();
        int n[4] = {0, 0, 0, 0}, cand = 0, checks = 0;
        for (auto& r : rows_) {
            ++n[(int)r.st];
            checks += lib_.is_open() && needs_check(r);
            if (lib_.is_open() && !current_pid(r) && best(r)) ++cand;
        }
        const int total = (int)rows_.size();
        const int ready = n[(int)Status::Taught] + n[(int)Status::Auto];
        const bool lib = lib_.is_open(), idle = !busy();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
        ImGui::BeginChild("##hdr", ImVec2(0, S(84)), ImGuiChildFlags_Borders);
        // line 1: sources
        auto chip = [&](const char* id, const std::string& txt, bool ok, bool optional, const std::string& tip) {
            const ImU32 col = ok ? t.ok : optional ? t.text_faint : t.warn;
            ImGui::PushID(id);
            ImVec2 sz = ImGui::CalcTextSize(txt.c_str());
            ImVec2 a = ImGui::GetCursorScreenPos();
            bool hit = ImGui::InvisibleButton("##c", ImVec2(sz.x + S(26), S(24)));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const bool hov = ImGui::IsItemHovered();
            dl->AddRectFilled(a, a + ImVec2(sz.x + S(26), S(24)), hov ? t.raised : alpha(col, 0x18), S(12));
            dl->AddCircleFilled(a + ImVec2(S(11), S(12)), S(4), col);
            dl->AddText(a + ImVec2(S(20), (S(24) - sz.y) / 2), ok ? t.text : t.text_dim, txt.c_str());
            if (hov) tooltip((tip + "\n\nClick to change.").c_str());
            ImGui::PopID();
            ImGui::SameLine(0, S(6));
            if (hit) defer([this] { settings_dialog(); });
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImColor(t.text_dim), "Teaching data");
        ImGui::SameLine(0, S(10));
        chip("lib", lib ? "Profiles: " + base_name(lib_path_.empty() ? std::string("saved copy") : lib_path_) + " (" + std::to_string(lib_.profiles.size()) + ")"
                        : std::string("Profiles: not set"), lib, false,
             "FlexLine package profile library (.flx), read only. Needed to assign profiles.\nPCB Plotter keeps a copy and uses it when the original is not reachable.");
        chip("conv", conv_note_.empty() ? std::string("House conventions: not set") : "House conventions: " + base_name(conv_path_.empty() ? std::string("saved") : conv_path_),
             !conv_note_.empty() && conv_note_.find("not") == std::string::npos, true,
             "Component DB the profile choices are learned from (only ever read, never written).\n" + (conv_note_.empty() ? std::string("Optional, but picks are much better with it.") : conv_note_));
        int nh = 0;
        for (size_t i = 0, j; i < hist_paths_.size(); i = j + 1) { j = hist_paths_.find('|', i); if (j == std::string::npos) j = hist_paths_.size(); nh += j > i; }
        chip("hist", nh ? "Machine history: " + std::to_string(nh) + " file" + (nh > 1 ? "s" : "") : std::string("Machine history: none"), hist_ok_, true,
             "pnp_harvest / cpf_harvest CSVs: real bodies, tape, pitch and nozzle of parts that ran.\nOptional; size rules fill in without it.");
        {
            const std::string src = house_rdb_ ? std::to_string(research_db_parts_) + " DB parts" + (snap_ && lib_rdb_ ? " + library" : "")
                                  : snap_ && lib_rdb_ ? std::string("library + rules") : std::string("built-in rules");
            chip("rsch", "H8H machine data: " + src, house_rdb_ != nullptr, true,
                 (research_note_.empty() ? std::string("With the house conventions DB set, H8H exports carry full machine data per part\n"
                                                       "(what the HLC editor's Database -> Research writes).")
                                         : research_note_) +
                     "\nParts not in the DB: a package-library profile taught for the part, else built-in laser-centring rules.\n"
                     "Names longer than 20 characters keep their beginning; clashes get '~' + the end that tells them apart.");
        }
        chip("noz", "Nozzles: " + (hist_.owned_nozzles().empty() ? std::string("any") : owned_text()) + (noz_text_.empty() ? " (auto)" : ""), true, true,
             "Nozzles the shop owns. Picks needing another nozzle move to a same-size profile with an owned one.");
        // actions, right-aligned on line 1
        const float bw1 = S(150), bw2 = S(120);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - bw1 - bw2 - S(8)));
        if (!lib) {
            if (button("Set up...", ui::Icon::Open, true, ImVec2(bw1 + bw2 + S(6), 0), idle)) defer([this] { settings_dialog(); });
        } else {
            if (button(cand ? ("Auto-assign " + std::to_string(cand)).c_str() : "Auto-assign", ui::Icon::Wand, cand > 0, ImVec2(bw1, 0), idle && cand > 0)) auto_assign();
            if (ImGui::IsItemHovered()) tooltip("Give every part without a profile its best pick (Export does this too).");
            ImGui::SameLine(0, S(6));
            if (button("Export...", ui::Icon::Export, cand == 0, ImVec2(bw2, 0), idle)) defer([this] { export_dialog(); });
            if (ImGui::IsItemHovered()) tooltip("Write a FlexLine component database. Never writes to your house DB.");
        }
        // line 2: progress
        ImGui::Dummy(ImVec2(0, S(2)));
        const float pw = ImGui::GetContentRegionAvail().x;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float fr = anim("flexdb_ready", total ? (float)ready / (float)total : 0.f, 6.f);
        dl->AddRectFilled(p, ImVec2(p.x + pw, p.y + S(6)), t.raised, S(3));
        if (fr > 0) dl->AddRectFilled(p, ImVec2(p.x + pw * fr, p.y + S(6)), ready < total ? t.accent : t.ok, S(3));
        ImGui::Dummy(ImVec2(pw, S(6)));
        std::string line = std::to_string(ready) + " of " + std::to_string(total) + " part numbers have a profile";
        if (lib && cand) line += "   |   " + std::to_string(cand) + " waiting for Auto-assign or Export";
        if (lib && checks) line += "   |   " + std::to_string(checks) + " worth a look (Check filter, optional)";
        if (!last_export_.empty()) line += "   |   last export: " + base_name(last_export_);
        if (!lib) line += "   |   set the profile library to start";
        ImGui::TextColored(ImColor(t.text_dim), "%s", line.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
        (void)width;
    }

    // ---- teaching data: one dialog for every persistent source
    void settings_dialog() {
        ui::Form f(L"Teaching data", 640);
        f.header(L"Teaching data").icon((int)ui::Icon::Bom)
            .subtitle(L"Remembered between sessions. PCB Plotter keeps copies in %APPDATA%\\PCBPlotter\\FlexDB and uses them when the originals are not reachable. Nothing here is ever written to.");
        f.file("lib", L"Profile library (.flx)", ui::W(lib_path_), {{L"FlexLine profile library (*.flx)", L"*.flx"}, {L"All files", L"*.*"}})
            .tip(L"FlexLine package profile library, e.g. package LM.flx (read only). Required.");
        f.file("conv", L"House conventions DB", ui::W(conv_path_), {{L"FlexLine component DB (*.mdb)", L"*.mdb"}, {L"All files", L"*.*"}})
            .tip(L"Your assigned house component DB (.mdb). Only read: PCB Plotter learns which profile you use for what.\nAlso used as the structure source when a new database is created (it is copied, never written).");
        std::vector<std::vector<std::wstring>> hrows;
        for (size_t i = 0, j; i < hist_paths_.size(); i = j + 1) {
            j = hist_paths_.find('|', i);
            if (j == std::string::npos) j = hist_paths_.size();
            if (j > i) { std::string h = hist_paths_.substr(i, j - i); hrows.push_back({ui::W(base_name(h)), ui::W(h)}); }
        }
        auto hist = std::make_shared<std::string>(hist_paths_);
        f.list("hist", {L"Machine history CSV", L"Path"}, hrows, 4).wide();
        f.button("hadd", L"Add CSV...", [hist](ui::Form& fm) {
            fm.later([hist, &fm] {
                std::wstring p;
                if (!ui::open_file_dialog(main_hwnd(), L"Machine history CSV (pnp_harvest / cpf_harvest)", {{L"CSV (*.csv)", L"*.csv"}, {L"All files", L"*.*"}}, p)) return;
                std::string u = ui::U(p);
                if (hist->find(u) != std::string::npos) return;
                *hist += (hist->empty() ? "" : "|") + u;
                std::vector<std::vector<std::wstring>> rows;
                for (size_t i = 0, j; i < hist->size(); i = j + 1) { j = hist->find('|', i); if (j == std::string::npos) j = hist->size(); if (j > i) { std::string h = hist->substr(i, j - i); rows.push_back({ui::W(base_name(h)), ui::W(h)}); } }
                fm.set_list("hist", rows);
            });
        });
        f.button("hclr", L"Clear history", [hist](ui::Form& fm) { hist->clear(); fm.set_list("hist", {}); });
        f.text("mach", L"HLC software folder", ui::W(machine_dir_))
            .tip(L"Folder of the HLC editor (normally C:\\HLC). Its SubCompo.ini / SubCompoFx1r.ini / SubCompoFx2.ini hold the default "
                 L"table accelerations Research uses where the component DB leaves them at 0.\nOnly read; PCB Plotter keeps a copy.").wide();
        std::wstring auto_noz;
        for (int n : hist_.owned_from_history()) auto_noz += (auto_noz.empty() ? L"" : L" ") + std::to_wstring(n);
        f.text("noz", L"Nozzles owned", ui::W(noz_text_))
            .tip(L"FlexLine nozzle numbers you have, e.g. 500 504 505 506 507 508. Empty = from machine history" +
                 (auto_noz.empty() ? std::wstring(L" (none loaded: any nozzle)") : L" (" + auto_noz + L")") + L".");
        f.ok_text(L"Save");
        if (!f.run(main_hwnd())) return;
        const std::string nl = f.u8("lib"), nc = f.u8("conv"), nn = f.u8("noz");
        if (nn != noz_text_) { noz_text_ = nn; std::snprintf(noz_buf_, sizeof noz_buf_, "%s", nn.c_str()); ini_set(L"Nozzles", noz_text_); apply_nozzles(); }
        const bool conv_changed = nc != conv_path_;
        if (conv_changed) { conv_path_ = nc; ini_set(L"Conventions", conv_path_); }
        if (*hist != hist_paths_) {
            hist_paths_ = *hist;
            ini_set(L"History", hist_paths_);
            if (hist_paths_.empty()) clear_history(); else load_history();
        }
        const std::string nm = f.u8("mach");
        const bool machine_changed = nm != machine_dir_;
        if (machine_changed) { machine_dir_ = nm; ini_set(L"Machine", machine_dir_); }
        if (conv_changed || machine_changed) load_research();
        if (!nl.empty() && nl != lib_path_) load_library(nl);   // learns + evaluates
        else if (conv_changed && lib_.is_open()) { learn(); evaluate_all(); apply_filter(); }
        else if (lib_.is_open()) { evaluate_all(); apply_filter(); }
    }

    void draw_filter_bar() {
        const GTheme& t = th();
        if (search_box("##flt", filter_, sizeof filter_, "Filter part, description, package", S(260))) apply_filter();
        ImGui::SameLine();
        static const char* sf[] = {"All", "Untaught", "Conflicts", "Assigned", "Renamed", "Check"};
        static const ui::Icon si[] = {ui::Icon::SelectAll, ui::Icon::Question, ui::Icon::Warning, ui::Icon::Check, ui::Icon::Text, ui::Icon::Eye};
        static const char* tip[] = {"Show every part number", "No FlexLine profile yet", "Description and footprint disagree",
                                    "Taught in the DB or pending assignment", "Machine name shortened to 20 characters",
                                    "Optional review: low-confidence automatic picks and conflicts"};
        for (int i = 0; i < F_Count; ++i) {
            ImGui::SameLine(0, S(4));
            if (button(sf[i], si[i], status_filter_ == i)) { status_filter_ = i; apply_filter(); }
            if (ImGui::IsItemHovered()) tooltip(tip[i]);
        }
        int n[4] = {0, 0, 0, 0};
        for (auto& r : rows_) ++n[(int)r.st];
        ImGui::TextColored(ImColor(t.text_dim), "%zu part numbers   %d untaught   %d taught   %d auto   %d conflicts%s", rows_.size(),
                           n[(int)Status::Untaught], n[(int)Status::Taught], n[(int)Status::Auto], n[(int)Status::Conflict],
                           dnp_ ? ("   " + std::to_string(dnp_) + " DNP / unassigned skipped").c_str() : "");
        if (msel_.size() > 1) { ImGui::SameLine(); ImGui::TextColored(ImColor(t.accent), "   %zu selected (Ctrl+C copies)", msel_.size()); }
        if (!lib_.is_open()) {
            ImGui::SameLine();
            ImGui::TextColored(ImColor(t.warn), "  Import a profile library (.flx) to get suggestions");
        }
        if (view_.empty() && !rows_.empty()) {
            if (empty_state(ui::Icon::FindRef, "No parts match", "Nothing matches the filter text / status chip.", "Clear filter", ui::Icon::Eraser)) {
                filter_[0] = 0; status_filter_ = 0; apply_filter();
            }
        }
    }

    ImU32 status_color(Status s) const {
        const GTheme& t = th();
        switch (s) { case Status::Taught: return t.ok; case Status::Auto: return t.accent; case Status::Conflict: return t.error; default: return t.text_faint; }
    }

    void draw_table(float h) {
        const GTheme& t = th();
        // every column fits the width (no horizontal scroll, so nothing slides under a frozen column);
        // clipped cells show their full text on hover. Right-click a header to show/hide columns (Conf is hidden).
        ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##comp2", C_Count, tf, ImVec2(0, h))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, S(78));
        ImGui::TableSetupColumn("Part Number", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_NoHide, 1.4f);
        ImGui::TableSetupColumn("Machine name", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Body LxWxH mm", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Conf", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, S(46));
        ImGui::TableSetupColumn("Tape", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Feeder / nozzle", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs())
            if (ss->SpecsDirty && ss->SpecsCount > 0) {
                sort_col_ = ss->Specs[0].ColumnIndex;
                sort_asc_ = ss->Specs[0].SortDirection != ImGuiSortDirection_Descending;
                ss->SpecsDirty = false;
                apply_filter();
            }
        const float row_h = std::round(ImGui::GetTextLineHeight() + S(9));
        const float ty = (row_h - ImGui::GetTextLineHeight()) * 0.5f;
        ImGuiListClipper clip;
        clip.Begin((int)view_.size(), row_h);
        if (scroll_to_) {
            for (int k = 0; k < (int)view_.size(); ++k)
                if (view_[(size_t)k] == sel_) { clip.IncludeItemByIndex(k); ImGui::SetScrollY(std::max(0.f, k * row_h - h * 0.4f)); }
            scroll_to_ = false;
        }
        while (clip.Step())
            for (int vi = clip.DisplayStart; vi < clip.DisplayEnd; ++vi) {
                int i = view_[(size_t)vi];
                Row& r = rows_[(size_t)i];
                ImGui::TableNextRow(0, row_h);
                ImGui::PushID(i);
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Header, alpha(t.accent, 0x50));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
                if (ImGui::Selectable("##r", sel_ == i || msel_.count(i), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, row_h)))
                    click_row(i, vi);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    if (!msel_.count(i)) { msel_.clear(); msel_.insert(i); anchor_ = i; }
                    select(i);
                    ctx_open_ = true;
                }
                ImGui::PopStyleColor(2);
                {
                    float hv = anim(ImGui::GetID("hv"), ImGui::IsItemHovered() ? 1.f : 0.f, 18.f);
                    float fl = flash_amount(row_key(r));
                    ImU32 bg = 0;
                    if (hv > 0.01f) bg = col_alpha(t.accent, 0.10f * hv);
                    if (fl > 0.01f) bg = col_alpha(t.ok, 0.35f * fl + 0.10f * hv);
                    if (bg) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, bg);
                }
                ImGui::SameLine(0, 0);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 c = ImGui::GetCursorScreenPos();
                    const char* sn = status_name(r.st);
                    ImVec2 ts = ImGui::CalcTextSize(sn);
                    ImU32 col = status_color(r.st);
                    dl->AddRectFilled(ImVec2(c.x, c.y - S(1)), ImVec2(c.x + ts.x + S(12), c.y + ts.y + S(1)), alpha(col, 0x38), S(8));
                    ImGui::SetCursorScreenPos(ImVec2(c.x + S(6), c.y));
                    ImGui::TextColored(ImColor(col), "%s", sn);
                }
                const Profile* cur = lib_.find(current_pid(r));
                const Suggestion* b = best(r);
                for (int col = C_PN; col < C_Count; ++col) {
                    if (!ImGui::TableSetColumnIndex(col)) continue;
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ty);
                    switch (col) {
                    case C_PN: ImGui::PushFont(g_font_bold); cell(r.pn, t.text); ImGui::PopFont(); break;
                    case C_Machine:
                        if (r.mname == r.pn) { ImGui::TextColored(ImColor(t.text_faint), "="); if (ImGui::IsItemHovered()) tooltip("Same as the part number"); break; }
                        ImGui::PushFont(g_font_mono);
                        cell(r.mname, t.warn, "Shortened / adjusted to the 20-character machine limit.\nThe full part number goes to ExtCompoName.");
                        ImGui::PopFont();
                        break;
                    case C_Desc: cell(r.desc, t.text_dim); break;
                    case C_Pkg: cell(r.pkg, t.text); break;
                    case C_Body: {
                        // the body that is written (machine / package / footprint), not the raw pad span
                        double L = r.has_mach && r.mach.l > 0 ? r.mach.l : r.ev.fp.valid ? r.ev.fp.body_l : 0;
                        double W = r.has_mach && r.mach.l > 0 ? r.mach.w : r.ev.fp.valid ? r.ev.fp.body_w : 0;
                        char bb[64];
                        if (L > 0) {
                            if (r.has_mach && r.mach.h > 0) std::snprintf(bb, sizeof bb, "%.2f x %.2f x %.2f", L, W, r.mach.h);
                            else std::snprintf(bb, sizeof bb, "%.2f x %.2f", L, W);
                            std::string src = r.mach.why.substr(0, r.mach.why.find(';'));
                            cell(bb, r.mach.from_history ? t.ok : t.text, src.c_str());
                        } else ImGui::TextColored(ImColor(t.text_faint), "-");
                        break;
                    }
                    case C_Profile:
                        if (cur) cell(cur->name + (r.pid && r.pid != r.db_pid ? " *" : ""), r.pid && r.pid != r.db_pid ? t.accent : t.text, r.mach_route.c_str());
                        else if (b) { const Profile* p = lib_.find(b->pid); cell((p ? p->name : std::string()) + "?", t.text_faint, b->why.c_str()); }
                        break;
                    case C_Conf:
                        if (b) ImGui::TextColored(ImColor(b->conf >= 0.85 ? t.ok : b->conf >= 0.7 ? t.warn : t.error), "%.2f", b->conf);
                        break;
                    case C_Tape: {
                        std::string tp;
                        if (r.tape >= 0) tp = r.tape ? "paper *" : "emboss *";
                        else if (cur && !(r.pid == 0 && r.db_tape)) tp = cur->tape_kind == 'P' ? "paper" : cur->tape_kind == 'E' ? "emboss" : tape_type_name(cur->tape_type);
                        else if (r.db_tape) tp = tape_type_name(r.db_tape);
                        ImGui::TextUnformatted(tp.c_str());
                        break;
                    }
                    case C_Feeder:
                        if (r.has_mach && (r.mach.tape || r.mach.nozzle)) {
                            char fb[96];
                            std::snprintf(fb, sizeof fb, "%s / %d / N%d", r.mach.tape ? tape_type_name(r.mach.tape).c_str() : "?", r.mach.pitch, r.mach.nozzle);
                            cell(fb, r.mach.from_history ? t.ok : t.text_dim, r.mach.why.c_str());
                        } else if (r.ev.feeder.tape_mm) ImGui::TextColored(ImColor(t.text_dim), "%dmm / N:%s", r.ev.feeder.tape_mm, r.ev.feeder.nozzle.c_str());
                        if (!r.ev.warnings.empty()) { ImGui::SameLine(); icon_image(ui::Icon::Warning, ImGui::GetFontSize()); if (ImGui::IsItemHovered()) tooltip(r.ev.warnings[0].c_str()); }
                        break;
                    }
                }
                ImGui::PopID();
            }
        ImGui::EndTable();
        if (ctx_open_) { ImGui::OpenPopup("##rowctx"); ctx_open_ = false; }
        if (ImGui::BeginPopup("##rowctx")) {
            std::vector<int> selv = selection();
            const int ns = (int)selv.size();
            if (ns == 1 && sel_ >= 0) {
                Row& r = rows_[(size_t)sel_];
                const Suggestion* bs = best(r);
                const Profile* bp = bs ? lib_.find(bs->pid) : nullptr;
                std::string lab = bp ? "Assign best: " + bp->name : std::string("Assign best suggestion");
                if (menu_item_icon(lab.c_str(), ui::Icon::Assign, "Dbl-click", false, bp != nullptr)) assign(r, bp->id);
            } else if (menu_item_icon(("Assign best pick to " + std::to_string(ns) + " parts").c_str(), ui::Icon::Assign, nullptr, false, ns > 0)) {
                for (int i : selv) { Row& r = rows_[(size_t)i]; if (const Suggestion* bs = best(r)) { r.pid = bs->pid; r.auto_set = false; update_status(r); flash(row_key(r)); } }
                apply_filter();
            }
            if (menu_item_icon(ns > 1 ? "Clear pending changes" : "Clear pending change", ui::Icon::Unassign, nullptr, false, ns > 0)) {
                for (int i : selv) { Row& r = rows_[(size_t)i]; r.pid = 0; r.tape = -1; r.auto_set = false; update_status(r); }
                apply_filter();
            }
            ImGui::Separator();
            if (menu_item_icon(ns > 1 ? ("Copy " + std::to_string(ns) + " rows (TSV)").c_str() : "Copy row (TSV)", ui::Icon::Copy, "Ctrl+C", false, ns > 0)) copy_rows(selv);
            if (menu_item_icon("Copy all visible rows (TSV)", ui::Icon::Copy, "Ctrl+Shift+C", false, !view_.empty())) copy_rows(view_);
            if (menu_item_icon("Select all visible", ui::Icon::SelectAll, "Ctrl+A")) { msel_.clear(); msel_.insert(view_.begin(), view_.end()); }
            if (menu_item_icon("Export report (CSV)...", ui::Icon::Export)) defer([this] { export_report(); });
            if (ns == 1 && sel_ >= 0) {
                ImGui::Separator();
                const Row& r = rows_[(size_t)sel_];
                if (menu_item_icon("Copy part number", ui::Icon::Copy)) ImGui::SetClipboardText(r.pn.c_str());
                if (menu_item_icon("Copy machine name", ui::Icon::Copy)) ImGui::SetClipboardText(r.mname.c_str());
                if (menu_item_icon("Search DigiKey", ui::Icon::Link)) open_digikey(r.pn);
            }
            ImGui::Separator();
            if (menu_item_icon("Find next untaught", ui::Icon::FindRef, "F3")) find_next_untaught();
            ImGui::EndPopup();
        }
    }
    // text that fits its cell; clipped text (or an explanation) shows on hover
    void cell(const std::string& txt, ImU32 col, const char* tip = nullptr) {
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::TextColored(ImColor(col), "%s", txt.c_str());
        if (!ImGui::IsItemHovered()) return;
        const bool clipped = ImGui::CalcTextSize(txt.c_str()).x > avail;
        if (clipped && tip && *tip) tooltip((txt + "\n\n" + tip).c_str());
        else if (clipped) tooltip(txt.c_str());
        else if (tip && *tip) tooltip(tip);
    }
    void click_row(int i, int vi) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift && anchor_ >= 0) {
            int va = -1;
            for (int k = 0; k < (int)view_.size(); ++k) if (view_[(size_t)k] == anchor_) va = k;
            if (!io.KeyCtrl) msel_.clear();
            if (va >= 0) for (int k = std::min(va, vi); k <= std::max(va, vi); ++k) msel_.insert(view_[(size_t)k]);
        } else if (io.KeyCtrl) {
            if (!msel_.erase(i)) msel_.insert(i);
            anchor_ = i;
        } else {
            msel_.clear();
            msel_.insert(i);
            anchor_ = i;
        }
        select(i);
    }

    // profile (body + leads) vs footprint overlay
    void draw_preview(const Row& r, const Profile* p, float w, float h) {
        const GTheme& t = th();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##pv", ImVec2(w, h));
        dl->AddRectFilled(o, o + ImVec2(w, h), t.canvas, S(8));
        dl->AddRect(o, o + ImVec2(w, h), t.border, S(8));
        const Package* pkg = package_for(r.pn);
        BBox bb;
        if (pkg) for (auto& pd : pkg->pads) { bb.add({pd.pos.x - pd.w / 2, pd.pos.y - pd.h / 2}); bb.add({pd.pos.x + pd.w / 2, pd.pos.y + pd.h / 2}); }
        bool fp_horiz = bb.empty() ? true : bb.w() >= bb.h();
        double pl = p ? std::max(p->l, p->w) : 0, pw = p ? std::min(p->l, p->w) : 0;
        double ex = std::max({bb.empty() ? 0.0 : std::max(std::fabs(bb.x0), std::fabs(bb.x1)) * 2, fp_horiz ? pl : pw, 0.5});
        double ey = std::max({bb.empty() ? 0.0 : std::max(std::fabs(bb.y0), std::fabs(bb.y1)) * 2, fp_horiz ? pw : pl, 0.5});
        float sc = (float)std::min((w - S(24)) / ex, (h - S(40)) / ey);
        ImVec2 c = o + ImVec2(w * 0.5f, h * 0.5f - S(8));
        auto P = [&](double x, double y) { return ImVec2(c.x + (float)x * sc, c.y - (float)y * sc); };
        if (pkg)
            for (auto& pd : pkg->pads) {
                double r0 = std::fmod(std::fabs(pd.rot), 180.0);
                bool sw = r0 > 45 && r0 < 135;
                double a = (sw ? pd.h : pd.w) / 2, b2 = (sw ? pd.w : pd.h) / 2;
                ImVec2 p0 = P(pd.pos.x - a, pd.pos.y + b2), p1 = P(pd.pos.x + a, pd.pos.y - b2);
                dl->AddRectFilled(p0, p1, alpha(t.warn, 0x90), S(1));
            }
        if (p && pl > 0) {
            double bx = fp_horiz ? pl : pw, by = fp_horiz ? pw : pl;
            dl->AddRect(P(-bx / 2, by / 2), P(bx / 2, -by / 2), t.accent, S(2), 0, S(2));
            // leads from the profile's Side rows (when the library was taught with them)
            const int* sc4 = p->side_count;
            double lp = p->lead_pitch > 0 ? p->lead_pitch : 0, ll = p->lead_len > 0 ? p->lead_len : 0.4, lw = p->lead_w > 0 ? p->lead_w : (lp > 0 ? lp * 0.5 : 0.3);
            for (int s = 0; s < 4 && lp > 0; ++s) {
                int n = sc4[s];
                for (int k = 0; k < n; ++k) {
                    double off = (k - (n - 1) / 2.0) * lp;
                    double x0, y0, x1, y1;
                    if (s == 0 || s == 2) { double y = (s == 2 ? 1 : -1) * by / 2; x0 = off - lw / 2; x1 = off + lw / 2; y0 = y; y1 = y + (s == 2 ? ll : -ll); }
                    else { double x = (s == 1 ? 1 : -1) * bx / 2; y0 = off - lw / 2; y1 = off + lw / 2; x0 = x; x1 = x + (s == 1 ? ll : -ll); }
                    dl->AddRect(P(std::min(x0, x1), std::max(y0, y1)), P(std::max(x0, x1), std::min(y0, y1)), alpha(t.accent, 0xC0));
                }
            }
        }
        ImGui::SetCursorScreenPos(o + ImVec2(S(10), h - S(24)));
        ImGui::TextColored(ImColor(t.warn), "footprint pads");
        ImGui::SameLine(0, S(12));
        ImGui::TextColored(ImColor(t.accent), "%s", p ? ("profile " + fmt2(pl) + " x " + fmt2(pw) + " mm").c_str() : "no profile");
        ImGui::SetCursorScreenPos(o + ImVec2(0, h + S(6)));
    }

    void draw_detail(float w) {
        const GTheme& t = th();
        if (!lib_.is_open()) {
            if (empty_state(ui::Icon::PackageEditor, "No FlexLine library set", "Import the machine package library (.flx) to get ranked profile suggestions.",
                            "Import .flx...", ui::Icon::Open))
                defer([this] { pick_library(); });
            return;
        }
        if (sel_ < 0 || sel_ >= (int)rows_.size()) {
            ImGui::Dummy(ImVec2(0, S(10)));
            ImGui::TextColored(ImColor(t.text_dim), "Select a part number.");
            ImGui::Spacing();
            ImGui::TextWrapped("Workflow: import the profile library, optionally point Conventions at your assigned DB (e.g. Parts.mdb), "
                               "review conflicts, Auto assign confident, teach the rest from the suggestions, then Export to FlexLine DB.");
            return;
        }
        Row& r = rows_[(size_t)sel_];
        ImGui::PushFont(g_font_big);
        ImGui::TextUnformatted(r.pn.c_str());
        ImGui::PopFont();
        ImGui::TextColored(ImColor(t.text_dim), "%s", r.desc.empty() ? "(no description)" : r.desc.c_str());
        ImGui::TextColored(ImColor(t.text_dim), "Machine name:");
        ImGui::SameLine();
        ImGui::PushFont(g_font_mono); ImGui::TextUnformatted(r.mname.c_str()); ImGui::PopFont();
        ImGui::SameLine();
        if (button("DigiKey", ui::Icon::Link)) open_digikey(r.pn);
        const Footprint& f = r.ev.fp;
        section_header("FOOTPRINT (project package)");
        if (f.valid) {
            ImGui::Text("%s  %s", r.pkg.c_str(), f.shape.c_str());
            ImGui::Text("Body %.2f x %.2f mm%s   pads %d   span %.2f x %.2f", f.body_l, f.body_w, f.body_from_art ? " (artwork)" : " (from pads)",
                        f.pads, f.ext_l, f.ext_w);
            if (f.pitch > 0) { ImGui::SameLine(); ImGui::Text("  pitch %.2f", f.pitch); }
            if (!f.chip_code.empty()) ImGui::TextColored(ImColor(t.text_dim), "Chip size from footprint: %s", f.chip_code.c_str());
        } else ImGui::TextColored(ImColor(t.text_faint), "No package geometry for this part number");
        if (r.ev.feeder.tape_mm)
            ImGui::TextColored(ImColor(t.text_dim), "Implied feeder: %d mm tape, %d mm pitch, nozzle %s", r.ev.feeder.tape_mm, r.ev.feeder.pitch_mm, r.ev.feeder.nozzle.c_str());
        if (r.has_mach) {
            const history::Proposal& m = r.mach;
            ImGui::TextColored(ImColor(m.from_history ? t.ok : t.text), "Machine %s: body %.2f x %.2f x %.2f mm, %s, pitch %d, nozzle %d",
                               m.from_history ? "history" : "size tables", m.l, m.w, m.h, m.tape ? tape_type_name(m.tape).c_str() : "carrier ?",
                               m.pitch, m.nozzle);
            if (ImGui::IsItemHovered()) {
                std::string tip = m.why + "\nprofile: " + r.mach_route;
                char b[96]; std::snprintf(b, sizeof b, "\nconfidence: tape %.2f, pitch %.2f, nozzle %.2f", m.conf_tape, m.conf_pitch, m.conf_nozzle);
                tooltip((tip + b).c_str());
            }
        }
        for (auto& wmsg : r.ev.warnings) {
            icon_image(ui::Icon::Warning, ImGui::GetFontSize());
            ImGui::SameLine();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - S(24));
            ImGui::TextColored(ImColor(t.warn), "%s", wmsg.c_str());
            ImGui::PopTextWrapPos();
        }
        section_header("SUGGESTED PROFILES");
        if (!lib_.is_open()) { ImGui::TextColored(ImColor(t.text_faint), "Import a profile library first"); return; }
        if (r.ev.sugg.empty()) ImGui::TextColored(ImColor(t.text_faint), "No suggestion (%s)", r.ev.text.why.c_str());
        float list_h = std::min(S(30) * (float)r.ev.sugg.size() + S(4), S(190));
        ImGui::BeginChild("##sugg", ImVec2(w, list_h));
        for (int k = 0; k < (int)r.ev.sugg.size(); ++k) {
            const Suggestion& s = r.ev.sugg[(size_t)k];
            const Profile* p = lib_.find(s.pid);
            if (!p) continue;
            ImGui::PushID(k);
            bool is_cur = current_pid(r) == s.pid;
            std::string lab = p->name + (is_cur ? "   (assigned)" : "");
            if (ImGui::Selectable("##s", sel_sugg_ == k, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, S(26)))) {
                sel_sugg_ = k;
                if (ImGui::IsMouseDoubleClicked(0)) assign(r, s.pid);
            }
            if (ImGui::IsItemHovered()) tooltip(s.why.c_str());
            ImGui::SameLine(S(6));
            ImGui::TextColored(ImColor(s.conf >= 0.85 ? t.ok : s.conf >= 0.7 ? t.warn : t.error), "%.2f", s.conf);
            ImGui::SameLine(S(50));
            ImGui::TextColored(ImColor(is_cur ? t.accent : t.text), "%s", lab.c_str());
            ImGui::SameLine(w - S(110));
            ImGui::TextColored(ImColor(t.text_faint), "%s", lib_.class_name(p->cls).c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        const Profile* sp = (sel_sugg_ >= 0 && sel_sugg_ < (int)r.ev.sugg.size()) ? lib_.find(r.ev.sugg[(size_t)sel_sugg_].pid) : lib_.find(current_pid(r));
        if (button("Assign", ui::Icon::Assign, true, ImVec2(0, 0), sp != nullptr)) assign(r, sp->id);
        ImGui::SameLine();
        if (button("Clear", ui::Icon::Unassign, false, ImVec2(0, 0), r.pid != 0 || r.tape >= 0)) { r.pid = 0; r.tape = -1; r.auto_set = false; update_status(r); apply_filter(); }
        ImGui::SameLine(0, S(12));
        int cur = current_pid(r);
        bool has = cur != 0;
        const Profile* cp = lib_.find(cur);
        bool paper = r.tape >= 0 ? r.tape == 1 : (cp && cp->tape_kind == 'P');
        if (button("Paper", ui::Icon::Tag, has && paper, ImVec2(0, 0), has)) set_tape(r, true);
        ImGui::SameLine(0, S(2));
        if (button("Emboss", ui::Icon::Tag, has && !paper, ImVec2(0, 0), has)) set_tape(r, false);
        if (ImGui::IsItemHovered()) tooltip("Carrier tape is a reel attribute (not derivable from the description):\n"
                                            "Feeder.TapeTypeID 1/2 + Expansion.AutoTeach 1/0, or the Pp/Em twin profile.");
        if (sp) {
            ImGui::TextColored(ImColor(t.text_dim), "%s: %s, %.2f x %.2f x %.2f mm, %s, nozzle %d", sp->name.c_str(), lib_.class_name(sp->cls).c_str(),
                               sp->l, sp->w, sp->h, tape_type_name(sp->tape_type).c_str(), sp->nozzle);
            draw_preview(r, sp, w, std::max(S(150), ImGui::GetContentRegionAvail().y - S(40)));
        }
    }
};

} // namespace

std::unique_ptr<View> make_components_view() { return std::make_unique<ComponentsView>(); }

// ---- Export page hooks (declared in output_views.cpp) --------------------------
bool tooling_readiness(const app::DocPtr& d, const std::set<std::string>* pns, flexdb::Readiness& out) {
    DocUI* u = docui_of(d.get());
    if (!u) return false;
    auto* v = dynamic_cast<ComponentsView*>(u->view(ViewKind::Components));
    if (!v) return false;
    v->fill_readiness(pns, out);
    return true;
}
void tooling_open(const app::DocPtr& d, bool find_next) {
    activate_view(d, ViewKind::Components);
    if (!find_next) return;
    if (auto* v = dynamic_cast<ComponentsView*>(find_view(d, ViewKind::Components))) v->focus_next();
}

} // namespace pcbp::gui
