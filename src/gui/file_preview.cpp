// ============================================================================
// file_preview.cpp — R20 Files tab preview modal. See file_preview.h.
// R22c: Markdown (.md / .markdown) is rendered (headings, bold, code, links, lists, quotes, code blocks, rules);
// "Show source" toggles the raw line view.
// ============================================================================
#include "file_preview.h"
#include "file_editors.h"
#include "gapp.h"
#include "gfx.h"
#include "../core/job_archive.h"
#include "../io/gerber.h"
#include "../io/sheet_preview.h"
#include "../io/xlsx.h"
#include "../platform/pdf_winrt.h"
#include "../platform/crash.h"
#include "../ui/win.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace pcbp::gui { HWND main_hwnd(); }

namespace pcbp::gui::preview {

namespace jz = pcbp::io::jobzip;

namespace {

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string ext_of(const std::string& n) { size_t d = n.find_last_of('.'); return d == std::string::npos ? std::string() : lower(n.substr(d + 1)); }

bool is_binary(const std::vector<uint8_t>& b) {
    const size_t n = std::min<size_t>(b.size(), 65536);
    if (n >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) return false;   // UTF-16 text
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i) { uint8_t c = b[i]; if (c == 0) return true; if (c < 32 && c != '\t' && c != '\r' && c != '\n' && c != '\f' && c != 0x1A && c != 0x1B) ++bad; }
    return bad * 50 > n;
}

// ---- async job helper: work runs on a worker thread, the UI polls `done` --------------------------------
struct Job {
    std::atomic<bool> done{false};
    std::string err;
    std::function<void(Job&)> work;
};
std::shared_ptr<Job> start(std::function<void(Job&)> work) {
    auto j = std::make_shared<Job>();
    j->work = std::move(work);
    std::thread([j]() {
        // R23a: C++ exceptions AND hardware faults (crash::guarded) end the job with an error card
        unsigned long code = 0;
        Job* jp = j.get();
        const bool ok = crash::guarded([jp] {
            try { jp->work(*jp); } catch (const std::exception& e) { jp->err = e.what(); } catch (...) { jp->err = "unexpected error"; }
        }, &code);
        if (!ok) j->err = "the preview failed (" + crash::exception_name(code) + "); the file itself is untouched";
        j->done = true;
        if (HWND h = main_hwnd()) PostMessageW(h, WM_NULL, 0, 0);   // wake the UI thread (request_redraw is UI-only)
    }).detach();
    return j;
}

// ---- per-kind state --------------------------------------------------------------------------------------
struct PdfState {
    std::shared_ptr<platform::pdf::Document> doc = std::make_shared<platform::pdf::Document>();
    std::shared_ptr<Job> job;                 // open or render in flight
    bool opened = false, unavailable = false;
    std::string why;                          // fallback / error text
    int page = 0, pages = 0;
    int tex_page = -1, tex_width = 0;         // what the texture holds
    StaticTexture tex;
    // render result handed from the worker
    std::shared_ptr<std::vector<uint8_t>> px = std::make_shared<std::vector<uint8_t>>();
    std::shared_ptr<int> pw = std::make_shared<int>(0), ph = std::make_shared<int>(0);
    int job_page = -1, job_width = 0;
    std::shared_ptr<bool> unav_flag;
};

struct FindBar {                              // R23a: Ctrl+F bar shared by the sheet and text views
    bool open = false, focus = false, match_case = false;
    char q[256] = {};
    std::string last;                          // query the counts were made for
    bool last_case = false;
    int matches = 0;
    bool none = false;                         // last search found nothing
};

struct GerberState {
    std::shared_ptr<gerber::Layer> layer;
    std::shared_ptr<Job> job;
    bool ok = false;
    ImVec2 pan{0, 0};
    float zoom = 1.f;                          // 1 = fit
};

struct SheetState {
    std::shared_ptr<io::SheetPreview> sp;
    std::shared_ptr<Job> job;
    bool ok = false;
    int tab = 0;
    // R23a selection: anchor (ar, ac) .. cursor (cr, cc), inclusive; -1 = none
    int ar = -1, ac = -1, cr = -1, cc = -1;
    bool dragging = false;
    int drag_kind = 0;                         // 0 cells, 1 rows (row header), 2 columns (column header)
    int scroll_r = -1, scroll_c = -1;          // bring this cell into view
    int seen_tab = -1;
    FindBar find;
};

struct MdBlock {
    enum Type { Para, Head, Bullet, Number, Quote, Code, Rule, Blank } type = Para;
    int level = 0;          // heading 1..6; list indent level
    std::string mark;       // numbered list: "3."
    std::string text;
};
struct TextState {
    bool hex = false, binary = false, built = false;
    std::vector<std::string> lines;
    bool md = false, md_src = false, md_built = false;   // R22c: Markdown rendered (md_src: show the raw lines)
    std::vector<MdBlock> blocks;
    // R23a: line selection (sa..sb inclusive, -1 none), find
    int sa = -1, sb = -1;
    bool drag = false;
    int scroll_line = -1;
    FindBar find;
};

struct State {
    Request rq;
    Kind kind = Kind::Text;
    bool want_open = true;
    float zoom = 1.f;
    bool fit = true;
    // image
    StaticTexture img;
    bool img_tried = false;
    std::string img_err;
    // others
    PdfState pdf;
    GerberState gbr;
    SheetState sheet;
    TextState text;
    std::unique_ptr<FileDoc> doc;             // docx / rtf / html / mail
    float* doc_zoom = nullptr;
    std::string doc_err;
    bool doc_tried = false;
    std::string fatal;                        // R23a: a draw path faulted -> error card from then on
};
std::unique_ptr<State> g;
std::unique_ptr<State> g_dead;   // closed this frame: its textures may still be in this frame's draw data
bool g_close = false;

// ---- shared canvas: centred bitmap with zoom (Ctrl+wheel) and scrollbars ---------------------------------
void bitmap_canvas(State& s, StaticTexture& tex, float src_w, float src_h, ImVec2 size) {
    const GTheme& t = th();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.dark ? IM_COL32(40, 40, 44, 255) : IM_COL32(222, 224, 228, 255));
    ImGui::BeginChild("##pvcanvas", size, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleColor();
    const ImVec2 av = ImGui::GetContentRegionAvail();
    if (s.fit && src_w > 0 && src_h > 0) s.zoom = std::clamp(std::min((av.x - S(32)) / src_w, (av.y - S(40)) / src_h), 0.05f, 8.f);
    const ImVec2 d(std::max(1.f, src_w * s.zoom), std::max(1.f, src_h * s.zoom));
    const ImVec2 pad(std::max(S(12), (av.x - d.x) * 0.5f), std::max(S(12), (av.y - d.y) * 0.5f));
    ImGui::SetCursorPos(pad);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (tex.imgui_id()) {
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p0.x + S(3), p0.y + S(3)), ImVec2(p0.x + d.x + S(3), p0.y + d.y + S(3)), IM_COL32(0, 0, 0, 60));
        ImGui::Image((ImTextureID)tex.imgui_id(), d);
    } else ImGui::Dummy(d);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + d.x, p0.y + d.y));
    ImGui::Dummy(ImVec2(std::max(1.f, pad.x - S(8)), std::max(1.f, pad.y - S(8))));
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0) { s.fit = false; s.zoom = std::clamp(s.zoom * (io.MouseWheel > 0 ? 1.15f : 1 / 1.15f), 0.05f, 8.f); request_redraw(2); }
    // drag to pan (middle or left button)
    if (ImGui::IsWindowHovered() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Left))) {
        ImVec2 dd = io.MouseDelta;
        ImGui::SetScrollX(ImGui::GetScrollX() - dd.x);
        ImGui::SetScrollY(ImGui::GetScrollY() - dd.y);
    }
    ImGui::EndChild();
}

// ---- image -------------------------------------------------------------------------------------------------
void draw_image(State& s, ImVec2 size) {
    if (!s.img_tried) {
        s.img_tried = true;
        std::vector<uint8_t> px; int w = 0, h = 0;
        if (!decode_image(s.rq.bytes, 4096, w, h, px) || !s.img.create(w, h, px.data(), w * 4)) s.img_err = "This image format cannot be decoded.";
        else { s.img.w = w; s.img.h = h; }
    }
    if (!s.img_err.empty()) { empty_state(ui::Icon::FileImage, "No preview", s.img_err.c_str()); return; }
    bitmap_canvas(s, s.img, (float)s.img.w, (float)s.img.h, size);
}

