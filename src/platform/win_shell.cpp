// ============================================================================
// win_shell.cpp — Windows shell integration (see win_shell.h, specs/WIN_SHELL.md).
// ============================================================================
#include "win_shell.h"

#include <algorithm>
#include <cwctype>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <chrono>
#include <filesystem>
#endif

namespace pcbp::platform {

// ---------------------------------------------------------------------------
// Type table (portable data)
// ---------------------------------------------------------------------------
const std::vector<TypeInfo>& file_types() {
    static const std::vector<TypeInfo> t = {
        {TG_Project, L"PCBPlotter.Project", L"PCB Plotter project", ICON_PCBP, {{L".pcbp", true}}, L"project"},
        {TG_Legacy, L"PCBPlotter.LegacyCam", L"Legacy CAM project (migration)", ICON_CPF, {{L".cpf", true}, {L".mdb", false}}, L"legacy"},
        {TG_ViewMate, L"PCBPlotter.ViewMateJob", L"ViewMate job", ICON_VMJOB, {{L".bin", false}}, L"viewmate"},
        {TG_Gerber, L"PCBPlotter.Gerber", L"Gerber / drill layer", ICON_GERBER,
         {{L".gbr", true}, {L".ger", true}, {L".gtl", true}, {L".gbl", true}, {L".gto", true}, {L".gbo", true}, {L".gts", true},
          {L".gbs", true}, {L".gtp", true}, {L".gbp", true}, {L".gko", true}, {L".gm1", true}, {L".gml", true}, {L".pho", true},
          {L".274", true}, {L".drl", true}, {L".xln", true}, {L".exc", true}, {L".art", false}, {L".spl", false}, {L".nc", false}},
         L"gerber"},
        {TG_Odb, L"PCBPlotter.Odb", L"ODB++ job archive", ICON_ODB, {{L".tgz", false}, {L".odb", false}}, L"odb"},
        {TG_Ipc, L"PCBPlotter.Ipc2581", L"IPC-2581 design", ICON_IPC, {{L".cvg", true}, {L".xml", false}}, L"ipc"},
        {TG_Pnp, L"PCBPlotter.PnPProgram", L"Pick-and-place program", ICON_PNP,
         {{L".h8h", true}, {L".h7h", true}, {L".h5h", true}, {L".h4h", true}, {L".iss", false}}, L"pnp"},
        {TG_Sheet, L"PCBPlotter.Sheet", L"Spreadsheet (PCB Plotter)", ICON_SHEET,
         {{L".xlsx", false}, {L".xlsm", false}, {L".xls", false}, {L".ods", false}, {L".csv", false}}, L"sheet"},
    };
    return t;
}

unsigned parse_groups(const std::wstring& list, std::wstring* bad) {
    unsigned m = 0;
    std::wstring w;
    auto flush = [&]() -> bool {
        if (w.empty()) return true;
        std::wstring k = w; w.clear();
        for (auto& c : k) c = (wchar_t)std::towlower(c);
        if (k == L"all") { m |= TG_All; return true; }
        if (k == L"default") { m |= TG_Default; return true; }
        if (k == L"import" || k == L"zip") { m |= TG_Import; return true; }
        for (auto& t : file_types()) if (k == t.key) { m |= t.group; return true; }
        if (bad) *bad = k;
        return false;
    };
    for (wchar_t c : list) {
        if (c == L',' || c == L';' || c == L' ' || c == L'+') { if (!flush()) return 0; } else w += c;
    }
    if (!flush()) return 0;
    return m;
}

std::wstring groups_to_string(unsigned mask) {
    std::wstring s;
    for (auto& t : file_types()) if (mask & t.group) { if (!s.empty()) s += L","; s += t.key; }
    if (mask & TG_Import) { if (!s.empty()) s += L","; s += L"import"; }
    return s;
}

#ifdef _WIN32

namespace {

const wchar_t* kAppName = L"PCB Plotter";
const wchar_t* kCapKey = L"Software\\PCBPlotter\\Capabilities";
const wchar_t* kVerb = L"PCBPlotter.Import";
const wchar_t* kImportOn[] = {L".zip", L".tgz", L".tar", L".gz"};
constexpr ULONG_PTR kCopyMagic = 0x50434250;   // 'PCBP'
// pcbp_thumb.dll (src/platform/thumb): IThumbnailProvider for .pcbp / .cpf
const wchar_t* kThumbClsid = L"{6C1E5A3B-8F2D-4B7A-9E41-2D5C7A1B9F10}";
const wchar_t* kThumbShellEx = L"\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";
std::wstring thumb_dll() {
    std::wstring e = exe_path();
    size_t s = e.find_last_of(L'\\');
    std::wstring d = (s == std::wstring::npos ? std::wstring() : e.substr(0, s + 1)) + L"pcbp_thumb.dll";
    return GetFileAttributesW(d.c_str()) == INVALID_FILE_ATTRIBUTES ? std::wstring() : d;
}

HKEY root_of(bool all_users) { return all_users ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }
std::wstring classes(const std::wstring& sub) { return L"Software\\Classes\\" + sub; }

bool set_str(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& val, std::wstring& err) {
    HKEY k;
    LONG r = RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) { err = L"cannot create " + key + L" (error " + std::to_wstring(r) + L")"; return false; }
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) { err = L"cannot write " + key + L" (error " + std::to_wstring(r) + L")"; return false; }
    return true;
}
bool set_none(HKEY root, const std::wstring& key, const wchar_t* name, std::wstring& err) {
    HKEY k;
    LONG r = RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) { err = L"cannot create " + key; return false; }
    r = RegSetValueExW(k, name, 0, REG_NONE, nullptr, 0);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
