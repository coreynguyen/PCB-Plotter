// ============================================================================
// file_assoc_dialog.cpp — Windows shell integration UI (see specs/WIN_SHELL.md):
//   * Edit > File Associations... dialog (register / unregister per type group,
//     per-user or all users, status, "Open Default Apps")
//   * command line: --register [types] [--all-users] [--replace] [--quiet],
//     --unregister [--all-users] [--quiet]
//   * import_auto(path): "Import into PCB Plotter" / --import on archives and
//     folders — content detection (core/content_detect) + the matching importer
// ============================================================================
#include "file_editors.h"
#include "gapp.h"
#include "../core/content_detect.h"
#include "../platform/win_shell.h"
#include "../ui/form.h"
#include "imgui.h"
#include "../core/fileio.h"
#include <algorithm>
#include <filesystem>

namespace pcbp::app { extern bool g_quiet_import; }

namespace pcbp::gui {

bool gerber_dialog_drop(const std::vector<std::wstring>&);

using namespace pcbp::platform;
using ui::U;
using ui::W;

namespace {

struct GroupRow { unsigned g; const wchar_t* label; };
const GroupRow kRows[] = {
    {TG_Project, L"PCB Plotter projects (.pcbp)"},
    {TG_Legacy, L"Legacy CAM projects (.cpf) / libraries (.mdb)"},
    {TG_ViewMate, L"ViewMate jobs (.bin, Open With only)"},
    {TG_Gerber, L"Gerber / Excellon (.gbr .gtl .gbl ... .drl .xln)"},
    {TG_Odb, L"ODB++ archives (.tgz, Open With only)"},
    {TG_Ipc, L"IPC-2581 (.cvg; .xml Open With only)"},
    {TG_Pnp, L"Pick-and-place programs (.h8h .h7h .h5h .h4h; .iss)"},
    {TG_Sheet, L"Spreadsheets (.xlsx .xls .ods .csv, Open With only)"},
    {TG_Import, L"\"Import into PCB Plotter\" on .zip / .tgz / .tar / .gz and folders"},
};

std::wstring group_names(unsigned m) {
    if (!m) return L"nothing";
    std::wstring s = groups_to_string(m);
    return s;
}

void cli_msg(bool quiet, const std::wstring& text, bool error) {
    if (quiet) return;
    MessageBoxW(nullptr, text.c_str(), L"PCB Plotter", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

// sniff hook: ask the plugin registry about files the built-in rules do not know (GenCAD, KiCad, ...)
int registry_sniff(const std::string& name, const std::string& head, std::string& id, detect::Kind& kind) {
    int best = 0;
    for (auto& im : PluginRegistry::get().importers()) {
        const std::string& iid = im->info().id;
        if (iid == "centroid" || iid == "bom" || iid == "gerber" || iid == "excellon" || iid == "pcbp.json") continue;
        const int s = im->sniff(name, head);
        if (s > best) { best = s; id = iid; }
    }
    kind = detect::Kind::Cad;
    return best;
}

std::wstring materialize_one(const detect::Result& r, const std::string& vpath, std::wstring& tmp, std::string& err) {
    const detect::Entry* e = r.find(vpath);
    if (e && !e->disk_path.empty()) return W(e->disk_path);
    if (tmp.empty()) tmp = import_temp_dir(W(file_stem(r.source)));
    std::vector<std::string> w;
    if (!detect::materialize(r, vpath, U(tmp), w, err) || w.empty()) return {};
    return W(w.front());
}

void ensure_doc() { if (!app::active_doc()) app::new_document(); }

void run_text_import(const detect::Result& r, const std::string& vpath, bool bom) {
    const detect::Entry* e = r.find(vpath);
    std::string bytes, err;
    if (!e || !detect::entry_bytes(*e, bytes, err)) { toast("Could not read " + vpath, ToastKind::Error, 6.f); return; }
    ensure_doc();
    import_from_bytes(std::move(bytes), -1, bom, W(vpath));
}

bool load_gerbers(const detect::Result& r, const std::vector<std::string>& files, const std::vector<std::string>& aps) {
    // only the layers + aperture lists, in a clean folder, so the dialog lists nothing else
    std::wstring tmp = import_temp_dir(W(file_stem(r.source)) + L"-gerber");
    std::string err;
    std::vector<std::string> all = files;
    all.insert(all.end(), aps.begin(), aps.end());
    std::string common = all.empty() ? std::string() : all.front().substr(0, all.front().find_last_of('/') == std::string::npos ? 0 : all.front().find_last_of('/'));
    for (auto& f : all) while (!common.empty() && f.compare(0, common.size() + 1, common + "/") != 0) {
        size_t s = common.find_last_of('/');
        common = s == std::string::npos ? std::string() : common.substr(0, s);
    }
    for (auto& f : all) {
        const detect::Entry* e = r.find(f);
        std::string bytes;
        if (!e || !detect::entry_bytes(*e, bytes, err)) continue;
        const std::string rel = common.empty() ? f : f.substr(common.size() + 1);
        const std::string dst = detect::materialized_path("", U(tmp), rel);
        std::filesystem::create_directories(std::filesystem::path(W(dst)).parent_path());
        write_file(dst, bytes.data(), bytes.size(), err);
    }
    ensure_doc();
    if (!gerber_dialog_drop({tmp})) { toast("No Gerber layers could be loaded from " + r.source, ToastKind::Error, 6.f); return false; }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// --import <path> / Explorer verb: detect and run the best import plan
// ---------------------------------------------------------------------------
bool import_auto(const std::wstring& path) {
    detect::Result r;
    std::string err;
    detect::Options opt;
    opt.sniff = registry_sniff;
    bool ok = false;
    const std::string src = U(path);
    run_with_progress_wait("Scanning", [&](Progress& p) { p.text(src.c_str()); ok = detect::scan(src, r, err, opt); }, ProgressOpts{true, false, 0.2f});
    if (!ok) { toast("Could not read " + src + ": " + err, ToastKind::Error, 8.f); return false; }
    if (r.plans.empty()) {
        ui::warning(app::frame_hwnd(), L"Import", L"Nothing PCB Plotter can import was found in\n" + path + L"\n\n" + W(r.summary()));
        return false;
    }
    size_t pick = 0;
    // ambiguous: several plans of similar confidence -> ask
    if (r.plans.size() > 1 && r.plans[1].score + 8 >= r.plans[0].score && r.plans[1].action != detect::Action::None) {
        std::vector<std::wstring> ch;
        for (size_t i = 0; i < r.plans.size() && i < 8; ++i) ch.push_back(W(std::string(detect::action_name(r.plans[i].action)) + ": " + r.plans[i].title));
        int c = ui::prompt_choice(app::frame_hwnd(), L"Import " + W(file_stem(src)), L"Several importable data sets were found. Import:", ch, 0);
        if (c < 0) return false;
        pick = (size_t)c;
    }
    const detect::Plan p = r.plans[pick];
    std::wstring tmp;
    switch (p.action) {
    case detect::Action::OpenProject:
    case detect::Action::ImportFile: {
        std::wstring f = materialize_one(r, p.files[0], tmp, err);
        if (f.empty()) { toast("Could not extract " + p.files[0] + ": " + err, ToastKind::Error, 8.f); return false; }
        if (!app::open_document(f)) return false;
        break;
    }
    case detect::Action::ImportOdb: {
        std::wstring dir;
        const std::string root = p.files[0];
        bool on_disk = r.source_is_dir;
        for (auto& e : r.entries) if ((root.empty() || e.vpath.rfind(root + "/", 0) == 0) && e.disk_path.empty()) on_disk = false;
        if (on_disk) dir = W(root.empty() ? src : src + "/" + root);
        else {
            tmp = import_temp_dir(W(file_stem(src)) + L"-odb");
            std::vector<std::string> w;
            if (!detect::materialize(r, root, U(tmp), w, err)) { toast("Could not extract the ODB++ job: " + err, ToastKind::Error, 8.f); return false; }
            dir = tmp;
        }
        if (!app::open_document(dir)) return false;
        break;
    }
    case detect::Action::LoadGerbers:
        if (!load_gerbers(r, p.files, p.aperture_lists)) return false;
        break;
    case detect::Action::ImportCentroid:
        run_text_import(r, p.files[0], false);
        break;
    case detect::Action::ImportBom:
        run_text_import(r, p.files[0], true);
        break;
    case detect::Action::OpenSheet: {
        std::wstring f = materialize_one(r, p.files[0], tmp, err);
        if (f.empty() || !files_open(f, 1)) return false;
        break;
    }
    case detect::Action::Unsupported:
        ui::warning(app::frame_hwnd(), L"Import", W(std::string(detect::kind_name(p.kind)) + " '" + p.files[0] +
                    "' was recognised, but PCB Plotter cannot read this format yet.\nExport the layers as Gerber / ODB++ from the original application and import those."));
        return false;
    default: return false;
    }
    // follow-ups found next to the main data (offered, never forced)
    if (p.action != detect::Action::LoadGerbers && !p.gerbers.empty()) {
        auto rr = std::make_shared<detect::Result>(std::move(r));
        auto g = p.gerbers; auto a = p.aperture_lists;
        toast(std::to_string(g.size()) + " Gerber / drill layer(s) were found next to it", ToastKind::Info, 12.f, "Load layers",
              [rr, g, a]() { load_gerbers(*rr, g, a); });
        return true;
    }
    if ((p.action == detect::Action::ImportCentroid || p.action == detect::Action::LoadGerbers || p.action == detect::Action::ImportFile ||
         p.action == detect::Action::ImportOdb) && !p.bom.empty()) {
        auto rr = std::make_shared<detect::Result>(std::move(r));
        std::string b = p.bom.front();
        toast("A BOM was found: " + b, ToastKind::Info, 12.f, "Import BOM", [rr, b]() { run_text_import(*rr, b, true); });
    } else if (p.action == detect::Action::LoadGerbers && !p.centroid.empty()) {
        auto rr = std::make_shared<detect::Result>(std::move(r));
        std::string c = p.centroid.front();
        toast("A centroid file was found: " + c, ToastKind::Info, 12.f, "Import centroid", [rr, c]() { run_text_import(*rr, c, false); });
    }
    return true;
}

// ---------------------------------------------------------------------------
// Command line (headless; before any window exists). Returns -1 when args are not ours.
// ---------------------------------------------------------------------------
int assoc_cli(const std::vector<std::wstring>& args) {
    bool reg = false, unreg = false, all = false, quiet = false, replace = false;
    std::wstring types;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--register") { reg = true; if (i + 1 < args.size() && args[i + 1][0] != L'-') types = args[++i]; }
        else if (a == L"--unregister") unreg = true;
        else if (a == L"--all-users") all = true;
        else if (a == L"--quiet") quiet = true;
        else if (a == L"--replace") replace = true;
    }
    if (!reg && !unreg) return -1;
    unsigned groups = TG_Default;
    if (!types.empty()) {
        std::wstring bad;
        groups = parse_groups(types, &bad);
        if (!groups) {
            cli_msg(quiet, L"Unknown file type group '" + bad + L"'.\nUse: project, legacy, viewmate, gerber, odb, ipc, pnp, sheet, import, default, all", true);
            return 2;
        }
    }
    if (all && !is_elevated()) {
        std::wstring cmd = reg ? L"--register " + groups_to_string(groups) : L"--unregister";
        cmd += L" --all-users --quiet";
        if (replace) cmd += L" --replace";
        unsigned long code = 1;
        std::wstring err;
        if (!relaunch_elevated(cmd, code, err)) { cli_msg(quiet, err, true); return 3; }
        cli_msg(quiet, code == 0 ? L"Done." : L"The elevated registration failed (exit code " + std::to_wstring(code) + L").", code != 0);
        return (int)code;
    }
    std::wstring err;
    bool ok = true;
    if (unreg || replace) ok = unregister_types(all, err);
    if (ok && reg) ok = register_types(groups, all, err);
    if (!ok) { cli_msg(quiet, L"File association update failed:\n" + err, true); return 1; }
    cli_msg(quiet, reg ? L"Registered " + group_names(groups) + (all ? L" for all users." : L" for the current user.") +
                         L"\n\nWindows 8 and later never let an application make itself the default: choose PCB Plotter in "
                         L"Settings > Apps > Default apps, or right-click a file > Open with > Choose another app > Always."
                       : std::wstring(L"File associations removed."), false);
    return 0;
}

// ---------------------------------------------------------------------------
// Edit > File Associations...
// ---------------------------------------------------------------------------
void file_assoc_dialog() {
    struct State { AssocStatus st; std::wstring msg; bool err = false; };
    auto S = std::make_shared<State>();
    S->st = query_status();
    const unsigned have = S->st.user_groups | S->st.machine_groups;
    const unsigned init = have ? have : TG_Default;

    ui::Form f(L"File Associations", 560);
    f.header(L"File Associations").icon((int)ui::Icon::Link)
        .subtitle(L"Explorer icons, Open With and right-click import for your board files");
    f.custom("status", [S](ui::Form&, float) {
        const GTheme& T = th();
        auto line = [&](const char* k, const std::string& v, ImU32 c) {
            ImGui::PushStyleColor(ImGuiCol_Text, T.text_dim); ImGui::TextUnformatted(k); ImGui::PopStyleColor();
            ImGui::SameLine(gui::S(130)); ImGui::PushStyleColor(ImGuiCol_Text, c); ImGui::TextWrapped("%s", v.c_str()); ImGui::PopStyleColor();
        };
        line("Current user", S->st.user_groups ? U(group_names(S->st.user_groups)) : "not registered", S->st.user_groups ? T.ok : T.text_faint);
        line("All users", S->st.machine_groups ? U(group_names(S->st.machine_groups)) : "not registered", S->st.machine_groups ? T.ok : T.text_faint);
        if (S->st.stale) line("Warning", "registered for another copy: " + U(S->st.user_exe.empty() ? S->st.machine_exe : S->st.user_exe) + " - Register again to update", T.warn);
        std::string d;
        for (auto& [e, a] : S->st.defaults) d += U(e) + " \xE2\x86\x92 " + U(a) + "    ";
        line("Opens with", d, T.text);
        if (!S->msg.empty()) line(S->err ? "Error" : "Result", U(S->msg), S->err ? T.error : T.ok);
    });
    f.separator();
    // R10 §5: two-column checkbox grid + one button row; the dialog stays inside the work area (gform clamp)
    struct Sel { bool on[sizeof(kRows) / sizeof(kRows[0])]; int scope; };
    auto sel = std::make_shared<Sel>();
    for (size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); ++i) sel->on[i] = (init & kRows[i].g) != 0;
    sel->scope = S->st.machine_groups && !S->st.user_groups ? 1 : 0;
    auto mask = [sel]() { unsigned m = 0; for (size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); ++i) if (sel->on[i]) m |= kRows[i].g; return m; };
    auto apply = [S, sel, mask](ui::Form& F, bool reg) {
        const bool all = sel->scope == 1;
        const unsigned m = mask();
        F.later([S, all, m, reg]() {
            std::wstring err;
            bool ok;
            if (all && !is_elevated()) {
                unsigned long code = 1;
                const std::wstring cmd = reg ? L"--register " + groups_to_string(m) + L" --all-users --replace --quiet" : L"--unregister --all-users --quiet";
                ok = (reg && !m) ? false : relaunch_elevated(cmd, code, err) && code == 0;
                if (!ok && err.empty()) err = reg && !m ? L"Select at least one type." : L"elevated helper failed (exit code " + std::to_wstring(code) + L")";
            } else {
                ok = unregister_types(all, err);
                if (ok && reg) { if (m) ok = register_types(m, all, err); else { ok = false; err = L"Select at least one type."; } }
            }
            S->st = query_status();
            S->err = !ok;
            S->msg = ok ? (reg ? L"Registered: " + group_names(m) : L"Removed all PCB Plotter file associations") : err;
            toast(U(S->msg), ok ? ToastKind::Success : ToastKind::Error, 5.f);
        });
    };
    f.custom("types", [sel](ui::Form&, float w) {
        const int n = (int)(sizeof(kRows) / sizeof(kRows[0]));
        // two columns only when every label fits its column (checkbox labels do not wrap)
        float need = 0;
        for (int i = 0; i < n; ++i) need = std::max(need, ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(U(kRows[i].label).c_str()).x);
        const bool two = w > gui::S(520) && need + gui::S(16) <= w * 0.5f;
        const float colw = two ? w * 0.5f : w;
        for (int i = 0; i < n; ++i) {
            ImGui::PushID(i);
            if (two && (i & 1)) ImGui::SameLine(colw + ImGui::GetStyle().WindowPadding.x);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + colw - gui::S(12));
            ImGui::Checkbox(U(kRows[i].label).c_str(), &sel->on[i]);
            ImGui::PopTextWrapPos();
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Register for");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::min(w - gui::S(120), gui::S(380)));
        const char* sc[] = {"Current user (no administrator rights needed)", "All users (asks for administrator rights)"};
        ImGui::Combo("##scope", &sel->scope, sc, 2);
    });
    f.label(L"PCB Plotter never takes over a file type that another program owns: it is added to \u201COpen with\u201D and to "
            L"Settings \u203A Default apps. Since Windows 8 only you can pick the default app (Windows protects that choice "
            L"with a per-user hash), so use \u201COpen Default Apps\u201D to choose PCB Plotter for .gbr, .xlsx \u2026 "
            L"Image formats (.tga, .bmp, .png ...) are never associated.").wide();
    f.custom("btns", [apply](ui::Form& F, float) {
        if (gui::button("Register", ui::Icon::Check, true)) apply(F, true);
        tooltip("Register the selected file types for PCB Plotter (Explorer icons, Open With, Import verb)");
        ImGui::SameLine();
        if (gui::button("Unregister", ui::Icon::Delete)) apply(F, false);
        tooltip("Remove every PCB Plotter file association for the chosen scope");
        ImGui::SameLine();
        if (gui::button("Open Default Apps\xE2\x80\xA6", ui::Icon::Link))
            F.later([]() { if (!open_default_apps(app::frame_hwnd())) toast("Could not open the Default Apps settings", ToastKind::Error); });
        tooltip("Windows Settings \xE2\x80\xBA Apps \xE2\x80\xBA Default apps");
    });
    f.ok_text(L"Close").no_cancel();
    f.run(app::frame_hwnd());
}

} // namespace pcbp::gui