// ---- PDF ---------------------------------------------------------------------------------------------------
void pdf_poll(State& s) {
    PdfState& p = s.pdf;
    if (!p.job) {
        if (!p.opened && !p.unavailable && p.why.empty()) {   // first frame: availability check + open, on a worker
            auto doc = p.doc;
            auto bytes = std::make_shared<std::vector<uint8_t>>(s.rq.bytes);
            auto unav = p.unav_flag = std::make_shared<bool>(false);
            p.job_page = -2;   // marks the open job
            p.job = start([doc, bytes, unav](Job& j) {
                std::string w;
                if (!platform::pdf::available(&w)) { *unav = true; j.err = w; return; }
                doc->open(*bytes, j.err);
            });
        }
        return;
    }
    if (!p.job->done) return;
    std::shared_ptr<Job> j = std::move(p.job);
    if (p.job_page == -2) {   // open finished
        if (p.unav_flag && *p.unav_flag) { p.unavailable = true; p.why = j->err; return; }
        if (!j->err.empty()) { p.why = j->err; return; }
        p.opened = true;
        p.pages = p.doc->page_count();
        return;
    }
    if (!j->err.empty()) { p.why = j->err; return; }
    if (!p.px->empty() && *p.pw > 0 && *p.ph > 0 && p.tex.create(*p.pw, *p.ph, p.px->data(), *p.pw * 4)) {
        p.tex.w = *p.pw; p.tex.h = *p.ph;
        p.tex_page = p.job_page; p.tex_width = p.job_width;
    }
}
void pdf_request(State& s, int width_px) {
    PdfState& p = s.pdf;
    if (p.job || !p.opened) return;
    if (p.tex_page == p.page && p.tex_width > 0 && std::abs(p.tex_width - width_px) < std::max(24, p.tex_width / 8)) return;
    auto doc = p.doc;
    auto px = p.px = std::make_shared<std::vector<uint8_t>>();
    auto pw = p.pw = std::make_shared<int>(0);
    auto ph = p.ph = std::make_shared<int>(0);
    const int page = p.page;
    width_px = std::clamp(width_px, 64, 4096);
    p.job_page = page; p.job_width = width_px;
    p.job = start([doc, px, pw, ph, page, width_px](Job& j) { doc->render(page, width_px, *px, *pw, *ph, j.err); });
}
void draw_pdf(State& s, ImVec2 size) {
    PdfState& p = s.pdf;
    pdf_poll(s);
    if (p.unavailable || (!p.opened && !p.job && !p.why.empty())) {
        ImGui::BeginChild("##pdffb", size);
        const float w = std::min(size.x - S(40), S(560));
        ImGui::SetCursorPos(ImVec2((size.x - w) * 0.5f, std::max(S(20), size.y * 0.25f)));
        ImGui::BeginGroup();
        icon_image(ui::Icon::FilePdf, S(56));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::PushFont(g_font_bold);
        ImGui::TextUnformatted(p.unavailable ? "The built-in PDF viewer is not available on this system" : "This PDF cannot be shown");
        ImGui::PopFont();
        if (p.unavailable)
            ImGui::TextDisabled("PCB Plotter renders PDF pages with the PDF component built into Windows (Windows.Data.Pdf, Windows 8.1 or later). "
                                "It could not be loaded here: %s", p.why.c_str());
        else ImGui::TextDisabled("%s", p.why.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("You can still open it in the PDF program installed on this PC, or extract it to a folder.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (button("Open externally", ui::Icon::Open, true) && s.rq.open_external) s.rq.open_external();
        ImGui::SameLine();
        if (button("Extract\xE2\x80\xA6", ui::Icon::Export) && s.rq.extract) s.rq.extract();
        ImGui::EndGroup();
        ImGui::EndChild();
        return;
    }
    if (!p.opened) {
        ImGui::BeginChild("##pdfwait", size);
        ImGui::SetCursorPos(ImVec2(size.x * 0.5f - S(60), size.y * 0.4f));
        spinner();
        ImGui::SameLine();
        ImGui::TextDisabled("Opening PDF\xE2\x80\xA6");
        ImGui::EndChild();
        request_redraw(2);
        return;
    }
    float pw = 0, ph = 0;
    p.doc->page_size(p.page, pw, ph);
    const float k = dpi_scale();
    const float src_w = pw * k, src_h = ph * k;   // 100 % = page size at screen DPI
    // render at the displayed resolution
    float zoom = s.zoom;
    if (s.fit) zoom = std::clamp(std::min((size.x - S(40)) / std::max(1.f, src_w), (size.y - S(40)) / std::max(1.f, src_h)), 0.05f, 8.f);
    pdf_request(s, (int)std::lround(src_w * zoom));
    if (p.tex_page != p.page && !p.tex.imgui_id()) {
        ImGui::BeginChild("##pdfwait", size);
        ImGui::SetCursorPos(ImVec2(size.x * 0.5f - S(60), size.y * 0.4f));
        spinner(); ImGui::SameLine(); ImGui::TextDisabled("Rendering page %d\xE2\x80\xA6", p.page + 1);
        ImGui::EndChild();
        return;
    }
    bitmap_canvas(s, p.tex, src_w, src_h, size);
    if (p.job) request_redraw(2);
}

// ---- documents / mail --------------------------------------------------------------------------------------
void draw_doc(State& s, ImVec2 size) {
    if (!s.doc_tried) {
        s.doc_tried = true;
        s.doc = make_preview_doc(s.rq.bytes, s.rq.name, s.doc_err, &s.doc_zoom);
        if (s.doc_zoom) *s.doc_zoom = s.zoom;
    }
    if (!s.doc) {
        empty_state(s.kind == Kind::Mail ? ui::Icon::FileEmail : ui::Icon::FileDoc, "No preview", (s.doc_err.empty() ? std::string("unsupported document") : s.doc_err).c_str());
        return;
    }
    if (s.doc_zoom) *s.doc_zoom = s.zoom;
    ImGui::BeginChild("##pvdoc", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    s.doc->draw(ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    if (s.doc_zoom) s.zoom = *s.doc_zoom;   // Ctrl+wheel inside the rich view
}

// ---- spreadsheet grid --------------------------------------------------------------------------------------
std::string col_name(int c) { std::string s; for (++c; c > 0; c = (c - 1) / 26) s.insert(s.begin(), (char)('A' + (c - 1) % 26)); return s; }
// ---- R23a: find bar (sheet + text views) --------------------------------------------------------------
// Returns +1 (next) / -1 (previous) when a search step was requested this frame, 0 otherwise.
int find_bar(FindBar& f, int total_matches_hint) {
    int step = 0;
    const GTheme& t = th();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Find");
    ImGui::SameLine();
    if (f.focus) { ImGui::SetKeyboardFocusHere(); f.focus = false; }
    ImGui::SetNextItemWidth(S(240));
    if (f.none) ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(230, 80, 60, 90));
    if (ImGui::InputTextWithHint("##findq", "text in any cell", f.q, sizeof f.q, ImGuiInputTextFlags_EnterReturnsTrue)) {
        step = ImGui::GetIO().KeyShift ? -1 : 1;
        f.focus = true;                         // keep typing / pressing Enter
    }
    if (f.none) ImGui::PopStyleColor();
    ImGui::SameLine();
    if (icon_button("##fprev", ui::Icon::ArrowLeft, "Previous match (Shift+F3 / Shift+Enter)", false, f.q[0] != 0)) step = -1;
    ImGui::SameLine(0, S(2));
    if (icon_button("##fnext", ui::Icon::ArrowRight, "Next match (F3 / Enter)", false, f.q[0] != 0)) step = 1;
    ImGui::SameLine();
    ImGui::Checkbox("Match case", &f.match_case);
    ImGui::SameLine();
    if (f.q[0]) {
        if (total_matches_hint == 0) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "No matches");
        else ImGui::TextDisabled("%d match%s", total_matches_hint, total_matches_hint == 1 ? "" : "es");
    }
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(26));
    if (icon_button("##fclose", ui::Icon::CloseDoc, "Close find (Esc)", false, true)) f.open = false;
    (void)t;
    return step;
}

void sheet_select_all(SheetState& st, const io::SheetPreview::Tab& tb) {
    if (tb.rows <= 0 || tb.cols <= 0) return;
    st.ar = 0; st.ac = 0; st.cr = tb.rows - 1; st.cc = tb.cols - 1;
}
bool sheet_has_sel(const SheetState& st) { return st.ar >= 0 && st.ac >= 0 && st.cr >= 0 && st.cc >= 0; }
void sheet_copy(const SheetState& st, const io::SheetPreview::Tab& tb) {
    if (!sheet_has_sel(st)) return;
    const std::string tsv = io::preview_tsv(tb, st.ar, st.ac, st.cr, st.cc);
    ImGui::SetClipboardText(tsv.c_str());
}

void draw_sheet(State& s, ImVec2 size) {
    SheetState& st = s.sheet;
    if (!st.job && !st.sp) {
        auto sp = std::make_shared<io::SheetPreview>();
        auto bytes = std::make_shared<std::vector<uint8_t>>(s.rq.bytes);
        std::string name = s.rq.name;
        st.sp = sp;
        st.job = start([sp, bytes, name](Job& j) { io::load_sheet_preview(*bytes, name, *sp, j.err); });
    }
    if (st.job) {
        if (!st.job->done) {
            ImGui::BeginChild("##shwait", size);
            ImGui::SetCursorPos(ImVec2(size.x * 0.5f - S(60), size.y * 0.4f));
            spinner(); ImGui::SameLine(); ImGui::TextDisabled("Reading spreadsheet\xE2\x80\xA6");
            ImGui::EndChild();
            return;
        }
        st.ok = st.job->err.empty() && !st.sp->tabs.empty();
        if (!st.ok) s.doc_err = st.job->err.empty() ? "nothing to show" : st.job->err;
        st.job.reset();
    }
    if (!st.ok) { empty_state(ui::Icon::FileSheet, "No preview", s.doc_err.c_str()); return; }
    const io::SheetPreview& sp = *st.sp;
    st.tab = std::clamp(st.tab, 0, (int)sp.tabs.size() - 1);
    const io::SheetPreview::Tab& tb = sp.tabs[(size_t)st.tab];
    if (st.seen_tab != st.tab) { st.seen_tab = st.tab; st.ar = st.ac = st.cr = st.cc = -1; st.dragging = false; st.find.last.clear(); }
    const GTheme& t = th();
    ImGuiIO& io = ImGui::GetIO();
    const bool modal_focus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // ---- keyboard: Ctrl+F / F3 / Ctrl+A / Ctrl+C ----
    int step = 0;
    if (modal_focus) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) { st.find.open = true; st.find.focus = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) { if (!st.find.open) { st.find.open = true; st.find.focus = true; } else step = io.KeyShift ? -1 : 1; }
        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) sheet_select_all(st, tb);
        if (!io.WantTextInput && io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_C, false) || ImGui::IsKeyPressed(ImGuiKey_Insert, false))) sheet_copy(st, tb);
    }
    // ---- find bar ----
    float find_h = 0;
    if (st.find.open) {
        const std::string q = st.find.q;
        if (q != st.find.last || st.find.match_case != st.find.last_case) {
            st.find.last = q; st.find.last_case = st.find.match_case;
            st.find.matches = q.empty() ? 0 : io::preview_count(tb, q, st.find.match_case);
            st.find.none = !q.empty() && st.find.matches == 0;
        }
        const float y0 = ImGui::GetCursorPosY();
        const int bs = find_bar(st.find, st.find.matches);
        if (bs) step = bs;
        find_h = ImGui::GetCursorPosY() - y0;
        if (modal_focus && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && io.WantTextInput) { st.find.open = false; g_close = false; }
    }
    if (step && st.find.q[0]) {
        int r = st.cr >= 0 ? st.cr : 0, c = st.cc >= 0 ? st.cc : 0;
        const bool from_start = st.cr < 0;
        if (io::preview_find(tb, st.find.q, st.find.match_case, step > 0, from_start, r, c)) {
            st.ar = st.cr = r; st.ac = st.cc = c; st.scroll_r = r; st.scroll_c = c; st.find.none = false;
        } else st.find.none = true;
        request_redraw(2);
    }

    // sheet tabs + info line at the bottom
    const float bar_h = S(30);
    ImGui::BeginChild("##shgrid", ImVec2(size.x, size.y - bar_h - find_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetWindowFontScale(s.zoom);
    if (tb.rows == 0 || tb.cols == 0) ImGui::TextDisabled("(empty sheet)");
    else {
        const int ncol = std::min(tb.cols, 500);
        const int r0 = std::min(st.ar, st.cr), r1 = std::max(st.ar, st.cr), c0 = std::min(st.ac, st.cc), c1 = std::max(st.ac, st.cc);
        const bool has_sel = sheet_has_sel(st);
        const ImU32 sel_bg = col_alpha(t.accent, 0.28f);
        const ImU32 cur_bg = col_alpha(t.accent, 0.50f);
        const ImU32 hit_bg = ImGui::GetColorU32(ImVec4(1.0f, 0.82f, 0.20f, 0.30f));
        const std::string q = st.find.open ? std::string(st.find.q) : std::string();
        ImGuiTableFlags fl = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable(("##sht" + std::to_string(st.tab)).c_str(), ncol + 1, fl)) {
            const float cw = ImGui::CalcTextSize("0").x;
            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableSetupColumn("##rh", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, cw * std::max(3.f, std::log10((float)tb.rows + 1) + 1.5f));
            for (int c = 0; c < ncol; ++c) ImGui::TableSetupColumn(col_name(c).c_str(), ImGuiTableColumnFlags_WidthFixed, std::clamp(tb.col_chars[(size_t)c], 2.f, 80.f) * cw + S(6));
            // header row: corner = select all, column letter = select column (shift extends)
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (int c = 0; c <= ncol; ++c) {
                ImGui::TableSetColumnIndex(c);
                ImGui::PushID(c);
                ImGui::TableHeader(c == 0 ? "" : ImGui::TableGetColumnName(c));
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    if (c == 0) sheet_select_all(st, tb);
                    else {
                        if (!(io.KeyShift && has_sel)) { st.ac = c - 1; }
                        st.ar = 0; st.cr = tb.rows - 1; st.cc = c - 1;
                        st.dragging = true; st.drag_kind = 2;
                    }
                }
                if (st.dragging && st.drag_kind == 2 && c > 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) st.cc = c - 1;
                ImGui::PopID();
            }
            ImGuiListClipper clip;
            clip.Begin(tb.rows);
            if (st.scroll_r >= 0 && st.scroll_r < tb.rows) clip.IncludeItemByIndex(st.scroll_r);
            const bool win_hover = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            bool st_ctx = false;
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    {   // row header: click = select row(s)
                        const ImVec2 p = ImGui::GetCursorScreenPos();
                        const ImRect rc(ImVec2(p.x - S(4), p.y - S(2)), ImVec2(p.x + ImGui::GetContentRegionAvail().x + S(4), p.y + ImGui::GetTextLineHeight() + S(2)));
                        const bool hov = win_hover && ImGui::IsMouseHoveringRect(rc.Min, rc.Max);
                        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            if (!(io.KeyShift && has_sel)) st.ar = r;
                            st.ac = 0; st.cc = tb.cols - 1; st.cr = r;
                            st.dragging = true; st.drag_kind = 1;
                        }
                        if (hov && st.dragging && st.drag_kind == 1) st.cr = r;
                        const bool rsel = has_sel && r >= r0 && r <= r1;
                        ImGui::PushStyleColor(ImGuiCol_Text, rsel ? t.text : t.text_dim);
                        ImGui::Text("%d", r + 1);
                        ImGui::PopStyleColor();
                    }
                    for (int c = 0; c < ncol; ++c) {
                        const size_t k = (size_t)r * tb.cols + c;
                        const std::string& v = tb.cells[k];
                        ImGui::TableSetColumnIndex(c + 1);
                        const ImVec2 p = ImGui::GetCursorScreenPos();
                        const ImGuiStyle& sty = ImGui::GetStyle();
                        const ImRect rc(ImVec2(p.x - sty.CellPadding.x, p.y - sty.CellPadding.y),
                                        ImVec2(p.x + ImGui::GetContentRegionAvail().x + sty.CellPadding.x, p.y + ImGui::GetTextLineHeight() + sty.CellPadding.y));
                        const bool hov = win_hover && ImGui::IsMouseHoveringRect(rc.Min, rc.Max, false);
                        if (hov && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !(has_sel && r >= r0 && r <= r1 && c >= c0 && c <= c1)))) {
                            if (!(io.KeyShift && has_sel) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { st.ar = r; st.ac = c; }
                            st.cr = r; st.cc = c;
                            st.dragging = ImGui::IsMouseClicked(ImGuiMouseButton_Left); st.drag_kind = 0;
                        }
                        if (hov && st.dragging && st.drag_kind == 0) { st.cr = r; st.cc = c; }
                        if (hov && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) st_ctx = true;
                        const bool in_sel = has_sel && r >= r0 && r <= r1 && c >= c0 && c <= c1;
                        const bool is_cur = has_sel && r == st.cr && c == st.cc;
                        const bool is_hit = !q.empty() && !v.empty() && io::preview_cell_matches(v, q, st.find.match_case);
                        if (is_cur && (r0 != r1 || c0 != c1 || is_hit)) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cur_bg);
                        else if (in_sel) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, sel_bg);
                        else if (is_hit) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, hit_bg);
                        if (r == st.scroll_r && c == st.scroll_c) {
                            ImGui::ScrollToRect(ImGui::GetCurrentWindow(), rc, ImGuiScrollFlags_KeepVisibleCenterY | ImGuiScrollFlags_KeepVisibleEdgeX);
                            st.scroll_r = st.scroll_c = -1;
                            request_redraw(2);
                        }
                        if (v.empty()) continue;
                        if (tb.right[k]) {
                            const float w = ImGui::CalcTextSize(v.c_str()).x, avw = ImGui::GetContentRegionAvail().x;
                            if (w < avw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avw - w);
                        }
                        if (tb.bold[k]) ImGui::PushFont(g_font_bold);
                        ImGui::TextUnformatted(v.c_str());
                        if (tb.bold[k]) ImGui::PopFont();
                        if (hov && !st.dragging && ImGui::CalcTextSize(v.c_str()).x > ImGui::GetColumnWidth()) tooltip(v.c_str());
                    }
                }
            // drag past the visible edge: keep extending + autoscroll
            if (st.dragging) {
                ImGuiWindow* w = ImGui::GetCurrentWindow();
                const float my = io.MousePos.y, top = w->InnerClipRect.Min.y, bot = w->InnerClipRect.Max.y;
                if (my > bot) { ImGui::SetScrollY(ImGui::GetScrollY() + (my - bot)); request_redraw(2); }
                else if (my < top + ImGui::GetFrameHeight()) { ImGui::SetScrollY(std::max(0.f, ImGui::GetScrollY() - (top + ImGui::GetFrameHeight() - my))); request_redraw(2); }
            }
            ImGui::EndTable();
            if (st_ctx) ImGui::OpenPopup("##shctx");
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) st.dragging = false;
        if (ImGui::BeginPopup("##shctx")) {
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, sheet_has_sel(st))) sheet_copy(st, tb);
            if (ImGui::MenuItem("Copy sheet", nullptr, false, tb.rows > 0)) {
                const std::string all = io::preview_tsv(tb, 0, 0, tb.rows - 1, tb.cols - 1);
                ImGui::SetClipboardText(all.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Select all", "Ctrl+A")) sheet_select_all(st, tb);
            if (ImGui::MenuItem("Find\xE2\x80\xA6", "Ctrl+F")) { st.find.open = true; st.find.focus = true; }
            ImGui::EndPopup();
        }
    }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.KeyCtrl && io.MouseWheel != 0) { s.zoom = std::clamp(s.zoom * (io.MouseWheel > 0 ? 1.1f : 1 / 1.1f), 0.5f, 3.f); request_redraw(2); }
    ImGui::EndChild();
    for (int i = 0; i < (int)sp.tabs.size(); ++i) {
        if (i) ImGui::SameLine(0, S(2));
        const bool cur = i == st.tab;
        ImGui::PushStyleColor(ImGuiCol_Button, cur ? col_alpha(t.accent, 0.30f) : t.panel2);
        if (ImGui::Button((sp.tabs[(size_t)i].name + "##tab" + std::to_string(i)).c_str())) st.tab = i;
        ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, S(16));
    char info[240];
    char selinfo[64] = "";
    if (sheet_has_sel(st)) {
        const int nr = std::abs(st.cr - st.ar) + 1, nc = std::abs(st.cc - st.ac) + 1;
        if (nr == 1 && nc == 1) std::snprintf(selinfo, sizeof selinfo, "  \xC2\xB7  %s%d", col_name(st.cc).c_str(), st.cr + 1);
        else std::snprintf(selinfo, sizeof selinfo, "  \xC2\xB7  %d x %d selected", nr, nc);
    }
    std::snprintf(info, sizeof info, "%s  \xC2\xB7  %d rows x %d columns%s%s  \xC2\xB7  read-only (Ctrl+C copies, Ctrl+F finds)", sp.how.c_str(), tb.total_rows, tb.total_cols,
                  tb.truncated() ? " (first rows / columns shown)" : "", selinfo);
    ImGui::TextDisabled("%s", info);
}

