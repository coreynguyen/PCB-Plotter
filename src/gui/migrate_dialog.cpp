// ============================================================================
// migrate_dialog.cpp — File > Migrate Legacy Projects (batch) + "--migrate" CLI.
//
// Options form -> scan (worker) -> confirmation with counts -> batch run on a
// worker with per-file progress / cancel (core/migrate) -> results table
// (status, counts, output, messages) with CSV export and "open converted".
// Deleting originals happens only inside migrate::run, after a verified
// conversion and (when archiving) a verified archive; Recycle Bin by default.
// ============================================================================
#include "../io/recentre.h"
#include "gapp.h"
#include "../core/migrate.h"
#include "../core/fileio.h"
#include "../ui/form.h"

#include <cstdio>
#include <shellapi.h>

namespace pcbp::gui {

using namespace pcbp::app;
namespace mg = pcbp::migrate;

namespace {

struct Remembered {                      // dialog state for the session
    std::vector<std::wstring> inputs;
    bool recursive = true, cpf = true, bin = false, recycle = true, dry = false;
    int output = 0, collision = 0, originals = 0;
    std::wstring out_dir, archive;
};
Remembered g_m;

std::vector<std::vector<std::wstring>> input_rows() {
    std::vector<std::vector<std::wstring>> r;
    for (auto& p : g_m.inputs) {
        DWORD a = GetFileAttributesW(p.c_str());
        const bool dir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        r.push_back({dir ? L"Folder" : L"File", p});
    }
    return r;
}

void add_input(const std::wstring& p) {
    for (auto& q : g_m.inputs) if (!lstrcmpiW(q.c_str(), p.c_str())) return;
    g_m.inputs.push_back(p);
}

std::string fmt_size(uint64_t b) {
    char s[32];
    if (b >= (1ull << 30)) std::snprintf(s, sizeof s, "%.2f GB", b / double(1ull << 30));
    else if (b >= (1ull << 20)) std::snprintf(s, sizeof s, "%.1f MB", b / double(1ull << 20));
    else std::snprintf(s, sizeof s, "%.0f KB", b / 1024.0);
    return s;
}

ImU32 status_col(mg::Status s) {
    const GTheme& t = th();
    switch (s) {
    case mg::Status::Converted: return t.ok;
    case mg::Status::DryRun: return t.accent;
    case mg::Status::Skipped: case mg::Status::Cancelled: return t.warn;
    case mg::Status::Failed: return t.error;
    default: return t.text_dim;
    }
}

void show_results(std::vector<mg::Item>& items, const mg::Summary& s, const mg::Options& o) {
    int sel = -1;
    ui::Form f(L"Migration Results", 980);
    f.icon((int)ui::Icon::Wand);
    f.header(ui::W(s.describe()));
    if (o.dry_run) f.subtitle(L"Dry run: files were imported and checked; nothing was written, archived or deleted.");
    else if (s.archive_attempted && !s.archive_ok) f.subtitle(L"The archive could not be written or verified, so no originals were deleted.");
    f.custom("results", [&](ui::Form&, float w) {
        const GTheme& t = th();
        const float h = S(360);
        if (!ImGui::BeginTable("##migres", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                                 ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit,
                               ImVec2(w, h)))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Status", 0, S(78));
        ImGui::TableSetupColumn("Source", 0, S(260));
        ImGui::TableSetupColumn("Comps", 0, S(52));
        ImGui::TableSetupColumn("Parts", 0, S(48));
        ImGui::TableSetupColumn("Pkgs", 0, S(44));
        ImGui::TableSetupColumn("Layers", 0, S(48));
        ImGui::TableSetupColumn("Output", 0, S(240));
        ImGui::TableSetupColumn("Message", 0, S(320));
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)items.size(); ++i) {
            const mg::Item& it = items[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            const std::string st = mg::status_name(it.status) + (it.archived ? " +zip" : "") + (it.deleted ? " +del" : "");
            ImGui::PushStyleColor(ImGuiCol_Text, status_col(it.status));
            if (ImGui::Selectable(st.c_str(), sel == i, ImGuiSelectableFlags_SpanAllColumns)) sel = i;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && (!it.warnings.empty() || !it.error.empty())) {
                std::string tip = it.src;
                if (!it.error.empty()) tip += "\n\nError: " + it.error;
                for (size_t k = 0; k < it.warnings.size() && k < 25; ++k) tip += "\n" + it.warnings[k];
                tooltip(tip.c_str());
            }
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(it.rel.c_str());
            auto num = [](int v) { ImGui::Text("%d", v); };
            ImGui::TableSetColumnIndex(2); num(it.comps);
            ImGui::TableSetColumnIndex(3); num(it.parts);
            ImGui::TableSetColumnIndex(4); num(it.packages);
            ImGui::TableSetColumnIndex(5); num(it.layers);
            ImGui::TableSetColumnIndex(6);
            if (it.status == mg::Status::Converted || it.status == mg::Status::DryRun) ImGui::TextUnformatted(it.out.c_str());
            ImGui::TableSetColumnIndex(7);
            if (!it.error.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, t.error); ImGui::TextUnformatted(it.error.c_str()); ImGui::PopStyleColor(); }
            else if (!it.warnings.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim); ImGui::Text("%zu message(s)", it.warnings.size()); ImGui::PopStyleColor(); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }, 372);
    f.extra_button(L"Export log CSV\x2026", [&](ui::Form&) {
        std::wstring p = L"migration_log.csv";
        if (!ui::save_file_dialog(frame_hwnd(), L"Export Migration Log", {{L"CSV (*.csv)", L"*.csv"}}, L"csv", p)) return false;
        std::string csv = mg::results_csv(items), err;
        if (!write_file(ui::U(p), csv.data(), csv.size(), err)) ui::warning(frame_hwnd(), L"Export Migration Log", ui::W(err));
        else toast("Log written: " + ui::U(p), ToastKind::Success);
        return false;
    });
    f.extra_button(L"Details\x2026", [&](ui::Form&) {
        std::string t;
        for (auto& it : items) {
            t += "[" + mg::status_name(it.status) + "] " + it.src + "\n";
            if (!it.out.empty() && (it.status == mg::Status::Converted || it.status == mg::Status::DryRun)) t += "    -> " + it.out + "\n";
            if (!it.error.empty()) t += "    ERROR: " + it.error + "\n";
            for (auto& w : it.warnings) t += "    " + w + "\n";
        }
        ui::show_text(frame_hwnd(), L"Migration Details", ui::W(t));
        return false;
    });
    f.extra_button(L"Open selected", [&](ui::Form& ff) {
        if (sel < 0 || sel >= (int)items.size() || items[sel].status != mg::Status::Converted) {
            ui::message(frame_hwnd(), L"Migration Results", L"Select a converted project first.");
            return false;
        }
        const std::wstring p = ui::W(items[sel].out);
        ff.close(true);
        defer([p] { open_document(p); });
        return false;
    });
    f.ok_text(L"Close");
    f.no_cancel();
    f.run(frame_hwnd());
}

} // namespace