bool get_str(HKEY root, const std::wstring& key, const wchar_t* name, std::wstring& out) {
    wchar_t buf[2048];
    DWORD n = sizeof buf, type = 0;
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, buf, &n) != ERROR_SUCCESS) return false;
    out = buf;
    return true;
}
bool key_exists(HKEY root, const std::wstring& key) {
    HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}
void del_tree(HKEY root, const std::wstring& key) { RegDeleteTreeW(root, key.c_str()); RegDeleteKeyW(root, key.c_str()); }
void del_value(HKEY root, const std::wstring& key, const wchar_t* name) {
    HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, name);
    RegCloseKey(k);
}
// delete `key` (and empty parents up to `stop`) when it has no values and no sub-keys
void del_if_empty(HKEY root, std::wstring key, const std::wstring& stop) {
    while (key.size() > stop.size()) {
        HKEY k;
        if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
        DWORD subs = 0, vals = 0;
        RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &subs, nullptr, nullptr, &vals, nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(k);
        if (subs || vals) return;
        if (RegDeleteKeyW(root, key.c_str()) != ERROR_SUCCESS) return;
        size_t p = key.find_last_of(L'\\');
        if (p == std::wstring::npos) return;
        key.resize(p);
    }
}

std::wstring quoted_exe() { return L"\"" + exe_path() + L"\""; }
std::wstring icon_ref(int id) { return exe_path() + L",-" + std::to_wstring(id); }

bool write_verb(HKEY root, const std::wstring& shell_key, const std::wstring& label, const std::wstring& cmd, int icon, std::wstring& err) {
    if (!label.empty() && !set_str(root, shell_key, L"MUIVerb", label, err)) return false;
    if (icon && !set_str(root, shell_key, L"Icon", icon_ref(icon), err)) return false;
    if (!set_str(root, shell_key, L"MultiSelectModel", L"Player", err)) return false;
    return set_str(root, shell_key + L"\\command", nullptr, cmd, err);
}

std::wstring ext_default(const std::wstring& ext) {
    // effective HKCR default (merged view), "" when unowned
    std::wstring v;
    get_str(HKEY_CLASSES_ROOT, ext, nullptr, v);
    return v;
}

} // namespace

std::wstring exe_path() {
    wchar_t p[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, p, (DWORD)(sizeof p / sizeof p[0]));
    return std::wstring(p, n);
}