// ---- R22c Markdown: simple rendering (headings, bold / italic / code / links, bullet + numbered lists, quotes,
// code blocks, rules). Everything else shows as text; "Show source" gives the raw lines.
std::vector<MdBlock> md_parse(const std::vector<std::string>& lines) {
    std::vector<MdBlock> out;
    bool code = false;
    auto ltrim = [](const std::string& s, size_t& ind) { size_t i = 0; ind = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) { ind += s[i] == '\t' ? 4 : 1; ++i; } return s.substr(i); };
    for (const std::string& raw : lines) {
        size_t ind = 0;
        const std::string ln = ltrim(raw, ind);
        if (ln.rfind("```", 0) == 0 || ln.rfind("~~~", 0) == 0) { code = !code; if (!code) out.push_back({MdBlock::Blank, 0, {}, {}}); continue; }
        if (code) { out.push_back({MdBlock::Code, 0, {}, raw}); continue; }
        if (ln.empty()) { out.push_back({MdBlock::Blank, 0, {}, {}}); continue; }
        if (ln[0] == '#') {
            size_t h = 0; while (h < ln.size() && ln[h] == '#') ++h;
            if (h <= 6 && (h == ln.size() || ln[h] == ' ')) {
                std::string t = h < ln.size() ? ln.substr(h + 1) : std::string();
                while (!t.empty() && (t.back() == '#' || t.back() == ' ')) t.pop_back();
                out.push_back({MdBlock::Head, (int)h, {}, t}); continue;
            }
        }
        {
            int n = 0; char c0 = 0; bool same = true;
            for (char c : ln) { if (c == ' ') continue; if (!c0) c0 = c; same &= c == c0; ++n; }
            if (n >= 3 && same && (c0 == '-' || c0 == '*' || c0 == '_')) { out.push_back({MdBlock::Rule, 0, {}, {}}); continue; }
        }
        if (ln.size() >= 2 && (ln[0] == '-' || ln[0] == '*' || ln[0] == '+') && ln[1] == ' ') {
            std::string t = ln.substr(2);
            if (t.rfind("[ ] ", 0) == 0) t = "\xE2\x98\x90 " + t.substr(4);
            else if (t.rfind("[x] ", 0) == 0 || t.rfind("[X] ", 0) == 0) t = "\xE2\x98\x91 " + t.substr(4);
            out.push_back({MdBlock::Bullet, (int)(ind / 2), {}, t}); continue;
        }
        {
            size_t d = 0; while (d < ln.size() && std::isdigit((unsigned char)ln[d])) ++d;
            if (d > 0 && d < 10 && d + 1 < ln.size() && (ln[d] == '.' || ln[d] == ')') && ln[d + 1] == ' ') {
                out.push_back({MdBlock::Number, (int)(ind / 2), ln.substr(0, d + 1), ln.substr(d + 2)}); continue;
            }
        }
        if (ln[0] == '>') { std::string t = ln.substr(1); if (!t.empty() && t[0] == ' ') t.erase(0, 1); out.push_back({MdBlock::Quote, 0, {}, t}); continue; }
        // paragraph: consecutive text lines join (a line ending in two spaces breaks)
        if (!out.empty() && (out.back().type == MdBlock::Para || out.back().type == MdBlock::Quote) && !out.back().text.empty() &&
            !(out.back().text.size() >= 2 && out.back().text.compare(out.back().text.size() - 2, 2, "  ") == 0) && out.back().type == MdBlock::Para) {
            out.back().text += " " + ln;
            continue;
        }
        out.push_back({MdBlock::Para, 0, {}, ln});
    }
    return out;
}
struct MdSpan { std::string t; int style; };   // 0 plain, 1 bold, 2 italic (no oblique UI face: drawn plain), 3 code, 4 link
std::vector<MdSpan> md_inline(const std::string& s) {
    std::vector<MdSpan> v;
    std::string cur; int bold = 0, ital = 0;
    auto style = [&]() { return bold ? 1 : ital ? 2 : 0; };
    auto flush = [&](int st) { if (!cur.empty()) { v.push_back({cur, st}); cur.clear(); } };
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && std::strchr("\\`*_[]#+-.!>", s[i + 1])) { cur += s[++i]; continue; }
        if (c == '`') {
            const size_t e = s.find('`', i + 1);
            if (e != std::string::npos) { flush(style()); v.push_back({s.substr(i + 1, e - i - 1), 3}); i = e; continue; }
        }
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c) { flush(style()); bold = !bold; ++i; continue; }
        if (c == '*' || (c == '_' && (i == 0 || !std::isalnum((unsigned char)s[i - 1]) || ital))) {
            const bool closes = ital, opens = !ital && i + 1 < s.size() && s[i + 1] != ' ' && s.find(c, i + 1) != std::string::npos;
            if (closes || opens) { flush(style()); ital = !ital; continue; }
        }
        if (c == '[') {
            const size_t rb = s.find(']', i + 1);
            if (rb != std::string::npos && rb + 1 < s.size() && s[rb + 1] == '(') {
                const size_t rp = s.find(')', rb + 2);
                if (rp != std::string::npos) { flush(style()); v.push_back({s.substr(i + 1, rb - i - 1), 4}); i = rp; continue; }
            }
        }
        cur += c;
    }
    flush(style());
    return v;
}
// Word-wrapped spans at (x0, y) within width; draws when dl != nullptr. Returns the height used.
float md_flow(ImDrawList* dl, float x0, float y, float width, const std::vector<MdSpan>& sp, float px, bool bold_all, ImU32 col, ImU32 link, ImU32 code_bg) {
    ImFont* f_reg = font_for_px(bold_all ? g_font_bold : g_font, px);
    ImFont* f_bold = font_for_px(g_font_bold, px);
    ImFont* f_mono = font_for_px(g_font_mono, px * 0.95f);
    const float lh = px * 1.35f;
    float x = x0, yy = y;
    for (const MdSpan& s : sp) {
        ImFont* f = s.style == 1 ? f_bold : s.style == 3 ? f_mono : f_reg;
        const float fpx = s.style == 3 ? px * 0.95f : px;
        size_t i = 0;
        while (i < s.t.size()) {
            size_t j = s.t.find(' ', i);
            j = j == std::string::npos ? s.t.size() : j + 1;
            const std::string w = s.t.substr(i, j - i);
            const float ww = f->CalcTextSizeA(fpx, FLT_MAX, 0.f, w.c_str()).x;
            if (x > x0 && x + ww > x0 + width) { x = x0; yy += lh; }
            if (dl) {
                const ImVec2 p(x, yy + (lh - fpx) * 0.5f);
                if (s.style == 3) dl->AddRectFilled(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + ww + 1, p.y + fpx + 1), code_bg, 3.f);
                dl->AddText(f, fpx, p, s.style == 4 ? link : col, w.c_str());
                if (s.style == 4) dl->AddLine(ImVec2(p.x, p.y + fpx), ImVec2(p.x + ww - (w.back() == ' ' ? f->CalcTextSizeA(fpx, FLT_MAX, 0.f, " ").x : 0), p.y + fpx), link, 1.f);
            }
            x += ww;
            i = j;
        }
    }
    return yy + lh - y;
}
void draw_markdown(State& s, ImVec2 size) {
    TextState& tx = s.text;
    if (!tx.md_built) { tx.md_built = true; tx.blocks = md_parse(tx.lines); }
    const GTheme& t = th();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.field);
    ImGui::BeginChild("##pvmd", size, ImGuiChildFlags_None, 0);
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float base = ImGui::GetFontSize() * std::clamp(s.zoom, 0.5f, 3.f) * 1.05f;
    const float margin = S(24);
    const float maxw = std::min(ImGui::GetContentRegionAvail().x - margin * 2, base * 48.f);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float x0 = o.x + margin;
    float y = o.y + S(16);
    const float clip0 = ImGui::GetWindowPos().y - S(200), clip1 = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() + S(200);
    const ImU32 link = t.accent, code_bg = col_alpha(t.text, 0.08f), dim = t.text_dim;
    for (size_t i = 0; i < tx.blocks.size(); ++i) {
        const MdBlock& b = tx.blocks[i];
        const bool vis = y > clip0 && y < clip1;   // measure always, draw only what is near the view
        ImDrawList* d = vis ? dl : nullptr;
        switch (b.type) {
        case MdBlock::Blank: y += base * 0.45f; break;
        case MdBlock::Rule: if (d) dl->AddLine(ImVec2(x0, y + base * 0.5f), ImVec2(x0 + maxw, y + base * 0.5f), t.border, 1.f); y += base; break;
        case MdBlock::Head: {
            static const float k[7] = {1, 1.75f, 1.45f, 1.25f, 1.1f, 1.0f, 0.95f};
            const float px = base * k[std::clamp(b.level, 1, 6)];
            y += px * 0.35f;
            y += md_flow(d, x0, y, maxw, md_inline(b.text), px, true, t.text, link, code_bg);
            if (b.level <= 2 && d) dl->AddLine(ImVec2(x0, y + S(2)), ImVec2(x0 + maxw, y + S(2)), t.border, 1.f);
            y += px * 0.3f;
        } break;
        case MdBlock::Bullet: case MdBlock::Number: {
            const float ind = base * (1.2f + 1.4f * b.level);
            if (d) {
                if (b.type == MdBlock::Bullet) dl->AddCircleFilled(ImVec2(x0 + ind - base * 0.6f, y + base * 1.35f * 0.5f), base * 0.16f, t.text);
                else { ImFont* f = font_for_px(g_font, base); const float mw = f->CalcTextSizeA(base, FLT_MAX, 0.f, b.mark.c_str()).x; dl->AddText(f, base, ImVec2(x0 + ind - mw - base * 0.3f, y + base * 0.175f), t.text, b.mark.c_str()); }
            }
            y += md_flow(d, x0 + ind, y, maxw - ind, md_inline(b.text), base, false, t.text, link, code_bg) + base * 0.1f;
        } break;
        case MdBlock::Quote: {
            const float h = md_flow(nullptr, x0 + base, y, maxw - base, md_inline(b.text), base, false, dim, link, code_bg);
            if (d) { dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + S(3), y + h), col_alpha(t.accent, 0.6f)); md_flow(dl, x0 + base, y, maxw - base, md_inline(b.text), base, false, dim, link, code_bg); }
            y += h + base * 0.2f;
        } break;
        case MdBlock::Code: {
            ImFont* f = font_for_px(g_font_mono, base * 0.95f);
            const float lh = base * 1.3f;
            if (d) { dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + maxw, y + lh), code_bg); dl->AddText(f, base * 0.95f, ImVec2(x0 + S(8), y + base * 0.15f), t.text, b.text.c_str()); }
            y += lh;
        } break;
        default:
            y += md_flow(d, x0, y, maxw, md_inline(b.text), base, false, t.text, link, code_bg) + base * 0.35f;
            break;
        }
    }
    ImGui::Dummy(ImVec2(maxw + margin * 2, y - o.y + S(16)));
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0) { s.zoom = std::clamp(s.zoom * (io.MouseWheel > 0 ? 1.1f : 1 / 1.1f), 0.5f, 3.f); request_redraw(2); }
    ImGui::EndChild();
}

