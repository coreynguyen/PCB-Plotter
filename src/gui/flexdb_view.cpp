// flexdb_view.cpp — "Components" view: JUKI FlexLine component database integration.
//
// Every project part number gets a 20-character machine name, the FlexAssign description
// classification, plus the evidence CircuitCAM has and the H7H export drops: the footprint
// (pad span/pitch/count) and package body. Ranked profile suggestions with confidence,
// tape / nozzle implications and mismatch warnings; assignments are written straight into a
// FlexLine component database (new from the empty template, or an existing one like Parts.mdb).
#include "gapp.h"
#include "../io/flexdb.h"
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

namespace pcbp::gui {

HWND main_hwnd();

namespace {

using namespace pcbp::flexdb;

ImU32 alpha(ImU32 c, int a) { return (c & 0x00FFFFFFu) | ((ImU32)a << 24); }
std::string fmt2(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

// ---- [FlexDB] section of PCBPlotter.ini -------------------------------------
std::wstring ini_file() {
    wchar_t b[MAX_PATH];
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring p = b;
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos ? std::wstring(L".") : p.substr(0, s)) + L"\\PCBPlotter.ini";
}
std::string ini_get(const wchar_t* key) {
    wchar_t b[1024] = {0};
    GetPrivateProfileStringW(L"FlexDB", key, L"", b, 1024, ini_file().c_str());
    return ui::U(b);
}
void ini_set(const wchar_t* key, const std::string& v) { WritePrivateProfileStringW(L"FlexDB", key, ui::W(v).c_str(), ini_file().c_str()); }

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
};

enum Col { C_Status, C_PN, C_Machine, C_Desc, C_Pkg, C_Body, C_Profile, C_Conf, C_Tape, C_Feeder, C_Count };

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
            if (!lib_path_.empty()) defer([this] { load_library(lib_path_); });
        }
    }

    void draw_toolbar() override {
        const bool idle = !busy();
        ImGui::SameLine(0, S(2));
        if (icon_button("lib", ui::Icon::Open, "Import profile library (.flx)...", false, idle, 0)) defer([this] { pick_library(); });
        ImGui::SameLine(0, S(2));
        if (icon_button("cnv", ui::Icon::Folder, "Learn house conventions from a component DB (read only)...", false, idle, 0)) defer([this] { pick_conventions(); });
        toolbar_separator();
        if (icon_button("fnu", ui::Icon::FindRef, "Find next untaught part  (F3)", false, !rows_.empty(), 0)) find_next_untaught();
        ImGui::SameLine(0, S(2));
        if (icon_button("aac", ui::Icon::Wand, "Auto-assign confident suggestions\n(conf >= threshold, no footprint conflict)", false, lib_.is_open() && idle, 0)) auto_assign();
        toolbar_separator();
        if (icon_button("exp", ui::Icon::Export, "Write to FlexLine DB...", false, lib_.is_open() && idle, 0)) defer([this] { export_dialog(); });
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
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F3, false)) find_next_untaught();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), S(6)));
        draw_config(size.x);
        draw_steps(size.x);
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
        if (status_filter_ != 0 && status_filter_ != 1 && status_filter_ != 2) { status_filter_ = 0; apply_filter(); }
        sel_ = -1;
        find_next_untaught();
    }