int windows_build() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static int b = -1;
    if (b >= 0) return b;
    b = 0;
    if (auto f = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) {
        OSVERSIONINFOW v{}; v.dwOSVersionInfoSize = sizeof v;
        if (f(&v) == 0) b = (int)v.dwBuildNumber;
    }
    return b;
}

bool register_types(unsigned groups, bool all_users, std::wstring& err) {
    const HKEY R = root_of(all_users);
    const std::wstring exe = exe_path();
    const std::wstring open_cmd = quoted_exe() + L" \"%1\"";
    std::vector<std::pair<std::wstring, std::wstring>> caps;   // ext -> progid
    for (auto& t : file_types()) {
        if (!(groups & t.group)) continue;
        const std::wstring pk = classes(t.progid);
        if (!set_str(R, pk, nullptr, t.friendly, err)) return false;
        if (!set_str(R, pk, L"FriendlyTypeName", t.friendly, err)) return false;
        if (!set_str(R, pk + L"\\DefaultIcon", nullptr, icon_ref(t.icon_id), err)) return false;
        if (!write_verb(R, pk + L"\\shell\\open", L"", open_cmd, 0, err)) return false;
        if (!set_str(R, pk + L"\\shell", nullptr, L"open", err)) return false;
        for (auto& e : t.exts) {
            const std::wstring ek = classes(e.ext);
            if (!set_none(R, ek + L"\\OpenWithProgids", t.progid, err)) return false;
            if (e.claim) {
                const std::wstring cur = ext_default(e.ext);
                if (cur.empty() || cur == t.progid) { if (!set_str(R, ek, nullptr, t.progid, err)) return false; }
            }
            set_str(R, classes(L"Applications\\PCBPlotter.exe\\SupportedTypes"), e.ext, L"", err);
            caps.push_back({e.ext, t.progid});
        }
    }
    const std::wstring tdll = thumb_dll();
    if (!tdll.empty() && (groups & (TG_Project | TG_Legacy))) {   // thumbnails (Explorer runs the stream handler isolated)
        const std::wstring ck = classes(std::wstring(L"CLSID\\") + kThumbClsid);
        if (!set_str(R, ck, nullptr, L"PCB Plotter thumbnail provider", err) || !set_str(R, ck + L"\\InprocServer32", nullptr, tdll, err) ||
            !set_str(R, ck + L"\\InprocServer32", L"ThreadingModel", L"Apartment", err))
            return false;
        if (groups & TG_Project) set_str(R, classes(std::wstring(L".pcbp") + kThumbShellEx), nullptr, kThumbClsid, err);
        if (groups & TG_Legacy) set_str(R, classes(std::wstring(L".cpf") + kThumbShellEx), nullptr, kThumbClsid, err);
    }
    if (groups & TG_Import) {
        const std::wstring cmd = quoted_exe() + L" --import \"%1\"";
        for (const wchar_t* x : kImportOn)
            if (!write_verb(R, classes(std::wstring(L"SystemFileAssociations\\") + x + L"\\shell\\" + kVerb), L"Import into PCB Plotter", cmd, ICON_IMPORT, err)) return false;
        if (!write_verb(R, classes(std::wstring(L"Directory\\shell\\") + kVerb), L"Import into PCB Plotter", cmd, ICON_IMPORT, err)) return false;
    }
    // Applications\PCBPlotter.exe: name + icon in "Open with"
    const std::wstring ak = classes(L"Applications\\PCBPlotter.exe");
    if (!set_str(R, ak, L"FriendlyAppName", kAppName, err)) return false;
    if (!set_str(R, ak + L"\\DefaultIcon", nullptr, exe + L",0", err)) return false;
    if (!set_str(R, ak + L"\\shell\\open\\command", nullptr, open_cmd, err)) return false;
    // Capabilities + RegisteredApplications (Settings > Default apps / Default Programs)
    if (!set_str(R, kCapKey, L"ApplicationName", kAppName, err)) return false;
    if (!set_str(R, kCapKey, L"ApplicationDescription", L"SMT assembly CAM: Gerber, ODB++, IPC-2581, centroid / BOM, pick-and-place programs", err)) return false;
    if (!set_str(R, kCapKey, L"ApplicationIcon", exe + L",0", err)) return false;
    for (auto& [e, p] : caps) if (!set_str(R, std::wstring(kCapKey) + L"\\FileAssociations", e.c_str(), p, err)) return false;
    if (!set_str(R, L"Software\\RegisteredApplications", kAppName, kCapKey, err)) return false;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    return true;
}