// ---- text / hex --------------------------------------------------------------------------------------------
int find_bar(FindBar& f, int total_matches_hint);
void text_copy(const TextState& tx, bool all) {
    std::string o;
    const int n = (int)tx.lines.size();
    int a = all ? 0 : std::min(tx.sa, tx.sb), b = all ? n - 1 : std::max(tx.sa, tx.sb);
    a = std::max(a, 0); b = std::min(b, n - 1);
    for (int i = a; i <= b; ++i) { o += tx.lines[(size_t)i]; if (i < b) o += "\r\n"; }
    ImGui::SetClipboardText(o.c_str());
}
void draw_text(State& s, ImVec2 size) {
    TextState& tx = s.text;
    if (!tx.built) {
        tx.built = true;
        tx.binary = is_binary(s.rq.bytes);
        tx.hex = tx.binary;
        tx.md = !tx.binary && (ext_of(s.rq.name) == "md" || ext_of(s.rq.name) == "markdown");
        if (!tx.binary) {
            std::string u = io::decode_text_bytes(s.rq.bytes, io::TextEncoding::Auto);
            size_t a = 0;
            while (a <= u.size() && tx.lines.size() < 2000000) {
                size_t b = u.find('\n', a);
                if (b == std::string::npos) b = u.size();
                std::string ln = u.substr(a, b - a);
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                for (auto& c : ln) if (c == '\t') c = ' ';
                tx.lines.push_back(std::move(ln));
                a = b + 1;
            }
        }
    }
    if (tx.md && !tx.md_src && !tx.hex) { draw_markdown(s, size); return; }
    const GTheme& t = th();
    if (!tx.hex) {   // R23a keyboard + find bar (line based)
        ImGuiIO& kio = ImGui::GetIO();
        const int n = (int)tx.lines.size();
        int step = 0;
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) { tx.find.open = true; tx.find.focus = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) { if (!tx.find.open) { tx.find.open = true; tx.find.focus = true; } else step = kio.KeyShift ? -1 : 1; }
            if (!kio.WantTextInput && kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false) && n) { tx.sa = 0; tx.sb = n - 1; }
            if (!kio.WantTextInput && kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) text_copy(tx, tx.sa < 0);
            if (tx.find.open && kio.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) tx.find.open = false;
        }
        if (tx.find.open) {
            const std::string q = tx.find.q;
            if (q != tx.find.last || tx.find.match_case != tx.find.last_case) {
                tx.find.last = q; tx.find.last_case = tx.find.match_case;
                int m = 0;
                if (!q.empty()) for (const auto& l : tx.lines) m += io::preview_cell_matches(l, q, tx.find.match_case) ? 1 : 0;
                tx.find.matches = m; tx.find.none = !q.empty() && m == 0;
            }
            const float y0 = ImGui::GetCursorPosY();
            if (int bs = find_bar(tx.find, tx.find.matches)) step = bs;
            size.y -= ImGui::GetCursorPosY() - y0;
        }
        if (step && tx.find.q[0] && n) {
            const int from = tx.sb >= 0 ? tx.sb : (step > 0 ? -1 : n);
            bool found = false;
            for (int k = 1; k <= n && !found; ++k) {
                const int i = ((from + step * k) % n + n) % n;
                if (io::preview_cell_matches(tx.lines[(size_t)i], tx.find.q, tx.find.match_case)) { tx.sa = tx.sb = i; tx.scroll_line = i; found = true; }
            }
            tx.find.none = !found;
            request_redraw(2);
        }
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.field);
    ImGui::BeginChild("##pvtext", size, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleColor();
    ImGui::PushFont(g_font_mono);
    ImGui::SetWindowFontScale(s.zoom);
    const float lh = ImGui::GetTextLineHeightWithSpacing();
    ImGuiListClipper clip;
    if (!tx.hex) {
        // R23a: click / drag / shift-click selects lines, Ctrl+A all, Ctrl+C copies, Ctrl+F finds
        const int digits = (int)std::to_string(tx.lines.size()).size();
        const int n = (int)tx.lines.size();
        if (tx.scroll_line >= 0 && tx.scroll_line < n) {
            ImGui::SetScrollY(std::max(0.f, tx.scroll_line * lh - ImGui::GetWindowHeight() * 0.4f));
            tx.scroll_line = -1;
        }
        const int a = std::min(tx.sa, tx.sb), b = std::max(tx.sa, tx.sb);
        const std::string q = tx.find.open ? std::string(tx.find.q) : std::string();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float x0 = ImGui::GetWindowPos().x, x1 = x0 + ImGui::GetWindowWidth();
        const bool hov_win = ImGui::IsWindowHovered();
        clip.Begin(n, lh);
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const bool hov = hov_win && ImGui::IsMouseHoveringRect(ImVec2(x0, p.y), ImVec2(x1, p.y + lh), false);
                if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { if (!(ImGui::GetIO().KeyShift && tx.sa >= 0)) tx.sa = i; tx.sb = i; tx.drag = true; }
                if (hov && tx.drag) tx.sb = i;
                const bool sel = tx.sa >= 0 && i >= a && i <= b;
                const bool hit = !q.empty() && io::preview_cell_matches(tx.lines[(size_t)i], q, tx.find.match_case);
                if (sel || hit) dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, p.y + lh), sel ? col_alpha(t.accent, hit ? 0.45f : 0.28f) : IM_COL32(255, 210, 50, 70));
                ImGui::PushStyleColor(ImGuiCol_Text, t.text_faint);
                ImGui::Text("%*d", digits, i + 1);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextUnformatted(tx.lines[(size_t)i].c_str());
            }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) tx.drag = false;
        else if (tx.drag) {
            const float my = ImGui::GetIO().MousePos.y, top = ImGui::GetWindowPos().y, bot = top + ImGui::GetWindowHeight();
            if (my > bot) ImGui::SetScrollY(ImGui::GetScrollY() + (my - bot));
            else if (my < top) ImGui::SetScrollY(std::max(0.f, ImGui::GetScrollY() - (top - my)));
            request_redraw(2);
        }
        if (hov_win && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) ImGui::OpenPopup("##txctx");
        if (ImGui::BeginPopup("##txctx")) {
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, tx.sa >= 0)) text_copy(tx, false);
            if (ImGui::MenuItem("Copy all")) text_copy(tx, true);
            ImGui::Separator();
            if (ImGui::MenuItem("Select all", "Ctrl+A", false, n > 0)) { tx.sa = 0; tx.sb = n - 1; }
            if (ImGui::MenuItem("Find\xE2\x80\xA6", "Ctrl+F")) { tx.find.open = true; tx.find.focus = true; }
            ImGui::EndPopup();
        }
    } else {
        const auto& b = s.rq.bytes;
        const int rows = (int)((b.size() + 15) / 16);
        clip.Begin(rows, lh);
        char line[128];
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const size_t o = (size_t)r * 16;
                int n = std::snprintf(line, sizeof line, "%08zX  ", o);
                for (int k = 0; k < 16; ++k) n += o + k < b.size() ? std::snprintf(line + n, sizeof line - n, "%02X%s", b[o + k], k == 7 ? "  " : " ") : std::snprintf(line + n, sizeof line - n, "  %s", k == 7 ? "  " : " ");
                n += std::snprintf(line + n, sizeof line - n, " ");
                for (int k = 0; k < 16 && o + k < b.size(); ++k) line[n++] = (b[o + k] >= 32 && b[o + k] < 127) ? (char)b[o + k] : '.';
                line[n] = 0;
                ImGui::TextUnformatted(line);
            }
    }
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0) { s.zoom = std::clamp(s.zoom * (io.MouseWheel > 0 ? 1.1f : 1 / 1.1f), 0.5f, 3.f); request_redraw(2); }
    ImGui::PopFont();
    ImGui::EndChild();
}

