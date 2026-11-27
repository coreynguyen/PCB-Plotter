// ============================================================================
// doc_viewer.cpp — built-in document and email viewers (REVIEW_V9 §15).
//
//   RichView  : wrapped rich-text layout + drawing for io::RichDoc (bold /
//               italic / underline / strike, sizes, colours, lists, quotes,
//               headings, rules, links (shown + copied, NEVER opened), inline
//               images (WIC-decoded, scaled to fit), basic tables).
//   RichDocDoc: document tab for .rtf / .docx / .html / images. Edit mode is
//               paragraph based (select a paragraph: format it, edit its text,
//               add / delete / move); saves .rtf or .txt (.docx is read-only).
//   MailDoc   : .eml / .msg tab: header card, Formatted / Plain / Headers body
//               views, attachment strip with Save / Save All / Open (routed to
//               the matching built-in viewer or importer; nothing is executed).
//
// Offline and inert: HTML is converted by io::html_to_rich (no scripts, no
// remote loads), RTF fields are not evaluated, attachments are never shelled.
// ============================================================================
#include "file_editors.h"
#include "gfx.h"
#include "../app/app.h"
#include "../app/commands.h"
#include "../core/fileio.h"
#include "../io/docx.h"
#include "../io/mail_eml.h"
#include "../io/mail_msg.h"
#include "../io/mime.h"
#include "../io/richdoc.h"
#include "../io/rtf.h"
#include "../ui/form.h"
#include "../ui/win.h"
#include "imgui_internal.h"
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <cctype>

namespace pcbp::gui {

namespace {

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string ext_of(const std::string& name) {
    size_t k = name.find_last_of('.');
    return k == std::string::npos ? std::string() : lower(name.substr(k + 1));
}
std::string human_size(size_t n) {
    char b[32];
    if (n < 1024) std::snprintf(b, sizeof b, "%zu B", n);
    else if (n < 1024 * 1024) std::snprintf(b, sizeof b, "%.1f KB", n / 1024.0);
    else std::snprintf(b, sizeof b, "%.1f MB", n / 1048576.0);
    return b;
}

// --------------------------------------------------------------------------- images
struct Pic { StaticTexture tex; int w = 0, h = 0; bool tried = false; };

bool decode_image(const std::vector<uint8_t>& data, std::vector<uint8_t>& bgra, int& w, int& h) {
    if (data.empty() || data.size() > (64u << 20)) return false;
    static bool co = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) || true;
    (void)co;
    ui::Com<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put())))) return false;
    ui::Com<IWICStream> st;
    if (FAILED(f->CreateStream(st.put())) || FAILED(st->InitializeFromMemory((BYTE*)data.data(), (DWORD)data.size()))) return false;
    ui::Com<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(st.get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.put()))) return false;
    ui::Com<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, fr.put()))) return false;
    UINT iw = 0, ih = 0;
    fr->GetSize(&iw, &ih);
    if (!iw || !ih) return false;
    IWICBitmapSource* src = fr.get();   // owned by fr / sc below
    ui::Com<IWICBitmapScaler> sc;
    UINT ow = iw, oh = ih;
    const UINT lim = 2048;
    if (iw > lim || ih > lim) {
        double k = std::min((double)lim / iw, (double)lim / ih);
        ow = std::max(1u, (UINT)(iw * k)); oh = std::max(1u, (UINT)(ih * k));
        if (SUCCEEDED(f->CreateBitmapScaler(sc.put())) && SUCCEEDED(sc->Initialize(src, ow, oh, WICBitmapInterpolationModeFant))) src = sc.get();
        else { ow = iw; oh = ih; }
    }
    ui::Com<IWICFormatConverter> cv;
    if (FAILED(f->CreateFormatConverter(cv.put())) || FAILED(cv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    bgra.resize((size_t)ow * oh * 4);
    if (FAILED(cv->CopyPixels(nullptr, ow * 4, (UINT)bgra.size(), bgra.data()))) return false;
    w = (int)ow; h = (int)oh;
    return true;
}

// --------------------------------------------------------------------------- layout
struct Piece {
    float x = 0, w = 0, h = 0;
    ImFont* font = nullptr;
    float px = 0;
    ImU32 col = 0;
    std::string text;
    bool u = false, s = false, it = false;
    std::string link;
    int image = -1;
};
struct Line { float y = 0, h = 0, asc = 0; std::vector<Piece> pcs; };
struct Block;
struct TableLay {
    std::vector<float> colx;          // ncols + 1 edges
    std::vector<float> rowy;          // nrows + 1 edges
    std::vector<std::vector<std::shared_ptr<Block>>> cells;
    std::vector<std::vector<bool>> header;
};
struct Item {
    int kind = 0;                     // 0 text, 1 table, 2 rule
    float y = 0, h = 0, x0 = 0;       // x0 = text indent
    std::vector<Line> lines;
    std::shared_ptr<TableLay> tab;
    int para = -1;                    // top-level paragraph index
    std::string marker;               // list bullet / number
    float marker_x = 0;
    bool quote = false;
    int heading = 0;
};
struct Block { std::vector<Item> items; float h = 0; };

class RichView {
public:
    io::RichDoc* doc = nullptr;
    float zoom = 1.f;
    int sel_para = -1;                // edit mode selection (top-level)
    bool edit_mode = false;
    uint64_t gen = 1;                 // bump when doc changes
    bool clicked_para = false;
    std::string status_link;

    void invalidate() { ++gen; }

    void draw(ImVec2 size) {
        const GTheme& t = th();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.field);
        ImGui::BeginChild("##rich", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoMove);
        ImGui::PopStyleColor();
        const float pad = S(18);
        float w = std::max(S(120), ImGui::GetContentRegionAvail().x - pad * 2);
        if (w != lay_w_ || gen != lay_gen_ || zoom != lay_zoom_ || g_font != lay_font_) {
            root_ = layout(doc ? doc->paras : empty_, w, true);
            lay_w_ = w; lay_gen_ = gen; lay_zoom_ = zoom; lay_font_ = g_font;
        }
        ImVec2 o = ImGui::GetCursorScreenPos();
        o.x += pad; o.y += pad * 0.6f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float vy0 = ImGui::GetWindowPos().y, vy1 = vy0 + ImGui::GetWindowHeight();
        hover_link_.clear();
        clicked_para = false;
        draw_block(dl, root_, o, vy0, vy1, true);
        ImGui::Dummy(ImVec2(w + pad * 2, root_.h + pad * 2));
        if (!hover_link_.empty()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            tooltip(("Link (not opened): " + hover_link_ + "\nClick to copy the address").c_str());
            if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered()) { ImGui::SetClipboardText(hover_link_.c_str()); toast("Link address copied", ToastKind::Info, 2.f); }
        }
        // Ctrl+wheel zoom, Ctrl+C copies all text (no partial selection in the rich view)
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0) { zoom = std::clamp(zoom * (io.MouseWheel > 0 ? 1.1f : 1 / 1.1f), 0.5f, 4.f); request_redraw(2); }
        if (ImGui::IsWindowFocused() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && doc) ImGui::SetClipboardText(doc->plain().c_str());
        if (edit_mode && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !clicked_para) {}
        ImGui::EndChild();
    }

    std::vector<Pic> pics;
