// ============================================================================
// pcbp_thumb.cpp — Explorer thumbnail provider for .pcbp / .cpf (pcbp_thumb.dll).
//
// In-process COM server: IThumbnailProvider + IInitializeWithStream (stream
// based, so Explorer may run it isolated in prevhost/dllhost). The project is
// parsed with the same core loaders as the app and rasterised by
// thumb_render.cpp (no GDI drawing; the HBITMAP is a 32-bpp DIB section).
//
// Registration (per user, no admin): `regsvr32 pcbp_thumb.dll` or
// `PCBPlotter.exe --register` when the DLL sits next to the exe (win_shell.cpp):
//   HKCU\Software\Classes\CLSID\{CLSID}\InprocServer32 = <dll>, ThreadingModel=Apartment
//   HKCU\Software\Classes\.pcbp\ShellEx\{e357fccd-a995-4576-b01f-234630154e96} = {CLSID}
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <shlobj.h>
#include <thumbcache.h>
#include <olectl.h>
#include <atomic>
#include <new>
#include <string>
#include "thumb_render.h"
#include "../../core/fileio.h"
#include "../../plugins/plugin.h"

using namespace pcbp;

// {6C1E5A3B-8F2D-4B7A-9E41-2D5C7A1B9F10}
static const CLSID kClsid = {0x6c1e5a3b, 0x8f2d, 0x4b7a, {0x9e, 0x41, 0x2d, 0x5c, 0x7a, 0x1b, 0x9f, 0x10}};
static const wchar_t* kClsidStr = L"{6C1E5A3B-8F2D-4B7A-9E41-2D5C7A1B9F10}";
static const wchar_t* kThumbIid = L"{e357fccd-a995-4576-b01f-234630154e96}";
static const IID kIidThumb = {0xe357fccd, 0xa995, 0x4576, {0xb0, 0x1f, 0x23, 0x46, 0x30, 0x15, 0x4e, 0x96}};
static const IID kIidInitStream = {0xb824b49d, 0x22ac, 0x4161, {0xac, 0x8a, 0x99, 0x16, 0xe8, 0xfa, 0x3f, 0x7f}};

static HMODULE g_mod = nullptr;
static std::atomic<long> g_refs{0};

static bool load_cpf(const std::string& bytes, Project& prj, std::string& err) {
    static bool once = false;
    auto& reg = PluginRegistry::get();
    if (!once) { register_builtin_plugins(reg); once = true; }
    Importer* im = reg.importer("circuitcam.cpf");
    if (!im) { err = "no legacy importer"; return false; }
    wchar_t dir[MAX_PATH + 1], file[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, dir);
    if (!GetTempFileNameW(dir, L"pth", 0, file)) { err = "temp file"; return false; }
    const std::string path = wide_to_utf8(file);
    bool ok = write_file(path, bytes.data(), bytes.size(), err);
    if (ok) {
        ImportReport rep;
        ok = im->run(path, default_options(im->info()), prj, rep, err);
    }
    DeleteFileW(file);
    return ok;
}

class ThumbProvider : public IThumbnailProvider, public IInitializeWithStream {
    std::atomic<long> ref_{1};
    std::string data_;
public:
    ThumbProvider() { ++g_refs; }
    virtual ~ThumbProvider() { --g_refs; }
    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIidThumb)) *ppv = static_cast<IThumbnailProvider*>(this);
        else if (IsEqualIID(riid, kIidInitStream)) *ppv = static_cast<IInitializeWithStream*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return (ULONG)++ref_; }
    IFACEMETHODIMP_(ULONG) Release() override { long r = --ref_; if (!r) delete this; return (ULONG)r; }
    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* s, DWORD) override {
        if (!s) return E_INVALIDARG;
        if (!data_.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        char buf[65536];
        ULONG n = 0;
        for (;;) {
            HRESULT hr = s->Read(buf, sizeof buf, &n);
            if (FAILED(hr)) return hr;
            data_.append(buf, n);
            if (n < sizeof buf) break;
            if (data_.size() > (256u << 20)) return E_OUTOFMEMORY;
        }
        return S_OK;
    }
    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* alpha) override {
        if (!phbmp || !alpha) return E_POINTER;
        *phbmp = nullptr;
        Project prj;
        std::string err;
        try {
            if (!thumb::load_bytes(data_, prj, err, load_cpf)) return E_FAIL;
            thumb::Image im;
            if (!thumb::render(prj, (int)cx, im)) return E_FAIL;
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = im.w;
            bi.bmiHeader.biHeight = -im.h;   // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP h = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!h || !bits) return E_OUTOFMEMORY;
            memcpy(bits, im.px.data(), im.px.size() * 4);
            *phbmp = h;
            *alpha = WTSAT_ARGB;
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }
};

class Factory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { ++g_refs; return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { --g_refs; return 1; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* p = new (std::nothrow) ThumbProvider();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override { if (lock) ++g_refs; else --g_refs; return S_OK; }
};
static Factory g_factory;

// exports: pcbp_thumb.def (STDAPI already gives C linkage)
extern "C" {

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_mod = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!IsEqualCLSID(clsid, kClsid)) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_refs == 0 ? S_OK : S_FALSE; }

static LONG set_sz(const std::wstring& key, const wchar_t* name, const std::wstring& v) {
    HKEY k;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (r) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * 2));
    RegCloseKey(k);
    return r;
}

// per-user registration (regsvr32 pcbp_thumb.dll)
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH * 2];
    GetModuleFileNameW(g_mod, path, MAX_PATH * 2);
    const std::wstring ck = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr;
    if (set_sz(ck, nullptr, L"PCB Plotter thumbnail provider") || set_sz(ck + L"\\InprocServer32", nullptr, path) ||
        set_sz(ck + L"\\InprocServer32", L"ThreadingModel", L"Apartment"))
        return SELFREG_E_CLASS;
    for (const wchar_t* e : {L".pcbp", L".cpf"})
        if (set_sz(std::wstring(L"Software\\Classes\\") + e + L"\\ShellEx\\" + kThumbIid, nullptr, kClsidStr)) return SELFREG_E_CLASS;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr).c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr).c_str());
    for (const wchar_t* e : {L".pcbp", L".cpf"})
        RegDeleteKeyW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + e + L"\\ShellEx\\" + kThumbIid).c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

} // extern "C"