// ---- Gerber / Excellon -------------------------------------------------------------------------------------
void draw_gerber(State& s, ImVec2 size) {
    GerberState& gs = s.gbr;
    if (!gs.layer && !gs.job) {
        auto L = std::make_shared<gerber::Layer>();
        auto text = std::make_shared<std::string>(s.rq.bytes.begin(), s.rq.bytes.end());
        std::string nm = s.rq.name;
        gs.layer = L;
        gs.job = start([L, text, nm](Job& j) {
            gerber::ParseOptions o;
            L->name = nm;
            if (!gerber::parse(*text, o, *L, j.err)) return;
            if (L->objs.empty()) j.err = L->empty_file ? "the layer is empty" : "no drawable objects in this file";
        });
    }
    if (gs.job) {
        if (!gs.job->done) {
            ImGui::BeginChild("##gwait", size);
            ImGui::SetCursorPos(ImVec2(size.x * 0.5f - S(60), size.y * 0.4f));
            spinner(); ImGui::SameLine(); ImGui::TextDisabled("Reading layer\xE2\x80\xA6");
            ImGui::EndChild();
            return;
        }
        gs.ok = gs.job->err.empty();
        if (!gs.ok) s.doc_err = gs.job->err;
        gs.job.reset();
    }
    if (!gs.ok) { empty_state(ui::Icon::FileGerber, "No preview", s.doc_err.c_str()); return; }
    const gerber::Layer& L = *gs.layer;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(14, 22, 18, 255));
    ImGui::BeginChild("##gcanvas", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##gcv", av, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    BBox ex = L.extent;
    if (ex.empty()) for (auto& ob : L.objs) ex.add(gerber::object_bbox(L, ob));
    if (ex.empty()) ex = BBox(0, 0, 10, 10);
    const double fitk = std::min((av.x - S(30)) / std::max(1e-6, ex.w()), (av.y - S(30)) / std::max(1e-6, ex.y1 - ex.y0));
    if (s.fit) { gs.zoom = 1.f; gs.pan = ImVec2(0, 0); }
    const double k = fitk * gs.zoom;
    s.zoom = (float)(k / (dpi_scale() * 96.0 / 25.4));   // shown as % of 1:1
    const ImVec2 c(o.x + av.x * 0.5f + gs.pan.x, o.y + av.y * 0.5f + gs.pan.y);
    const double mx = (ex.x0 + ex.x1) * 0.5, my = (ex.y0 + ex.y1) * 0.5;
    auto P = [&](Vec2 v) { return ImVec2((float)(c.x + (v.x - mx) * k), (float)(c.y - (v.y - my) * k)); };
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && io.MouseWheel != 0) {   // zoom about the cursor
        const float f = io.MouseWheel > 0 ? 1.2f : 1 / 1.2f;
        const ImVec2 m(io.MousePos.x - c.x, io.MousePos.y - c.y);
        gs.zoom = std::clamp(gs.zoom * f, 0.05f, 2000.f);
        gs.pan.x -= m.x * (f - 1); gs.pan.y -= m.y * (f - 1);
        s.fit = false; request_redraw(2);
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(0) || ImGui::IsMouseDragging(2))) { gs.pan.x += io.MouseDelta.x; gs.pan.y += io.MouseDelta.y; s.fit = false; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(o, ImVec2(o.x + av.x, o.y + av.y), true);
    const ImU32 dark = L.excellon ? IM_COL32(120, 200, 255, 255) : IM_COL32(222, 160, 70, 255);
    const ImU32 clr = IM_COL32(14, 22, 18, 255);
    const ImVec2 clip0 = o, clip1(o.x + av.x, o.y + av.y);
    std::vector<ImVec2> pts;
    for (const gerber::Object& ob : L.objs) {
        const ImU32 col = ob.clear ? clr : dark;
        const gerber::Aperture* ap = gerber::find_ap(L, ob.dcode);
        if (ob.kind == gerber::Object::Region) {
            pts.clear();
            for (auto& v : ob.poly) pts.push_back(P(v));
            if (pts.size() >= 3) dl->AddConcavePolyFilled(pts.data(), (int)pts.size(), col);
            continue;
        }
        BBox bb = gerber::object_bbox(L, ob);
        if (!bb.empty()) {
            ImVec2 a = P({bb.x0, bb.y1}), b = P({bb.x1, bb.y0});
            if (b.x < clip0.x || a.x > clip1.x || b.y < clip0.y || a.y > clip1.y) continue;
        }
        if (ob.kind == gerber::Object::Flash) {
            if (!ap) continue;
            if (L.excellon) {   // drill hit: solid hole + ring (flash_polys would return the hole as a clear area)
                const float r = std::max(1.5f, (float)(ap->w * 0.5 * k));
                dl->AddCircleFilled(P(ob.a), r, col);
                if (r > 3) dl->AddCircle(P(ob.a), r, IM_COL32(220, 240, 255, 255), 0, 1.f);
                continue;
            }
            std::vector<bool> cl;
            auto polys = gerber::flash_polys(*ap, ob.a, 24, &cl);
            for (size_t i = 0; i < polys.size(); ++i) {
                pts.clear();
                for (auto& v : polys[i]) pts.push_back(P(v));
                if (pts.size() >= 3) dl->AddConcavePolyFilled(pts.data(), (int)pts.size(), (i < cl.size() && cl[i]) ? clr : col);
            }
        } else {
            const float w = std::max(1.f, (float)((ap ? std::max(ap->w, 0.0) : 0.1) * k));
            if (ob.kind == gerber::Object::Draw) {
                const ImVec2 a = P(ob.a), b = P(ob.b);
                dl->AddLine(a, b, col, w);
                if (w > 2.5f && (!ap || ap->shape == gerber::Aperture::Circle)) { dl->AddCircleFilled(a, w * 0.5f, col); dl->AddCircleFilled(b, w * 0.5f, col); }
            } else {
                pts.clear();
                for (auto& v : gerber::arc_points(ob, 72)) pts.push_back(P(v));
                if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, w);
            }
        }
    }
    dl->PopClipRect();
    char inf[200];
    std::snprintf(inf, sizeof inf, "%s  \xC2\xB7  %d flashes, %d draws  \xC2\xB7  %.2f x %.2f mm  \xC2\xB7  wheel = zoom, drag = pan",
                  L.excellon ? "Excellon drill" : L.rs274x ? "Gerber RS-274X" : "Gerber RS-274D", L.flash_count(), L.draw_count(), ex.w(), ex.y1 - ex.y0);
    dl->AddText(ImVec2(o.x + S(10), o.y + av.y - S(24)), IM_COL32(200, 214, 205, 255), inf);   // canvas is always dark
    ImGui::EndChild();
}

