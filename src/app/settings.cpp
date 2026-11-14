// ============================================================================
// settings.cpp — PCBPlotter.ini persistence + small app helpers.
// ============================================================================
#include "app.h"
#include "../ui/theme.h"
#include <cwchar>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace pcbp::app {

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring s = buf;
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}

static std::wstring ini_path() {
    std::wstring p = exe_dir() + L"\\PCBPlotter.ini";
    // writable next to exe? else %APPDATA%\PCBPlotter
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return p; }
    wchar_t app[MAX_PATH] = L"";
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, app))) {
        std::wstring d = std::wstring(app) + L"\\PCBPlotter";
        CreateDirectoryW(d.c_str(), nullptr);
        return d + L"\\PCBPlotter.ini";
    }
    return p;
}

Settings& settings() { static Settings s; return s; }

static std::wstring rd(const wchar_t* sec, const wchar_t* key, const std::wstring& def) {
    wchar_t b[2048];
    GetPrivateProfileStringW(sec, key, def.c_str(), b, 2048, ini_path().c_str());
    return b;
}
static int rdi(const wchar_t* sec, const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(sec, key, def, ini_path().c_str()); }
static double rdd(const wchar_t* sec, const wchar_t* key, double def) { return ui::parse_num(rd(sec, key, L""), def); }
static void wr(const wchar_t* sec, const wchar_t* key, const std::wstring& v) { WritePrivateProfileStringW(sec, key, v.c_str(), ini_path().c_str()); }
static void wri(const wchar_t* sec, const wchar_t* key, long long v) { wr(sec, key, std::to_wstring(v)); }
static void wrd(const wchar_t* sec, const wchar_t* key, double v) { wr(sec, key, ui::fmt_num(v, 6)); }

void Settings::load() {
    theme = rdi(L"General", L"Theme", 0);
    units = rdi(L"General", L"Units", 0);   // default mm
    company = rd(L"General", L"Company", L"");
    company_addr = rd(L"General", L"CompanyAddress", L"");
    print_scale_x = rdd(L"General", L"PrintScaleX", 1.0);
    print_scale_y = rdd(L"General", L"PrintScaleY", 1.0);
    frame_color = (uint32_t)std::wcstoul(rd(L"General", L"FrameColor", L"FF2F9E44").c_str(), nullptr, 16);
    import_netlist = rdi(L"General", L"ImportNetlist", 0) != 0;
    dir_projects = rd(L"Directories", L"ProjectPath", L"");
    dir_gerber = rd(L"Directories", L"GerberPath", L"");
    dir_cad = rd(L"Directories", L"CADPath", L"");
    dir_bom = rd(L"Directories", L"BOMPath", L"");
    dir_centroid = rd(L"Directories", L"ASCIIPath", L"");
    dir_programs = rd(L"Directories", L"ProgramPath", L"");
    dir_pictures = rd(L"Directories", L"PicturePath", L"");
    library_path = rd(L"Directories", L"LibraryPath", L"");
    m1_server = rd(L"M1", L"Server", L""); m1_database = rd(L"M1", L"Database", L""); m1_user = rd(L"M1", L"User", L"");
    m1_auth = rdi(L"M1", L"Auth", 1); m1_pass_dpapi = rd(L"M1", L"PasswordDPAPI", L"");
    mru.clear();
    for (int i = 0; i < 8; ++i) {
        std::wstring k = L"File" + std::to_wstring(i);
        std::wstring v = rd(L"Recent", k.c_str(), L"");
        if (!v.empty()) mru.push_back(v);
    }
    pinned.clear();
    for (int i = 0; i < 16; ++i) {
        std::wstring k = L"File" + std::to_wstring(i);
        std::wstring v = rd(L"Pinned", k.c_str(), L"");
        if (!v.empty()) pinned.push_back(v);
    }
    tb_file = rdi(L"Layout", L"TbFile", 1) != 0;
    tb_zoom = rdi(L"Layout", L"TbZoom", 1) != 0;
    tb_master = rdi(L"Layout", L"TbMaster", 1) != 0;
    tb_viewer = rdi(L"Layout", L"TbViewer", 1) != 0;
    tb_align = rdi(L"Layout", L"TbAlign", 0) != 0;
    tb_visaid = rdi(L"Layout", L"TbVisAid", 0) != 0;
    status_bar = rdi(L"Layout", L"StatusBar", 0) != 0;   // P2-06: hidden by default (View > Status Bar)
    factory_bar = rdi(L"Layout", L"FactoryBar", 1) != 0;
    factory_width = rdi(L"Layout", L"FactoryWidth", 230);
    win.left = rdi(L"Layout", L"X", CW_USEDEFAULT);
    win.top = rdi(L"Layout", L"Y", CW_USEDEFAULT);
    win.right = win.left + rdi(L"Layout", L"W", 1400);
    win.bottom = win.top + rdi(L"Layout", L"H", 900);
    maximized = rdi(L"Layout", L"Max", 1) != 0;
    grid_mm = rdd(L"Edit", L"GridMM", 1.27);
    grid_visible = rdi(L"Edit", L"Grid", 1) != 0;
    snap = rdi(L"Edit", L"Snap", 0) != 0;
    ui_scale_pct = std::clamp(rdi(L"Appearance", L"UiScale", 100), 80, 250);   // R12 §11: clean install = 100%
    icon_scale_pct = std::clamp(rdi(L"Appearance", L"IconScale", 100), 50, 250);
    perf_mode = std::clamp(rdi(L"Appearance", L"PerformanceMode", 0), 0, 2);
    perf_suggest_seen = rdi(L"Appearance", L"PerfSuggestSeen", 0) != 0;
    disabled_plugins.clear();
    std::wstring dis = rd(L"Plugins", L"Disabled", L"");
    size_t a = 0;
    while (a < dis.size()) {
        size_t b = dis.find(L';', a);
        if (b == std::wstring::npos) b = dis.size();
        if (b > a) disabled_plugins.push_back(ui::U(dis.substr(a, b - a)));
        a = b + 1;
    }
}