private:
    std::vector<io::RichPara> empty_;
    Block root_;
    float lay_w_ = -1, lay_zoom_ = 0;
    uint64_t lay_gen_ = 0;
    ImFont* lay_font_ = nullptr;
    std::string hover_link_;

    Pic* pic(int i) {
        if (!doc || i < 0 || i >= (int)doc->images.size()) return nullptr;
        if ((int)pics.size() < (int)doc->images.size()) pics.resize(doc->images.size());
        Pic& p = pics[i];
        if (!p.tried) {
            p.tried = true;
            std::vector<uint8_t> px;
            if (decode_image(doc->images[i].data, px, p.w, p.h)) p.tex.create(p.w, p.h, px.data(), p.w * 4);
            else p.w = p.h = 0;
        }
        return p.w ? &p : nullptr;
    }

    float base_px() const { return (g_font ? g_font->FontSize : 15.f) * zoom; }   // 11 pt body text == UI text size

    void style_of(const io::RichRun& r, int heading, ImFont*& f, float& px, ImU32& col) const {
        const GTheme& t = th();
        float pt = r.size > 0 ? r.size : heading ? (heading == 1 ? 18.f : heading == 2 ? 15.f : heading == 3 ? 13.f : 11.5f) : 11.f;
        px = std::round(base_px() * pt / 11.f);
        bool bold = r.b || heading;
        ImFont* fam = r.mono ? g_font_mono : bold ? g_font_bold : g_font;
        if (!fam) fam = ImGui::GetFont();
        f = font_for_px(fam, px);
        col = t.text;
        if (!r.link.empty()) col = t.accent;
        else if (r.color) {
            uint32_t c = r.color;
            ImU32 u = IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, 255);
            // keep document colours readable on the theme background
            float lum = (0.299f * ((c >> 16) & 255) + 0.587f * ((c >> 8) & 255) + 0.114f * (c & 255)) / 255.f;
            if (t.dark && lum < 0.25f) u = t.text;
            else if (!t.dark && lum > 0.85f) u = t.text;
            col = u;
        }
    }

    Block layout(const std::vector<io::RichPara>& paras, float width, bool top) {
        Block b;
        float y = 0;
        const float gap = std::round(S(5) * zoom);
        for (size_t pi = 0; pi < paras.size(); ++pi) {
            const io::RichPara& p = paras[pi];
            Item it;
            it.para = top ? (int)pi : -1;
            it.y = y;
            if (p.table >= 0 && doc && p.table < (int)doc->tables.size()) {
                it.kind = 1;
                it.tab = layout_table(doc->tables[p.table], width);
                it.h = it.tab->rowy.back();
            } else if (p.rule) {
                it.kind = 2;
                it.h = S(12) * zoom;
            } else {
                float ind = (p.level + (p.list ? 1 : 0)) * S(22) * zoom + (p.quote ? S(12) * zoom : 0);
                it.x0 = ind;
                it.quote = p.quote;
                it.heading = p.heading;
                if (p.list == 1) it.marker = p.level % 2 ? "-" : "\xE2\x80\xA2";   // v17: U+25E6 not in the font
                else if (p.list == 2) it.marker = std::to_string(p.num > 0 ? p.num : 1) + ".";
                it.marker_x = ind - S(18) * zoom;
                wrap(p, width - ind, it);
                it.h = it.lines.empty() ? base_px() * 1.25f : it.lines.back().y + it.lines.back().h;
            }
            y += it.h + (it.kind == 1 ? gap * 2 : p.heading ? gap * 1.6f : gap);
            b.items.push_back(std::move(it));
        }
        b.h = y;
        return b;
    }

    void wrap(const io::RichPara& p, float width, Item& it) {
        Line ln;
        float x = 0;
        auto new_line = [&]() {
            float h = 0, asc = 0;
            for (auto& pc : ln.pcs) { h = std::max(h, pc.h); asc = std::max(asc, pc.image >= 0 ? pc.h : pc.px); }
            if (ln.pcs.empty()) h = base_px();
            ln.h = std::round(h * 1.22f);
            ln.asc = asc;
            float y = it.lines.empty() ? 0 : it.lines.back().y + it.lines.back().h;
            ln.y = y;
            if (p.align && !ln.pcs.empty()) {
                float used = ln.pcs.back().x + ln.pcs.back().w, sh = width - used;
                if (p.align == 1) sh *= 0.5f;
                for (auto& pc : ln.pcs) pc.x += std::max(0.f, sh);
            }
            it.lines.push_back(std::move(ln));
            ln = Line{};
            x = 0;
        };
        for (const io::RichRun& r : p.runs) {
            ImFont* f; float px; ImU32 col;
            style_of(r, p.heading, f, px, col);
            if (r.image >= 0) {
                Pic* pc = pic(r.image);
                Piece pe;
                pe.link = r.link;
                if (pc) {
                    float k = std::min(1.f, width / std::max(1, pc->w)) * zoom * std::max(1.f, dpi_scale() * 0.75f);
                    k = std::min(k, width / std::max(1, pc->w));
                    pe.w = pc->w * k; pe.h = pc->h * k; pe.image = r.image;
                } else {
                    pe.text = "[image: " + (doc ? doc->images[r.image].name : std::string()) + "]";
                    pe.font = f; pe.px = px; pe.col = th().text_dim; pe.it = true;
                    pe.w = f->CalcTextSizeA(px, FLT_MAX, 0, pe.text.c_str()).x; pe.h = px;
                }
                if (x > 0 && x + pe.w > width) new_line();
                pe.x = x;
                x += pe.w + S(4);
                ln.pcs.push_back(std::move(pe));
                continue;
            }
            // split on spaces / newlines / tabs, keep trailing space with the word
            const std::string& s = r.text;
            size_t i = 0;
            while (i < s.size()) {
                if (s[i] == '\n') { new_line(); ++i; continue; }
                if (s[i] == '\t') {
                    float tw = S(36) * zoom;
                    x = (std::floor(x / tw) + 1) * tw;
                    ++i;
                    continue;
                }
                size_t e = i;
                while (e < s.size() && s[e] != ' ' && s[e] != '\n' && s[e] != '\t') ++e;
                while (e < s.size() && s[e] == ' ') ++e;
                std::string word = s.substr(i, e - i);
                float ww = f->CalcTextSizeA(px, FLT_MAX, 0, word.c_str()).x;
                std::string trimmed = word;
                while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
                float tw = f->CalcTextSizeA(px, FLT_MAX, 0, trimmed.c_str()).x;
                if (x > 0 && x + tw > width) new_line();
                // a single word wider than the line: hard-break by characters
                if (tw > width && x == 0) {
                    size_t a = 0;
                    while (a < word.size()) {
                        size_t bpos = a + 1;
                        while (bpos < word.size()) {
                            size_t nb = bpos + 1;
                            while (nb < word.size() && ((unsigned char)word[nb] & 0xC0) == 0x80) ++nb;
                            if (f->CalcTextSizeA(px, FLT_MAX, 0, word.c_str() + a, word.c_str() + nb).x > width) break;
                            bpos = nb;
                        }
                        while (bpos < word.size() && ((unsigned char)word[bpos] & 0xC0) == 0x80) ++bpos;
                        Piece pe{0, f->CalcTextSizeA(px, FLT_MAX, 0, word.c_str() + a, word.c_str() + bpos).x, px, f, px, col, word.substr(a, bpos - a), r.u, r.s, r.i, r.link, -1};
                        ln.pcs.push_back(pe);
                        a = bpos;
                        if (a < word.size()) new_line();
                        else x = pe.w;
                    }
                    i = e;
                    continue;
                }
                // merge with previous piece of the same style
                if (!ln.pcs.empty()) {
                    Piece& lp = ln.pcs.back();
                    if (lp.font == f && lp.px == px && lp.col == col && lp.u == r.u && lp.s == r.s && lp.it == r.i && lp.link == r.link && lp.image < 0 && std::fabs(lp.x + lp.w - x) < 0.5f) {
                        lp.text += word; lp.w += ww; x += ww; i = e; continue;
                    }
                }
                Piece pe{x, ww, px, f, px, col, word, r.u, r.s, r.i, r.link, -1};
                ln.pcs.push_back(std::move(pe));
                x += ww;
                i = e;
            }
        }
        if (!ln.pcs.empty() || it.lines.empty()) new_line();
    }

    std::shared_ptr<TableLay> layout_table(const io::RichTable& t, float width) {
        auto tl = std::make_shared<TableLay>();
        size_t nc = 0;
        for (auto& r : t.rows) nc = std::max(nc, r.size());
        if (!nc) nc = 1;
        // column widths: proportional to the longest plain text per column, with a floor
        std::vector<float> want(nc, S(40));
        for (auto& r : t.rows)
            for (size_t c = 0; c < r.size(); ++c) {
                float m = 0;
                for (auto& p : r[c].paras) m = std::max(m, (g_font ? g_font->CalcTextSizeA(base_px(), FLT_MAX, 0, p.text().c_str()).x : 50.f));
                want[c] = std::max(want[c], std::min(m + S(16), width * 0.6f));
            }
        float sum = 0;
        for (float v : want) sum += v;
        float k = sum > width ? width / sum : 1.f;
        tl->colx.push_back(0);
        for (size_t c = 0; c < nc; ++c) tl->colx.push_back(tl->colx.back() + want[c] * k);
        const float cp = S(6) * zoom;
        tl->rowy.push_back(0);
        for (auto& r : t.rows) {
            std::vector<std::shared_ptr<Block>> row;
            std::vector<bool> hdr;
            float h = base_px() * 1.3f;
            for (size_t c = 0; c < nc; ++c) {
                auto blk = std::make_shared<Block>();
                if (c < r.size()) {
                    std::vector<io::RichPara> ps = r[c].paras;
                    if (r[c].header) for (auto& p : ps) for (auto& rr : p.runs) rr.b = true;
                    *blk = layout(ps, std::max(S(10), tl->colx[c + 1] - tl->colx[c] - cp * 2), false);
                }
                h = std::max(h, blk->h + cp * 2);
                row.push_back(blk);
                hdr.push_back(c < r.size() && r[c].header);
            }
            tl->cells.push_back(std::move(row));
            tl->header.push_back(std::move(hdr));
            tl->rowy.push_back(tl->rowy.back() + h);
        }
        return tl;
    }

    void draw_text_piece(ImDrawList* dl, const Piece& pc, ImVec2 at) {
        int v0 = dl->VtxBuffer.Size;
        dl->AddText(pc.font, pc.px, at, pc.col, pc.text.c_str(), pc.text.c_str() + pc.text.size());
        if (pc.it) {   // synthetic italic: shear the glyph quads
            float base = at.y + pc.px;
            for (int v = v0; v < dl->VtxBuffer.Size; ++v) dl->VtxBuffer[v].pos.x += (base - dl->VtxBuffer[v].pos.y) * 0.2f;
        }
        std::string tt = pc.text;
        while (!tt.empty() && tt.back() == ' ') tt.pop_back();
        float tw = pc.font->CalcTextSizeA(pc.px, FLT_MAX, 0, tt.c_str()).x;
        float lw = std::max(1.f, pc.px / 14.f);
        if (pc.u) dl->AddLine(ImVec2(at.x, at.y + pc.px * 0.98f), ImVec2(at.x + tw, at.y + pc.px * 0.98f), pc.col, lw);
        if (pc.s) dl->AddLine(ImVec2(at.x, at.y + pc.px * 0.55f), ImVec2(at.x + tw, at.y + pc.px * 0.55f), pc.col, lw);
        if (!pc.link.empty() && ImGui::IsMouseHoveringRect(at, ImVec2(at.x + pc.w, at.y + pc.h * 1.2f)) && ImGui::IsWindowHovered()) hover_link_ = pc.link;
    }

    void draw_block(ImDrawList* dl, const Block& b, ImVec2 o, float vy0, float vy1, bool top) {
        const GTheme& t = th();
        for (const Item& it : b.items) {
            float y0 = o.y + it.y, y1 = y0 + it.h;
            if (y1 < vy0 - S(20) || y0 > vy1 + S(20)) {
                if (top && edit_mode) hit_para(it, o, y0, y1);
                continue;
            }
            if (top && edit_mode) {
                bool hov = hit_para(it, o, y0, y1);
                if (it.para == sel_para) dl->AddRectFilled(ImVec2(o.x - S(10), y0 - S(2)), ImVec2(o.x + lay_w_ + S(10), y1 + S(2)), col_alpha(t.accent, 0.14f), S(3));
                else if (hov) dl->AddRectFilled(ImVec2(o.x - S(10), y0 - S(2)), ImVec2(o.x + lay_w_ + S(10), y1 + S(2)), col_alpha(t.text, 0.05f), S(3));
            }
            if (it.kind == 2) { float yy = y0 + it.h * 0.5f; dl->AddLine(ImVec2(o.x, yy), ImVec2(o.x + lay_w_, yy), t.border, S(1)); continue; }
            if (it.kind == 1) {
                const TableLay& tl = *it.tab;
                float W = tl.colx.back();
                for (size_t r = 0; r + 1 < tl.rowy.size(); ++r) {
                    float ry0 = y0 + tl.rowy[r], ry1 = y0 + tl.rowy[r + 1];
                    bool hdr = !tl.header[r].empty() && tl.header[r][0];
                    if (hdr || r == 0) dl->AddRectFilled(ImVec2(o.x, ry0), ImVec2(o.x + W, ry1), col_alpha(t.text, hdr ? 0.08f : 0.03f));
                    for (size_t c = 0; c < tl.cells[r].size(); ++c)
                        draw_block(dl, *tl.cells[r][c], ImVec2(o.x + tl.colx[c] + S(6) * zoom, ry0 + S(6) * zoom), vy0, vy1, false);
                    dl->AddLine(ImVec2(o.x, ry0), ImVec2(o.x + W, ry0), t.border, 1.f);
                }
                dl->AddLine(ImVec2(o.x, y1), ImVec2(o.x + W, y1), t.border, 1.f);
                for (float cx : tl.colx) dl->AddLine(ImVec2(o.x + cx, y0), ImVec2(o.x + cx, y1), t.border, 1.f);
                continue;
            }
            if (it.quote) dl->AddRectFilled(ImVec2(o.x + it.x0 - S(10) * zoom, y0), ImVec2(o.x + it.x0 - S(7) * zoom, y1), col_alpha(t.accent, 0.5f));
            if (!it.marker.empty() && !it.lines.empty()) {
                const Line& l0 = it.lines[0];
                float px = l0.pcs.empty() ? base_px() : l0.pcs[0].px;
                ImFont* f = font_for_px(g_font, px);
                dl->AddText(f, px, ImVec2(o.x + it.marker_x, y0 + l0.y + (l0.asc - px)), t.text_dim, it.marker.c_str());
            }
            for (const Line& l : it.lines) {
                float ly = y0 + l.y;
                if (ly > vy1 || ly + l.h < vy0) continue;
                for (const Piece& pc : l.pcs) {
                    ImVec2 at(o.x + it.x0 + pc.x, ly + (l.asc - (pc.image >= 0 ? pc.h : pc.px)));
                    if (pc.image >= 0) {
                        if (Pic* p = pic(pc.image)) dl->AddImage((ImTextureID)p->tex.imgui_id(), at, ImVec2(at.x + pc.w, at.y + pc.h));
                        if (!pc.link.empty() && ImGui::IsMouseHoveringRect(at, ImVec2(at.x + pc.w, at.y + pc.h)) && ImGui::IsWindowHovered()) hover_link_ = pc.link;
                        continue;
                    }
                    draw_text_piece(dl, pc, at);
                }
            }
        }
    }
    bool hit_para(const Item& it, ImVec2 o, float y0, float y1) {
        bool hov = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(o.x - S(10), y0 - S(2)), ImVec2(o.x + lay_w_ + S(10), y1 + S(2)), false);
        if (hov && ImGui::IsMouseClicked(0)) { sel_para = it.para; clicked_para = true; request_redraw(2); }
        return hov;
    }
};