void draw_toolbar(State& s) {
    const GTheme& t = th();
    (void)t;
    const ui::Icon ic = s.kind == Kind::Pdf ? ui::Icon::FilePdf : s.kind == Kind::Doc ? ui::Icon::FileDoc : s.kind == Kind::Mail ? ui::Icon::FileEmail
                      : s.kind == Kind::Sheet ? ui::Icon::FileSheet : s.kind == Kind::Image ? ui::Icon::FileImage : s.kind == Kind::Gerber ? ui::Icon::FileGerber : ui::Icon::FileText;
    icon_image(ic, S(28));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted(s.rq.name.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s  \xC2\xB7  %s", s.kind == Kind::Text && s.text.md ? "Markdown" : kind_label(s.kind), s.rq.where.c_str());
    ImGui::EndGroup();
    // zoom
    ImGui::SameLine(0, S(24));
    const bool zoomable = s.kind != Kind::Mail && !(s.kind == Kind::Pdf && (s.pdf.unavailable || (!s.pdf.opened && !s.pdf.why.empty())));
    ImGui::BeginDisabled(!zoomable);
    if (icon_button("##zout", ui::Icon::ZoomOut, "Zoom out (Ctrl+-)")) { s.fit = false; s.zoom = std::max(0.05f, s.zoom / 1.2f); if (s.kind == Kind::Gerber) s.gbr.zoom /= 1.2f; }
    ImGui::SameLine(0, S(2));
    char zl[24]; std::snprintf(zl, sizeof zl, "%d%%", (int)std::lround(s.zoom * 100));
    if (ImGui::Button(zl, ImVec2(S(64), 0))) { s.fit = false; if (s.kind == Kind::Gerber) s.gbr.zoom *= 1.f / std::max(0.01f, s.zoom); s.zoom = 1.f; }
    if (ImGui::IsItemHovered()) tooltip("Actual size (Ctrl+0 = fit)");
    ImGui::SameLine(0, S(2));
    if (icon_button("##zin", ui::Icon::ZoomIn, "Zoom in (Ctrl++)")) { s.fit = false; s.zoom = std::min(8.f, s.zoom * 1.2f); if (s.kind == Kind::Gerber) s.gbr.zoom *= 1.2f; }
    ImGui::SameLine(0, S(2));
    if (icon_button("##zfit", ui::Icon::ZoomAll, "Fit (Ctrl+0)", s.fit)) { s.fit = true; if (s.kind != Kind::Image && s.kind != Kind::Pdf && s.kind != Kind::Gerber) s.zoom = 1.f; }
    ImGui::EndDisabled();
    // pages
    if (s.kind == Kind::Pdf && s.pdf.opened) {
        ImGui::SameLine(0, S(18));
        if (icon_button("##pprev", ui::Icon::ArrowLeft, "Previous page (PgUp)", false, s.pdf.page > 0)) s.pdf.page--;
        ImGui::SameLine(0, S(2));
        int pg = s.pdf.page + 1;
        ImGui::SetNextItemWidth(S(56));
        if (ImGui::InputInt("##pg", &pg, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) s.pdf.page = std::clamp(pg - 1, 0, s.pdf.pages - 1);
        ImGui::SameLine(0, S(4));
        ImGui::TextDisabled("/ %d", s.pdf.pages);
        ImGui::SameLine(0, S(4));
        if (icon_button("##pnext", ui::Icon::ArrowRight, "Next page (PgDn)", false, s.pdf.page + 1 < s.pdf.pages)) s.pdf.page++;
    }
    if (s.kind == Kind::Text) {
        ImGui::SameLine(0, S(18));
        if (s.text.md && !s.text.hex) {
            if (button(s.text.md_src ? "Show formatted" : "Show source", ui::Icon::Notes)) s.text.md_src = !s.text.md_src;
            ImGui::SameLine(0, S(4));
        }
        if (button(s.text.hex ? "Show as text" : "Show as hex", ui::Icon::Text)) s.text.hex = !s.text.hex;
    }
    // actions, right aligned
    const float bw = S(150) + S(130) + (s.rq.import_kinds ? S(118) : 0);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + S(12), ImGui::GetWindowContentRegionMax().x - bw));
    if (s.rq.import_kinds && s.rq.import_as) {
        if (button("Import as", ui::Icon::ImportCad)) ImGui::OpenPopup("##pvimp");
        if (ImGui::BeginPopup("##pvimp")) {
            const char* names[] = {"Gerbers", "CAD", "BOM", "Centroid"};
            const int bits[] = {jz::ImpGerber, jz::ImpCad, jz::ImpBom, jz::ImpCentroid};
            const ui::Icon icons[] = {ui::Icon::ImportGerber, ui::Icon::ImportCad, ui::Icon::ImportBom, ui::Icon::ImportCentroid};
            for (int i = 0; i < 4; ++i)
                if (s.rq.import_kinds & bits[i])
                    if (menu_item_icon(names[i], icons[i])) { auto f = s.rq.import_as; int b = bits[i]; g_close = true; defer([f, b]() { f(b); }); }
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, S(4));
    }
    if (button("Extract\xE2\x80\xA6", ui::Icon::Export) && s.rq.extract) s.rq.extract();
    ImGui::SameLine(0, S(4));
    if (button("Open externally", ui::Icon::Open) && s.rq.open_external) s.rq.open_external();
    if (ImGui::IsItemHovered()) tooltip("Open a temporary copy in the program Windows associates with this file type");
}

