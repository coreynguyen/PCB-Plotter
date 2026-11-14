#pragma once
// ============================================================================
// win_shell.h — Windows shell integration for PCB Plotter (see specs/WIN_SHELL.md).
//
//   * file types: ProgIDs + DefaultIcon (icons embedded in the exe) + open verb,
//     OpenWithProgids (Open With, never steals a default), "claim if unclaimed"
//     for PCB Plotter-specific extensions, Capabilities + RegisteredApplications
//     (Settings > Default apps), Applications\PCBPlotter.exe
//   * verbs: "Import into PCB Plotter" on .zip/.tgz/.tar/.gz and folders
//     (SystemFileAssociations / Directory — no ownership of .zip)
//   * per-user (HKCU\Software\Classes, no admin) or all users (HKLM, elevated;
//     relaunch_elevated() re-runs the exe with "runas")
//   * single instance: named mutex + WM_COPYDATA forwarding of the command line
//
// Everything is Windows-only; on other platforms the functions are no-op stubs
// so the portable layers / Linux tests compile.
// ============================================================================
#include <functional>
#include <string>
#include <vector>

namespace pcbp::platform {

// File-type groups (bit mask). Stable values: used on the command line and in PCBPlotter.ini.
enum TypeGroup : unsigned {
    TG_Project  = 1u << 0,   // .pcbp
    TG_Legacy   = 1u << 1,   // .cpf (+ .mdb Open With)
    TG_ViewMate = 1u << 2,   // ViewMate .bin (Open With only)
    TG_Gerber   = 1u << 3,   // Gerber / drill extensions
    TG_Odb      = 1u << 4,   // ODB++ (.tgz Open With)
    TG_Ipc      = 1u << 5,   // IPC-2581 .cvg (+ .xml Open With)
    TG_Pnp      = 1u << 6,   // .h8h/.h7h/.h5h/.h4h (+ .iss Open With)
    TG_Sheet    = 1u << 7,   // .xlsx/.xls/.xlsm/.ods/.csv (Open With + Default Apps only)
    TG_Import   = 1u << 8,   // "Import into PCB Plotter" verb on archives + folders
    TG_Default  = TG_Project | TG_Legacy | TG_ViewMate | TG_Gerber | TG_Odb | TG_Ipc | TG_Pnp | TG_Import,
    TG_All      = 0x1FF,
};

struct ExtInfo {
    const wchar_t* ext;      // ".gbr"
    bool claim;              // set as the extension's default ProgID when nothing else owns it
};
struct TypeInfo {
    unsigned group;
    const wchar_t* progid;   // "PCBPlotter.Gerber"
    const wchar_t* friendly; // "Gerber layer"
    int icon_id;             // RT_GROUP_ICON resource id in PCBPlotter.exe (res/app.rc)
    std::vector<ExtInfo> exts;
    const wchar_t* key;      // command-line / ini key: "project", "gerber", ...
};
const std::vector<TypeInfo>& file_types();

// Resource ids (res/app.rc). Never renumber: Explorer caches "exe,-id".
enum IconId { ICON_APP = 1, ICON_PCBP = 101, ICON_CPF = 102, ICON_VMJOB = 103, ICON_GERBER = 104, ICON_ODB = 105,
              ICON_IPC = 106, ICON_PNP = 107, ICON_SHEET = 108, ICON_IMPORT = 120 };

// "project,gerber" / "all" / "default" -> mask (0 = parse error, `bad` gets the unknown word)
unsigned parse_groups(const std::wstring& list, std::wstring* bad = nullptr);
std::wstring groups_to_string(unsigned mask);

struct AssocStatus {
    unsigned user_groups = 0, machine_groups = 0;   // groups whose ProgID / verb is registered
    std::wstring user_exe, machine_exe;              // exe path the registration points at
    bool stale = false;                              // registration points at another exe path
    std::vector<std::pair<std::wstring, std::wstring>> defaults;   // ext -> effective default (AssocQueryString), for display
};

bool register_types(unsigned groups, bool all_users, std::wstring& err);
bool unregister_types(bool all_users, std::wstring& err);   // removes every group
AssocStatus query_status();
// Settings > Apps > Default apps (Win11 deep link to our page when supported, Win10 page,
// Control Panel "Set Associations" UI on Win7/8). Returns false when nothing could be opened.
bool open_default_apps(void* owner_hwnd);
int windows_build();   // e.g. 22631; 7601 on Win7 SP1 (RtlGetVersion)

bool is_elevated();
// Re-run this exe elevated with `args` and wait for it. exit_code = child's exit code.
bool relaunch_elevated(const std::wstring& args, unsigned long& exit_code, std::wstring& err);

std::wstring exe_path();

// ---- single instance ------------------------------------------------------
// Call first thing in wWinMain. When another instance is running (or starting) and `args`
// is worth forwarding, sends them (with the current directory) and returns true: exit then.
bool forward_to_running_instance(const std::vector<std::wstring>& args, const wchar_t* window_class);
// Receiver: install the handler (runs on the UI thread from WM_COPYDATA; it should defer work).
void set_forward_handler(std::function<void(const std::vector<std::wstring>&)> fn);
// Call from the main wndproc for WM_COPYDATA; returns true when the message was ours.
bool handle_copydata(void* hwnd, long long lparam);

// %TEMP%\PCBPlotter\import\<name>-<n>\ (created). Old folders (> 7 days) are removed.
std::wstring import_temp_dir(const std::wstring& name);

} // namespace pcbp::platform