// --------------------------------------------------------------------------- helpers
std::wstring temp_dir_for(const std::string& sub) {
    wchar_t tmp[MAX_PATH + 2] = {};
    GetTempPathW(MAX_PATH + 1, tmp);
    std::wstring d = std::wstring(tmp) + L"PCB Plotter";
    CreateDirectoryW(d.c_str(), nullptr);
    d += L"\\Attachments";
    CreateDirectoryW(d.c_str(), nullptr);
    std::string s;
    for (char c : sub) s += std::isalnum((unsigned char)c) ? c : '_';
    if (s.size() > 40) s = s.substr(0, 40);
    d += L"\\" + ui::W(s.empty() ? std::string("mail") : s);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

ui::Icon icon_for(const std::string& name, const std::string& mime) {
    std::string e = ext_of(name);
    if (e == "xlsx" || e == "xls" || e == "xlsm" || e == "ods" || e == "csv" || e == "tsv") return ui::Icon::Bom;
    if (e == "png" || e == "jpg" || e == "jpeg" || e == "gif" || e == "bmp" || e == "tif" || e == "tiff" || mime.rfind("image/", 0) == 0) return ui::Icon::Image;
    if (e == "eml" || e == "msg" || mime == "message/rfc822") return ui::Icon::Link;
    if (e == "zip" || e == "tgz" || e == "gz" || e == "7z" || e == "rar") return ui::Icon::Folder;
    if (e == "gbr" || e == "ger" || e == "gtl" || e == "gbl" || e == "drl" || e == "odb") return ui::Icon::ImportGerber;
    if (e == "txt" || e == "log" || e == "rtf" || e == "docx" || e == "doc" || e == "pdf" || e == "htm" || e == "html") return ui::Icon::Notes;
    if (e == "exe" || e == "bat" || e == "cmd" || e == "js" || e == "vbs" || e == "ps1" || e == "scr" || e == "lnk" || e == "msi" || e == "hta") return ui::Icon::Warning;
    return ui::Icon::Open;
}

bool risky_ext(const std::string& name) {
    std::string e = ext_of(name);
    for (const char* r : {"exe", "bat", "cmd", "com", "js", "jse", "vbs", "vbe", "wsf", "ps1", "scr", "lnk", "msi", "hta", "pif", "cpl", "jar", "docm", "xlsm", "dll"})
        if (e == r) return true;
    return false;
}

class MailDoc;
std::unique_ptr<FileDoc> make_mail_doc_from(std::shared_ptr<io::MailMessage> m, const std::string& name);
std::unique_ptr<FileDoc> make_rich_doc_from(io::RichDoc d, const std::string& name, const std::string& kind, bool editable);

// open attachment bytes in the matching built-in viewer (never executes anything)
void open_bytes(const std::vector<uint8_t>& data, const std::string& name, const std::string& mime, const std::string& tmp_key) {
    std::string e = ext_of(name), err;
    if (e == "eml" || mime == "message/rfc822" || (e.empty() && io::is_eml_bytes(data.data(), data.size()))) {
        auto m = std::make_shared<io::MailMessage>();
        if (io::eml_parse(data, *m, err)) { files_add(make_mail_doc_from(m, name)); return; }
    }
    if (e == "msg" || (io::is_cfb_bytes(data.data(), data.size()) && e != "xls" && e != "doc")) {
        auto m = std::make_shared<io::MailMessage>();
        if (io::msg_parse(data, *m, err)) { files_add(make_mail_doc_from(m, name)); return; }
    }
    if (io::is_rtf_bytes(data.data(), data.size())) {
        io::RichDoc d;
        if (io::rtf_to_rich(std::string(data.begin(), data.end()), d, err)) { files_add(make_rich_doc_from(std::move(d), name, "Rich text", true)); return; }
    }
    if (e == "docx" || io::is_docx_bytes(data)) {
        io::RichDoc d;
        if (io::docx_read(data, d, err)) { files_add(make_rich_doc_from(std::move(d), name, "Word document", false)); return; }
        toast(name + ": Word document not supported (" + err + ")", ToastKind::Warning, 6.f);   // R11 P2-11: never as raw text
        files_add(make_hex_doc_from(data, name));
        return;
    }
    if (e == "htm" || e == "html" || mime == "text/html") {
        io::RichDoc d;
        std::string h(data.begin(), data.end());
        std::string cs = io::html_meta_charset(h);
        io::html_to_rich(io::charset_to_utf8(h, cs), d);
        files_add(make_rich_doc_from(std::move(d), name, "Web page (safe view)", false));
        return;
    }
    if (e == "png" || e == "jpg" || e == "jpeg" || e == "gif" || e == "bmp" || e == "tif" || e == "tiff" || mime.rfind("image/", 0) == 0) {
        io::RichDoc d;
        d.images.push_back({name, mime, "", data});
        io::RichPara p; io::RichRun r; r.image = 0; r.text = name; p.runs.push_back(r);
        d.paras.push_back(p);
        files_add(make_rich_doc_from(std::move(d), name, "Image", false));
        return;
    }
    // everything else goes through a temp copy so the regular open routing (spreadsheet editor,
    // text editor, importers) applies; the copy is read by PCB Plotter only
    std::wstring dir = temp_dir_for(tmp_key);
    std::wstring p = dir + L"\\" + ui::W(name);
    if (!write_file(ui::U(p), data.data(), data.size(), err)) { toast("Could not write a temporary copy: " + err, ToastKind::Error, 5.f); return; }
    if (e == "xlsx" || e == "xls" || e == "ods" || e == "csv" || e == "tsv") { files_open(p, 1); return; }
    if (e == "txt" || e == "log" || e == "ini" || e == "xml" || e == "json") { files_open(p, 2); return; }
    int kind = sniff_document_kind(p);
    if (kind == 2) { files_open(p, 2); return; }
    // archives / CAD / Gerber / machine programs: hand over to the normal Open routing (asks first)
    std::wstring pp = p;
    defer([pp]() {
        app::DocPtr before = app::active_doc();
        app::DocPtr d = app::open_document(pp);
        (void)before; (void)d;
    });
}

// --------------------------------------------------------------------------- RichDocDoc
class RichDocDoc final : public FileDoc {
public:
    io::RichDoc d;
    RichView view;
    std::string kind_ = "Rich text";
    bool editable = true;
    bool dirty_ = false;
    std::string edit_buf;
    int edit_para = -1;
    float size_pt = 11;

    bool is_sheet() const override { return false; }
    bool dirty() const override { return dirty_; }
    const char* kind() const override { return kind_.c_str(); }
    ui::Icon icon() const override { return kind_ == "Image" ? ui::Icon::Image : ui::Icon::Notes; }
    std::string status() const override {
        size_t words = 0;
        std::string pl = d.plain();
        bool in = false;
        for (unsigned char c : pl) { bool w = c > ' '; if (w && !in) ++words; in = w; }
        return kind_ + "  \xC2\xB7  " + std::to_string(d.paras.size()) + " paragraphs, " + std::to_string(words) + " words" + (d.images.empty() ? "" : ", " + std::to_string(d.images.size()) + " images") + (editable ? "" : "  \xC2\xB7  read-only (Save As writes .rtf)");
    }

    void changed() { dirty_ = true; view.invalidate(); request_redraw(2); }

    io::RichPara* sel() { return view.sel_para >= 0 && view.sel_para < (int)d.paras.size() ? &d.paras[view.sel_para] : nullptr; }

    void for_runs(const std::function<void(io::RichRun&)>& fn) {
        io::RichPara* p = sel();
        if (!p || p->table >= 0) return;
        for (auto& r : p->runs) fn(r);
        changed();
    }
    bool all_runs(const std::function<bool(const io::RichRun&)>& fn) {
        io::RichPara* p = sel();
        if (!p || p->runs.empty()) return false;
        for (auto& r : p->runs) if (!fn(r)) return false;
        return true;
    }

    void draw(ImVec2 size) override {
        const GTheme& t = th();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + S(38)), t.panel);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + S(4), p0.y + S(5)));
        ImGui::BeginGroup();
        if (fe_tool("save", ui::Icon::Save, "Save (Ctrl+S)")) defer([this]() { save(false); });
        if (fe_tool("saveas", ui::Icon::SaveAs, "Save As .rtf / .txt")) defer([this]() { save(true); });
        if (fe_tool("copy", ui::Icon::Copy, "Copy all text")) ImGui::SetClipboardText(d.plain().c_str());
        if (fe_tool("totext", ui::Icon::Notes, "Open as plain text in the Text editor")) { std::string s = d.plain(); std::string n = name + ".txt"; defer([s, n]() { files_add(make_text_doc_from(s, n)); }); }
        ImGui::SameLine(0, S(10));
        if (fe_tool("edit", ui::Icon::Text, editable ? "Edit mode: click a paragraph to format / edit it" : "Read-only source: enable editing (Save As writes .rtf)", true, view.edit_mode)) { view.edit_mode = !view.edit_mode; editable = true; view.invalidate(); }
        if (view.edit_mode) {
            io::RichPara* p = sel();
            bool en = p && p->table < 0;
            if (fe_tool("b", ui::Icon::Text, "Bold", en, en && all_runs([](const io::RichRun& r) { return r.b; }))) { bool v = !all_runs([](const io::RichRun& r) { return r.b; }); for_runs([v](io::RichRun& r) { r.b = v; }); }
            ImGui::SameLine(0, S(2));
            auto tbtn = [&](const char* id, const char* lbl, const char* tip, bool on) {
                ImGui::PushStyleColor(ImGuiCol_Button, on ? col_alpha(t.accent, 0.35f) : 0);
                bool c = ImGui::Button(lbl, ImVec2(S(28), S(28)));
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) tooltip(tip);
                (void)id;
                ImGui::SameLine(0, S(2));
                return c && en;
            };
            ImGui::BeginDisabled(!en);
            if (tbtn("i", "I", "Italic", en && all_runs([](const io::RichRun& r) { return r.i; }))) { bool v = !all_runs([](const io::RichRun& r) { return r.i; }); for_runs([v](io::RichRun& r) { r.i = v; }); }
            if (tbtn("u", "U", "Underline", en && all_runs([](const io::RichRun& r) { return r.u; }))) { bool v = !all_runs([](const io::RichRun& r) { return r.u; }); for_runs([v](io::RichRun& r) { r.u = v; }); }
            if (tbtn("s", "S", "Strikethrough", en && all_runs([](const io::RichRun& r) { return r.s; }))) { bool v = !all_runs([](const io::RichRun& r) { return r.s; }); for_runs([v](io::RichRun& r) { r.s = v; }); }
            ImGui::SetNextItemWidth(S(64));
            static const char* sizes[] = {"8", "9", "10", "11", "12", "14", "16", "18", "20", "24", "28", "36"};
            int cur = -1;
            if (p && !p->runs.empty()) { float sz = p->runs[0].size > 0 ? p->runs[0].size : 11; for (int k = 0; k < 12; ++k) if (std::fabs(std::atof(sizes[k]) - sz) < 0.1) cur = k; }
            if (ImGui::BeginCombo("##sz", cur >= 0 ? sizes[cur] : "\xE2\x80\x94")) {
                for (int k = 0; k < 12; ++k) if (ImGui::Selectable(sizes[k], k == cur)) { float v = (float)std::atof(sizes[k]); for_runs([v](io::RichRun& r) { r.size = v == 11 ? 0 : v; }); }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) tooltip("Font size (pt)");
            ImGui::SameLine(0, S(2));
            if (tbtn("col", "A", "Text colour\xE2\x80\xA6", false)) defer([this]() { uint32_t c = 0xFFC00000; if (ui::pick_color_dialog(app::frame_hwnd(), c)) for_runs([c](io::RichRun& r) { r.color = c | 0xFF000000u; }); });
            if (tbtn("bul", "\xE2\x80\xA2", "Bulleted list", p && p->list == 1)) { p->list = p->list == 1 ? 0 : 1; changed(); }
            if (tbtn("num", "1.", "Numbered list", p && p->list == 2)) { p->list = p->list == 2 ? 0 : 2; renumber(); changed(); }
            if (tbtn("ind", "\xE2\x86\x92", "Indent", false)) { p->level = std::min(6, p->level + 1); changed(); }
            if (tbtn("out", "\xE2\x86\x90", "Outdent", false)) { p->level = std::max(0, p->level - 1); changed(); }
            if (tbtn("al", p && p->align == 1 ? "Ctr" : p && p->align == 2 ? "Rgt" : "Lft", "Alignment (left / centre / right)", false)) { p->align = (p->align + 1) % 3; changed(); }
            ImGui::EndDisabled();
            ImGui::SameLine(0, S(10));
            if (fe_tool("addp", ui::Icon::Plus, "New paragraph below")) { int at = view.sel_para < 0 ? (int)d.paras.size() : view.sel_para + 1; io::RichPara np; if (p) { np.list = p->list; np.level = p->level; } d.paras.insert(d.paras.begin() + at, np); view.sel_para = at; renumber(); changed(); }
            if (fe_tool("up", ui::Icon::ArrowUp, "Move paragraph up", en && view.sel_para > 0)) { std::swap(d.paras[view.sel_para], d.paras[view.sel_para - 1]); --view.sel_para; renumber(); changed(); }
            if (fe_tool("dn", ui::Icon::ArrowDown, "Move paragraph down", en && view.sel_para + 1 < (int)d.paras.size())) { std::swap(d.paras[view.sel_para], d.paras[view.sel_para + 1]); ++view.sel_para; renumber(); changed(); }
            if (fe_tool("del", ui::Icon::Delete, "Delete paragraph", p != nullptr)) { d.paras.erase(d.paras.begin() + view.sel_para); view.sel_para = std::min(view.sel_para, (int)d.paras.size() - 1); renumber(); changed(); }
        }
        ImGui::EndGroup();
        // zoom on the right
        {
            char zl[16]; std::snprintf(zl, sizeof zl, "%d%%", (int)std::lround(view.zoom * 100));
            float bw = S(52);
            ImGui::SetCursorScreenPos(ImVec2(p0.x + size.x - bw * 3 - S(8), p0.y + S(6)));
            if (ImGui::Button("-##z", ImVec2(bw, S(26)))) { view.zoom = std::max(0.5f, view.zoom / 1.1f); }
            ImGui::SameLine(0, 0);
            if (ImGui::Button(zl, ImVec2(bw, S(26)))) view.zoom = 1.f;
            ImGui::SameLine(0, 0);
            if (ImGui::Button("+##z", ImVec2(bw, S(26)))) { view.zoom = std::min(4.f, view.zoom * 1.1f); }
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + S(38)));
        float edit_h = 0;
        io::RichPara* p = view.edit_mode ? sel() : nullptr;
        if (p && p->table < 0) edit_h = S(110);
        view.draw(ImVec2(size.x, size.y - S(38) - edit_h));
        if (edit_h > 0) {
            // paragraph text editor (plain text of the selected paragraph)
            if (edit_para != view.sel_para) { edit_para = view.sel_para; edit_buf = p->text(); }
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), S(6)));
            ImGui::TextDisabled("Paragraph %d text \xE2\x80\x94 editing text keeps the paragraph's first style; formatting buttons apply to the whole paragraph", view.sel_para + 1);
            std::vector<char> buf(edit_buf.begin(), edit_buf.end());
            buf.resize(edit_buf.size() + 4096, 0);
            if (ImGui::InputTextMultiline("##ptext", buf.data(), buf.size(), ImVec2(size.x - S(8), edit_h - S(30)))) {
                std::string nt(buf.data());
                if (nt != edit_buf) {
                    edit_buf = nt;
                    io::RichRun proto = p->runs.empty() ? io::RichRun{} : p->runs[0];
                    bool has_img = false;
                    for (auto& r : p->runs) if (r.image >= 0) has_img = true;
                    if (has_img) { std::vector<io::RichRun> keep; for (auto& r : p->runs) if (r.image >= 0) keep.push_back(r); p->runs = keep; }
                    else p->runs.clear();
                    proto.text = nt; proto.image = -1;
                    if (!nt.empty()) p->runs.insert(p->runs.begin(), proto);
                    changed();
                }
            }
            ImGui::PopStyleVar();
        } else edit_para = -1;
    }

    void renumber() {
        int n = 0, lvl = -1;
        for (auto& p : d.paras) {
            if (p.list == 2) { if (lvl != p.level) { n = 0; lvl = p.level; } p.num = ++n; }
            else if (p.list == 0) { n = 0; lvl = -1; }
        }
    }

    bool save(bool save_as) override {
        std::wstring p = path;
        std::string e = ext_of(ui::U(p));
        if (save_as || p.empty() || (e != "rtf" && e != "txt")) {
            std::string stem = name;
            size_t k = stem.find_last_of('.');
            if (k != std::string::npos) stem.resize(k);
            p = (path.empty() ? std::wstring() : path.substr(0, path.find_last_of(L"\\/") + 1)) + ui::W(stem) + L".rtf";
            if (!ui::save_file_dialog(app::frame_hwnd(), L"Save Document", {{L"Rich Text Format (*.rtf)", L"*.rtf"}, {L"Plain text (*.txt)", L"*.txt"}}, L"rtf", p)) return false;
            e = ext_of(ui::U(p));
        }
        std::string out = e == "txt" ? d.plain() : io::rich_to_rtf(d);
        std::string err;
        if (!write_file(ui::U(p), out.data(), out.size(), err)) { toast("Save failed: " + err, ToastKind::Error, 6.f); return false; }
        path = p;
        name = ui::U(file_name_of(p));
        kind_ = e == "txt" ? "Text" : "Rich text";
        dirty_ = false;
        gui::status("Saved " + name, ToastKind::Success);
        return true;
    }
    bool on_command(int id) override {
        if (id == app::ID_EDIT_COPY) { ImGui::SetClipboardText(d.plain().c_str()); return true; }
        return false;
    }
};