void handle_keys(State& s) {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !io.WantTextInput) {
        if (s.kind == Kind::Sheet && s.sheet.find.open) s.sheet.find.open = false;        // R23a: Esc closes Find first
        else if (s.kind == Kind::Text && s.text.find.open) s.text.find.open = false;
        else g_close = true;
    }
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) { s.fit = false; s.zoom = std::min(8.f, s.zoom * 1.2f); if (s.kind == Kind::Gerber) s.gbr.zoom *= 1.2f; }
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) { s.fit = false; s.zoom = std::max(0.05f, s.zoom / 1.2f); if (s.kind == Kind::Gerber) s.gbr.zoom /= 1.2f; }
        if (ImGui::IsKeyPressed(ImGuiKey_0, false)) s.fit = true;
    }
    if (s.kind == Kind::Pdf && s.pdf.opened && !io.WantTextInput) {
        if ((ImGui::IsKeyPressed(ImGuiKey_PageDown) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) && s.pdf.page + 1 < s.pdf.pages) s.pdf.page++;
        if ((ImGui::IsKeyPressed(ImGuiKey_PageUp) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) && s.pdf.page > 0) s.pdf.page--;
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) s.pdf.page = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) s.pdf.page = s.pdf.pages - 1;
    }
}

} // namespace