bool unregister_types(bool all_users, std::wstring& err) {
    (void)err;
    const HKEY R = root_of(all_users);
    const std::wstring stop = L"Software\\Classes";
    for (const wchar_t* e : {L".pcbp", L".cpf"}) {
        const std::wstring k = classes(std::wstring(e) + kThumbShellEx);
        std::wstring v;
        if (get_str(R, k, nullptr, v) && v == kThumbClsid) { RegDeleteKeyW(R, k.c_str()); del_if_empty(R, classes(std::wstring(e) + L"\\ShellEx"), classes(e)); }
    }
    del_tree(R, classes(std::wstring(L"CLSID\\") + kThumbClsid));
    for (auto& t : file_types()) {
        del_tree(R, classes(t.progid));
        for (auto& e : t.exts) {
            const std::wstring ek = classes(e.ext);
            del_value(R, ek + L"\\OpenWithProgids", t.progid);
            del_if_empty(R, ek + L"\\OpenWithProgids", ek);
            std::wstring cur;
            if (get_str(R, ek, nullptr, cur) && cur == t.progid) del_value(R, ek, nullptr);
            del_if_empty(R, ek, stop);
        }
    }
    for (const wchar_t* x : kImportOn) {
        const std::wstring base = classes(std::wstring(L"SystemFileAssociations\\") + x);
        del_tree(R, base + L"\\shell\\" + kVerb);
        del_if_empty(R, base + L"\\shell", stop);
    }
    del_tree(R, classes(std::wstring(L"Directory\\shell\\") + kVerb));
    del_tree(R, classes(L"Applications\\PCBPlotter.exe"));
    del_tree(R, kCapKey);
    del_if_empty(R, L"Software\\PCBPlotter", L"Software");
    del_value(R, L"Software\\RegisteredApplications", kAppName);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    return true;
}

AssocStatus query_status() {
    AssocStatus s;
    const std::wstring me = exe_path();
    for (int m = 0; m < 2; ++m) {
        const HKEY R = m ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
        unsigned& g = m ? s.machine_groups : s.user_groups;
        std::wstring& exe = m ? s.machine_exe : s.user_exe;
        for (auto& t : file_types()) {
            const std::wstring pk = classes(t.progid);
            if (!key_exists(R, pk)) continue;
            g |= t.group;
            std::wstring cmd;
            if (exe.empty() && get_str(R, pk + L"\\shell\\open\\command", nullptr, cmd) && cmd.size() > 2 && cmd[0] == L'"') {
                size_t q = cmd.find(L'"', 1);
                if (q != std::wstring::npos) exe = cmd.substr(1, q - 1);
            }
        }
        if (key_exists(R, classes(std::wstring(L"SystemFileAssociations\\.zip\\shell\\") + kVerb))) g |= TG_Import;
        if (!exe.empty() && lstrcmpiW(exe.c_str(), me.c_str()) != 0) s.stale = true;
    }
    for (const wchar_t* e : {L".pcbp", L".cpf", L".gbr", L".drl", L".cvg", L".h8h", L".xlsx", L".csv", L".zip"}) {
        wchar_t buf[512]; DWORD n = 512;
        std::wstring v = L"(none)";
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_FRIENDLYAPPNAME, e, nullptr, buf, &n))) v = buf;
        s.defaults.push_back({e, v});
    }
    return s;
}

