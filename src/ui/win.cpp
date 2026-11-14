// ============================================================================
// win.cpp — small Win32 helpers shared by the UI.
// ============================================================================
#include "win.h"
#include "../core/dialog_guard.h"
#include <shobjidl.h>
#include <cstdio>
#include <cwchar>
#include <map>

namespace pcbp::ui {

HINSTANCE app_instance() { return GetModuleHandleW(nullptr); }

UINT window_dpi(HWND h) {
    using fnGetDpiForWindow = UINT(WINAPI*)(HWND);
    static fnGetDpiForWindow p = (fnGetDpiForWindow)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (p && h) { UINT d = p(h); if (d) return d; }
    HDC dc = GetDC(nullptr);
    UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

std::wstring get_window_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize((size_t)n);
    return s;
}

std::wstring fmt_num(double v, int decimals) {
    wchar_t b[64];
    std::swprintf(b, 64, L"%.*f", decimals, v);
    // trim trailing zeros but keep at least one decimal digit when decimals>0
    std::wstring s = b;
    if (decimals > 0) {
        while (s.size() > 1 && s.back() == L'0' && s[s.size() - 2] != L'.') s.pop_back();
    }
    if (s == L"-0" || s == L"-0.0") s = s.substr(1);
    return s;
}

double parse_num(const std::wstring& s, double def) {
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    double v = std::wcstod(s.c_str(), &end);
    return end == s.c_str() ? def : v;
}

HFONT ui_font(UINT dpi, int pt_tenths, bool bold) {
    static std::map<unsigned long long, HFONT> cache;
    unsigned long long key = ((unsigned long long)dpi << 32) | ((unsigned)pt_tenths << 1) | (bold ? 1u : 0u);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(pt_tenths, (int)dpi, 720);
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyW(lf.lfFaceName, L"Segoe UI");
    HFONT f = CreateFontIndirectW(&lf);
    cache[key] = f;
    return f;
}

static std::wstring build_filter(const std::vector<FileFilter>& f) {
    std::wstring s;
    for (auto& x : f) { s += x.name; s.push_back(L'\0'); s += x.pattern; s.push_back(L'\0'); }
    s.push_back(L'\0');
    return s;
}

static bool open_file_dialog_raw(HWND owner, const std::wstring& title, const std::vector<FileFilter>& f,
                      std::wstring& path, bool multi, std::vector<std::wstring>* multi_out) {
    std::wstring filt = build_filter(f);
    std::vector<wchar_t> buf(multi ? 65536 : MAX_PATH * 4, 0);
    if (!path.empty() && path.size() < buf.size()) std::wmemcpy(buf.data(), path.c_str(), path.size());
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = owner;
    of.lpstrFilter = filt.c_str();
    of.lpstrFile = buf.data();
    of.nMaxFile = (DWORD)buf.size();
    of.lpstrTitle = title.c_str();
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY | (multi ? OFN_ALLOWMULTISELECT : 0);
    if (!GetOpenFileNameW(&of)) return false;
    if (multi && multi_out) {
        multi_out->clear();
        std::wstring first = buf.data();
        const wchar_t* p = buf.data() + first.size() + 1;
        if (!*p) multi_out->push_back(first);
        else while (*p) { std::wstring n = p; multi_out->push_back(first + L"\\" + n); p += n.size() + 1; }
        path = multi_out->empty() ? L"" : multi_out->front();
        return true;
    }
    path = buf.data();
    return true;
}

static bool save_file_dialog_raw(HWND owner, const std::wstring& title, const std::vector<FileFilter>& f,
                      const std::wstring& def_ext, std::wstring& path) {
    std::wstring filt = build_filter(f);
    std::vector<wchar_t> buf(MAX_PATH * 4, 0);
    if (!path.empty() && path.size() < buf.size()) std::wmemcpy(buf.data(), path.c_str(), path.size());
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = owner;
    of.lpstrFilter = filt.c_str();
    of.lpstrFile = buf.data();
    of.nMaxFile = (DWORD)buf.size();
    of.lpstrTitle = title.c_str();
    of.lpstrDefExt = def_ext.empty() ? nullptr : def_ext.c_str();
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&of)) return false;
    path = buf.data();
    return true;
}

