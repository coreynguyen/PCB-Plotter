// gmain.cpp — PCB Plotter entry point (v2 ImGui shell)
#include "gapp.h"
#include "../ui/theme.h"
#include "../platform/win_shell.h"
#include "../core/content_detect.h"
#include <filesystem>
#include <fstream>

namespace pcbp::gui { int run_app(int show); int migrate_cli_main(const std::vector<std::wstring>& args); int recentre_cli_main(const std::vector<std::wstring>& args); }
namespace pcbp::app { extern bool g_quiet_import; }
using namespace pcbp;
namespace pcbp::gui { bool import_auto(const std::wstring& path); int assoc_cli(const std::vector<std::wstring>& args); bool gerber_dialog_drop(const std::vector<std::wstring>&); }

static bool is_container(const std::wstring& f) {   // archives go through content detection (nested jobs / layer sets)
    std::string e = file_ext_lower(wide_to_utf8(f));
    return e == ".zip" || e == ".tgz" || e == ".gz" || e == ".tar";
}

// command line: files / --import f / --cmd id (automation). Also runs for arguments forwarded
// by a second instance (Explorer multi-select, "Import into PCB Plotter").
static void process_args(const std::vector<std::wstring>& args) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--new") app::new_document();
        else if (a == L"--import" && i + 1 < args.size()) {
            std::wstring f = args[++i];
            auto d = app::active_doc();
            const DWORD at = GetFileAttributesW(f.c_str());
            Importer* im = (at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY) && !is_container(f)) ? PluginRegistry::get().detect(wide_to_utf8(f)) : nullptr;
            if (!im) gui::import_auto(f);   // folder / archive / unclaimed file: content detection
            else {
                app::g_quiet_import = true;
                if (d) app::run_importer(app::frame_hwnd(), d, im, f); else app::open_document(f);
                app::g_quiet_import = false;
            }
        } else if (a == L"--cmd" && i + 1 < args.size()) gui::post_command(_wtoi(args[++i].c_str()));
        else if (a == L"--view" && i + 1 < args.size()) { int k = _wtoi(args[++i].c_str()); if (auto d = app::active_doc()) gui::activate_view(d, (gui::ViewKind)k); }
        else if (!a.empty() && a[0] != L'-') {
            // R16d: a BOM / centroid table (command line, double-click, "Open with") takes the same content sniff as
            // drag and drop (shell.cpp open_paths): straight into the matching wizard, no "Open in Spreadsheet" prompt.
            if (app::is_dropped_table(a)) {
                auto d = app::active_doc();
                if (!d) { d = app::new_document(); gui::activate_view(d, gui::ViewKind::Plotter); }
                if (d && app::route_dropped_table(app::frame_hwnd(), *d, a)) continue;
            }
            app::g_quiet_import = true; app::open_document(a); app::g_quiet_import = false;
        }
    }
}
// Explorer starts one process per selected file; the extra processes forward their file to this
// one. Collect what arrives within a short quiet period and handle it as one batch, so selecting
// eight Gerber layers opens ONE "Load Gerber Layers" dialog instead of eight documents.
static std::vector<std::wstring> g_batch;
static DWORD g_batch_t = 0;
static bool g_batch_sched = false;
static bool all_gerber(const std::vector<std::wstring>& a) {
    if (a.size() < 2) return false;
    for (auto& f : a) {
        if (f.empty() || f[0] == L'-') return false;
        std::ifstream in(std::filesystem::path(f), std::ios::binary);
        std::string head(65536, '\0');
        in.read(head.data(), (std::streamsize)head.size());
        head.resize((size_t)in.gcount());
        int sc = 0; std::string id;
        const detect::Kind k = detect::classify(wide_to_utf8(f), head, sc, id);
        if (k != detect::Kind::Gerber && k != detect::Kind::Drill && k != detect::Kind::ApertureList) return false;
    }
    return true;
}
static void flush_batch() {
    if (GetTickCount() - g_batch_t < 450) { Sleep(15); gui::defer(flush_batch); return; }
    g_batch_sched = false;
    std::vector<std::wstring> a = std::move(g_batch);
    g_batch.clear();
    if (all_gerber(a)) {
        if (!app::active_doc()) app::new_document();
        if (gui::gerber_dialog_drop(a)) return;
    }
    process_args(a);
}
static void queue_args(const std::vector<std::wstring>& a) {
    for (auto& x : a) if (x == L"--cmd" || x == L"--view" || x == L"--new") { gui::defer([a]() { process_args(a); }); return; }   // automation: as given
    g_batch.insert(g_batch.end(), a.begin(), a.end());
    g_batch_t = GetTickCount();
    if (!g_batch_sched) { g_batch_sched = true; gui::defer(flush_batch); }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int show) {
    using fnSetCtx = BOOL(WINAPI*)(HANDLE);
    if (auto p = (fnSetCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
        p((HANDLE)-4);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    // command line
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; ++i) args.push_back(argv[i]);
    if (argv) LocalFree(argv);
    if (!args.empty() && args[0] == L"--migrate") { int rc = gui::migrate_cli_main(args); CoUninitialize(); return rc; }   // headless batch
    if (!args.empty() && args[0] == L"--recentre") { int rc = gui::recentre_cli_main(args); CoUninitialize(); return rc; }   // R25 headless: placements to the centre of the lands
    if (int rc = gui::assoc_cli(args); rc >= 0) { CoUninitialize(); return rc; }   // --register / --unregister
    if (platform::forward_to_running_instance(args, L"PCBPlotter2")) { CoUninitialize(); return 0; }
    app::settings().load();
    ui::theme_init();
    ui::theme_set_mode((ui::ThemeMode)app::settings().theme);
    auto& reg = PluginRegistry::get();
    register_builtin_plugins(reg);
    app::load_plugins_dir(reg, app::exe_dir() + L"\\plugins");
    platform::set_forward_handler([](const std::vector<std::wstring>& a) { queue_args(a); });
    if (!args.empty()) queue_args(args);
    int r = gui::run_app(show);
    app::settings().save();
    CoUninitialize();
    return r;
}
