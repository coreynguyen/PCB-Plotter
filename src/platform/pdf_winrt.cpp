// ============================================================================
// pdf_winrt.cpp — see pdf_winrt.h.
// ============================================================================
#include "pdf_winrt.h"
#include <windows.h>
#include <inspectable.h>
#include <objidl.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <algorithm>
#include <mutex>

namespace pcbp::platform::pdf {

// R23a: NOT an anonymous namespace. With internal linkage GCC sees every class derived from these
// hand-declared ABI interfaces (none), so it "devirtualizes" calls on them to __cxa_pure_virtual, a weak
// undefined symbol under -static that resolves to address 0 -> call to ImageBase-0x40000000 -> AV on
// real Windows (v22 crash). External linkage keeps the calls virtual. tests/check_devirt.sh guards it.
namespace winrt_abi {

// ---- WinRT types declared by hand (Windows SDK metadata: Windows.Foundation / Windows.Data.Pdf) --------
typedef struct HSTRING__* HSTR;
enum AsyncStatusV { AS_Started = 0, AS_Completed = 1, AS_Canceled = 2, AS_Error = 3 };
struct SizeF { float Width, Height; };
struct RectF { float X, Y, Width, Height; };
struct ColorU { BYTE A, R, G, B; };

// IAsyncInfo {00000036-0000-0000-C000-000000000046}
const GUID kIID_IAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
struct IAsyncInfoV : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* hr) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
// IAsyncAction (Windows.Foundation): Completed (put/get), GetResults
struct IAsyncActionV : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
};
struct IPdfDocumentV;
// IAsyncOperation<PdfDocument>: Completed (put/get), GetResults(out PdfDocument)
struct IAsyncOpPdfDocV : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(IPdfDocumentV** result) = 0;
};
// Windows.Data.Pdf.IPdfPageRenderOptions {3C98056F-B7CF-4C29-9A04-52D90267F425}
const GUID kIID_IPdfPageRenderOptions = {0x3c98056f, 0xb7cf, 0x4c29, {0x9a, 0x04, 0x52, 0xd9, 0x02, 0x67, 0xf4, 0x25}};
struct IPdfPageRenderOptionsV : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceRect(RectF* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SourceRect(RectF v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationWidth(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationWidth(UINT32 v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationHeight(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationHeight(UINT32 v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BackgroundColor(ColorU* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BackgroundColor(ColorU v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsIgnoringHighContrast(boolean* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsIgnoringHighContrast(boolean v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BitmapEncoderId(GUID* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BitmapEncoderId(GUID v) = 0;
};
// Windows.Data.Pdf.IPdfPage {9DB4B0C8-5320-4CFC-AD76-493FDAD0E594}
struct IPdfPageV : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RenderToStreamAsync(IUnknown* output, IAsyncActionV** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE RenderWithOptionsToStreamAsync(IUnknown* output, IPdfPageRenderOptionsV* options, IAsyncActionV** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreparePageAsync(IAsyncActionV** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Index(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(SizeF* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Dimensions(IUnknown** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Rotation(int* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PreferredZoom(float* v) = 0;
};
// Windows.Data.Pdf.IPdfDocument {AC7EBEDD-80FA-4089-846E-81B77FF5A86C}
struct IPdfDocumentV : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPage(UINT32 index, IPdfPageV** page) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PageCount(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPasswordProtected(boolean* v) = 0;
};
// Windows.Data.Pdf.IPdfDocumentStatics {433A0B5F-C007-4788-90F2-08143D922599}
const GUID kIID_IPdfDocumentStatics = {0x433a0b5f, 0xc007, 0x4788, {0x90, 0xf2, 0x08, 0x14, 0x3d, 0x92, 0x25, 0x99}};
struct IPdfDocumentStaticsV : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileAsync(IUnknown* file, IAsyncOpPdfDocV** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileWithPasswordAsync(IUnknown* file, HSTR password, IAsyncOpPdfDocV** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamAsync(IUnknown* input, IAsyncOpPdfDocV** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamWithPasswordAsync(IUnknown* input, HSTR password, IAsyncOpPdfDocV** op) = 0;
};
// Windows.Storage.Streams.IRandomAccessStream {905A0FE1-BC53-11DF-8C49-001E4FC686DA}
const GUID kIID_IRandomAccessStream = {0x905a0fe1, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};

// ---- run-time entry points ----------------------------------------------------------------------------
struct Api {
    HRESULT(WINAPI* RoInitialize)(int) = nullptr;
    void(WINAPI* RoUninitialize)() = nullptr;
    HRESULT(WINAPI* RoGetActivationFactory)(HSTR, REFIID, void**) = nullptr;
    HRESULT(WINAPI* RoActivateInstance)(HSTR, IInspectable**) = nullptr;
    HRESULT(WINAPI* WindowsCreateString)(PCWSTR, UINT32, HSTR*) = nullptr;
    HRESULT(WINAPI* WindowsDeleteString)(HSTR) = nullptr;
    HRESULT(WINAPI* CoIncrementMTAUsage)(void**) = nullptr;
    HRESULT(WINAPI* CreateRandomAccessStreamOverStream)(IStream*, int, REFIID, void**) = nullptr;
    bool ok = false;
    std::string why;
};
const Api& api() {
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE cb = LoadLibraryW(L"combase.dll");
        HMODULE sc = LoadLibraryW(L"shcore.dll");
        if (!cb) { a.why = "combase.dll not present (Windows 8 or later required)"; return; }
        auto g = [](HMODULE m, const char* n) { return m ? (void*)GetProcAddress(m, n) : nullptr; };
        a.RoInitialize = (decltype(a.RoInitialize))g(cb, "RoInitialize");
        a.RoUninitialize = (decltype(a.RoUninitialize))g(cb, "RoUninitialize");
        a.RoGetActivationFactory = (decltype(a.RoGetActivationFactory))g(cb, "RoGetActivationFactory");
        a.RoActivateInstance = (decltype(a.RoActivateInstance))g(cb, "RoActivateInstance");
        a.WindowsCreateString = (decltype(a.WindowsCreateString))g(cb, "WindowsCreateString");
        a.WindowsDeleteString = (decltype(a.WindowsDeleteString))g(cb, "WindowsDeleteString");
        a.CoIncrementMTAUsage = (decltype(a.CoIncrementMTAUsage))g(cb, "CoIncrementMTAUsage");
        a.CreateRandomAccessStreamOverStream = (decltype(a.CreateRandomAccessStreamOverStream))g(sc, "CreateRandomAccessStreamOverStream");
        if (!a.RoInitialize || !a.RoGetActivationFactory || !a.RoActivateInstance || !a.WindowsCreateString || !a.WindowsDeleteString) { a.why = "WinRT activation functions are missing"; return; }
        if (!a.CreateRandomAccessStreamOverStream) { a.why = "shcore.dll CreateRandomAccessStreamOverStream is missing"; return; }
        a.ok = true;
    });
    return a;
}

template <class T> struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() { if (p) p->Release(); }
    T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct HStr {
    HSTR h = nullptr;
    explicit HStr(const wchar_t* s) { api().WindowsCreateString(s, (UINT32)wcslen(s), &h); }
    ~HStr() { if (h) api().WindowsDeleteString(h); }
};

// The calling (worker) thread joins the MTA for the duration of a call; the MTA itself is kept
// alive for the process (CoIncrementMTAUsage), so WinRT objects survive between worker threads.
struct MtaScope {
    bool init = false;
    MtaScope() {
        static std::once_flag keep;
        std::call_once(keep, [] { void* cookie = nullptr; if (api().CoIncrementMTAUsage) api().CoIncrementMTAUsage(&cookie); });
        HRESULT hr = api().RoInitialize(1 /*RO_INIT_MULTITHREADED*/);
        init = SUCCEEDED(hr);
    }
    ~MtaScope() { if (init && api().RoUninitialize) api().RoUninitialize(); }
};

std::string hr_text(HRESULT hr) {
    char b[64];
    std::snprintf(b, sizeof b, "0x%08lX", (unsigned long)hr);
    return b;
}

// Polls IAsyncInfo until the operation leaves "Started" (no completion handler: no apartment marshaling).
HRESULT wait(IUnknown* op, DWORD timeout_ms = 60000) {
    Ref<IAsyncInfoV> info;
    HRESULT hr = op->QueryInterface(kIID_IAsyncInfo, (void**)info.put());
    if (FAILED(hr)) return hr;
    const ULONGLONG t0 = GetTickCount64();
    int st = AS_Started;
    for (;;) {
        if (FAILED(hr = info->get_Status(&st))) return hr;
        if (st != AS_Started) break;
        if (GetTickCount64() - t0 > timeout_ms) { info->Cancel(); return HRESULT_FROM_WIN32(ERROR_TIMEOUT); }
        Sleep(2);
    }
    if (st == AS_Completed) return S_OK;
    if (st == AS_Canceled) return E_ABORT;
    HRESULT ec = E_FAIL;
    info->get_ErrorCode(&ec);
    return FAILED(ec) ? ec : E_FAIL;
}

bool decode_png(const std::vector<uint8_t>& png, std::vector<uint8_t>& bgra, int& w, int& h) {
    Ref<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)f.put()))) return false;
    Ref<IWICStream> st;
    if (FAILED(f->CreateStream(st.put())) || FAILED(st->InitializeFromMemory((BYTE*)png.data(), (DWORD)png.size()))) return false;
    Ref<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(st.p, nullptr, WICDecodeMetadataCacheOnDemand, dec.put()))) return false;
    Ref<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, fr.put()))) return false;
    UINT iw = 0, ih = 0;
    fr->GetSize(&iw, &ih);
    if (!iw || !ih) return false;
    Ref<IWICFormatConverter> cv;
    if (FAILED(f->CreateFormatConverter(cv.put())) ||
        FAILED(cv->Initialize(fr.p, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    w = (int)iw; h = (int)ih;
    bgra.resize((size_t)iw * ih * 4);
    return SUCCEEDED(cv->CopyPixels(nullptr, iw * 4, (UINT)bgra.size(), bgra.data()));
}

} // namespace winrt_abi
using namespace winrt_abi;

bool available(std::string* why) {
    const Api& a = api();
    if (!a.ok) { if (why) *why = a.why; return false; }
    static int cached = -1;
    static std::string cached_why;
    static std::mutex mx;
    std::lock_guard<std::mutex> l(mx);
    if (cached < 0) {
        MtaScope mta;
        HStr cls(L"Windows.Data.Pdf.PdfDocument");
        Ref<IPdfDocumentStaticsV> st;
        HRESULT hr = a.RoGetActivationFactory(cls.h, kIID_IPdfDocumentStatics, (void**)st.put());
        cached = SUCCEEDED(hr) && st ? 1 : 0;
        if (!cached) cached_why = "Windows.Data.Pdf is not available on this system (" + hr_text(hr) + ")";
    }
    if (!cached && why) *why = cached_why;
    return cached == 1;
}

struct Document::Impl {
    IPdfDocumentV* doc = nullptr;
    std::vector<SizeF> sizes;
    ~Impl() { if (doc) doc->Release(); }
};

Document::Document() : p_(std::make_unique<Impl>()) {}
Document::~Document() = default;

int Document::page_count() const { return (int)p_->sizes.size(); }
bool Document::page_size(int i, float& w, float& h) const {
    if (i < 0 || i >= (int)p_->sizes.size()) return false;
    w = p_->sizes[(size_t)i].Width; h = p_->sizes[(size_t)i].Height;
    return true;
}

bool Document::open(const std::vector<uint8_t>& bytes, std::string& err) {
    if (!available(&err)) return false;
    const Api& a = api();
    MtaScope mta;
    HStr cls(L"Windows.Data.Pdf.PdfDocument");
    Ref<IPdfDocumentStaticsV> st;
    HRESULT hr = a.RoGetActivationFactory(cls.h, kIID_IPdfDocumentStatics, (void**)st.put());
    if (FAILED(hr)) { err = "PdfDocument factory: " + hr_text(hr); return false; }
    Ref<IStream> ms;
    ms.p = SHCreateMemStream(bytes.data(), (UINT)bytes.size());
    if (!ms) { err = "out of memory"; return false; }
    Ref<IUnknown> ras;
    hr = a.CreateRandomAccessStreamOverStream(ms.p, 0 /*BSOS_DEFAULT*/, kIID_IRandomAccessStream, (void**)ras.put());
    if (FAILED(hr)) { err = "stream adapter: " + hr_text(hr); return false; }
    Ref<IAsyncOpPdfDocV> op;
    hr = st->LoadFromStreamAsync(ras.p, op.put());
    if (FAILED(hr) || !op) { err = "LoadFromStreamAsync: " + hr_text(hr); return false; }
    hr = wait(op.p);
    if (FAILED(hr)) { err = hr == (HRESULT)0x80070005 ? "the PDF is password protected" : "the PDF could not be read (" + hr_text(hr) + ")"; return false; }
    IPdfDocumentV* d = nullptr;
    hr = op->GetResults(&d);
    if (FAILED(hr) || !d) { err = "PdfDocument result: " + hr_text(hr); return false; }
    if (p_->doc) p_->doc->Release();
    p_->doc = d;
    UINT32 n = 0;
    d->get_PageCount(&n);
    p_->sizes.clear();
    for (UINT32 i = 0; i < n && i < 100000; ++i) {
        Ref<IPdfPageV> pg;
        SizeF s{612.f * 96 / 72, 792.f * 96 / 72};   // letter, if a page cannot be queried
        if (SUCCEEDED(d->GetPage(i, pg.put())) && pg) pg->get_Size(&s);
        if (s.Width <= 0 || s.Height <= 0) s = SizeF{816, 1056};
        p_->sizes.push_back(s);
    }
    if (p_->sizes.empty()) { err = "the PDF has no pages"; return false; }
    return true;
}

bool Document::render(int index, int width_px, std::vector<uint8_t>& bgra, int& w, int& h, std::string& err) {
    if (!p_->doc || index < 0 || index >= page_count()) { err = "no such page"; return false; }
    const Api& a = api();
    MtaScope mta;
    Ref<IPdfPageV> pg;
    HRESULT hr = p_->doc->GetPage((UINT32)index, pg.put());
    if (FAILED(hr) || !pg) { err = "GetPage: " + hr_text(hr); return false; }
    // options: destination width (height follows the aspect ratio), white background
    Ref<IPdfPageRenderOptionsV> opt;
    {
        HStr cls(L"Windows.Data.Pdf.PdfPageRenderOptions");
        Ref<IInspectable> ins;
        if (SUCCEEDED(a.RoActivateInstance(cls.h, ins.put())) && ins) ins->QueryInterface(kIID_IPdfPageRenderOptions, (void**)opt.put());
    }
    const SizeF ps = p_->sizes[(size_t)index];
    width_px = std::clamp(width_px, 16, 8192);
    const UINT32 hpx = (UINT32)std::clamp((double)width_px * ps.Height / std::max(1.f, ps.Width), 16.0, 16384.0);
    if (opt) {
        opt->put_DestinationWidth((UINT32)width_px);
        opt->put_DestinationHeight(hpx);
        opt->put_BackgroundColor(ColorU{255, 255, 255, 255});
    }
    Ref<IStream> out;
    out.p = SHCreateMemStream(nullptr, 0);
    if (!out) { err = "out of memory"; return false; }
    std::vector<uint8_t> png;
    {
        Ref<IUnknown> ras;
        hr = a.CreateRandomAccessStreamOverStream(out.p, 0, kIID_IRandomAccessStream, (void**)ras.put());
        if (FAILED(hr)) { err = "stream adapter: " + hr_text(hr); return false; }
        Ref<IAsyncActionV> act;
        hr = opt ? pg->RenderWithOptionsToStreamAsync(ras.p, opt.p, act.put()) : pg->RenderToStreamAsync(ras.p, act.put());
        if (FAILED(hr) || !act) { err = "RenderToStreamAsync: " + hr_text(hr); return false; }
        hr = wait(act.p);
        if (SUCCEEDED(hr)) hr = act->GetResults();
        if (FAILED(hr)) { err = "page render failed (" + hr_text(hr) + ")"; return false; }
    }
    STATSTG stg{};
    if (FAILED(out->Stat(&stg, STATFLAG_NONAME)) || stg.cbSize.QuadPart == 0 || stg.cbSize.QuadPart > (1ull << 31)) { err = "empty render output"; return false; }
    png.resize((size_t)stg.cbSize.QuadPart);
    LARGE_INTEGER zero{};
    out->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG got = 0;
    out->Read(png.data(), (ULONG)png.size(), &got);
    png.resize(got);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // WIC on this worker (already MTA: S_FALSE)
    const bool ok = decode_png(png, bgra, w, h);
    CoUninitialize();
    if (!ok) { err = "could not decode the rendered page"; return false; }
    return true;
}

} // namespace pcbp::platform::pdf
