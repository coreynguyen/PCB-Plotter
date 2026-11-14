#pragma once
// ============================================================================
// pdf_winrt.h — R20 PDF page rendering through the Windows built-in WinRT API Windows.Data.Pdf
// (PdfDocument -> PdfPage::RenderWithOptionsToStreamAsync), Windows 8.1+.
//
// Deps-free: combase.dll / shcore.dll entry points are resolved at run time (GetProcAddress) and
// the few WinRT interfaces are declared by hand (mingw-w64 ships no windows.data.pdf.h). IIDs and
// vtable order come from the Windows SDK metadata (Windows.Data.Pdf.winmd, as generated into the
// official windows-rs bindings). Where WinRT or the PDF component is missing (older Windows, Wine)
// available() is false and callers show a fallback ("Open externally").
//
// All calls block: run them on a worker thread (each call initialises the thread for the MTA).
// The output pixels are 32-bit premultiplied BGRA (WIC-decoded PNG from the render stream).
// ============================================================================
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pcbp::platform::pdf {

// true when Windows.Data.Pdf can be activated; `why` gets the reason otherwise.
bool available(std::string* why = nullptr);

class Document {
public:
    Document();
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    bool open(const std::vector<uint8_t>& bytes, std::string& err);   // whole PDF in memory
    int page_count() const;
    // Page size in DIPs (1/96 inch) as reported by PdfPage::Size.
    bool page_size(int index, float& w, float& h) const;
    // Render page `index` scaled to `width_px` wide (height keeps the aspect ratio).
    bool render(int index, int width_px, std::vector<uint8_t>& bgra, int& w, int& h, std::string& err);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace pcbp::platform::pdf