// --------------------------------------------------------------------------- MailDoc
class MailDoc final : public FileDoc {
public:
    std::shared_ptr<io::MailMessage> m;
    io::RichDoc body;                 // formatted
    io::RichDoc plain;
    RichView view;
    int mode = 0;                     // 0 formatted, 1 plain, 2 headers
    std::string body_src;             // "HTML" / "RTF" / "Text"
    std::map<int, int> att_image;     // attachment -> body image index

    bool is_sheet() const override { return false; }
    bool dirty() const override { return false; }
    const char* kind() const override { return m && m->format == "MSG" ? "Outlook message" : "Email"; }
    ui::Icon icon() const override { return ui::Icon::Link; }
    std::string status() const override {
        return std::string(kind()) + "  \xC2\xB7  body: " + body_src + "  \xC2\xB7  " + std::to_string(m ? m->atts.size() : 0) + " attachment(s)  \xC2\xB7  links and remote images are never opened";
    }

    void build() {
        body = io::RichDoc{};
        auto resolve = [&](const std::string& cid) -> int {
            for (size_t i = 0; i < m->atts.size(); ++i)
                if (!m->atts[i].cid.empty() && m->atts[i].cid == cid) {
                    auto it = att_image.find((int)i);
                    if (it != att_image.end()) return it->second;
                    body.images.push_back({m->atts[i].name, m->atts[i].mime, cid, m->atts[i].data});
                    int k = (int)body.images.size() - 1;
                    att_image[(int)i] = k;
                    return k;
                }
            return -1;
        };
        std::string err;
        if (!m->html.empty()) { io::html_to_rich(m->html, body, resolve); body_src = "HTML (safe view)"; }
        else if (!m->rtf.empty() && !io::rtf_is_fromtext(m->rtf) && io::rtf_to_rich(m->rtf, body, err) && !body.empty()) body_src = "RTF";
        else { body = io::rich_from_plain(m->text); body_src = "Text"; }
        plain = io::rich_from_plain(m->text.empty() ? body.plain() : m->text);
        for (auto& p : plain.paras) for (auto& r : p.runs) r.mono = false;
        view.doc = &body;
        view.invalidate();
    }