namespace shell_abi {   // R23a: named (external linkage) so GCC cannot devirtualize to __cxa_pure_virtual
// IApplicationAssociationRegistrationUI (shobjidl.h; declared here so old SDK headers are fine)
struct IAppAssocRegUI : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE LaunchAdvancedAssociationUI(LPCWSTR app) = 0;
};
const CLSID kClsidAarUI = {0x1968106d, 0xf3b5, 0x44cf, {0x89, 0x0e, 0x11, 0x6f, 0xcb, 0x9e, 0xce, 0xf1}};
const IID kIidAarUI = {0x1f76a169, 0xf994, 0x40ac, {0x8f, 0xc8, 0x09, 0x59, 0xe8, 0x87, 0x47, 0x10}};
}
using namespace shell_abi;

bool open_default_apps(void* owner) {
    const int b = windows_build();
    auto shell = [&](const std::wstring& what, const wchar_t* params = nullptr) {
        return (INT_PTR)ShellExecuteW((HWND)owner, L"open", what.c_str(), params, nullptr, SW_SHOWNORMAL) > 32;
    };
    if (b >= 10240) {   // Windows 10 / 11: Settings. Win11 (21H2+ with 2023-04 CU) honours the per-app deep link.
        const AssocStatus st = query_status();
        std::wstring uri = L"ms-settings:defaultapps";
        if (b >= 22000) {
            if (st.user_groups) uri += L"?registeredAppUser=PCB%20Plotter";
            else if (st.machine_groups) uri += L"?registeredAppMachine=PCB%20Plotter";
        }
        if (shell(uri)) return true;
        return shell(L"ms-settings:defaultapps");
    }
    // Windows 7 / 8.x: Default Programs "Set Associations" page for our registered app
    IAppAssocRegUI* ui = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidAarUI, nullptr, CLSCTX_INPROC_SERVER, kIidAarUI, (void**)&ui)) && ui) {
        HRESULT hr = ui->LaunchAdvancedAssociationUI(kAppName);
        ui->Release();
        if (SUCCEEDED(hr)) return true;
    }
    return shell(L"control.exe", L"/name Microsoft.DefaultPrograms /page pageDefaultProgram");
}

bool is_elevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{}; DWORD n = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n);
    CloseHandle(tok);
    return ok && e.TokenIsElevated;
}

bool relaunch_elevated(const std::wstring& args, unsigned long& code, std::wstring& err) {
    const std::wstring exe = exe_path();
    SHELLEXECUTEINFOW si{sizeof si};
    si.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    si.lpVerb = L"runas";
    si.lpFile = exe.c_str();
    si.lpParameters = args.c_str();
    si.nShow = SW_HIDE;
    if (!ShellExecuteExW(&si) || !si.hProcess) {
        DWORD e = GetLastError();
        err = e == ERROR_CANCELLED ? L"elevation was cancelled" : L"could not start elevated (error " + std::to_wstring(e) + L")";
        return false;
    }
    WaitForSingleObject(si.hProcess, 120000);
    DWORD c = 1;
    GetExitCodeProcess(si.hProcess, &c);
    CloseHandle(si.hProcess);
    code = c;
    return true;
}

// ---------------------------------------------------------------------------
// single instance
// ---------------------------------------------------------------------------
namespace {
std::function<void(const std::vector<std::wstring>&)> g_fwd;
bool forwardable(const std::vector<std::wstring>& a) {
    if (a.empty()) return false;
    for (auto& s : a)
        if (s == L"--new-instance" || s == L"--cmd" || s == L"--view" || s == L"--new" || s == L"--register" ||
            s == L"--unregister" || s == L"--all-users")
            return false;
    if (_wgetenv(L"PCBP_OPEN_EDITOR") || _wgetenv(L"PCBP_DEMO_FEEDBACK")) return false;
    return true;
}
}

