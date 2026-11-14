// ============================================================================
// text_editor.cpp — Notepad-like text editor for BOM / centroid source files.
//   Own virtualized editor widget (no InputTextMultiline): caret, selection,
//   box / column selection (Alt+drag, Alt+Shift+arrows) for fixed-width files,
//   undo/redo, word wrap, show whitespace, line numbers, find/replace (plain or
//   regex), go to line, encoding detect (UTF-8 / BOM / UTF-16 LE/BE / 1252) and
//   save with a chosen encoding + line ending. Open in Spreadsheet / Import.
// ============================================================================
#include "file_editors.h"
#include "../app/app.h"
#include "../app/commands.h"
#include "../core/fileio.h"
#include "../io/text_import.h"
#include "../ui/form.h"
#include "../ui/win.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>

namespace pcbp::gui {

namespace {

struct Pos {
    int l = 0, c = 0;   // line, byte offset
    bool operator==(const Pos& o) const { return l == o.l && c == o.c; }
    bool operator<(const Pos& o) const { return l < o.l || (l == o.l && c < o.c); }
};

struct Edit { Pos a; std::string removed, inserted; Pos caret_before, anchor_before; int group = 0; };

int cp_len(unsigned char b) { return b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1; }

static const uint16_t k1252[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
                                   0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

std::vector<uint32_t> to_cps(const std::string& s) {
    std::vector<uint32_t> v;
    for (size_t i = 0; i < s.size();) {
        unsigned char b = s[i];
        int n = cp_len(b);
        uint32_t cp = n == 1 ? b : n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = cp << 6 | (s[i + k] & 0x3F);
        v.push_back(cp);
        i += n;
    }
    return v;
}

class TextDoc final : public FileDoc {
public:
    std::vector<std::string> L{std::string()};
    io::Encoding enc = io::Encoding::Utf8;
    int eol = 0;                    // 0 CRLF, 1 LF, 2 CR
    int tab = 8;
    bool dirty_ = false;
    Pos caret, anchor;
    bool box = false;               // column selection
    int box_va = 0, box_vc = 0;     // visual columns (anchor / caret)
    float want_vcol = -1;           // sticky column for up/down
    std::vector<Edit> undo, redo;
    int group = 1;
    bool wrap = false, show_ws = false, line_numbers = true;
    bool find_open = false, repl_open = false, find_focus = false, use_regex = false, match_case = false;
    char find_buf[256] = {}, repl_buf[256] = {};
    bool goto_open = false;
    int goto_line = 1;
    bool scroll_to_caret = false;
    bool dragging = false;
    // wrap cache
    std::vector<int> row_start;     // first visual row of each line (+ total at end)
    int wrap_cols = 0;
    bool wrap_dirty = true;
    std::string msg;

    bool is_sheet() const override { return false; }
    bool dirty() const override { return dirty_; }

    // ------------------------------------------------------------ text model
    void set_text(const std::string& utf8) {
        L.clear();
        size_t p = 0;
        for (;;) {
            size_t k = utf8.find('\n', p);
            if (k == std::string::npos) { L.push_back(utf8.substr(p)); break; }
            L.push_back(utf8.substr(p, k - p));
            p = k + 1;
        }
        if (L.empty()) L.emplace_back();
        caret = anchor = {};
        wrap_dirty = true;
    }
    std::string all_text() const {
        std::string o;
        size_t n = 0;
        for (auto& l : L) n += l.size() + 1;
        o.reserve(n);
        for (size_t i = 0; i < L.size(); ++i) { if (i) o += '\n'; o += L[i]; }
        return o;
    }
    Pos clamp(Pos p) const {
        p.l = std::clamp(p.l, 0, (int)L.size() - 1);
        p.c = std::clamp(p.c, 0, (int)L[p.l].size());
        while (p.c > 0 && p.c < (int)L[p.l].size() && (L[p.l][p.c] & 0xC0) == 0x80) --p.c;
        return p;
    }
    std::string range(Pos a, Pos b) const {
        if (b < a) std::swap(a, b);
        if (a.l == b.l) return L[a.l].substr(a.c, b.c - a.c);
        std::string o = L[a.l].substr(a.c);
        for (int l = a.l + 1; l < b.l; ++l) { o += '\n'; o += L[l]; }
        o += '\n'; o += L[b.l].substr(0, b.c);
        return o;
    }
    void raw_erase(Pos a, Pos b) {
        if (b < a) std::swap(a, b);
        if (a.l == b.l) { L[a.l].erase(a.c, b.c - a.c); return; }
        L[a.l] = L[a.l].substr(0, a.c) + L[b.l].substr(b.c);
        L.erase(L.begin() + a.l + 1, L.begin() + b.l + 1);
    }
    Pos raw_insert(Pos a, const std::string& t) {
        if (t.find('\n') == std::string::npos) { L[a.l].insert(a.c, t); return {a.l, a.c + (int)t.size()}; }
        std::string tail = L[a.l].substr(a.c);
        L[a.l].erase(a.c);
        std::vector<std::string> parts;
        size_t p = 0;
        for (;;) { size_t k = t.find('\n', p); if (k == std::string::npos) { parts.push_back(t.substr(p)); break; } parts.push_back(t.substr(p, k - p)); p = k + 1; }
        L[a.l] += parts[0];
        std::vector<std::string> mid(parts.begin() + 1, parts.end());
        Pos end{a.l + (int)mid.size(), (int)mid.back().size()};
        mid.back() += tail;
        L.insert(L.begin() + a.l + 1, mid.begin(), mid.end());
        return end;
    }
    static Pos end_of(Pos a, const std::string& t) {
        size_t k = t.rfind('\n');
        if (k == std::string::npos) return {a.l, a.c + (int)t.size()};
        int nl = (int)std::count(t.begin(), t.end(), '\n');
        return {a.l + nl, (int)(t.size() - k - 1)};
    }
    Pos replace(Pos a, Pos b, const std::string& t) {
        if (b < a) std::swap(a, b);
        Edit e{a, range(a, b), t, caret, anchor, group};
        raw_erase(a, b);
        Pos end = raw_insert(a, t);
        if (!(e.removed.empty() && e.inserted.empty())) { undo.push_back(std::move(e)); redo.clear(); }
        if (undo.size() > 20000) undo.erase(undo.begin(), undo.begin() + 5000);
        dirty_ = true; wrap_dirty = true;
        return end;
    }
    void new_group() { ++group; }
    void do_undo() {
        if (undo.empty()) return;
        int g = undo.back().group;
        while (!undo.empty() && undo.back().group == g) {
            Edit e = std::move(undo.back()); undo.pop_back();
            raw_erase(e.a, end_of(e.a, e.inserted));
            raw_insert(e.a, e.removed);
            caret = e.caret_before; anchor = e.anchor_before;
            redo.push_back(std::move(e));
        }
        box = false; dirty_ = true; wrap_dirty = true; scroll_to_caret = true; new_group();
    }
    void do_redo() {
        if (redo.empty()) return;
        int g = redo.back().group;
        while (!redo.empty() && redo.back().group == g) {
            Edit e = std::move(redo.back()); redo.pop_back();
            raw_erase(e.a, end_of(e.a, e.removed));
            caret = anchor = raw_insert(e.a, e.inserted);
            undo.push_back(std::move(e));
        }
        box = false; dirty_ = true; wrap_dirty = true; scroll_to_caret = true; new_group();
    }

    // ------------------------------------------------------------ visual columns
    int vcol(int l, int c) const {
        const std::string& s = L[l];
        int v = 0;
        for (int i = 0; i < c && i < (int)s.size();) {
            if (s[i] == '\t') v = (v / tab + 1) * tab; else ++v;
            i += cp_len((unsigned char)s[i]);
        }
        return v;
    }
    int byte_at_vcol(int l, int vc, bool pad_ok = false, int* reached = nullptr) const {
        const std::string& s = L[l];
        int v = 0, i = 0;
        while (i < (int)s.size()) {
            int nv = s[i] == '\t' ? (v / tab + 1) * tab : v + 1;
            if (nv > vc) { if (vc - v > nv - vc) { v = nv; i += cp_len((unsigned char)s[i]); } break; }
            v = nv;
            i += cp_len((unsigned char)s[i]);
        }
        (void)pad_ok;
        if (reached) *reached = v;
        return i;
    }
    int vlen(int l) const { return vcol(l, (int)L[l].size()); }

    // ------------------------------------------------------------ selection
    bool has_sel() const { return box ? (caret.l != anchor.l || box_va != box_vc) : !(caret == anchor); }
    void sel_bounds(Pos& a, Pos& b) const { a = anchor; b = caret; if (b < a) std::swap(a, b); }
    std::string selected_text() const {
        if (!has_sel()) return {};
        if (box) {
            int l0 = std::min(anchor.l, caret.l), l1 = std::max(anchor.l, caret.l);
            int v0 = std::min(box_va, box_vc), v1 = std::max(box_va, box_vc);
            std::string o;
            for (int l = l0; l <= l1; ++l) {
                if (l > l0) o += '\n';
                int a = byte_at_vcol(l, v0), b = byte_at_vcol(l, v1);
                o += L[l].substr(a, b - a);
            }
            return o;
        }
        Pos a, b; sel_bounds(a, b);
        return range(a, b);
    }
    // replace each box line's [v0,v1) with text (per line when lines != null)
    void box_replace(const std::vector<std::string>* lines, const std::string& t, int del_dir = 0) {
        new_group();
        int l0 = std::min(anchor.l, caret.l), l1 = std::max(anchor.l, caret.l);
        int v0 = std::min(box_va, box_vc), v1 = std::max(box_va, box_vc);
        if (v0 == v1 && del_dir < 0 && v0 > 0) v0 -= 1;
        if (v0 == v1 && del_dir > 0) v1 += 1;
        int newv = v0;
        for (int l = l0; l <= l1; ++l) {
            int reached;
            int a = byte_at_vcol(l, v0, true, &reached);
            std::string ins = lines ? ((l - l0) < (int)lines->size() ? (*lines)[l - l0] : std::string()) : t;
            if (reached < v0) {   // short line: pad with spaces so columns stay aligned
                if (ins.empty()) continue;
                ins = std::string(v0 - reached, ' ') + ins;
            }
            int b = std::max(a, byte_at_vcol(l, v1));
            replace({l, a}, {l, b}, ins);
            newv = vcol(l, a + (int)ins.size());
        }
        box_va = box_vc = newv;
        anchor.l = l0; caret.l = l1;
        caret.c = byte_at_vcol(caret.l, newv); anchor.c = byte_at_vcol(anchor.l, newv);
        new_group();
    }
    void insert_text(const std::string& t) {
        if (box) {
            if (t.find('\n') != std::string::npos) {
                std::vector<std::string> lines;
                size_t p = 0;
                for (;;) { size_t k = t.find('\n', p); if (k == std::string::npos) { lines.push_back(t.substr(p)); break; } lines.push_back(t.substr(p, k - p)); p = k + 1; }
                if (!lines.empty() && lines.back().empty()) lines.pop_back();
                int n = std::abs(caret.l - anchor.l) + 1;
                if ((int)lines.size() == n) { box_replace(&lines, ""); return; }
                if (n == 1) {   // box paste at a caret: one line per row, extending down
                    int l1 = std::min((int)L.size() - 1, caret.l + (int)lines.size() - 1);
                    anchor.l = std::min(anchor.l, caret.l); caret.l = l1;
                    box_replace(&lines, "");
                    return;
                }
                box = false;
            } else { box_replace(nullptr, t); return; }
        }
        Pos a, b; sel_bounds(a, b);
        caret = anchor = replace(a, b, t);
        scroll_to_caret = true; want_vcol = -1;
    }
    void delete_sel_or(int dir) {   // dir -1 backspace, +1 delete
        if (box) { box_replace(nullptr, "", has_sel() && box_va != box_vc ? 0 : dir); scroll_to_caret = true; return; }
        new_group();
        if (has_sel()) { Pos a, b; sel_bounds(a, b); caret = anchor = replace(a, b, ""); }
        else if (dir < 0) {
            Pos p = caret;
            if (p.c > 0) { int k = p.c - 1; while (k > 0 && (L[p.l][k] & 0xC0) == 0x80) --k; caret = anchor = replace({p.l, k}, p, ""); }
            else if (p.l > 0) { Pos q{p.l - 1, (int)L[p.l - 1].size()}; caret = anchor = replace(q, p, ""); }
        } else {
            Pos p = caret;
            if (p.c < (int)L[p.l].size()) replace(p, {p.l, p.c + cp_len((unsigned char)L[p.l][p.c])}, "");
            else if (p.l + 1 < (int)L.size()) replace(p, {p.l + 1, 0}, "");
        }
        new_group();
        scroll_to_caret = true; want_vcol = -1;
    }

    // ------------------------------------------------------------ caret moves
    Pos step(Pos p, int dir, bool word) const {
        if (dir < 0) {
            if (p.c == 0) return p.l > 0 ? Pos{p.l - 1, (int)L[p.l - 1].size()} : p;
            int k = p.c - 1;
            while (k > 0 && (L[p.l][k] & 0xC0) == 0x80) --k;
            if (word) {
                auto isw = [&](int i) { unsigned char ch = L[p.l][i]; return isalnum(ch) || ch == '_' || ch >= 0x80; };
                while (k > 0 && !isw(k)) --k;
                while (k > 0 && isw(k - 1)) --k;
            }
            return {p.l, k};
        }
        if (p.c >= (int)L[p.l].size()) return p.l + 1 < (int)L.size() ? Pos{p.l + 1, 0} : p;
        int k = p.c + cp_len((unsigned char)L[p.l][p.c]);
        if (word) {
            auto isw = [&](int i) { unsigned char ch = L[p.l][i]; return isalnum(ch) || ch == '_' || ch >= 0x80; };
            int n = (int)L[p.l].size();
            while (k < n && isw(k)) ++k;
            while (k < n && !isw(k)) ++k;
        }
        return {p.l, k};
    }
    void move_to(Pos p, bool extend, bool boxsel = false) {
        p = clamp(p);
        if (boxsel) {
            if (!box) { box = true; box_va = vcol(caret.l, caret.c); if (!extend) anchor = caret; else box_va = vcol(anchor.l, anchor.c); }
            caret = p; box_vc = vcol(p.l, p.c);
        } else {
            box = false;
            caret = p;
            if (!extend) anchor = p;
        }
        scroll_to_caret = true;
    }

    // ------------------------------------------------------------ find
    bool find_from(Pos start, bool back, Pos& fa, Pos& fb) {
        std::string f = find_buf;
        if (f.empty()) return false;
        std::regex re;
        if (use_regex) {
            try { re = std::regex(f, match_case ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase); }
            catch (...) { msg = "invalid regular expression"; return false; }
        }
        auto lower = [](std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; };
        std::string fl = match_case ? f : lower(f);
        const int n = (int)L.size();
        for (int k = 0; k <= n; ++k) {
            int l = ((back ? start.l - k : start.l + k) % n + n) % n;
            const std::string& s0 = L[l];
            std::vector<std::pair<int, int>> hits;
            if (use_regex) {
                for (std::sregex_iterator it(s0.begin(), s0.end(), re), e; it != e; ++it) {
                    if (it->length(0) == 0) continue;
                    hits.push_back({(int)it->position(0), (int)(it->position(0) + it->length(0))});
                }
            } else {
                std::string s = match_case ? s0 : lower(s0);
                for (size_t p = s.find(fl); p != std::string::npos; p = s.find(fl, p + 1)) hits.push_back({(int)p, (int)(p + fl.size())});
            }
            if (back) std::reverse(hits.begin(), hits.end());
            for (auto [a, b] : hits) {
                bool ok = k == 0 ? (back ? a < start.c : a >= start.c) : true;
                if (k == n) ok = back ? a >= start.c : a < start.c;
                if (ok) { fa = {l, a}; fb = {l, b}; return true; }
            }
        }
        return false;
    }
    bool find_next(bool back = false) {
        Pos a, b; sel_bounds(a, b);
        Pos fa, fb;
        if (find_from(back ? a : (has_sel() ? b : caret), back, fa, fb)) { box = false; anchor = fa; caret = fb; scroll_to_caret = true; return true; }
        if (msg.empty()) msg = "\"" + std::string(find_buf) + "\" not found";
        gui::status(msg, ToastKind::Warning);
        msg.clear();
        return false;
    }
    std::string repl_for(const std::string& matched) {
        if (!use_regex) return repl_buf;
        try { return std::regex_replace(matched, std::regex(find_buf, match_case ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase), std::string(repl_buf), std::regex_constants::format_first_only); }
        catch (...) { return repl_buf; }
    }
    void replace_one() {
        if (has_sel() && !box) {
            Pos a, b; sel_bounds(a, b);
            Pos fa, fb;
            if (find_from(a, false, fa, fb) && fa == a && fb == b) { new_group(); std::string r = repl_for(range(a, b)); caret = anchor = replace(a, b, r); new_group(); }
        }
        find_next();
    }
    void replace_all() {
        new_group();
        int n = 0;
        Pos p{0, 0}, fa, fb;
        int guard = 0;
        while (guard++ < 1000000 && find_from(p, false, fa, fb)) {
            if (fa < p) break;   // wrapped
            std::string r = repl_for(range(fa, fb));
            p = replace(fa, fb, r);
            ++n;
            if (p.l >= (int)L.size()) break;
        }
        new_group();
        caret = anchor = clamp(caret);
        toast("Replaced " + std::to_string(n) + " occurrences", n ? ToastKind::Success : ToastKind::Info, 2.5f);
    }

    // ------------------------------------------------------------ io
    bool load(const std::wstring& p, std::string& err) {
        std::string bytes;
        if (!read_text_file(ui::U(p), bytes, err)) return false;
        io::Encoding det = io::Encoding::Utf8;
        std::string t = io::decode_text(bytes, io::Encoding::Auto, &det);
        enc = det;
        // line ending of the original
        size_t crlf = 0, lf = 0, cr = 0;
        bool u16 = det == io::Encoding::Utf16LE || det == io::Encoding::Utf16BE;
        size_t st = u16 ? 2 : 1;
        for (size_t i = (u16 && det == io::Encoding::Utf16BE) ? 1 : 0; i < bytes.size() && i < (1u << 22); i += st) {
            if (bytes[i] == '\r' && (u16 ? (i + 1 < bytes.size() && bytes[i ^ 1] == 0) : true)) {
                if (i + st < bytes.size() && bytes[i + st] == '\n') { ++crlf; i += st; } else ++cr;
            } else if (bytes[i] == '\n') ++lf;
        }
        eol = crlf >= lf && crlf >= cr ? 0 : lf >= cr ? 1 : 2;
        if (!crlf && !lf && !cr) eol = 0;
        set_text(t);
        path = p;
        name = ui::U(file_name_of(p));
        dirty_ = false;
        return true;
    }
    std::string encode() const {
        std::string nl = eol == 0 ? "\r\n" : eol == 1 ? "\n" : "\r";
        std::string u;
        for (size_t i = 0; i < L.size(); ++i) { if (i) u += nl; u += L[i]; }
        std::string o;
        switch (enc) {
        case io::Encoding::Utf8Bom: o = "\xEF\xBB\xBF" + u; break;
        case io::Encoding::Utf16LE: case io::Encoding::Utf16BE: {
            bool be = enc == io::Encoding::Utf16BE;
            auto put = [&](uint16_t w) { if (be) { o += (char)(w >> 8); o += (char)(w & 0xFF); } else { o += (char)(w & 0xFF); o += (char)(w >> 8); } };
            put(0xFEFF);
            for (uint32_t cp : to_cps(u)) {
                if (cp >= 0x10000) { cp -= 0x10000; put(uint16_t(0xD800 | (cp >> 10))); put(uint16_t(0xDC00 | (cp & 0x3FF))); }
                else put((uint16_t)cp);
            }
        } break;
        case io::Encoding::Cp1252:
            for (uint32_t cp : to_cps(u)) {
                if (cp < 0x80 || (cp >= 0xA0 && cp < 0x100)) { o += (char)cp; continue; }
                int k = -1;
                for (int i = 0; i < 32; ++i) if (k1252[i] == cp) k = i;
                o += k >= 0 ? (char)(0x80 + k) : '?';
            }
            break;
        default: o = u;
        }
        return o;
    }
    bool save(bool save_as) override {
        std::wstring p = path;
        if (save_as || p.empty()) {
            std::wstring def = p.empty() ? ui::W(name) : p;
            if (!ui::save_file_dialog(app::frame_hwnd(), L"Save Text As", {{L"Text files (*.txt;*.csv;*.prn)", L"*.txt;*.csv;*.prn;*.tsv"}, {L"All files", L"*.*"}}, L"txt", def)) return false;
            p = def;
        }
        std::string b = encode(), err;
        if (!write_file(ui::U(p), b.data(), b.size(), err)) { toast("Could not save: " + err, ToastKind::Error, 6.f); return false; }
        path = p; name = ui::U(file_name_of(p)); dirty_ = false;
        toast("Saved " + name + " (" + io::encoding_name(enc) + ", " + (eol == 0 ? "CRLF" : eol == 1 ? "LF" : "CR") + ")", ToastKind::Success, 2.5f);
        return true;
    }
    void select_all() { box = false; anchor = {0, 0}; caret = {(int)L.size() - 1, (int)L.back().size()}; }
    void copy() { std::string s = selected_text(); if (!s.empty()) ImGui::SetClipboardText(s.c_str()); }
    void cut() { copy(); if (has_sel()) delete_sel_or(0); }
    void paste() {
        const char* c = ImGui::GetClipboardText();
        if (!c || !*c) return;
        std::string t;
        for (const char* p = c; *p; ++p) { if (*p == '\r') { if (p[1] != '\n') t += '\n'; } else t += *p; }
        new_group(); insert_text(t); new_group();
    }
    bool on_command(int id) override {
        using namespace app;
        switch (id) {
        case ID_EDIT_UNDO: do_undo(); return true;
        case ID_EDIT_REDO: do_redo(); return true;
        case ID_EDIT_COPY: copy(); return true;
        case ID_EDIT_CUT: cut(); return true;
        case ID_EDIT_PASTE: paste(); return true;
        case ID_EDIT_DELETE: delete_sel_or(1); return true;
        case ID_EDIT_SELECT_ALL: select_all(); return true;
        case ID_TOOL_SEARCH: find_open = find_focus = true; return true;
        }
        return false;
    }
    std::string status() const override {
        char b[160];
        std::snprintf(b, sizeof b, "Ln %d, Col %d  \xC2\xB7  %d lines  \xC2\xB7  %s  \xC2\xB7  %s%s", caret.l + 1, vcol(caret.l, caret.c) + 1, (int)L.size(),
                      io::encoding_name(enc), eol == 0 ? "CRLF" : eol == 1 ? "LF" : "CR", box ? "  \xC2\xB7  column selection" : "");
        std::string s = b;
        if (has_sel()) s += "  \xC2\xB7  " + std::to_string(selected_text().size()) + " bytes selected";
        return s;
    }
    void import_as(bool bom) override {
        std::string b = all_text();
        int start = caret.l > 0 && caret == anchor ? caret.l : 0;
        std::wstring disp = path.empty() ? ui::W(name) : path;
        defer([b, start, bom, disp]() { import_from_bytes(b, start, bom, disp); });
    }

    void draw(ImVec2 size) override;
    void draw_toolbar();
    void draw_findbar();
    void draw_text(ImVec2 size);
    void handle_keys();
    void rebuild_wrap(int cols) {
        wrap_cols = cols;
        row_start.resize(L.size() + 1);
        int r = 0;
        for (size_t i = 0; i < L.size(); ++i) { row_start[i] = r; r += wrap ? std::max(1, (vlen((int)i) + cols - 1) / std::max(1, cols)) : 1; }
        row_start[L.size()] = r;
        wrap_dirty = false;
    }
};

static bool tb(const char* id, ui::Icon ic, const char* tip, bool en = true, bool ck = false) { return fe_tool(id, ic, tip, en, ck); }

void TextDoc::draw(ImVec2 size) {
    ImVec2 p0 = ImGui::GetCursorPos();
    draw_toolbar();
    if (find_open) draw_findbar();
    float used = ImGui::GetCursorPosY() - p0.y;
    draw_text(ImVec2(size.x, size.y - used));
}

void TextDoc::draw_toolbar() {
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + S(4), ImGui::GetCursorPosY() + S(4)));
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, S(28)));
    if (tb("sv", ui::Icon::Save, "Save  (Ctrl+S)")) defer([this]() { save(false); });
    if (tb("sva", ui::Icon::SaveAs, "Save As\xE2\x80\xA6")) defer([this]() { save(true); });
    ImGui::SameLine(0, S(8));
    static const io::Encoding encs[] = {io::Encoding::Utf8, io::Encoding::Utf8Bom, io::Encoding::Utf16LE, io::Encoding::Utf16BE, io::Encoding::Cp1252};
    static const char* enc_lbl[] = {"UTF-8", "UTF-8 with BOM", "UTF-16 LE", "UTF-16 BE", "ANSI (Windows-1252)"};
    int ei = 0;
    for (int i = 0; i < 5; ++i) if (encs[i] == enc) ei = i;
    ImGui::SetNextItemWidth(S(150));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(2));
    if (ImGui::Combo("##enc", &ei, enc_lbl, 5)) { enc = encs[ei]; dirty_ = true; }
    if (ImGui::IsItemHovered()) tooltip("Encoding used when saving");
    ImGui::SameLine(0, S(4));
    static const char* eol_lbl[] = {"Windows (CRLF)", "Unix (LF)", "Mac (CR)"};
    ImGui::SetNextItemWidth(S(130));
    if (ImGui::Combo("##eol", &eol, eol_lbl, 3)) dirty_ = true;
    if (ImGui::IsItemHovered()) tooltip("Line endings used when saving");
    ImGui::SameLine(0, S(6));
    if (tb("un", ui::Icon::Undo, "Undo  (Ctrl+Z)", !undo.empty())) do_undo();
    if (tb("re", ui::Icon::Redo, "Redo  (Ctrl+Y)", !redo.empty())) do_redo();
    if (tb("fd", ui::Icon::FindRef, "Find / Replace  (Ctrl+F / Ctrl+H)", true, find_open)) { find_open = !find_open; find_focus = find_open; }
    if (tb("gt", ui::Icon::ArrowDown, "Go to line  (Ctrl+G)")) { goto_open = true; goto_line = caret.l + 1; }
    if (tb("wr", ui::Icon::Text, "Word wrap", true, wrap)) { wrap = !wrap; wrap_dirty = true; }
    if (tb("ws", ui::Icon::Eye, "Show whitespace (tabs / spaces)", true, show_ws)) show_ws = !show_ws;
    if (tb("ln", ui::Icon::Grid, "Line numbers", true, line_numbers)) line_numbers = !line_numbers;
    ImGui::SameLine(0, S(14));
    if (button("Open in Spreadsheet", ui::Icon::Bom, false, ImVec2(0, S(28)))) {
        std::string t = all_text(), nm = name;
        std::wstring pth = path;
        defer([t, nm, pth]() {
            std::string how;
            sheet::Workbook wb;
            wb.sheets.push_back(sheet::sheet_from_text(t, &how));
            std::string base = nm;
            size_t k = base.find_last_of('.');
            wb.sheets[0].name = (k == std::string::npos ? base : base.substr(0, k)).substr(0, 31);
            files_add(make_sheet_doc_from(std::move(wb), base + " (sheet)", L""));
            toast("Split as " + how, ToastKind::Info, 3.f);
        });
    }
    if (ImGui::IsItemHovered()) tooltip("Split this text into columns (delimiter / fixed width detection) in a new spreadsheet tab");
    ImGui::SameLine(0, S(6));
    if (button("Import as BOM\xE2\x80\xA6", ui::Icon::ImportBom, true, ImVec2(0, S(28)))) import_as(true);
    if (ImGui::IsItemHovered()) tooltip("Send the text to the BOM import wizard (starting at the caret line when it is not on line 1)");
    ImGui::SameLine(0, S(6));
    if (button("Import as Centroid\xE2\x80\xA6", ui::Icon::ImportCentroid, false, ImVec2(0, S(28)))) import_as(false);
    ImGui::EndGroup();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4));
    if (goto_open) { ImGui::OpenPopup("Go to line"); goto_open = false; }
    if (ImGui::BeginPopup("Go to line")) {
        ImGui::Text("Line number (1 - %d)", (int)L.size());
        ImGui::SetNextItemWidth(S(160));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool go = ImGui::InputInt("##gl", &goto_line, 1, 100, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (button("Go", ui::Icon::None, true) || go) { move_to({std::clamp(goto_line, 1, (int)L.size()) - 1, 0}, false); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void TextDoc::draw_findbar() {
    const GTheme& t = th();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + W, p.y + S(36)), t.panel2);
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(8), p.y + S(4)));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Find");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(220));
    if (find_focus) { ImGui::SetKeyboardFocusHere(); find_focus = false; }
    if (ImGui::InputTextWithHint("##find", "text or regex", find_buf, sizeof find_buf, ImGuiInputTextFlags_EnterReturnsTrue)) { find_next(ImGui::GetIO().KeyShift); find_focus = true; }
    ImGui::SameLine();
    if (button("Next", ui::Icon::ArrowDown)) find_next();
    ImGui::SameLine(0, S(4));
    if (button("Prev", ui::Icon::ArrowUp)) find_next(true);
    ImGui::SameLine(0, S(12));
    ImGui::Checkbox("Match case", &match_case);
    ImGui::SameLine();
    ImGui::Checkbox("Regex", &use_regex);
    ImGui::SameLine(0, S(12));
    if (!repl_open) { if (button("Replace\xE2\x80\xA6", ui::Icon::Wand)) repl_open = true; }
    else {
        ImGui::SetNextItemWidth(S(180));
        ImGui::InputTextWithHint("##repl", use_regex ? "replace ($1 groups)" : "replace with", repl_buf, sizeof repl_buf);
        ImGui::SameLine();
        if (button("Replace")) replace_one();
        ImGui::SameLine(0, S(4));
        if (button("Replace all", ui::Icon::None, true)) replace_all();
    }
    ImGui::SameLine(0, S(10));
    if (icon_button("fclose", ui::Icon::ErrorX, "Close (Esc)", false, true, S(26))) find_open = repl_open = false;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(38)));
}