void Settings::save() const {
    wri(L"General", L"Theme", theme);
    wri(L"General", L"Units", units);
    wri(L"Appearance", L"UiScale", ui_scale_pct);
    wri(L"Appearance", L"IconScale", icon_scale_pct);
    wri(L"Appearance", L"PerformanceMode", perf_mode);
    wri(L"Appearance", L"PerfSuggestSeen", perf_suggest_seen);
    wr(L"General", L"Company", company);
    wr(L"General", L"CompanyAddress", company_addr);
    wrd(L"General", L"PrintScaleX", print_scale_x);
    wrd(L"General", L"PrintScaleY", print_scale_y);
    wchar_t b[16]; std::swprintf(b, 16, L"%08X", frame_color); wr(L"General", L"FrameColor", b);
    wri(L"General", L"ImportNetlist", import_netlist);
    wr(L"Directories", L"ProjectPath", dir_projects);
    wr(L"Directories", L"GerberPath", dir_gerber);
    wr(L"Directories", L"CADPath", dir_cad);
    wr(L"Directories", L"BOMPath", dir_bom);
    wr(L"Directories", L"ASCIIPath", dir_centroid);
    wr(L"Directories", L"ProgramPath", dir_programs);
    wr(L"Directories", L"PicturePath", dir_pictures);
    wr(L"Directories", L"LibraryPath", library_path);
    wr(L"M1", L"Server", m1_server); wr(L"M1", L"Database", m1_database); wr(L"M1", L"User", m1_user);
    wri(L"M1", L"Auth", m1_auth); wr(L"M1", L"PasswordDPAPI", m1_pass_dpapi);
    for (int i = 0; i < 8; ++i) {
        std::wstring k = L"File" + std::to_wstring(i);
        wr(L"Recent", k.c_str(), i < (int)mru.size() ? mru[i] : L"");
    }
    for (int i = 0; i < 16; ++i) {
        std::wstring k = L"File" + std::to_wstring(i);
        wr(L"Pinned", k.c_str(), i < (int)pinned.size() ? pinned[i] : L"");
    }
    wri(L"Layout", L"TbFile", tb_file); wri(L"Layout", L"TbZoom", tb_zoom); wri(L"Layout", L"TbMaster", tb_master);
    wri(L"Layout", L"TbViewer", tb_viewer); wri(L"Layout", L"TbAlign", tb_align); wri(L"Layout", L"TbVisAid", tb_visaid);
    wri(L"Layout", L"StatusBar", status_bar); wri(L"Layout", L"FactoryBar", factory_bar); wri(L"Layout", L"FactoryWidth", factory_width);
    wri(L"Layout", L"X", win.left); wri(L"Layout", L"Y", win.top);
    wri(L"Layout", L"W", win.right - win.left); wri(L"Layout", L"H", win.bottom - win.top); wri(L"Layout", L"Max", maximized);
    wrd(L"Edit", L"GridMM", grid_mm); wri(L"Edit", L"Grid", grid_visible); wri(L"Edit", L"Snap", snap);
    std::wstring dis;
    for (auto& d : disabled_plugins) { if (!dis.empty()) dis += L';'; dis += ui::W(d); }
    wr(L"Plugins", L"Disabled", dis);
}