const char* kind_label(Kind k) {
    switch (k) {
    case Kind::Pdf: return "PDF document";
    case Kind::Doc: return "Document";
    case Kind::Mail: return "E-mail";
    case Kind::Sheet: return "Spreadsheet (read-only)";
    case Kind::Image: return "Image";
    case Kind::Gerber: return "Gerber / drill layer";
    default: return "Text";
    }
}

Kind kind_for(const std::string& name, const std::vector<uint8_t>& b, int import_kinds) {
    const std::string e = ext_of(name);
    if (e == "pdf" || (b.size() > 5 && std::equal(b.begin(), b.begin() + 5, "%PDF-"))) return Kind::Pdf;
    if (jz::is_image_name(name)) return Kind::Image;
    if (e == "eml" || e == "msg") return Kind::Mail;
    if (e == "docx" || e == "dotx" || e == "rtf" || e == "htm" || e == "html") return Kind::Doc;
    if (e == "xlsx" || e == "xlsm" || e == "xls" || e == "ods" || e == "csv" || e == "tsv") return Kind::Sheet;
    if (io::is_jet_bytes(b)) return Kind::Sheet;   // R23a: .mdb / .cpf / .flx (Jet3/Jet4) as read-only tables
    if ((import_kinds & jz::ImpGerber) && !jz::is_zip_name(name)) return Kind::Gerber;
    return Kind::Text;
}

static bool decode_image_raw(const std::vector<uint8_t>& data, int max_dim, int& w, int& h, std::vector<uint8_t>& px);
bool decode_image(const std::vector<uint8_t>& data, int max_dim, int& w, int& h, std::vector<uint8_t>& px) {
    bool r = false;   // R23a: a codec fault on a malformed image -> "no preview", not a crash
    if (!crash::guarded([&] { try { r = decode_image_raw(data, max_dim, w, h, px); } catch (...) { r = false; } })) { px.clear(); return false; }
    return r;
}
static bool decode_image_raw(const std::vector<uint8_t>& data, int max_dim, int& w, int& h, std::vector<uint8_t>& px) {
    if (data.empty() || data.size() > (256u << 20)) return false;
    ui::Com<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put())))) return false;
    ui::Com<IWICStream> st;
    if (FAILED(f->CreateStream(st.put())) || FAILED(st->InitializeFromMemory((BYTE*)data.data(), (DWORD)data.size()))) return false;
    ui::Com<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(st.get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.put()))) return false;
    ui::Com<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, fr.put()))) return false;
    UINT sw = 0, sh = 0; fr->GetSize(&sw, &sh);
    if (!sw || !sh) return false;
    IWICBitmapSource* src = fr.get();
    ui::Com<IWICBitmapScaler> sc;
    UINT ow = sw, oh = sh;
    if ((int)std::max(sw, sh) > max_dim) {
        const double k = (double)max_dim / std::max(sw, sh);
        ow = std::max(1u, (UINT)(sw * k)); oh = std::max(1u, (UINT)(sh * k));
        if (SUCCEEDED(f->CreateBitmapScaler(sc.put())) && SUCCEEDED(sc->Initialize(fr.get(), ow, oh, WICBitmapInterpolationModeFant))) src = sc.get();
        else { ow = sw; oh = sh; }
    }
    ui::Com<IWICFormatConverter> cv;
    if (FAILED(f->CreateFormatConverter(cv.put())) ||
        FAILED(cv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    w = (int)ow; h = (int)oh;
    px.resize((size_t)ow * oh * 4);
    return SUCCEEDED(cv->CopyPixels(nullptr, ow * 4, (UINT)px.size(), px.data()));
}

void open(Request r) {
    if (g) g_dead = std::move(g);
    g = std::make_unique<State>();
    g->kind = kind_for(r.name, r.bytes, r.import_kinds);
    g->fit = g->kind == Kind::Image || g->kind == Kind::Pdf || g->kind == Kind::Gerber;   // "fit" only means something for canvases
    g->rq = std::move(r);
    g->want_open = true;
    g_close = false;
    request_redraw(3);
}
bool is_open() { return g != nullptr; }
void close() { g_close = true; request_redraw(2); }

void draw() {
    g_dead.reset();
    if (!g) return;
    State& s = *g;
    if (s.want_open) { ImGui::OpenPopup("###filepreview"); s.want_open = false; }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.82f, vp->WorkSize.y * 0.86f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(480), S(320)), vp->WorkSize);
    bool open_flag = true;
    const std::string title = "Preview \xE2\x80\x94 " + s.rq.name + "###filepreview";
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    if (ImGui::BeginPopupModal(title.c_str(), &open_flag, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar();
        handle_keys(s);
        draw_toolbar(s);
        ImGui::Separator();
        const ImVec2 av = ImGui::GetContentRegionAvail();
        if (s.fatal.empty()) {
            // R23a: every preview body is fault-isolated. A C++ exception or a hardware fault in a viewer
            // unwinds the ImGui stack back to this point and switches the modal to an error card.
            ImGuiErrorRecoveryState rec;
            ImGui::ErrorRecoveryStoreState(&rec);
            std::string why;
            unsigned long code = 0;
            const bool ok = crash::guarded([&] {
                try {
                    switch (s.kind) {
                    case Kind::Image: draw_image(s, av); break;
                    case Kind::Pdf: draw_pdf(s, av); break;
                    case Kind::Doc: case Kind::Mail: draw_doc(s, av); break;
                    case Kind::Sheet: draw_sheet(s, av); break;
                    case Kind::Gerber: draw_gerber(s, av); break;
                    default: draw_text(s, av); break;
                    }
                } catch (const std::exception& e) { why = e.what(); if (why.empty()) why = "error"; }
                catch (...) { why = "unexpected error"; }
            }, &code);
            if (!ok) why = crash::exception_name(code);
            if (!why.empty()) {
                ImGui::ErrorRecoveryTryToRecoverState(&rec);
                s.fatal = "This file could not be previewed (" + why + "). Nothing was changed; use Extract or Open externally.";
            }
        }
        if (!s.fatal.empty()) {
            ImGui::BeginChild("##pvfatal", av);
            ImGui::Dummy(ImVec2(0, S(24)));
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "Preview failed");
            ImGui::TextWrapped("%s", s.fatal.c_str());
            ImGui::EndChild();
        }
        if (g_close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    } else ImGui::PopStyleVar();
    if (!open_flag || g_close || !ImGui::IsPopupOpen("###filepreview")) {
        if (!s.want_open) { g_dead = std::move(g); g_close = false; request_redraw(2); }
    }
}

} // namespace pcbp::gui::preview