static bool pick_folder_legacy(HWND owner, const std::wstring& title, std::wstring& path) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t buf[MAX_PATH] = L"";
    bool ok = SHGetPathFromIDListW(pidl, buf) != FALSE;
    CoTaskMemFree(pidl);
    if (ok) path = buf;
    return ok;
}

static std::wstring item_path(IShellItem* it) {
    std::wstring r;
    PWSTR s = nullptr;
    if (it && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &s)) && s) { r = s; CoTaskMemFree(s); }
    return r;
}

static void set_start_folder(IFileDialog* d, const std::wstring& path) {
    if (path.empty()) return;
    std::wstring dir = path;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        const size_t sl = dir.find_last_of(L"\\/");
        if (sl == std::wstring::npos) return;
        dir.resize(sl);
    }
    IShellItem* it = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_IShellItem, (void**)&it)) && it) { d->SetFolder(it); it->Release(); }
}

// R14: IFileOpenDialog with FOS_PICKFOLDERS (falls back to SHBrowseForFolder).
static bool pick_folder_dialog_raw(HWND owner, const std::wstring& title, std::wstring& path) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&d)) || !d)
        return pick_folder_legacy(owner, title, path);
    FILEOPENDIALOGOPTIONS o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(title.c_str());
    set_start_folder(d, path);
    bool ok = false;
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it)) && it) {
            std::wstring r = item_path(it);
            it->Release();
            if (!r.empty()) { path = r; ok = true; }
        }
    }
    d->Release();
    return ok;
}

namespace {
// Event sink: the custom "Select folder..." push button closes the file dialog and flags the caller.
struct FolderButtonSink final : IFileDialogEvents, IFileDialogControlEvents {
    LONG refs = 1;
    bool pressed = false;
    std::wstring folder;                       // the folder the user was browsing when pressing
    STDMETHODIMP QueryInterface(REFIID riid, void** pv) override {
        if (riid == IID_IUnknown || riid == IID_IFileDialogEvents) { *pv = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK; }
        if (riid == IID_IFileDialogControlEvents) { *pv = static_cast<IFileDialogControlEvents*>(this); AddRef(); return S_OK; }
        *pv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return (ULONG)r; }
    // IFileDialogEvents
    STDMETHODIMP OnFileOk(IFileDialog*) override { return S_OK; }
    STDMETHODIMP OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    STDMETHODIMP OnFolderChange(IFileDialog*) override { return S_OK; }
    STDMETHODIMP OnSelectionChange(IFileDialog*) override { return S_OK; }
    STDMETHODIMP OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE*) override { return S_OK; }
    STDMETHODIMP OnTypeChange(IFileDialog*) override { return S_OK; }
    STDMETHODIMP OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE*) override { return S_OK; }
    // IFileDialogControlEvents
    IFileDialog* dlg = nullptr;
    STDMETHODIMP OnItemSelected(IFileDialogCustomize*, DWORD, DWORD) override { return S_OK; }
    STDMETHODIMP OnButtonClicked(IFileDialogCustomize*, DWORD id) override {
        if (id != 1 || !dlg) return S_OK;
        pressed = true;
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetFolder(&it)) && it) { folder = item_path(it); it->Release(); }
        dlg->Close(S_FALSE);
        return S_OK;
    }
    STDMETHODIMP OnCheckButtonToggled(IFileDialogCustomize*, DWORD, BOOL) override { return S_OK; }
    STDMETHODIMP OnControlActivating(IFileDialogCustomize*, DWORD) override { return S_OK; }
};
} // namespace