private:
    // ------------------------------------------------------------------ data
    Library lib_;
    Classifier cls_;
    ComponentDB db_;
    std::string lib_path_, conv_path_, tpl_path_, status_ = "No profile library loaded";
    std::string conv_note_;
    std::vector<Row> rows_;
    std::vector<int> view_;      // filtered row indices
    int sel_ = -1, sel_sugg_ = 0;
    uint64_t gen_ = 0;
    bool tried_load_ = false;
    char filter_[128] = {0};
    int status_filter_ = 0;      // 0 all, 1 untaught, 2 conflict, 3 taught/assigned
    float threshold_ = 0.85f;
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
        auto add = [&](const std::string& pn) {
            if (pn.empty() || !seen.insert(pn).second) return;
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
        update_status(r);
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
        struct Res { Library l; std::string err; bool ok = false; };
        auto res = std::make_shared<Res>();
        app::DocPtr d = doc;
        run_with_progress("Opening FlexLine library", [res, path](Progress& p) {
            p.set(-1.f, "Reading package profiles...");
            res->ok = res->l.open(path, res->err);
        }, [this, res, path, d](bool ok) {
            if (!ok || !d || !docui_of(d.get())) return;
            if (!res->ok) { toast("Cannot open library: " + res->err, ToastKind::Error, 6.f); return; }
            install_library(std::move(res->l), path);
        }, ProgressOpts{true, false, 0.2f});
    }
    void install_library(Library&& l, const std::string& path) {
        lib_ = std::move(l);
        cls_ = Classifier{};
        cls_.set_library(lib_);
        lib_path_ = path;
        ini_set(L"Library", path);
        learn();
        status_ = "Library: " + std::to_string(lib_.profiles.size()) + " profiles; " + conv_note_;
        rebuild_rows();
        if (sel_ < 0) find_next_untaught();   // land on the first part that needs attention
    }
    void learn() {
        std::vector<Classifier::Row> rows;
        conv_note_.clear();
        std::string err;
        if (!conv_path_.empty()) {
            ComponentDB c;
            if (c.open(conv_path_, err)) {
                for (auto& p : c.parts()) if (lib_.find(p.pkg_id)) rows.push_back({p.name, p.desc, p.pkg_id});
                conv_note_ = "conventions from " + std::to_string(rows.size()) + " assigned parts";
            } else conv_note_ = "conventions DB not readable (" + err + ")";
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
            if (status_filter_ == 1 && r.st != Status::Untaught) continue;
            if (status_filter_ == 2 && r.st != Status::Conflict) continue;
            if (status_filter_ == 3 && !(r.st == Status::Taught || r.st == Status::Auto)) continue;
            if (status_filter_ == 4 && r.mname == r.pn) continue;
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

    void auto_assign() {
        int n = 0;
        for (auto& r : rows_) {
            if (current_pid(r) || r.st == Status::Conflict) continue;
            const Suggestion* b = best(r);
            if (!b || b->conf < threshold_) continue;
            r.pid = b->pid; r.auto_set = true; ++n;
            update_status(r);
            flash(row_key(r));
        }
        apply_filter();
        if (n) toast("Auto-assigned " + std::to_string(n) + " part numbers (conf >= " + fmt2(threshold_) + ")", ToastKind::Success);
        else toast("Nothing above conf " + fmt2(threshold_) + " to assign", ToastKind::Info);
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
    void open_target() {
        std::wstring p;
        if (!ui::open_file_dialog(main_hwnd(), L"Open FlexLine component database", {{L"FlexLine component DB (*.mdb)", L"*.mdb"}, {L"All files", L"*.*"}}, p)) return;
        struct Res { ComponentDB db; std::string err; bool ok = false; };
        auto res = std::make_shared<Res>();
        std::string path = ui::U(p);
        app::DocPtr dp = doc;
        run_with_progress("Opening FlexLine database", [res, path](Progress& pr) {
            pr.set(-1.f, "Reading components...");
            res->ok = res->db.open(path, res->err);
        }, [this, res, path, dp](bool ok) {
            if (!ok || !dp || !docui_of(dp.get())) return;
            if (!res->ok) { toast("Cannot open FlexLine DB: " + res->err, ToastKind::Error, 6.f); return; }
            db_ = std::move(res->db);
            refresh_db_state();
            for (auto& r : rows_) update_status(r);
            apply_filter();
            toast("Opened FlexLine DB (" + std::to_string(db_.parts().size()) + " components)", ToastKind::Success);
        }, ProgressOpts{true, false, 0.2f});
    }
    bool new_target() {
        std::wstring p = ui::W(tpl_path_);
        if (tpl_path_.empty() || !file_exists(tpl_path_)) {
            if (!ui::open_file_dialog(main_hwnd(), L"FlexLine EMPTY database template", {{L"FlexLine empty DB (*.mdb)", L"*.mdb"}, {L"All files", L"*.*"}}, p)) return false;
        }
        std::string err;
        ComponentDB d;
        if (!d.create_from_template(ui::U(p), err)) { ui::message(main_hwnd(), L"FlexLine template", ui::W(err)); return false; }
        tpl_path_ = ui::U(p);
        ini_set(L"Template", tpl_path_);
        db_ = std::move(d);
        refresh_db_state();
        for (auto& r : rows_) update_status(r);
        apply_filter();
        return true;
    }
    static bool file_exists(const std::string& p) { DWORD a = GetFileAttributesW(ui::W(p).c_str()); return a != INVALID_FILE_ATTRIBUTES; }

    void export_dialog() {
        if (!lib_.is_open()) return;
        int pending = 0, tapes = 0, newparts = 0;
        for (auto& r : rows_) { pending += r.pid && r.pid != r.db_pid; tapes += r.tape >= 0; newparts += r.db_id == 0; }
        std::wstring msg = L"Write " + std::to_wstring(pending) + L" profile assignments, " + std::to_wstring(tapes) +
                           L" carrier changes and " + std::to_wstring(newparts) + L" new component names.\n\n"
                           L"Yes: create a NEW FlexLine database from the empty template\n"
                           L"No: update an EXISTING database (e.g. Parts.mdb - back it up first)";
        int r = ui::msg_box(main_hwnd(), msg, L"Export to FlexLine DB", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return;
        if (r == IDYES) { if (!new_target()) return; }
        else if (!db_.is_open() || db_.path().empty()) { open_target(); if (!db_.is_open()) return; }
        std::wstring out = ui::W(db_.path());
        if (!ui::save_file_dialog(main_hwnd(), L"Save FlexLine component database", {{L"FlexLine component DB (*.mdb)", L"*.mdb"}}, L"mdb", out)) return;
        write_db(ui::U(out));
    }

    // Jet writes are slow: run on a worker against a moved-out DB, reinstall on the UI thread.
    void write_db(const std::string& out) {
        struct Item { std::string pn, mname, desc; int db_id, pid, db_pid, cls_override, tape; };
        struct Res { ComponentDB db; int added = 0, applied = 0, taped = 0, fails = 0; std::string first_err, save_err; bool saved = false; };
        auto items = std::make_shared<std::vector<Item>>();
        for (auto& r : rows_) {
            if (r.qty == 0 && !r.pid) continue;
            items->push_back({r.pn, r.mname, r.desc, r.db_id, r.pid, r.db_pid, r.ev.text.pid == r.pid ? r.ev.text.cls_override : 0, r.tape});
        }
        auto res = std::make_shared<Res>();
        res->db = std::move(db_);
        const Library* lib = &lib_;   // read-only on both threads while the modal job runs
        app::DocPtr d = doc;
        run_with_progress("Writing FlexLine database", [res, items, lib, out](Progress& p) {
            std::string err;
            const size_t n = items->size();
            for (size_t i = 0; i < n; ++i) {
                if (p.cancelled()) return;
                const Item& it = (*items)[i];
                p.set(0.9f * (float)i / (float)std::max<size_t>(n, 1), it.mname.c_str());
                int id = it.db_id;
                if (!id) {
                    id = res->db.add_part(it.mname, it.desc, it.mname != it.pn ? it.pn : std::string(), err);
                    if (!id) { ++res->fails; if (res->first_err.empty()) res->first_err = it.pn + ": " + err; continue; }
                    ++res->added;
                }
                if (it.pid && it.pid != it.db_pid) {
                    if (res->db.apply_profile(id, *lib, it.pid, it.cls_override, true, err)) ++res->applied;
                    else { ++res->fails; if (res->first_err.empty()) res->first_err = it.pn + ": " + err; }
                }
                if (it.tape >= 0) {
                    if (res->db.set_tape(id, it.tape == 1, err)) ++res->taped;
                    else { ++res->fails; if (res->first_err.empty()) res->first_err = it.pn + ": " + err; }
                }
            }
            p.set(0.95f, "Saving database file...");
            res->saved = res->db.save(out, res->save_err);
            p.set(1.f, "Done");
        }, [this, res, out, d](bool ok) {
            if (!d || !docui_of(d.get())) return;
            db_ = std::move(res->db);
            if (!ok) { refresh_db_state(); return; }   // cancelled: pending edits are kept, nothing written
            if (!res->saved) { toast("FlexLine DB save failed: " + res->save_err, ToastKind::Error, 8.f); return; }
            for (auto& r : rows_) { r.pid = 0; r.tape = -1; r.auto_set = false; }
            refresh_db_state();
            for (auto& r : rows_) { update_status(r); flash(row_key(r)); }
            apply_filter();
            std::string m = "FlexLine DB written: " + std::to_string(res->added) + " added, " + std::to_string(res->applied) +
                            " profiles, " + std::to_string(res->taped) + " carrier";
            std::wstring dir = ui::W(out);
            size_t sl = dir.find_last_of(L"\\/");
            if (sl != std::wstring::npos) dir.resize(sl);
            auto open_dir = [dir] { ShellExecuteW(main_hwnd(), L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
            if (res->fails) toast(m + "; " + std::to_string(res->fails) + " problems (first: " + res->first_err + ")", ToastKind::Warning, 10.f, "Open folder", open_dir);
            else toast(m, ToastKind::Success, 6.f, "Open folder", open_dir);
            gui::status("FlexLine DB written: " + out, ToastKind::Success);
        });
    }

    void path_line(const char* label, const std::string& path, const char* btn, ui::Icon ic, const char* tip, int which) {
        const GTheme& t = th();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImColor(t.text_dim), "%s", label);
        ImGui::SameLine(S(130));
        std::string shown = path.empty() ? "(not set)" : path;
        ImGui::PushFont(g_font_mono);
        float w = ImGui::GetContentRegionAvail().x - S(150);
        ImGui::PushStyleColor(ImGuiCol_Text, path.empty() ? t.text_faint : t.text);
        ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(w, S(30)), true);
        ImGui::TextUnformatted(shown.c_str());
        ImGui::PopClipRect();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (ImGui::IsItemHovered() && !path.empty()) tooltip(path.c_str());
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - S(140));
        ImGui::PushID(which);
        if (button(btn, ic, false, ImVec2(S(140), 0))) {
            if (which == 0) defer([this] { pick_library(); });
            if (which == 1) defer([this] { pick_conventions(); });
            if (which == 2) defer([this] { open_target(); });
        }
        ImGui::PopID();
        if (tip && ImGui::IsItemHovered()) tooltip(tip);
    }

    void draw_config(float w) {
        (void)w;
        const GTheme& t = th();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
        ImGui::BeginChild("##cfg", ImVec2(0, S(118)), ImGuiChildFlags_Borders);
        path_line("Profile library", lib_path_, "Import .flx...", ui::Icon::Open, "FlexLine package profile library (read only), e.g. 'package LM.flx'", 0);
        path_line("Conventions", conv_path_, "Learn from...", ui::Icon::Bom, "Assigned component DB to learn house conventions from (read only), e.g. Parts.mdb", 1);
        std::string tgt = db_.is_open() ? (db_.path().empty() ? "(new database from template, not saved yet)" : db_.path()) : std::string();
        path_line("Target DB", tgt, "Open DB...", ui::Icon::Open, "FlexLine component database to update (or use Export to create a new one)", 2);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // Guided workflow: 1 name -> 2 auto-assign -> 3 review -> 4 write
    void draw_steps(float width) {
        const GTheme& t = th();
        int n[4] = {0, 0, 0, 0}, renamed = 0, cand = 0, pending = 0;
        for (auto& r : rows_) {
            ++n[(int)r.st];
            renamed += r.mname != r.pn;
            if (lib_.is_open() && !current_pid(r) && r.st != Status::Conflict && best(r) && best(r)->conf >= threshold_) ++cand;
            pending += (r.pid && r.pid != r.db_pid) || r.tape >= 0;
        }
        const int total = (int)rows_.size();
        const int open = n[(int)Status::Untaught] + n[(int)Status::Conflict];
        const int ready = n[(int)Status::Taught] + n[(int)Status::Auto];
        const float gap = S(8), h = S(96);
        const float cw = std::floor((width - S(4) - gap * 3) / 4);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        struct St { const char* title; std::string sub; bool done; bool active; };
        const bool lib = lib_.is_open();
        St st[4] = {
            {"Name parts", std::to_string(total) + " names" + (renamed ? ", " + std::to_string(renamed) + " shortened to 20 chars" : std::string(", all fit 20 chars")), total > 0, false},
            {"Auto-assign confident", !lib ? std::string("needs a profile library") : cand ? std::to_string(cand) + " ready at conf " + fmt2(threshold_) : std::string("nothing left above threshold"), lib && cand == 0, false},
            {"Review untaught", !lib ? std::string("-") : open ? std::to_string(n[(int)Status::Untaught]) + " untaught, " + std::to_string(n[(int)Status::Conflict]) + " conflicts" : std::string("all parts have a profile"), lib && open == 0, false},
            {"Write to FlexLine DB", pending ? std::to_string(pending) + " change(s) not written" : db_.is_open() ? std::string("DB up to date") : std::string("no target DB yet"), lib && pending == 0 && db_.is_open() && open == 0, false},
        };
        int active = 3;
        for (int i = 0; i < 4; ++i) if (!st[i].done) { active = i; break; }
        st[active].active = true;
        ImVec2 o = ImGui::GetCursorScreenPos();
        for (int i = 0; i < 4; ++i) {
            ImVec2 a(o.x + i * (cw + gap), o.y), b(a.x + cw, a.y + h);
            ImU32 bc = st[i].active ? col_lerp(t.accent, t.border, 0.35f * pulse(2.4f)) : t.border;
            dl->AddRectFilled(a, b, st[i].active ? alpha(t.accent, 0x1C) : t.panel, S(10));
            dl->AddRect(a, b, bc, S(10), 0, st[i].active ? S(2) : S(1));
            // number badge
            ImVec2 c(a.x + S(22), a.y + S(22));
            dl->AddCircleFilled(c, S(12), st[i].done ? t.ok : st[i].active ? t.accent : t.raised);
            char num[4]; std::snprintf(num, sizeof num, "%d", i + 1);
            if (st[i].done) { dl->AddLine(ImVec2(c.x - S(5), c.y), ImVec2(c.x - S(1), c.y + S(4)), t.accent_text, S(2)); dl->AddLine(ImVec2(c.x - S(1), c.y + S(4)), ImVec2(c.x + S(6), c.y - S(4)), t.accent_text, S(2)); }
            else { ImVec2 ts = ImGui::CalcTextSize(num); dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), st[i].active ? t.accent_text : t.text, num); }
            dl->PushClipRect(a, b, true);
            ImGui::PushFont(g_font_bold);
            dl->AddText(ImVec2(a.x + S(42), a.y + S(12)), t.text, st[i].title);
            ImGui::PopFont();
            dl->AddText(ImVec2(a.x + S(42), a.y + S(32)), t.text_dim, st[i].sub.c_str());
            dl->PopClipRect();
            ImGui::SetCursorScreenPos(ImVec2(a.x + S(12), b.y - S(36)));
            ImGui::PushID(i);
            switch (i) {
            case 0:
                if (button("Review names", ui::Icon::Text, false, ImVec2(0, 0), renamed > 0)) { status_filter_ = 4; apply_filter(); }
                if (ImGui::IsItemHovered()) tooltip("The machine library keys parts by a 20-character name. Longer part numbers are\nshortened here; the full number goes to ExtCompoName in FlexLine.");
                break;
            case 1:
                if (!lib) { if (button("Import .flx...", ui::Icon::Open, true)) defer([this] { pick_library(); }); break; }
                if (button(cand ? ("Auto-assign " + std::to_string(cand)).c_str() : "Auto-assign", ui::Icon::Wand, st[i].active, ImVec2(0, 0), cand > 0)) auto_assign();
                ImGui::SameLine(0, S(6));
                ImGui::SetNextItemWidth(std::max(S(60), cw - (ImGui::GetCursorScreenPos().x - a.x) - S(12)));
                ImGui::SliderFloat("##thr", &threshold_, 0.5f, 0.99f, "conf %.2f");
                if (ImGui::IsItemHovered()) tooltip("Only suggestions at or above this confidence and without a footprint conflict are assigned.");
                break;
            case 2:
                if (button("Find next", ui::Icon::FindRef, st[i].active, ImVec2(0, 0), lib && open > 0)) { if (status_filter_ == 3 || status_filter_ == 4) { status_filter_ = 0; apply_filter(); } find_next_untaught(); }
                ImGui::SameLine(0, S(6));
                if (button("Show", ui::Icon::Eye, status_filter_ == 1, ImVec2(0, 0), lib && open > 0)) { status_filter_ = n[(int)Status::Untaught] ? 1 : 2; apply_filter(); }
                break;
            case 3:
                if (button("Write to DB...", ui::Icon::Export, st[i].active, ImVec2(0, 0), lib)) defer([this] { export_dialog(); });
                break;
            }
            ImGui::PopID();
        }
        // progress
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h + S(8)));
        float pw = width - S(4);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float fr = anim("flexdb_ready", total ? (float)ready / (float)total : 0.f, 6.f);
        dl->AddRectFilled(p, ImVec2(p.x + pw, p.y + S(6)), t.raised, S(3));
        if (fr > 0) dl->AddRectFilled(p, ImVec2(p.x + pw * fr, p.y + S(6)), open ? t.accent : t.ok, S(3));
        ImGui::Dummy(ImVec2(pw, S(6)));
        ImGui::TextColored(ImColor(t.text_dim), "%d of %d part numbers have a FlexLine profile%s", ready, total, lib ? "" : "  (import a profile library to classify)");
    }

    void draw_filter_bar() {
        const GTheme& t = th();
        if (search_box("##flt", filter_, sizeof filter_, "Filter part, description, package", S(260))) apply_filter();
        ImGui::SameLine();
        static const char* sf[] = {"All", "Untaught", "Conflicts", "Assigned", "Renamed"};
        static const ui::Icon si[] = {ui::Icon::SelectAll, ui::Icon::Question, ui::Icon::Warning, ui::Icon::Check, ui::Icon::Text};
        static const char* tip[] = {"Show every part number", "No FlexLine profile yet", "Description and footprint disagree",
                                    "Taught in the DB or pending assignment", "Machine name shortened to 20 characters"};
        for (int i = 0; i < 5; ++i) {
            ImGui::SameLine(0, S(4));
            if (button(sf[i], si[i], status_filter_ == i)) { status_filter_ = i; apply_filter(); }
            if (ImGui::IsItemHovered()) tooltip(tip[i]);
        }
        int n[4] = {0, 0, 0, 0};
        for (auto& r : rows_) ++n[(int)r.st];
        ImGui::TextColored(ImColor(t.text_dim), "%zu part numbers   %d untaught   %d taught   %d auto   %d conflicts", rows_.size(),
                           n[(int)Status::Untaught], n[(int)Status::Taught], n[(int)Status::Auto], n[(int)Status::Conflict]);
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
        ImGuiTableFlags tf = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                             ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("##comp", C_Count, tf, ImVec2(0, h))) return;
        ImGui::TableSetupScrollFreeze(2, 1);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, S(74));
        ImGui::TableSetupColumn("Part Number", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_NoHide, S(150));
        ImGui::TableSetupColumn("Machine name (20)", ImGuiTableColumnFlags_WidthFixed, S(150));
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthFixed, S(220));
        ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthFixed, S(100));
        ImGui::TableSetupColumn("Body LxW mm", ImGuiTableColumnFlags_WidthFixed, S(92));
        ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthFixed, S(170));
        ImGui::TableSetupColumn("Conf", ImGuiTableColumnFlags_WidthFixed, S(46));
        ImGui::TableSetupColumn("Tape", ImGuiTableColumnFlags_WidthFixed, S(80));
        ImGui::TableSetupColumn("Fits (tape/noz)", ImGuiTableColumnFlags_WidthStretch, S(110));
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
                if (ImGui::Selectable("##r", sel_ == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, row_h))) select(i);
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
                    case C_PN: ImGui::PushFont(g_font_bold); ImGui::TextUnformatted(r.pn.c_str()); ImGui::PopFont(); break;
                    case C_Machine:
                        ImGui::PushFont(g_font_mono);
                        ImGui::TextColored(ImColor(r.mname != r.pn ? t.warn : t.text), "%s", r.mname.c_str());
                        ImGui::PopFont();
                        if (r.mname != r.pn && ImGui::IsItemHovered()) tooltip("Truncated / adjusted to the 20-character machine limit.\nThe full part number goes to ExtCompoName.");
                        break;
                    case C_Desc: ImGui::TextColored(ImColor(t.text_dim), "%s", r.desc.c_str()); break;
                    case C_Pkg: ImGui::TextUnformatted(r.pkg.c_str()); break;
                    case C_Body:
                        if (r.ev.fp.valid) ImGui::Text("%.2f x %.2f", r.ev.fp.body_l, r.ev.fp.body_w);
                        else ImGui::TextColored(ImColor(t.text_faint), "-");
                        break;
                    case C_Profile:
                        if (cur) ImGui::TextColored(ImColor(r.pid && r.pid != r.db_pid ? t.accent : t.text), "%s%s", cur->name.c_str(), r.pid && r.pid != r.db_pid ? " *" : "");
                        else if (b) { const Profile* p = lib_.find(b->pid); ImGui::TextColored(ImColor(t.text_faint), "%s?", p ? p->name.c_str() : ""); }
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
                        if (r.ev.feeder.tape_mm) ImGui::TextColored(ImColor(t.text_dim), "%dmm / N:%s", r.ev.feeder.tape_mm, r.ev.feeder.nozzle.c_str());
                        if (!r.ev.warnings.empty()) { ImGui::SameLine(); icon_image(ui::Icon::Warning, ImGui::GetFontSize()); if (ImGui::IsItemHovered()) tooltip(r.ev.warnings[0].c_str()); }
                        break;
                    }
                }
                ImGui::TableSetColumnIndex(0);
                if (ImGui::BeginPopupContextItem("##rowctx")) {
                    select(i);
                    const Suggestion* bs = best(r);
                    const Profile* bp = bs ? lib_.find(bs->pid) : nullptr;
                    std::string lab = bp ? "Assign best: " + bp->name : std::string("Assign best suggestion");
                    if (menu_item_icon(lab.c_str(), ui::Icon::Assign, "Dbl-click", false, bp != nullptr)) assign(r, bp->id);
                    if (menu_item_icon("Clear pending change", ui::Icon::Unassign, nullptr, false, r.pid != 0 || r.tape >= 0)) { r.pid = 0; r.tape = -1; r.auto_set = false; update_status(r); apply_filter(); }
                    ImGui::Separator();
                    if (menu_item_icon("Copy part number", ui::Icon::Copy)) ImGui::SetClipboardText(r.pn.c_str());
                    if (menu_item_icon("Copy machine name", ui::Icon::Copy)) ImGui::SetClipboardText(r.mname.c_str());
                    if (menu_item_icon("Search DigiKey", ui::Icon::Link)) open_digikey(r.pn);
                    ImGui::Separator();
                    if (menu_item_icon("Find next untaught", ui::Icon::FindRef, "F3")) find_next_untaught();
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        ImGui::EndTable();
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
