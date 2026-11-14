// ============================================================================
// test_files_preview.cpp — R20 Files tab (specs/DOCUMENTS.md "R20").
//   * DOCX extractor (io::docx_read) on a synthetic .docx built here: heading, bold / italic runs, table,
//     embedded image
//   * read-only spreadsheet preview loader (io::load_sheet_preview) on a synthetic .xlsx (written by the
//     sheet engine), CSV (auto separator) and TSV; non-spreadsheet bytes are refused
//   * file classes / DOS date text used by the explorer (core/job_archive)
//   * open-dialog loop regression: (a) Dear ImGui keeps Ctrl+O "down" after a focus-lost + focus-regained
//     pair in one frame, so IsKeyPressed(O, repeat) re-fires = the reported loop; imgui_drop_stale_keys()
//     (shell, on WM_SETFOCUS) stops it; (b) DialogLoopGuard never re-shows a cancelled dialog without
//     fresh user input and never re-enters an open one.
// Synthetic fixtures only.
// ============================================================================
#include "../src/core/dialog_guard.h"
#include "../src/core/job_archive.h"
#include "../src/core/sheet.h"
#include "../src/gui/input_resync.h"
#include "../src/io/docx.h"
#include "../src/io/richdoc.h"
#include "../src/io/sheet_preview.h"
#include "../src/io/xlsx.h"
#include "imgui.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace pcbp;

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<uint8_t> B(const std::string& s) { return {s.begin(), s.end()}; }