    void set_mode(int k) {
        mode = k;
        if (k == 0) view.doc = &body;
        else if (k == 1) view.doc = &plain;
        else {
            hdr_doc = io::rich_from_plain(m->raw_headers.empty() ? std::string("(no header block)") : m->raw_headers);
            for (auto& p : hdr_doc.paras) for (auto& r : p.runs) r.mono = true;
            view.doc = &hdr_doc;
        }
        view.pics.clear();
        view.invalidate();
    }
    io::RichDoc hdr_doc;

    void save_att(int i) {
        const io::MailAttachment& a = m->atts[i];
        std::wstring p = ui::W(io::safe_attachment_name(a, i));
        std::string e = ext_of(ui::U(p));
        std::wstring pat = e.empty() ? L"*.*" : L"*." + ui::W(e);
        if (!ui::save_file_dialog(app::frame_hwnd(), L"Save Attachment", {{L"Attachment", pat}, {L"All files", L"*.*"}}, ui::W(e), p)) return;
        std::string err;
        if (!write_file(ui::U(p), a.data.data(), a.data.size(), err)) { toast("Could not save: " + err, ToastKind::Error, 6.f); return; }
        gui::status("Saved " + ui::U(file_name_of(p)), ToastKind::Success);
    }
    void save_all() {
        std::wstring dir;
        if (!ui::pick_folder_dialog(app::frame_hwnd(), L"Save all attachments to", dir)) return;
        int n = 0;
        std::string err;
        for (size_t i = 0; i < m->atts.size(); ++i) {
            std::string nm = io::safe_attachment_name(m->atts[i], (int)i);
            std::wstring p = dir + L"\\" + ui::W(nm);
            for (int k = 2; GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && k < 1000; ++k) {   // never overwrite
                size_t dot = nm.find_last_of('.');
                std::string alt = dot == std::string::npos ? nm + " (" + std::to_string(k) + ")" : nm.substr(0, dot) + " (" + std::to_string(k) + ")" + nm.substr(dot);
                p = dir + L"\\" + ui::W(alt);
            }
            if (write_file(ui::U(p), m->atts[i].data.data(), m->atts[i].data.size(), err)) ++n;
        }
        toast("Saved " + std::to_string(n) + " of " + std::to_string(m->atts.size()) + " attachments", n == (int)m->atts.size() ? ToastKind::Success : ToastKind::Warning, 4.f);
    }
    void open_att(int i) {
        const io::MailAttachment& a = m->atts[i];
        std::string nm = io::safe_attachment_name(a, i);
        if (a.embedded) { files_add(make_mail_doc_from(a.embedded, nm)); return; }
        if (risky_ext(nm)) {
            ui::message(app::frame_hwnd(), L"Open Attachment", L"\"" + ui::W(nm) + L"\" is a program, script or macro-enabled file.\nPCB Plotter never runs attachments. Use Save to store it, then inspect it with care.\n\nIt will be shown in the hex / text inspector.");
            files_add(make_hex_doc_from(a.data, nm));
            return;
        }
        open_bytes(a.data, nm, a.mime, m->subject.empty() ? name : m->subject);
    }