static int open_file_or_folder_dialog_raw(HWND owner, const std::wstring& title, const std::vector<FileFilter>& f,
                               std::wstring& path, const std::wstring& folder_label, const std::wstring& folder_title) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&d)) || !d)
        return open_file_dialog_raw(owner, title, f, path, false, nullptr) ? 1 : 0;
    std::vector<COMDLG_FILTERSPEC> spec;
    for (auto& x : f) spec.push_back({x.name.c_str(), x.pattern.c_str()});
    if (!spec.empty()) d->SetFileTypes((UINT)spec.size(), spec.data());
    FILEOPENDIALOGOPTIONS o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    d->SetTitle(title.c_str());
    set_start_folder(d, path);
    auto* sink = new FolderButtonSink();
    sink->dlg = d;
    DWORD cookie = 0;
    IFileDialogCustomize* cu = nullptr;
    if (SUCCEEDED(d->QueryInterface(IID_IFileDialogCustomize, (void**)&cu)) && cu) {
        cu->AddPushButton(1, folder_label.c_str());
        cu->Release();
    }
    d->Advise(sink, &cookie);
    int ret = 0;
    const HRESULT hr = d->Show(owner);
    if (SUCCEEDED(hr) && !sink->pressed) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it)) && it) {
            std::wstring r = item_path(it);
            it->Release();
            if (!r.empty()) { path = r; ret = 1; }
        }
    }
    const bool pressed = sink->pressed;
    std::wstring start = sink->folder.empty() ? path : sink->folder;
    if (cookie) d->Unadvise(cookie);
    sink->dlg = nullptr;
    sink->Release();
    d->Release();
    if (pressed && pick_folder_dialog_raw(owner, folder_title, start)) { path = start; ret = 2; }
    return ret;
}

bool pick_color_dialog(HWND owner, uint32_t& argb) {
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = owner;
    cc.rgbResult = argb_to_colorref(argb);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    argb = colorref_to_argb(cc.rgbResult);
    return true;
}

int themed_msg_box(HWND owner, const std::wstring& text, const std::wstring& caption, UINT flags);   // gui/gform.cpp (ImGui, themed)
int msg_box(HWND owner, const std::wstring& text, const std::wstring& caption, UINT flags) {
    return themed_msg_box(owner, text, caption, flags);
}


// ---------------------------------------------------------------------------
// R20 loop guard (core/dialog_guard.h): every blocking file dialog goes through it. A dialog is never
// re-entered, and after a Cancel the same dialog only reappears once the user produced fresh input
// (dialog_note_input(), called by the app window for key-down / button-down messages).
static DialogLoopGuard& dlg_guard() { static DialogLoopGuard g; return g; }
void dialog_note_input() { dlg_guard().note_input(); }
int dialog_blocked_count() { return dlg_guard().blocked(); }
namespace {
struct GuardScope {
    std::string key; bool ok;
    explicit GuardScope(const std::wstring& title) : key(U(title)), ok(dlg_guard().begin(key)) {
        if (!ok) OutputDebugStringW((L"PCB Plotter: dialog \"" + title + L"\" suppressed (re-trigger without new user input)\n").c_str());
    }
    void done(bool cancelled) { if (ok) dlg_guard().end(key, cancelled); ok = false; }
    ~GuardScope() { done(true); }
};
}
bool open_file_dialog(HWND owner, const std::wstring& title, const std::vector<FileFilter>& f,
                      std::wstring& path, bool multi, std::vector<std::wstring>* multi_out) {
    GuardScope g(title);
    if (!g.ok) return false;
    const bool r = open_file_dialog_raw(owner, title, f, path, multi, multi_out);
    g.done(!r);
    return r;
}
bool save_file_dialog(HWND owner, const std::wstring& title, const std::vector<FileFilter>& f,
                      const std::wstring& def_ext, std::wstring& path) {
    GuardScope g(title);
    if (!g.ok) return false;
    const bool r = save_file_dialog_raw(owner, title, f, def_ext, path);
    g.done(!r);
    return r;
}
bool pick_folder_dialog(HWND owner, const std::wstring& title, std::wstring& path) {
    GuardScope g(title);
    if (!g.ok) return false;
    const bool r = pick_folder_dialog_raw(owner, title, path);
    g.done(!r);
    return r;
}
int open_file_or_folder_dialog(HWND owner, const std::wstring& title, const std::vector<FileFilter>& f,
                               std::wstring& path, const std::wstring& folder_label, const std::wstring& folder_title) {
    GuardScope g(title);
    if (!g.ok) return 0;
    const int r = open_file_or_folder_dialog_raw(owner, title, f, path, folder_label, folder_title);
    g.done(r == 0);
    return r;
}

} // namespace pcbp::ui