// 1x1 PNG (red pixel)
static const unsigned char kPng[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01,
                                     0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,
                                     0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB0, 0x00, 0x00, 0x00,
                                     0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

static std::vector<uint8_t> make_docx() {
    const std::string W = "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
                          "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                          "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
                          "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                          "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"";
    std::string doc =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document " + W + "><w:body>"
        "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/></w:pPr><w:r><w:t>Assembly notes</w:t></w:r></w:p>"
        "<w:p><w:r><w:t xml:space=\"preserve\">Plain then </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>bold</w:t></w:r>"
        "<w:r><w:t xml:space=\"preserve\"> and </w:t></w:r><w:r><w:rPr><w:i/></w:rPr><w:t>italic</w:t></w:r></w:p>"
        "<w:tbl><w:tr><w:trPr><w:tblHeader/></w:trPr><w:tc><w:p><w:r><w:t>Ref</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Qty</w:t></w:r></w:p></w:tc></w:tr>"
        "<w:tr><w:tc><w:p><w:r><w:t>C1, C2</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>2</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"9525\" cy=\"9525\"/><a:graphic><a:graphicData><pic:pic><pic:blipFill>"
        "<a:blip r:embed=\"rId5\"/></pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"Heading2\"/></w:pPr><w:r><w:t>Second heading</w:t></w:r></w:p>"
        "<w:sectPr/></w:body></w:document>";
    std::string styles = "<?xml version=\"1.0\"?><w:styles " + W + ">"
                         "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/></w:style>"
                         "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/></w:style></w:styles>";
    std::string rels = "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                       "<Relationship Id=\"rId5\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/image1.png\"/></Relationships>";
    std::string root_rels = "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/></Relationships>";
    std::string ct = "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                     "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                     "<Default Extension=\"png\" ContentType=\"image/png\"/>"
                     "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>";
    std::string png((const char*)kPng, sizeof kPng);
    return io::zip_write({{"[Content_Types].xml", ct}, {"_rels/.rels", root_rels}, {"word/document.xml", doc}, {"word/styles.xml", styles},
                          {"word/_rels/document.xml.rels", rels}, {"word/media/image1.png", png}});
}

static void test_docx() {
    auto bytes = make_docx();
    CHECK(io::is_docx_bytes(bytes));
    io::RichDoc d;
    std::string err;
    CHECK(io::docx_read(bytes, d, err));
    if (!err.empty()) std::printf("  docx err: %s\n", err.c_str());
    // paragraphs: heading, mixed runs, table block, image paragraph, heading 2
    CHECK(d.paras.size() == 5);
    if (d.paras.size() < 5) return;
    CHECK(d.paras[0].heading == 1);
    CHECK(d.paras[0].text() == "Assembly notes");
    CHECK(d.paras[1].text() == "Plain then bold and italic");
    bool bold = false, italic = false, plain_not_bold = false;
    for (auto& r : d.paras[1].runs) {
        if (r.text == "bold") bold = r.b && !r.i;
        if (r.text == "italic") italic = r.i && !r.b;
        if (r.text.rfind("Plain", 0) == 0) plain_not_bold = !r.b && !r.i;
    }
    CHECK(bold); CHECK(italic); CHECK(plain_not_bold);
    CHECK(d.paras[2].table == 0);
    CHECK(d.tables.size() == 1);
    if (!d.tables.empty()) {
        const auto& t = d.tables[0];
        CHECK(t.rows.size() == 2);
        CHECK(t.rows[0].size() == 2 && t.rows[0][0].header);
        CHECK(t.rows[0][0].paras.size() == 1 && t.rows[0][0].paras[0].text() == "Ref");
        CHECK(t.rows[1][0].paras[0].text() == "C1, C2");
        CHECK(t.rows[1][1].paras[0].text() == "2");
    }
    CHECK(d.images.size() == 1);
    if (!d.images.empty()) {
        CHECK(d.images[0].mime == "image/png");
        CHECK(d.images[0].data.size() == sizeof kPng);
    }
    bool img_run = false;
    for (auto& r : d.paras[3].runs) if (r.image == 0) img_run = true;
    CHECK(img_run);
    CHECK(d.paras[4].heading == 2);
    // not a docx
    io::RichDoc d2;
    CHECK(!io::docx_read(B("PK\x03\x04garbage"), d2, err));
}

static void test_sheet_preview() {
    sheet::Book bk;
    bk.set_input(0, 0, 0, "Item"); bk.set_input(0, 0, 1, "Qty"); bk.set_input(0, 0, 2, "Ref"); bk.set_input(0, 0, 3, "Value");
    bk.set_input(0, 1, 0, "1"); bk.set_input(0, 1, 1, "2"); bk.set_input(0, 1, 2, "C1,C2"); bk.set_input(0, 1, 3, "0.1");
    bk.set_input(0, 2, 0, "2"); bk.set_input(0, 2, 1, "1"); bk.set_input(0, 2, 2, "R7"); bk.set_input(0, 2, 3, "=B2+B3");
    bk.sheets[0].name = "BOM";
    bk.add_sheet("Notes");
    bk.set_input(1, 0, 0, "rev B");
    auto x = io::xlsx_write(bk);
    io::SheetPreview sp;
    std::string err;
    CHECK(io::load_sheet_preview(x, "BOM 1234.xlsx", sp, err));
    if (!err.empty()) std::printf("  xlsx err: %s\n", err.c_str());
    CHECK(sp.tabs.size() == 2);
    if (sp.tabs.size() == 2) {
        const auto& t = sp.tabs[0];
        CHECK(t.name == "BOM");
        CHECK(t.rows == 3 && t.cols == 4);
        CHECK(t.at(0, 2) == "Ref");
        CHECK(t.at(1, 2) == "C1,C2");
        CHECK(t.at(1, 3) == "0.1");
        CHECK(t.right[(size_t)1 * t.cols + 3] == 1);    // number: right aligned
        CHECK(t.right[(size_t)1 * t.cols + 2] == 0);    // text: left
        CHECK(t.at(2, 3) == "3");                       // formula: cached / computed value, not the formula text
        CHECK(!t.truncated());
        CHECK(sp.tabs[1].name == "Notes" && sp.tabs[1].at(0, 0) == "rev B");
    }
    CHECK(sp.how.find("xlsx") != std::string::npos);
    // limits
    io::SheetPreview small;
    CHECK(io::load_sheet_preview(x, "a.xlsx", small, err, 2, 2));
    CHECK(!small.tabs.empty() && small.tabs[0].rows == 2 && small.tabs[0].cols == 2 && small.tabs[0].truncated());
    // CSV (auto separator ';') and TSV
    io::SheetPreview c;
    CHECK(io::load_sheet_preview(B("Ref;Qty;Part\nR1;2;RC0603\nR2;1;RC0805\n"), "Bill of materials.csv", c, err));
    CHECK(c.tabs.size() == 1 && c.tabs[0].cols == 3 && c.tabs[0].rows == 3 && c.tabs[0].at(2, 2) == "RC0805");
    CHECK(c.tabs.size() == 1 && c.tabs[0].name == "Bill of materials");
    io::SheetPreview tsv;
    CHECK(io::load_sheet_preview(B("A\tB\n1\t2\n"), "x.tsv", tsv, err) && tsv.tabs[0].at(1, 1) == "2");
    // not a spreadsheet
    io::SheetPreview bad;
    CHECK(!io::load_sheet_preview(B("%PDF-1.4 ..."), "x.pdf", bad, err));
    CHECK(!io::load_sheet_preview({}, "x.xlsx", bad, err));
}

static void test_classes() {
    using namespace io::jobzip;
    CHECK(file_class("Datasheets/LM358.PDF") == FileClass::Pdf);
    CHECK(file_class("Drawings/assy.docx") == FileClass::Doc);
    CHECK(file_class("x.xlsx") == FileClass::Sheet);
    CHECK(file_class("Bill of Materials/BOM rev B.xlsx") == FileClass::Bom);
    CHECK(file_class("pnp.csv") == FileClass::Csv);
    CHECK(file_class("Gerbers rev A.zip") == FileClass::Zip);
    CHECK(file_class("top.GTL") == FileClass::Gerber);
    CHECK(file_class("panel.gbr") == FileClass::Gerber);
    CHECK(file_class("holes.drl") == FileClass::Drill);
    CHECK(file_class("photo.jpg") == FileClass::Image);
    CHECK(file_class("Re rev B.msg") == FileClass::Email);
    CHECK(file_class("board.kicad_pcb") == FileClass::Cad);
    CHECK(file_class("readme.txt") == FileClass::Text);
    CHECK(file_class("blob.bin") == FileClass::Generic);
    CHECK(file_class("Folder/") == FileClass::Generic);
    // 2026-11-12 13:37:20 -> DOS date ((46<<9)|(11<<5)|12), time ((13<<11)|(37<<5)|10)
    CHECK(dos_datetime_text((uint16_t)((46 << 9) | (11 << 5) | 12), (uint16_t)((13 << 11) | (37 << 5) | 10)) == "2026-11-12 13:37");
    CHECK(dos_datetime_text(0, 0).empty());
}

// ---- loop guard ----------------------------------------------------------------------------------------------
// One simulated "Ctrl+O -> modal file dialog -> Cancel" cycle in a headless ImGui context, then 2 s of frames.
// Returns how many times the editor's Ctrl+O handler would have re-posted File > Open.
static int simulate_ctrl_o(bool resync) {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.f / 60.f;
    io.IniFilename = nullptr;
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    auto frame = [&](bool keys_stale) {
        if (keys_stale && resync) gui::imgui_drop_stale_keys(io);
        ImGui::NewFrame();
        // sheet_grid.cpp / text_editor.cpp before R20: if (ctrl && IsKeyPressed(ImGuiKey_O, repeat=true)) post ID_FILE_OPEN
        const int fired = (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, true)) ? 1 : 0;
        ImGui::Render();
        return fired;
    };
    frame(false);
    // user presses Ctrl+O
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_LeftCtrl, true);
    io.AddKeyEvent(ImGuiKey_O, true);
    int first = frame(false);
    // the command opens the modal dialog between frames: WM_KILLFOCUS, the key-ups go to the dialog, Cancel, WM_SETFOCUS
    io.AddFocusEvent(false);
    io.AddFocusEvent(true);
    int again = 0;
    for (int i = 0; i < 120; ++i) again += frame(i == 0);
    ImGui::DestroyContext(ctx);
    return first == 1 ? again : -1;
}