// ---------------------------------------------------------------------------
// R11 UI preferences: small named ints in [UI] (tutorial mode, dismissed hints "Hint.<key>", suppressed
// informational confirmations "Confirm.<key>", tab placement ...). Written immediately; cached.
// ---------------------------------------------------------------------------
static std::map<std::string, int>& ui_prefs() {
    static std::map<std::string, int> m;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::vector<wchar_t> buf(32768);
        DWORD n = GetPrivateProfileSectionW(L"UI", buf.data(), (DWORD)buf.size(), ini_path().c_str());
        for (const wchar_t* q = buf.data(); q < buf.data() + n && *q; q += wcslen(q) + 1) {
            std::wstring line = q;
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos) m[ui::U(line.substr(0, eq))] = _wtoi(line.c_str() + eq + 1);
        }
    }
    return m;
}
int ui_pref(const char* key, int def) { auto& m = ui_prefs(); auto it = m.find(key); return it == m.end() ? def : it->second; }
void set_ui_pref(const char* key, int v) {
    ui_prefs()[key] = v;
    wri(L"UI", ui::W(key).c_str(), v);
}
int reset_ui_prefs(const char* prefix) {
    int n = 0;
    auto& m = ui_prefs();
    for (auto it = m.begin(); it != m.end();) {
        if (it->first.rfind(prefix, 0) == 0) { WritePrivateProfileStringW(L"UI", ui::W(it->first).c_str(), nullptr, ini_path().c_str()); it = m.erase(it); ++n; }
        else ++it;
    }
    return n;
}

void Settings::add_mru(const std::wstring& path) {
    for (size_t i = 0; i < mru.size(); ++i) if (!lstrcmpiW(mru[i].c_str(), path.c_str())) { mru.erase(mru.begin() + (long)i); break; }
    mru.insert(mru.begin(), path);
    if (mru.size() > 8) mru.resize(8);
}

void Settings::remove_mru(const std::wstring& path) {
    for (size_t i = 0; i < mru.size(); ++i) if (!lstrcmpiW(mru[i].c_str(), path.c_str())) { mru.erase(mru.begin() + (long)i); break; }
    set_pinned(path, false);
}
bool Settings::is_pinned(const std::wstring& path) const {
    for (auto& p : pinned) if (!lstrcmpiW(p.c_str(), path.c_str())) return true;
    return false;
}
void Settings::set_pinned(const std::wstring& path, bool pin) {
    for (size_t i = 0; i < pinned.size(); ++i) if (!lstrcmpiW(pinned[i].c_str(), path.c_str())) { if (pin) return; pinned.erase(pinned.begin() + (long)i); return; }
    if (pin && pinned.size() < 16) pinned.push_back(path);
}

std::wstring units_label() { return ui::W(units_suffix((Units)settings().units)); }

std::wstring fmt_len(double mm, bool with_units) {
    Units u = (Units)settings().units;
    int dec = u == Units::Mil ? 1 : u == Units::Inch ? 4 : 3;
    std::wstring s = ui::fmt_num(to_display(mm, u), dec);
    if (with_units) s += L" " + units_label();
    return s;
}

double parse_len(const std::wstring& s, double def_mm) {
    std::wstring t = s;
    Units u = (Units)settings().units;
    auto ends = [&](const wchar_t* suf) {
        size_t n = wcslen(suf);
        if (t.size() >= n && !_wcsicmp(t.c_str() + t.size() - n, suf)) { t.resize(t.size() - n); return true; }
        return false;
    };
    while (!t.empty() && t.back() == L' ') t.pop_back();
    if (ends(L"mm")) u = Units::MM;
    else if (ends(L"mil")) u = Units::Mil;
    else if (ends(L"in") || ends(L"\"")) u = Units::Inch;
    double v = ui::parse_num(t, std::numeric_limits<double>::quiet_NaN());
    if (v != v) return def_mm;
    return from_display(v, u);
}

} // namespace pcbp::app