void TextDoc::draw_text(ImVec2 size) {
    const GTheme& t = th();
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = g_font_mono ? g_font_mono : ImGui::GetFont();
    const float fs = font->FontSize;
    const float cw = font->CalcTextSizeA(fs, FLT_MAX, 0, "M").x;
    const float lh = std::round(fs * 1.35f);
    char nb[16];
    std::snprintf(nb, sizeof nb, "%d", (int)L.size());
    const float gutter = line_numbers ? (std::strlen(nb) + 2) * cw + S(8) : S(6);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.field);
    ImGui::BeginChild("##text", size, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleColor();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const ImRect in = win->InnerRect;
    int cols = std::max(8, (int)((in.GetWidth() - gutter - S(8)) / cw));
    if (wrap_dirty || (wrap && cols != wrap_cols) || row_start.size() != L.size() + 1) rebuild_wrap(cols);
    const int total_rows = row_start.back();
    int maxv = 0;
    if (!wrap) { int step = L.size() > 20000 ? (int)L.size() / 20000 : 1; for (int i = 0; i < (int)L.size(); i += step) maxv = std::max(maxv, (int)L[i].size()); maxv = std::max(maxv, vlen(caret.l)); }
    ImGui::Dummy(ImVec2(wrap ? 0 : gutter + (maxv + 4) * cw, total_rows * lh + lh));
    const float sx = ImGui::GetScrollX(), sy = ImGui::GetScrollY();
    const ImVec2 o = in.Min;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto line_of_row = [&](int r) { return (int)(std::upper_bound(row_start.begin(), row_start.end() - 1, r) - row_start.begin()) - 1; };
    int r0 = std::max(0, (int)(sy / lh)), r1 = std::min(total_rows - 1, (int)((sy + in.GetHeight()) / lh) + 1);
    int l0 = r0 < total_rows ? line_of_row(r0) : 0;
    const float tx0 = o.x + gutter - sx + S(4);
    // selection helpers
    Pos sa, sb; sel_bounds(sa, sb);
    int bl0 = std::min(anchor.l, caret.l), bl1 = std::max(anchor.l, caret.l), bv0 = std::min(box_va, box_vc), bv1 = std::max(box_va, box_vc);
    const ImU32 selc = col_alpha(t.accent, 0.30f), wsc = col_alpha(t.text_faint, 0.6f);
    dl->PushClipRect(ImVec2(o.x + gutter, o.y), in.Max, true);
    std::string disp;
    for (int l = l0; l < (int)L.size() && row_start[l] <= r1; ++l) {
        const std::string& s = L[l];
        // expand tabs; remember visual col of each byte
        disp.clear();
        int v = 0;
        for (size_t i = 0; i < s.size();) {
            int n = cp_len((unsigned char)s[i]);
            if (s[i] == '\t') { int nv = (v / tab + 1) * tab; disp.append(nv - v, ' '); v = nv; }
            else { disp.append(s, i, n); ++v; }
            i += n;
        }
        int vtotal = v;
        int nrows = row_start[l + 1] - row_start[l];
        // selection span (visual)
        int sv0 = -1, sv1 = -1;
        if (box && has_sel()) { if (l >= bl0 && l <= bl1) { sv0 = bv0; sv1 = std::max(bv1, bv0); } }
        else if (has_sel() && l >= sa.l && l <= sb.l) { sv0 = l == sa.l ? vcol(l, sa.c) : 0; sv1 = l == sb.l ? vcol(l, sb.c) : vtotal + 1; }
        // map visual col -> byte offset in disp (ASCII mostly; handle UTF-8)
        std::vector<int> voff;   // byte offset in disp for visual col k
        voff.reserve(vtotal + 1);
        for (size_t i = 0; i < disp.size();) { voff.push_back((int)i); i += cp_len((unsigned char)disp[i]); }
        voff.push_back((int)disp.size());
        for (int rr = 0; rr < nrows; ++rr) {
            int row = row_start[l] + rr;
            if (row < r0 || row > r1) continue;
            float y = o.y + row * lh - sy;
            int va = wrap ? rr * cols : 0, vb = wrap ? std::min(vtotal, va + cols) : vtotal;
            if (sv0 >= 0) {
                int a = std::max(sv0, va), b = std::min(sv1, wrap ? va + cols : sv1);
                if (box && sv0 == sv1 && sv0 >= va && sv0 <= vb) dl->AddRectFilled(ImVec2(tx0 + (sv0 - va) * cw - 1, y), ImVec2(tx0 + (sv0 - va) * cw + 1, y + lh), selc);
                else if (b > a) dl->AddRectFilled(ImVec2(tx0 + (a - va) * cw, y), ImVec2(tx0 + (b - va) * cw, y + lh), selc);
            }
            if (vb > va) {
                const char* tb_ = disp.c_str() + voff[std::min(va, (int)voff.size() - 1)];
                const char* te = disp.c_str() + voff[std::min(vb, (int)voff.size() - 1)];
                // only draw the horizontally visible part
                int vis0 = wrap ? 0 : std::max(0, (int)(sx / cw) - 1);
                if (!wrap && vis0 > 0 && vis0 < (int)voff.size()) tb_ = disp.c_str() + voff[vis0];
                float x = tx0 + (wrap ? 0 : (tb_ == disp.c_str() ? 0 : vis0) * cw);
                int vis1 = wrap ? vb : std::min(vb, vis0 + (int)(in.GetWidth() / cw) + 2);
                if (!wrap) te = disp.c_str() + voff[std::min(vis1, (int)voff.size() - 1)];
                if (te > tb_) dl->AddText(font, fs, ImVec2(x, y + (lh - fs) * 0.5f), t.text, tb_, te);
            }
            if (show_ws) {
                int v2 = 0;
                for (size_t i = 0; i < s.size();) {
                    int n = cp_len((unsigned char)s[i]);
                    int nv = s[i] == '\t' ? (v2 / tab + 1) * tab : v2 + 1;
                    if (v2 >= va && v2 < vb + (wrap ? 0 : 1)) {
                        float x = tx0 + (v2 - va) * cw, ym = y + lh * 0.5f;
                        if (s[i] == ' ') dl->AddCircleFilled(ImVec2(x + cw * 0.5f, ym), std::max(1.f, cw * 0.09f), wsc);
                        else if (s[i] == '\t') {
                            float x2 = tx0 + (nv - va) * cw - cw * 0.3f;
                            dl->AddLine(ImVec2(x + cw * 0.2f, ym), ImVec2(x2, ym), wsc);
                            dl->AddLine(ImVec2(x2 - cw * 0.3f, ym - cw * 0.3f), ImVec2(x2, ym), wsc);
                            dl->AddLine(ImVec2(x2 - cw * 0.3f, ym + cw * 0.3f), ImVec2(x2, ym), wsc);
                        }
                    }
                    v2 = nv; i += n;
                }
                if (rr == nrows - 1 && l + 1 < (int)L.size()) dl->AddText(font, fs, ImVec2(tx0 + (vtotal - va) * cw + cw * 0.2f, y + (lh - fs) * 0.5f), wsc, eol == 0 ? "\xC2\xB6" : "\xC2\xAC");
            }
        }
    }
    // caret
    {
        int cv = vcol(caret.l, caret.c);
        int rr = wrap ? std::min(cv / cols, row_start[caret.l + 1] - row_start[caret.l] - 1) : 0;
        int row = row_start[caret.l] + rr;
        float x = tx0 + (cv - (wrap ? rr * cols : 0)) * cw, y = o.y + row * lh - sy;
        bool focused = ImGui::IsWindowFocused();
        if (focused && std::fmod(ImGui::GetTime(), 1.06) < 0.62) dl->AddRectFilled(ImVec2(x, y + S(1)), ImVec2(x + S(2), y + lh - S(1)), t.accent);
        if (focused) request_redraw(2);
        if (box && caret.l != anchor.l) for (int l = bl0; l <= bl1; ++l) if (l != caret.l && !wrap) {
            float yy = o.y + row_start[l] * lh - sy;
            dl->AddRectFilled(ImVec2(tx0 + box_vc * cw, yy + S(1)), ImVec2(tx0 + box_vc * cw + S(1), yy + lh - S(1)), col_alpha(t.accent, 0.7f));
        }
        // current-line highlight under the text
        dl->AddRectFilled(ImVec2(o.x + gutter, y), ImVec2(in.Max.x, y + lh), col_alpha(t.accent, 0.05f));
        if (scroll_to_caret) {
            scroll_to_caret = false;
            float cy = row * lh;
            if (cy < sy) ImGui::SetScrollY(cy);
            else if (cy + lh > sy + in.GetHeight()) ImGui::SetScrollY(cy + lh - in.GetHeight());
            if (!wrap) {
                float cx = cv * cw;
                float vw = in.GetWidth() - gutter - S(10);
                if (cx < sx) ImGui::SetScrollX(std::max(0.f, cx - cw * 4));
                else if (cx > sx + vw) ImGui::SetScrollX(cx - vw + cw * 4);
            }
        }
    }
    dl->PopClipRect();
    // gutter
    if (line_numbers) {
        dl->AddRectFilled(o, ImVec2(o.x + gutter, in.Max.y), t.panel2);
        for (int l = l0; l < (int)L.size() && row_start[l] <= r1; ++l) {
            float y = o.y + row_start[l] * lh - sy;
            char b[16]; std::snprintf(b, sizeof b, "%d", l + 1);
            float w = font->CalcTextSizeA(fs, FLT_MAX, 0, b).x;
            dl->AddText(font, fs, ImVec2(o.x + gutter - w - S(8), y + (lh - fs) * 0.5f), l == caret.l ? t.text : t.text_faint, b);
        }
        dl->AddLine(ImVec2(o.x + gutter - 0.5f, o.y), ImVec2(o.x + gutter - 0.5f, in.Max.y), t.border);
    }
    // mouse
    auto hit = [&](ImVec2 m) {
        int row = std::clamp((int)std::floor((m.y - o.y + sy) / lh), 0, std::max(0, total_rows - 1));
        int l = line_of_row(row);
        int vc = std::max(0, (int)std::floor((m.x - tx0) / cw + 0.5f));
        if (wrap) vc += (row - row_start[l]) * cols;
        return std::pair<Pos, int>{Pos{l, byte_at_vcol(l, vc)}, vc};
    };
    const bool hov = ImGui::IsWindowHovered() && in.Contains(io.MousePos);
    if (hov && io.MousePos.x > o.x + gutter) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::SetWindowFocus();
        auto [p, vc] = hit(io.MousePos);
        if (io.KeyAlt) {
            if (!io.KeyShift) { anchor = p; box_va = vc; }
            box = true; caret = p; box_vc = vc;
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            Pos a = p, b = p;
            auto isw = [&](int i) { unsigned char ch = L[p.l][i]; return isalnum(ch) || ch == '_' || ch == '.' || ch == '-' || ch >= 0x80; };
            while (a.c > 0 && isw(a.c - 1)) --a.c;
            while (b.c < (int)L[p.l].size() && isw(b.c)) ++b.c;
            box = false; anchor = a; caret = b;
        } else { move_to(p, io.KeyShift); scroll_to_caret = false; }
        dragging = !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        want_vcol = -1;
    }
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { ImGui::SetWindowFocus(); ImGui::OpenPopup("##tctx"); }
    if (dragging) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging = false;
        else {
            auto [p, vc] = hit(io.MousePos);
            caret = p;
            if (box) box_vc = vc;
            if (io.MousePos.y > in.Max.y - S(6)) ImGui::SetScrollY(sy + lh);
            else if (io.MousePos.y < o.y + S(6)) ImGui::SetScrollY(std::max(0.f, sy - lh));
        }
    }
    if (ImGui::BeginPopup("##tctx")) {
        if (menu_item_icon("Cut", ui::Icon::Cut, "Ctrl+X", false, has_sel())) cut();
        if (menu_item_icon("Copy", ui::Icon::Copy, "Ctrl+C", false, has_sel())) copy();
        if (menu_item_icon("Paste", ui::Icon::Paste, "Ctrl+V")) paste();
        if (menu_item_icon("Select all", ui::Icon::SelectAll, "Ctrl+A")) select_all();
        ImGui::Separator();
        if (menu_item_icon("Column selection: Alt + drag", ui::Icon::Rect, nullptr, box, false)) {}
        ImGui::EndPopup();
    }
    if (ImGui::IsWindowFocused()) handle_keys();
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