static void test_loop_guard() {
    const int without = simulate_ctrl_o(false);
    const int with = simulate_ctrl_o(true);
    std::printf("  Ctrl+O re-fires after Cancel: %d without resync, %d with resync\n", without, with);
    CHECK(without > 5);   // the reported loop: the stale key keeps auto-repeating
    CHECK(with == 0);
    // dialog-level guard
    DialogLoopGuard g;
    CHECK(g.begin("Open"));
    CHECK(!g.begin("Open"));                 // re-entry while showing
    g.end("Open", true);                     // Cancel
    CHECK(!g.begin("Open"));                 // re-trigger without new input: suppressed
    CHECK(!g.begin("Open"));
    CHECK(g.begin("Import BOM"));            // another dialog is not affected
    g.end("Import BOM", false);
    CHECK(!g.begin("Open"));                 // still no input since the Open cancel
    g.note_input();                          // the user clicks / presses a key
    CHECK(g.begin("Open"));
    g.end("Open", false);                    // a file was chosen
    CHECK(g.begin("Open"));                  // ok without new input after a non-cancel close
    g.end("Open", true);
    CHECK(g.blocked() == 4);
}

int main() {
    test_docx();
    test_sheet_preview();
    test_classes();
    test_loop_guard();
    std::printf("test_files_preview: %d/%d checks passed\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