void show_migrate_dialog() {
    HWND h = frame_hwnd();
    ui::Form f(L"Migrate Legacy Projects", 720);
    f.icon((int)ui::Icon::Wand);
    f.header(L"Convert .cpf projects and ViewMate .bin jobs to .pcbp");
    f.list("inputs", {L"Kind", L"Path"}, input_rows(), 6);
    f.button("add_files", L"Add files\x2026", [&](ui::Form& ff) {
        std::wstring p; std::vector<std::wstring> many;
        if (!ui::open_file_dialog(h, L"Add Legacy Projects", {{L"Legacy projects / ViewMate jobs", L"*.cpf;*.bin"}, {L"All files", L"*.*"}}, p, true, &many)) return;
        if (many.empty() && !p.empty()) many.push_back(p);
        for (auto& q : many) add_input(q);
        ff.set_list("inputs", input_rows());
    });
    f.button("add_dir", L"Add folder\x2026", [&](ui::Form& ff) {
        std::wstring p;
        if (!ui::pick_folder_dialog(h, L"Add Folder", p)) return;
        add_input(p);
        ff.set_list("inputs", input_rows());
    });
    f.button("remove", L"Remove selected", [&](ui::Form& ff) {
        int i = ff.list_selection("inputs");
        if (i >= 0 && i < (int)g_m.inputs.size()) g_m.inputs.erase(g_m.inputs.begin() + i);
        ff.set_list("inputs", input_rows());
    });
    f.separator();
    f.check("recursive", L"Include subfolders", g_m.recursive);
    f.check("cpf", L"Legacy CAM projects (*.cpf)", g_m.cpf);
    f.check("bin", L"ViewMate jobs (*.bin, signature checked)", g_m.bin);
    f.combo("output", L"Write .pcbp files", {L"Next to each source file", L"Into a folder (mirror the source tree)"}, g_m.output == 1 ? L"Into a folder (mirror the source tree)" : L"Next to each source file");
    f.folder("out_dir", L"Output folder", g_m.out_dir);
    f.combo("collision", L"If the .pcbp already exists", {L"Rename (keep both)", L"Skip", L"Overwrite"},
            g_m.collision == 1 ? L"Skip" : g_m.collision == 2 ? L"Overwrite" : L"Rename (keep both)");
    static const std::vector<std::wstring> orig = {L"Keep", L"Archive into a .zip", L"Archive into a .zip, then delete", L"Delete"};
    f.combo("originals", L"Originals afterwards", orig, orig[std::clamp(g_m.originals, 0, 3)]);
    f.file("archive", L"Archive (.zip)", g_m.archive, {{L"Zip archive (*.zip)", L"*.zip"}}, true);
    f.check("recycle", L"Delete to the Recycle Bin", g_m.recycle);
    f.check("dry", L"Dry run (import and check only; write nothing)", g_m.dry);
    auto refresh = [](ui::Form& ff, const std::string&) {
        ff.enable("out_dir", ff.index("output") == 1);
        const int og = ff.index("originals");
        ff.enable("archive", og == 1 || og == 2);
        ff.enable("recycle", og >= 2);
    };
    for (const char* k : {"output", "originals"}) if (ui::Field* fl = f.field(k)) fl->on_change = refresh;
    f.validate([&](ui::Form& ff, std::wstring& err) {
        if (g_m.inputs.empty()) { err = L"Add at least one file or folder."; return false; }
        if (!ff.checked("cpf") && !ff.checked("bin")) { err = L"Choose at least one file type."; return false; }
        if (ff.index("output") == 1 && ff.str("out_dir").empty()) { err = L"Choose the output folder."; return false; }
        const int og = ff.index("originals");
        if ((og == 1 || og == 2) && ff.str("archive").empty()) { err = L"Choose the archive file."; return false; }
        return true;
    });
    f.ok_text(L"Scan\x2026");
    {
        // initial enable state
        ui::Field* a = f.field("out_dir"); if (a) a->enabled = g_m.output == 1;
        ui::Field* b = f.field("archive"); if (b) b->enabled = g_m.originals == 1 || g_m.originals == 2;
        ui::Field* c = f.field("recycle"); if (c) c->enabled = g_m.originals >= 2;
    }
    if (!f.run(h)) return;
    g_m.recursive = f.checked("recursive"); g_m.cpf = f.checked("cpf"); g_m.bin = f.checked("bin");
    g_m.output = f.index("output"); g_m.out_dir = f.str("out_dir"); g_m.collision = f.index("collision");
    g_m.originals = f.index("originals"); g_m.archive = f.str("archive"); g_m.recycle = f.checked("recycle"); g_m.dry = f.checked("dry");

    mg::Options o;
    o.recursive = g_m.recursive; o.include_cpf = g_m.cpf; o.include_bin = g_m.bin;
    o.output = g_m.output == 1 ? mg::OutputMode::MirrorTree : mg::OutputMode::NextToSource;
    o.out_dir = ui::U(g_m.out_dir);
    o.collision = g_m.collision == 1 ? mg::Collision::Skip : g_m.collision == 2 ? mg::Collision::Overwrite : mg::Collision::Rename;
    o.archive = g_m.originals == 1 || g_m.originals == 2;
    o.archive_path = ui::U(g_m.archive);
    if (o.archive && file_ext_lower(o.archive_path) != ".zip") o.archive_path += ".zip";
    o.delete_originals = g_m.originals >= 2;
    o.recycle = g_m.recycle;
    o.dry_run = g_m.dry;

    std::vector<std::string> inputs;
    for (auto& p : g_m.inputs) inputs.push_back(ui::U(p));
    std::vector<mg::Item> items;
    std::vector<std::string> errors;
    if (!run_with_progress_wait("Scanning for legacy projects", [&](Progress& p) { p.text("Listing files..."); items = mg::collect(inputs, o, errors); },
                                ProgressOpts{true, false, 0.2f}))
        return;
    if (items.empty()) {
        std::wstring t = L"No matching files were found.";
        for (auto& e : errors) t += L"\n" + ui::W(e);
        ui::message(h, L"Migrate Legacy Projects", t);
        return;
    }
    int ncpf = 0, nbin = 0;
    uint64_t bytes = 0;
    for (auto& it : items) { (file_ext_lower(it.src) == ".bin" ? nbin : ncpf)++; bytes += it.size; }
    std::string text = std::to_string(items.size()) + " file(s) found (" + fmt_size(bytes) + "): " + std::to_string(ncpf) + " .cpf, " +
                       std::to_string(nbin) + " .bin.\n\n";
    if (o.dry_run) text += "Dry run: every file is imported and checked; nothing is written.\n";
    else {
        text += o.output == mg::OutputMode::MirrorTree ? "Output: " + o.out_dir + " (mirrored folder tree)\n" : "Output: next to each source file\n";
        text += std::string("Existing .pcbp: ") + (o.collision == mg::Collision::Skip ? "skip" : o.collision == mg::Collision::Overwrite ? "OVERWRITE" : "rename") + "\n";
        if (o.archive) text += "Originals are archived into " + o.archive_path + " (verified by re-reading every entry).\n";
        if (o.delete_originals)
            text += "Originals are DELETED" + std::string(o.recycle ? " (to the Recycle Bin)" : " PERMANENTLY") +
                    " — only after their conversion was verified" + (o.archive ? " and the archive was verified" : "") + ".\n";
        else if (!o.archive) text += "Originals are kept.\n";
    }
    for (size_t i = 0; i < errors.size() && i < 8; ++i) text += "\nNote: " + errors[i];
    if (!confirm("Migrate Legacy Projects", text, o.dry_run ? "Check" : o.delete_originals ? "Convert and delete" : "Convert", o.delete_originals))
        return;

    mg::Summary sum;
    bool finished = run_with_progress_wait("Migrating projects", [&](Progress& p) {
        sum = mg::run(items, o, [&](int i, int n, const std::string& msg, float frac) {
            char b[64]; std::snprintf(b, sizeof b, " (%d/%d)", i + 1, n);
            p.set(frac, (msg + b).c_str());
            return !p.cancelled();
        });
    }, ProgressOpts{true, true, 0.f});
    if (!finished) {   // cancelled: items keep their per-file status
        for (auto& it : items) if (it.status == mg::Status::Pending) { it.status = mg::Status::Cancelled; ++sum.cancelled; }
    }
    toast(sum.describe(), sum.failed || (sum.archive_attempted && !sum.archive_ok) ? ToastKind::Warning : ToastKind::Success, 6.f);
    show_results(items, sum, o);
}