static void close_current_file_deferred();

void TextDoc::handle_keys() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetCurrentContext()->WantTextInputNextFrame = 1;
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    auto pr = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    if (ctrl && !alt) {
        if (pr(ImGuiKey_C)) copy();
        else if (pr(ImGuiKey_X)) cut();
        else if (pr(ImGuiKey_V)) paste();
        else if (pr(ImGuiKey_Z)) { if (shift) do_redo(); else do_undo(); }
        else if (pr(ImGuiKey_Y)) do_redo();
        else if (pr(ImGuiKey_A)) select_all();
        else if (pr(ImGuiKey_F)) { find_open = find_focus = true; std::string s = selected_text(); if (!s.empty() && s.find('\n') == std::string::npos) std::snprintf(find_buf, sizeof find_buf, "%s", s.c_str()); }
        else if (pr(ImGuiKey_H)) { find_open = repl_open = find_focus = true; }
        else if (pr(ImGuiKey_G)) { goto_open = true; goto_line = caret.l + 1; }
        else if (pr(ImGuiKey_S)) { bool as = shift; defer([this, as]() { save(as); }); }
        else if (pr(ImGuiKey_W)) close_current_file_deferred();
        else if (ImGui::IsKeyPressed(ImGuiKey_O, false)) post_command(app::ID_FILE_OPEN);   // R20: no key repeat for a dialog command
    }
    if (pr(ImGuiKey_F3)) find_next(shift);
    if (pr(ImGuiKey_Escape) && find_open) find_open = repl_open = false;
    // movement
    const bool boxsel = alt && shift;
    auto vmove = [&](int dl_) {
        if (want_vcol < 0) want_vcol = (float)(box ? box_vc : vcol(caret.l, caret.c));
        int l = std::clamp(caret.l + dl_, 0, (int)L.size() - 1);
        int vc = (int)want_vcol;
        Pos p{l, byte_at_vcol(l, vc)};
        float keep = want_vcol;
        move_to(p, shift, boxsel);
        if (boxsel) box_vc = vc;
        want_vcol = keep;
    };
    if (pr(ImGuiKey_UpArrow)) vmove(-1);
    else if (pr(ImGuiKey_DownArrow)) vmove(1);
    else if (pr(ImGuiKey_PageUp)) vmove(-30);
    else if (pr(ImGuiKey_PageDown)) vmove(30);
    else if (pr(ImGuiKey_LeftArrow)) {
        if (boxsel) { move_to(caret, true, true); box_vc = std::max(0, box_vc - 1); caret.c = byte_at_vcol(caret.l, box_vc); }
        else if (!shift && has_sel() && !box) { Pos a, b; sel_bounds(a, b); move_to(a, false); }
        else move_to(step(caret, -1, ctrl), shift);
        if (!boxsel) want_vcol = -1;
    } else if (pr(ImGuiKey_RightArrow)) {
        if (boxsel) { move_to(caret, true, true); box_vc += 1; caret.c = byte_at_vcol(caret.l, box_vc); }
        else if (!shift && has_sel() && !box) { Pos a, b; sel_bounds(a, b); move_to(b, false); }
        else move_to(step(caret, 1, ctrl), shift);
        if (!boxsel) want_vcol = -1;
    } else if (pr(ImGuiKey_Home)) {
        if (ctrl) move_to({0, 0}, shift);
        else { int k = (int)L[caret.l].find_first_not_of(" \t"); if (k < 0) k = 0; move_to({caret.l, caret.c == k ? 0 : k}, shift); }
        want_vcol = -1;
    } else if (pr(ImGuiKey_End)) {
        if (ctrl) move_to({(int)L.size() - 1, (int)L.back().size()}, shift);
        else move_to({caret.l, (int)L[caret.l].size()}, shift);
        want_vcol = -1;
    }
    // editing
    if (ctrl && !alt) return;
    if (pr(ImGuiKey_Enter) || pr(ImGuiKey_KeypadEnter)) { new_group(); box = false; insert_text("\n"); new_group(); }
    else if (pr(ImGuiKey_Backspace)) delete_sel_or(-1);
    else if (pr(ImGuiKey_Delete)) delete_sel_or(1);
    else if (pr(ImGuiKey_Tab)) insert_text("\t");
    if (!io.InputQueueCharacters.empty()) {
        std::string s;
        for (ImWchar w : io.InputQueueCharacters) {
            if (w < 32 || w == 127) continue;
            if (w < 0x80) s += (char)w;
            else if (w < 0x800) { s += (char)(0xC0 | w >> 6); s += (char)(0x80 | (w & 63)); }
            else { s += (char)(0xE0 | w >> 12); s += (char)(0x80 | ((w >> 6) & 63)); s += (char)(0x80 | (w & 63)); }
        }
        io.InputQueueCharacters.resize(0);
        if (!s.empty()) {
            if (s == " " || has_sel()) new_group();   // word-ish undo granularity
            insert_text(s);
        }
    }
}

static void close_current_file_deferred() { defer([]() { files_on_command(app::ID_FILE_CLOSE); }); }

} // namespace

std::unique_ptr<FileDoc> make_text_doc(const std::wstring& path, std::string& err) {
    auto d = std::make_unique<TextDoc>();
    if (path.empty()) { d->name = "Untitled.txt"; return d; }
    if (!d->load(path, err)) return nullptr;
    return d;
}

std::unique_ptr<FileDoc> make_text_doc_from(const std::string& utf8, const std::string& name) {
    auto d = std::make_unique<TextDoc>();
    d->set_text(utf8);
    d->name = name;
    d->dirty_ = true;
    return d;
}

} // namespace pcbp::gui