    void field(const char* k, const std::string& v) {
        if (v.empty()) return;
        ImGui::TextDisabled("%s", k);
        ImGui::SameLine(S(70));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(v.c_str());
        ImGui::PopTextWrapPos();
    }

    void draw(ImVec2 size) override {
        const GTheme& t = th();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        // ---- header card
        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.panel);
        ImGui::BeginChild("##mailhdr", ImVec2(size.x, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        ImGui::PushFont(g_font_big ? g_font_bold : g_font);
        ImGui::PushTextWrapPos(size.x - S(260));
        ImGui::TextUnformatted(m->subject.empty() ? "(no subject)" : m->subject.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        float right = ImGui::GetWindowPos().x + size.x - S(250);
        ImVec2 save_cur = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(right, ImGui::GetWindowPos().y + S(8)));
        ImGui::BeginGroup();
        const char* modes[] = {"Formatted", "Plain text", "Headers"};
        for (int k = 0; k < 3; ++k) {
            if (k) ImGui::SameLine(0, S(2));
            ImGui::PushStyleColor(ImGuiCol_Button, mode == k ? col_alpha(t.accent, 0.4f) : t.raised);
            if (ImGui::Button(modes[k])) set_mode(k);
            ImGui::PopStyleColor();
        }
        if (ImGui::SmallButton("Body in Text editor")) { std::string s = view.doc ? view.doc->plain() : std::string(); std::string n = name + " (body).txt"; defer([s, n]() { files_add(make_text_doc_from(s, n)); }); }
        ImGui::SameLine();
        if (ImGui::SmallButton("As document")) { io::RichDoc copy = view.doc ? *view.doc : io::RichDoc{}; std::string n = name; defer([copy, n]() mutable { files_add(make_rich_doc_from(std::move(copy), n + " (body)", "Rich text", true)); }); }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(save_cur);
        field("From", m->from);
        field("To", m->to);
        field("Cc", m->cc);
        field("Bcc", m->bcc);
        field("Date", m->date);
        for (auto& w : m->warnings) {   // v17: Warning icon (U+26A0 is outside the font atlas ranges)
            const float fs = ImGui::GetFontSize();
            icon_draw(ImGui::GetWindowDrawList(), ui::Icon::Warning, ImGui::GetCursorScreenPos(), fs);
            ImGui::Indent(fs + S(4));
            ImGui::PushStyleColor(ImGuiCol_Text, t.warn); ImGui::TextWrapped("%s", w.c_str()); ImGui::PopStyleColor();
            ImGui::Unindent(fs + S(4));
        }
        // ---- attachments
        if (!m->atts.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("%d attachment%s", (int)m->atts.size(), m->atts.size() == 1 ? "" : "s");
            ImGui::SameLine();
            if (ImGui::SmallButton("Save All\xE2\x80\xA6")) defer([this]() { save_all(); });
            float x0 = ImGui::GetCursorScreenPos().x, xmax = ImGui::GetWindowPos().x + size.x - S(12);
            float chip_w = S(250), chip_h = S(40);
            float x = x0;
            ImGui::Dummy(ImVec2(0, S(2)));
            float y = ImGui::GetCursorScreenPos().y;
            for (size_t i = 0; i < m->atts.size(); ++i) {
                const io::MailAttachment& a = m->atts[i];
                if (x + chip_w > xmax && x > x0) { x = x0; y += chip_h + S(6); }
                ImGui::SetCursorScreenPos(ImVec2(x, y));
                ImGui::PushID((int)i);
                ImVec2 c0(x, y), c1(x + chip_w, y + chip_h);
                bool hov = ImGui::IsMouseHoveringRect(c0, c1);
                ImGui::GetWindowDrawList()->AddRectFilled(c0, c1, hov ? t.raised : t.panel2, S(6));
                ImGui::GetWindowDrawList()->AddRect(c0, c1, t.border, S(6));
                std::string nm = io::safe_attachment_name(a, (int)i);
                icon_draw(ImGui::GetWindowDrawList(), icon_for(nm, a.mime), ImVec2(x + S(8), y + (chip_h - S(22)) * 0.5f), S(22));
                std::string sz = a.embedded ? std::string("message") : human_size(a.data.size());
                if (a.inline_part) sz += " \xC2\xB7 inline";
                ImGui::GetWindowDrawList()->PushClipRect(c0, ImVec2(c1.x - S(66), c1.y), true);
                ImGui::GetWindowDrawList()->AddText(ImVec2(x + S(36), y + S(4)), t.text, nm.c_str());
                ImGui::GetWindowDrawList()->AddText(ImVec2(x + S(36), y + S(21)), t.text_dim, sz.c_str());
                ImGui::GetWindowDrawList()->PopClipRect();
                ImGui::SetCursorScreenPos(ImVec2(c1.x - S(64), y + S(7)));
                if (icon_button("sv", ui::Icon::Save, "Save\xE2\x80\xA6", false, true, S(26))) { int k = (int)i; defer([this, k]() { save_att(k); }); }
                ImGui::SameLine(0, S(2));
                if (icon_button("op", ui::Icon::Open, a.embedded ? "Open message" : "Open in PCB Plotter (built-in viewer / importer)", false, true, S(26))) { int k = (int)i; defer([this, k]() { open_att(k); }); }
                ImGui::SetCursorScreenPos(c0);
                ImGui::InvisibleButton("##chip", ImVec2(chip_w - S(68), chip_h));
                if (ImGui::IsItemHovered()) tooltip((nm + "\n" + (a.mime.empty() ? std::string("application/octet-stream") : a.mime) + "\n" + human_size(a.data.size()) + "\nDouble-click to open in PCB Plotter").c_str());
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { int k = (int)i; defer([this, k]() { open_att(k); }); }
                if (ImGui::BeginPopupContextItem("##ctx")) {
                    if (menu_item_icon("Open in PCB Plotter", ui::Icon::Open)) { int k = (int)i; defer([this, k]() { open_att(k); }); }
                    if (menu_item_icon("Save\xE2\x80\xA6", ui::Icon::Save)) { int k = (int)i; defer([this, k]() { save_att(k); }); }
                    if (menu_item_icon("Inspect bytes (hex)", ui::Icon::Query)) { std::vector<uint8_t> dd = a.data; defer([dd, nm]() { files_add(make_hex_doc_from(dd, nm)); }); }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                x += chip_w + S(6);
            }
            ImGui::SetCursorScreenPos(ImVec2(x0, y + chip_h + S(4)));
            ImGui::Dummy(ImVec2(1, 1));
        }
        ImGui::EndChild();
        (void)dl; (void)p0;
        ImVec2 rest = ImGui::GetContentRegionAvail();
        view.draw(ImVec2(size.x, std::max(S(60), rest.y)));
    }
    bool save(bool) override {
        // "Save As" for a message: .eml copy (the original file is never modified)
        std::wstring p = ui::W(io::safe_attachment_name(io::MailAttachment{m->subject.empty() ? name : m->subject, "message/rfc822", "", {}, false, m}, 0));
        if (ext_of(ui::U(p)) != "eml") p += L".eml";
        if (!ui::save_file_dialog(app::frame_hwnd(), L"Save Message As", {{L"Email message (*.eml)", L"*.eml"}}, L"eml", p)) return false;
        std::string out = io::eml_write(*m), err;
        if (!write_file(ui::U(p), out.data(), out.size(), err)) { toast("Save failed: " + err, ToastKind::Error, 6.f); return false; }
        gui::status("Saved " + ui::U(file_name_of(p)), ToastKind::Success);
        return true;
    }
    bool on_command(int id) override {
        if (id == app::ID_EDIT_COPY && view.doc) { ImGui::SetClipboardText(view.doc->plain().c_str()); return true; }
        return false;
    }
};

std::unique_ptr<FileDoc> make_mail_doc_from(std::shared_ptr<io::MailMessage> m, const std::string& name) {
    auto d = std::make_unique<MailDoc>();
    d->m = std::move(m);
    d->name = name;
    d->build();
    return d;
}

std::unique_ptr<FileDoc> make_rich_doc_from(io::RichDoc doc, const std::string& name, const std::string& kind, bool editable) {
    auto d = std::make_unique<RichDocDoc>();
    d->d = std::move(doc);
    d->view.doc = &d->d;
    d->name = name;
    d->kind_ = kind;
    d->editable = editable;
    return d;
}

} // namespace

// --------------------------------------------------------------------------- public
int sniff_document_kind(const std::wstring& path) {
    std::vector<uint8_t> d;
    std::string err;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    d.resize(65536);
    DWORD got = 0;
    ReadFile(h, d.data(), (DWORD)d.size(), &got, nullptr);
    CloseHandle(h);
    d.resize(got);
    std::string e = ext_of(ui::U(path));
    if (d.empty()) return 2;
    if (io::is_rtf_bytes(d.data(), d.size())) return 3;
    if (io::is_cfb_bytes(d.data(), d.size())) {
        if (e == "xls") return 1;
        if (e == "msg") return 4;
        // look for MAPI stream names (UTF-16) in the first directory sectors
        std::string s(d.begin(), d.end());
        static const char sig[] = "_\0_\0s\0u\0b\0s\0t\0g\0";
        if (s.find(std::string(sig, sizeof sig - 1)) != std::string::npos) return 4;
        static const char wb[] = "W\0o\0r\0k\0b\0o\0o\0k\0";
        if (s.find(std::string(wb, sizeof wb - 1)) != std::string::npos) return 1;
        return 5;
    }
    if (d.size() >= 4 && d[0] == 'P' && d[1] == 'K') {
        if (e == "docx" || e == "dotx") return 3;
        if (e == "xlsx" || e == "xlsm" || e == "ods") return 1;
        std::vector<uint8_t> all;
        if (read_file(ui::U(path), all, err)) { if (io::is_docx_bytes(all)) return 3; }
        return 5;
    }
    if (io::is_eml_bytes(d.data(), d.size())) return 4;
    // text: UTF-16 BOM, or no NULs and mostly printable
    if (d.size() >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) return 2;
    size_t bad = 0;
    for (uint8_t c : d) { if (c == 0) return 5; if (c < 32 && c != '\t' && c != '\r' && c != '\n' && c != '\f' && c != 0x1A && c != 0x1B) ++bad; }
    return bad * 100 <= d.size() ? 2 : 5;
}

std::unique_ptr<FileDoc> make_rich_doc(const std::wstring& path, std::string& err) {
    std::vector<uint8_t> data;
    if (!read_file(ui::U(path), data, err)) return nullptr;
    std::string name = ui::U(file_name_of(path)), e = ext_of(name);
    io::RichDoc d;
    std::unique_ptr<FileDoc> out;
    if (io::is_rtf_bytes(data.data(), data.size())) {
        if (!io::rtf_to_rich(std::string(data.begin(), data.end()), d, err)) return nullptr;
        out = make_rich_doc_from(std::move(d), name, "Rich text", true);
    } else if (io::is_docx_bytes(data)) {
        if (!io::docx_read(data, d, err)) return nullptr;
        out = make_rich_doc_from(std::move(d), name, "Word document", false);
    } else if (e == "htm" || e == "html") {
        std::string h(data.begin(), data.end());
        io::html_to_rich(io::charset_to_utf8(h, io::html_meta_charset(h)), d);
        out = make_rich_doc_from(std::move(d), name, "Web page (safe view)", false);
    } else if (e == "doc" && io::is_cfb_bytes(data.data(), data.size())) {
        err = "Word 97-2003 (.doc) binary documents are not supported; open it as hex to inspect, or save it as .docx / .rtf";
        return nullptr;
    } else if ((data.size() >= 4 && data[0] == 'P' && data[1] == 'K') || std::find(data.begin(), data.begin() + std::min<size_t>(data.size(), 4096), 0) != data.begin() + std::min<size_t>(data.size(), 4096)) {
        // R11 P2-11: a zip package or binary that is not a readable document is never dumped as text
        err = e == "docx" || e == "dotx" ? "this .docx package has no readable Word document part (unsupported or damaged)"
                                         : "unsupported document format (binary content)";
        return nullptr;
    } else {
        std::string s(data.begin(), data.end());
        d = io::rich_from_plain(io::charset_to_utf8(s, ""));
        out = make_rich_doc_from(std::move(d), name, "Text", true);
    }
    out->path = path;
    if (auto* rd = dynamic_cast<RichDocDoc*>(out.get())) { if (e == "docx") rd->path = path; }
    return out;
}

std::unique_ptr<FileDoc> make_mail_doc(const std::wstring& path, std::string& err) {
    std::vector<uint8_t> data;
    if (!read_file(ui::U(path), data, err)) return nullptr;
    auto m = std::make_shared<io::MailMessage>();
    bool ok = io::is_cfb_bytes(data.data(), data.size()) ? io::msg_parse(data, *m, err) : io::eml_parse(data, *m, err);
    if (!ok) return nullptr;
    auto d = make_mail_doc_from(m, ui::U(file_name_of(path)));
    d->path = path;
    return d;
}

} // namespace pcbp::gui

