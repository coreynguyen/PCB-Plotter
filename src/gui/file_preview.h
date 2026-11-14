#pragma once
// ============================================================================
// file_preview.h — R20 Files tab preview modal (file_preview.cpp).
//
// One resizable / movable modal window (Esc closes) that previews an archive entry from its BYTES:
//   PDF      Windows.Data.Pdf (platform/pdf_winrt) pages -> D3D11 textures, zoom, page navigation;
//            clean fallback (+ Open externally) where WinRT PDF is unavailable (old Windows, Wine)
//   DOCX/RTF/HTML  io::docx_read / rtf / html -> RichView (doc_viewer.cpp), zoom
//   EML/MSG  the e-mail viewer (doc_viewer.cpp)
//   XLSX/XLS/ODS/CSV/TSV  io::load_sheet_preview -> read-only grid, sheet tabs, zoom
//   images   WIC -> texture, zoom / pan
//   Gerber / Excellon  gerber::parse -> vector render, zoom / pan (+ Import as)
//   text and anything else: text view with a hex fallback
// Nothing goes through the spreadsheet workspace or File > Open, and nothing external is launched
// unless the user presses "Open externally" (the caller's callback).
// ============================================================================
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pcbp::gui::preview {

enum class Kind { Text, Image, Pdf, Doc, Mail, Sheet, Gerber };
Kind kind_for(const std::string& name, const std::vector<uint8_t>& bytes, int import_kinds);
const char* kind_label(Kind k);

struct Request {
    std::string name;                         // file name (title)
    std::string where;                        // archive path shown under the title
    std::vector<uint8_t> bytes;
    int import_kinds = 0;                     // jobzip Imp* bits (Import as menu)
    std::function<void()> extract;            // "Extract..."
    std::function<void()> open_external;      // "Open externally" (the only way an external app starts)
    std::function<void(int)> import_as;       // jobzip Imp* bit
};

void open(Request r);     // shows the modal from the next draw()
void draw();              // call once per frame (Files view); no-op when closed
bool is_open();
void close();

// WIC decode -> premultiplied BGRA (shared with the Files tab inline preview)
bool decode_image(const std::vector<uint8_t>& data, int max_dim, int& w, int& h, std::vector<uint8_t>& px);

} // namespace pcbp::gui::preview