// "PCBPlotter.exe --migrate ..." — runs without the UI. Output goes to the parent console when there is one.
// where a headless command's report goes: the redirected file / pipe, the console it was started from, else a box
static void cli_show(const std::string& out, int rc, const wchar_t* title) {
    HANDLE hs = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD ft = (hs && hs != INVALID_HANDLE_VALUE) ? GetFileType(hs) : FILE_TYPE_UNKNOWN;
    if (ft == FILE_TYPE_DISK || ft == FILE_TYPE_PIPE) {   // redirected: "PCBPlotter --migrate ... > report.txt"
        DWORD n = 0;
        WriteFile(hs, out.data(), (DWORD)out.size(), &n, nullptr);
    } else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
        std::wstring w = L"\r\n" + ui::W(out);
        DWORD n = 0;
        if (!WriteConsoleW(ho, w.c_str(), (DWORD)w.size(), &n, nullptr)) {
            std::string u8 = "\r\n" + out;   // redirected to a file / pipe
            WriteFile(ho, u8.data(), (DWORD)u8.size(), &n, nullptr);
        }
        FreeConsole();
    } else {
        MessageBoxW(nullptr, ui::W(out.size() > 6000 ? out.substr(0, 6000) + "\n..." : out).c_str(), title,
                    MB_OK | (rc ? MB_ICONWARNING : MB_ICONINFORMATION));
    }
}

int migrate_cli_main(const std::vector<std::wstring>& wargs) {
    auto& reg = PluginRegistry::get();
    if (reg.importers().empty()) {   // the CLI may run before the shell registered anything
        register_builtin_plugins(reg);
        load_plugins_dir(reg, exe_dir() + L"\\plugins");
    }
    std::vector<std::string> args;
    for (auto& a : wargs) args.push_back(ui::U(a));
    std::string out;
    const int rc = mg::cli_main(args, out);
    cli_show(out, rc, L"PCB Plotter - Migrate");
    return rc;
}

// R25: PCBPlotter --recentre <program> ... --project <file.pcbp>   (io/recentre.h)
int recentre_cli_main(const std::vector<std::wstring>& wargs) {
    std::vector<std::string> args;
    for (auto& a : wargs) args.push_back(ui::U(a));
    std::string out;
    const int rc = io::recentre_cli(args, out);
    cli_show(out, rc, L"PCB Plotter - Recentre");
    return rc;
}

} // namespace pcbp::gui