// --------------------------------------------------------------------------- R20 preview (Files tab modal)
namespace pcbp::gui {
namespace {
class RichPreview final : public FileDoc {
public:
    io::RichDoc d;
    RichView view;
    std::string kind_;
    bool is_sheet() const override { return false; }
    bool dirty() const override { return false; }
    void draw(ImVec2 size) override { view.draw(size); }
    bool save(bool) override { return false; }
    const char* kind() const override { return kind_.c_str(); }
    bool on_command(int id) override {
        if (id == app::ID_EDIT_COPY) { ImGui::SetClipboardText(d.plain().c_str()); return true; }
        return false;
    }
};
} // namespace

std::unique_ptr<FileDoc> make_preview_doc(const std::vector<uint8_t>& data, const std::string& name, std::string& err, float** zoom) {
    if (zoom) *zoom = nullptr;
    const std::string e = ext_of(name);
    if (e == "eml" || e == "msg" || (e != "docx" && (io::is_eml_bytes(data.data(), data.size()) || io::is_cfb_bytes(data.data(), data.size())))) {
        auto m = std::make_shared<io::MailMessage>();
        const bool cfb = io::is_cfb_bytes(data.data(), data.size());
        if (cfb ? io::msg_parse(data, *m, err) : io::eml_parse(data, *m, err)) return make_mail_doc_from(m, name);
        if (e == "eml" || e == "msg") return nullptr;
        err.clear();
    }
    auto rp = std::make_unique<RichPreview>();
    rp->name = name;
    if (io::is_rtf_bytes(data.data(), data.size())) {
        if (!io::rtf_to_rich(std::string(data.begin(), data.end()), rp->d, err)) return nullptr;
        rp->kind_ = "Rich text";
    } else if (e == "docx" || e == "dotx" || io::is_docx_bytes(data)) {
        if (!io::docx_read(data, rp->d, err)) return nullptr;
        rp->kind_ = "Word document";
    } else if (e == "htm" || e == "html") {
        std::string h(data.begin(), data.end());
        io::html_to_rich(io::charset_to_utf8(h, io::html_meta_charset(h)), rp->d);
        rp->kind_ = "Web page (safe view)";
    } else {
        err = "not a document format the preview understands";
        return nullptr;
    }
    rp->view.doc = &rp->d;
    if (zoom) *zoom = &rp->view.zoom;
    return rp;
}

} // namespace pcbp::gui