bool forward_to_running_instance(const std::vector<std::wstring>& args, const wchar_t* cls) {
    // kept open for the process lifetime (the kernel closes it on exit)
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\PCBPlotter.SingleInstance");
    const bool other = mutex && GetLastError() == ERROR_ALREADY_EXISTS;
    if (!other || !forwardable(args)) return false;
    // absolute paths: the receiver has another current directory
    std::wstring payload = L"PCBP1";
    payload.push_back(L'\0');
    for (auto& a : args) {
        std::wstring v = a;
        if (!a.empty() && a[0] != L'-') {
            wchar_t full[MAX_PATH * 4];
            DWORD n = GetFullPathNameW(a.c_str(), (DWORD)(sizeof full / sizeof full[0]), full, nullptr);
            if (n && n < sizeof full / sizeof full[0]) v = full;
        }
        payload += v;
        payload.push_back(L'\0');
    }
    // the first instance may still be starting (Explorer launches one process per selected file)
    for (int i = 0; i < 100; ++i) {
        HWND w = FindWindowW(cls, nullptr);
        if (w) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            AllowSetForegroundWindow(pid);
            COPYDATASTRUCT cd{kCopyMagic, (DWORD)(payload.size() * sizeof(wchar_t)), (void*)payload.data()};
            DWORD_PTR res = 0;
            if (SendMessageTimeoutW(w, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 20000, &res) && res == 1) return true;
            return false;   // hung / refused: run standalone
        }
        Sleep(100);
    }
    return false;
}

void set_forward_handler(std::function<void(const std::vector<std::wstring>&)> fn) { g_fwd = std::move(fn); }

bool handle_copydata(void* hwnd, long long lparam) {
    auto* cd = (const COPYDATASTRUCT*)(LPARAM)lparam;
    if (!cd || cd->dwData != kCopyMagic || !cd->lpData || cd->cbData < 12) return false;
    const wchar_t* p = (const wchar_t*)cd->lpData;
    const size_t n = cd->cbData / sizeof(wchar_t);
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (size_t i = 0; i < n; ++i) { if (p[i]) cur += p[i]; else { parts.push_back(cur); cur.clear(); } }
    if (parts.empty() || parts[0] != L"PCBP1") return false;
    parts.erase(parts.begin());
    HWND h = (HWND)hwnd;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
    if (g_fwd) g_fwd(parts);
    return true;
}

std::wstring import_temp_dir(const std::wstring& name) {
    namespace fs = std::filesystem;
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, tmp);
    fs::path base = fs::path(tmp) / L"PCBPlotter" / L"import";
    std::error_code ec;
    fs::create_directories(base, ec);
    const auto now = fs::file_time_type::clock::now();
    for (auto it = fs::directory_iterator(base, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        auto t = it->last_write_time(ec);
        if (!ec && now - t > std::chrono::hours(24 * 7)) fs::remove_all(it->path(), ec);
    }
    std::wstring safe;
    for (wchar_t c : name) safe += (std::iswalnum(c) || c == L'-' || c == L'_' || c == L'.') ? c : L'_';
    if (safe.size() > 40) safe.resize(40);
    for (unsigned i = GetTickCount() & 0xFFFF;; ++i) {
        fs::path d = base / (safe + L"-" + std::to_wstring(i));
        if (fs::create_directory(d, ec)) return d.wstring();
        if (i > 0x20000) return base.wstring();
    }
}

#else  // !_WIN32 — stubs

std::wstring exe_path() { return L""; }
int windows_build() { return 0; }
bool register_types(unsigned, bool, std::wstring& err) { err = L"not Windows"; return false; }
bool unregister_types(bool, std::wstring& err) { err = L"not Windows"; return false; }
AssocStatus query_status() { return {}; }
bool open_default_apps(void*) { return false; }
bool is_elevated() { return false; }
bool relaunch_elevated(const std::wstring&, unsigned long& code, std::wstring& err) { code = 1; err = L"not Windows"; return false; }
bool forward_to_running_instance(const std::vector<std::wstring>&, const wchar_t*) { return false; }
void set_forward_handler(std::function<void(const std::vector<std::wstring>&)>) {}
bool handle_copydata(void*, long long) { return false; }
std::wstring import_temp_dir(const std::wstring&) { return L"/tmp"; }

#endif

} // namespace pcbp::platform
