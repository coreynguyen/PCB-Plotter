// ============================================================================
// rtf.cpp — see rtf.h.
// ============================================================================
#include "rtf.h"
#include "mime.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>

namespace pcbp::io {

namespace {

void put_utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | (cp >> 6 & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | (cp >> 12 & 0x3F)); o += (char)(0x80 | (cp >> 6 & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}
int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

int charset_cp(int cs, int ansi) {
    switch (cs) {
    case 0: return ansi; case 1: return ansi; case 2: return 0;   // symbol: pass-through
    case 77: return 10000; case 128: return 932; case 129: return 949; case 134: return 936; case 136: return 950;
    case 161: return 1253; case 162: return 1254; case 163: return 1258; case 177: return 1255; case 178: return 1256;
    case 186: return 1257; case 204: return 1251; case 222: return 874; case 238: return 1250; case 255: return 437;
    default: return ansi;
    }
}

struct Tok {
    enum K { End, Open, Close, Word, Hex, Text, Bin } k = End;
    std::string word;
    bool has_param = false;
    long param = 0;
    std::string text;   // Text: literal bytes; Hex: one byte; Bin: data
};

class Lexer {
public:
    explicit Lexer(const std::string& s) : s_(s) {}
    Tok next() {
        Tok t;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == '{') { ++i_; t.k = Tok::Open; return t; }
            if (c == '}') { ++i_; t.k = Tok::Close; return t; }
            if (c == '\\') {
                ++i_;
                if (i_ >= s_.size()) { t.k = Tok::End; return t; }
                char d = s_[i_];
                if (std::isalpha((unsigned char)d)) {
                    size_t a = i_;
                    while (i_ < s_.size() && std::isalpha((unsigned char)s_[i_]) && i_ - a < 32) ++i_;
                    t.word = s_.substr(a, i_ - a);
                    bool neg = false;
                    if (i_ < s_.size() && s_[i_] == '-' && i_ + 1 < s_.size() && std::isdigit((unsigned char)s_[i_ + 1])) { neg = true; ++i_; }
                    if (i_ < s_.size() && std::isdigit((unsigned char)s_[i_])) {
                        long v = 0;
                        int nd = 0;
                        while (i_ < s_.size() && std::isdigit((unsigned char)s_[i_])) { if (nd++ < 10) v = v * 10 + (s_[i_] - '0'); ++i_; }
                        t.has_param = true;
                        t.param = neg ? -v : v;
                    }
                    if (i_ < s_.size() && s_[i_] == ' ') ++i_;
                    t.k = Tok::Word;
                    if (t.word == "bin" && t.has_param && t.param > 0) {
                        size_t n = std::min<size_t>((size_t)t.param, s_.size() - i_);
                        t.k = Tok::Bin;
                        t.text = s_.substr(i_, n);
                        i_ += n;
                    }
                    return t;
                }
                if (d == '\'') {
                    int a = i_ + 1 < s_.size() ? hexv(s_[i_ + 1]) : -1, b = i_ + 2 < s_.size() ? hexv(s_[i_ + 2]) : -1;
                    i_ += 3;
                    t.k = Tok::Hex;
                    t.text = std::string(1, (char)((a < 0 ? 0 : a) << 4 | (b < 0 ? 0 : b)));
                    return t;
                }
                ++i_;
                // control symbols
                t.k = Tok::Word;
                t.word = std::string(1, d);
                if (d == '\r' || d == '\n') t.word = "par";
                return t;
            }
            if (c == '\r' || c == '\n') { ++i_; continue; }
            size_t a = i_;
            while (i_ < s_.size() && s_[i_] != '{' && s_[i_] != '}' && s_[i_] != '\\' && s_[i_] != '\r' && s_[i_] != '\n') ++i_;
            t.k = Tok::Text;
            t.text = s_.substr(a, i_ - a);
            return t;
        }
        t.k = Tok::End;
        return t;
    }
private:
    const std::string& s_;
    size_t i_ = 0;
};

enum class Dest { Normal, Skip, FontTbl, ColorTbl, Pict, FldInst, FldRslt, HtmlTag };

struct State {
    Dest dest = Dest::Normal;
    bool b = false, i = false, u = false, s = false;
    int fs = 0;        // half-points
    int cf = 0;
    int font = -1;
    int uc = 1;
    bool intbl = false;
    bool htmlrtf = false;
    std::string link;
    bool star = false;   // \* seen at group start
    int pict_kind = 0;   // 1 png 2 jpeg
    bool mono = false;
};

const char* const kSkipDests[] = {"stylesheet", "info", "header", "footer", "headerl", "headerr", "footerl", "footerr", "headerf", "footerf",
                                  "listtable", "listoverridetable", "rsidtbl", "xmlnstbl", "themedata", "colorschememapping", "latentstyles",
                                  "datastore", "generator", "filetbl", "revtbl", "pgdsctbl", "object", "objdata", "footnote", "annotation",
                                  "pnseclvl", "pntxta", "pntxtb", "nonshppict", "shpinst", "sp", "mmathPr", "operator", "author", "title",
                                  "company", "userprops", "bkmkstart", "bkmkend", "fldtype", "ftnsep", "ftnsepc", "aftnsep", "wgrffmtfilter",
                                  "template", "docvar", "xe", "tc", "datafield", "falt", "panose", "fontemb", "fontfile", "mhtmltag", "pn"};

bool is_skip_dest(const std::string& w) {
    for (const char* k : kSkipDests) if (w == k) return true;
    return false;
}

class Reader {
public:
    RichDoc& doc;
    explicit Reader(RichDoc& d) : doc(d) {}
    int ansi_cp = 1252;
    std::map<int, int> font_cp;
    std::map<int, bool> font_mono;
    std::vector<uint32_t> colors;
    std::string col_acc;      // colour table entry being built
    int cr = -1, cg = -1, cbl = -1;
    int cur_font_def = -1;
    std::string font_name_acc;
    std::vector<State> stack;
    State st;
    RichPara para;
    std::string pend;          // bytes pending in the current code page
    int pend_cp = 1252;
    int skip_after_u = 0;
    std::string fldinst;
    std::string pict_hex;
    // tables
    int cur_table = -1;
    std::vector<RichCell> row;
    RichCell cell;
    int list_mode = 0;         // \listtext / \pntext seen for this paragraph
    bool in_listtext = false;
    std::string listtext;

    int cp_now() const {
        auto it = font_cp.find(st.font);
        return it == font_cp.end() ? ansi_cp : it->second;
    }
    RichRun proto() const {
        RichRun r;
        r.b = st.b; r.i = st.i; r.u = st.u || !st.link.empty(); r.s = st.s;
        r.size = st.fs > 0 ? st.fs / 2.f : 0;
        if (st.cf > 0 && st.cf < (int)colors.size()) r.color = colors[st.cf];
        r.link = st.link;
        auto it = font_mono.find(st.font);
        r.mono = st.mono || (it != font_mono.end() && it->second);
        return r;
    }
    void add_text_utf8(const std::string& u) {
        if (u.empty()) return;
        if (in_listtext) { listtext += u; return; }
        RichRun r = proto();
        if (!para.runs.empty() && para.runs.back().same_style(r)) para.runs.back().text += u;
        else { r.text = u; para.runs.push_back(std::move(r)); }
    }
    void flush() {
        if (pend.empty()) return;
        std::string u = pend_cp == 0 ? pend : codepage_to_utf8(pend, pend_cp);
        pend.clear();
        add_text_utf8(u);
    }
    void add_bytes(const std::string& b) {
        if (st.dest != Dest::Normal && st.dest != Dest::FldRslt) {
            if (st.dest == Dest::FldInst) fldinst += b;
            else if (st.dest == Dest::FontTbl) font_name_acc += b;
            else if (st.dest == Dest::Pict) pict_hex += b;
            return;
        }
        int cp = cp_now();
        if (!pend.empty() && cp != pend_cp) flush();
        pend_cp = cp;
        pend += b;
    }
    void end_para(bool force_block) {
        flush();
        if (in_listtext) return;
        if (!listtext.empty()) {
            // "1." / "a)" -> numbered; bullet glyphs -> bullet
            std::string lt = listtext;
            while (!lt.empty() && (lt.back() == ' ' || lt.back() == '\t')) lt.pop_back();
            bool num = !lt.empty() && (std::isdigit((unsigned char)lt[0]) || ((std::isalpha((unsigned char)lt[0])) && lt.size() <= 3));
            para.list = num ? 2 : 1;
            if (num) para.num = std::atoi(lt.c_str());
            listtext.clear();
        }
        if (st.intbl) {
            cell.paras.push_back(std::move(para));
        } else {
            if (cur_table >= 0) cur_table = -1;   // table ended
            doc.paras.push_back(std::move(para));
        }
        para = RichPara{};
    }
    void end_cell() {
        flush();
        if (!para.runs.empty()) cell.paras.push_back(std::move(para));
        para = RichPara{};
        row.push_back(std::move(cell));
        cell = RichCell{};
    }
    void end_row() {
        flush();
        if (!para.runs.empty()) { cell.paras.push_back(std::move(para)); para = RichPara{}; }
        if (!cell.paras.empty()) { row.push_back(std::move(cell)); cell = RichCell{}; }
        if (row.empty()) return;
        if (cur_table < 0) {
            cur_table = (int)doc.tables.size();
            doc.tables.emplace_back();
            RichPara blk;
            blk.table = cur_table;
            doc.paras.push_back(std::move(blk));
        }
        doc.tables[cur_table].rows.push_back(std::move(row));
        row.clear();
    }

    void group_end() {
        // closing a destination group: finish it
        flush();
        State closing = st;
        if (stack.empty()) return;
        st = stack.back();
        stack.pop_back();
        if (closing.dest == Dest::FontTbl && cur_font_def >= 0) {
            std::string n = font_name_acc;
            for (auto& c : n) c = (char)std::tolower((unsigned char)c);
            if (n.find("courier") != std::string::npos || n.find("consol") != std::string::npos || n.find("mono") != std::string::npos || n.find("lucida console") != std::string::npos)
                font_mono[cur_font_def] = true;
        }
        if (closing.dest == Dest::FldInst && st.dest != Dest::FldInst) {
            // HYPERLINK "url" [\l "anchor"]
            size_t k = fldinst.find("HYPERLINK");
            if (k != std::string::npos) {
                size_t q = fldinst.find('"', k);
                size_t q2 = q == std::string::npos ? q : fldinst.find('"', q + 1);
                std::string url = q2 != std::string::npos ? fldinst.substr(q + 1, q2 - q - 1) : fldinst.substr(k + 9);
                while (!url.empty() && url.front() == ' ') url.erase(0, 1);
                while (!url.empty() && url.back() == ' ') url.pop_back();
                pending_link = url;
            }
            fldinst.clear();
        }
        if (closing.dest == Dest::Pict && st.dest != Dest::Pict) {
            if (closing.pict_kind) {
                RichImage im;
                im.mime = closing.pict_kind == 1 ? "image/png" : "image/jpeg";
                im.name = "picture" + std::to_string(doc.images.size() + 1) + (closing.pict_kind == 1 ? ".png" : ".jpg");
                if (!pict_bin.empty()) im.data.assign(pict_bin.begin(), pict_bin.end());
                else {
                    int hi = -1;
                    for (char c : pict_hex) { int v = hexv(c); if (v < 0) continue; if (hi < 0) hi = v; else { im.data.push_back((uint8_t)(hi << 4 | v)); hi = -1; } }
                }
                if (!im.data.empty()) {
                    RichRun r = proto();
                    r.image = (int)doc.images.size();
                    r.text = "[image]";
                    doc.images.push_back(std::move(im));
                    para.runs.push_back(std::move(r));
                }
            } else add_text_utf8("[picture]");
            pict_hex.clear();
            pict_bin.clear();
        }
        if (closing.dest == Dest::FldRslt) pending_link.clear();
        if (closing.dest == Dest::FontTbl && st.dest != Dest::FontTbl) cur_font_def = -1;
    }
    std::string pending_link;
    std::string pict_bin;

    void word(const Tok& t) {
        const std::string& w = t.word;
        long p = t.param;
        bool hp = t.has_param;
        // skip-after-\u: control words count as one character too (spec says so)
        if (skip_after_u > 0 && w != "u") { --skip_after_u; return; }
        if (w == "*") { st.star = true; return; }
        // destinations
        if (w == "fonttbl") { flush(); st.dest = Dest::FontTbl; return; }
        if (w == "colortbl") { flush(); st.dest = Dest::ColorTbl; colors.clear(); cr = cg = cbl = -1; return; }
        if (w == "pict") { flush(); st.dest = Dest::Pict; st.pict_kind = 0; pict_hex.clear(); return; }
        if (w == "fldinst") { flush(); st.dest = Dest::FldInst; fldinst.clear(); return; }
        if (w == "fldrslt") { flush(); st.dest = Dest::FldRslt; st.link = pending_link; return; }
        if (w == "listtext" || w == "pntext") { flush(); in_listtext = true; listtext_depth = (int)stack.size(); return; }
        if (is_skip_dest(w) || (st.star && st.dest == Dest::Normal && w != "shppict" && w != "fldinst" && w != "htmltag")) {
            if (w == "shppict") return;
            flush(); st.dest = Dest::Skip; return;
        }
        if (st.dest == Dest::Skip) return;
        if (st.dest == Dest::Pict) {
            if (w == "pngblip") st.pict_kind = 1;
            else if (w == "jpegblip") st.pict_kind = 2;
            else if (w == "wmetafile" || w == "emfblip" || w == "macpict" || w == "dibitmap" || w == "wbitmap") st.pict_kind = 0;
            return;
        }
        if (st.dest == Dest::ColorTbl) {
            if (w == "red") cr = (int)p; else if (w == "green") cg = (int)p; else if (w == "blue") cbl = (int)p;
            return;
        }
        if (st.dest == Dest::FontTbl) {
            if (w == "f" && hp) { cur_font_def = (int)p; font_name_acc.clear(); }
            else if (w == "fcharset" && hp && cur_font_def >= 0) font_cp[cur_font_def] = charset_cp((int)p, ansi_cp);
            else if (w == "fmodern" && cur_font_def >= 0) font_mono[cur_font_def] = true;
            return;
        }
        if (w == "ansicpg" && hp) { ansi_cp = (int)p; return; }
        if (w == "mac") { ansi_cp = 10000; return; }
        if (w == "pca") { ansi_cp = 850; return; }
        if (w == "pc") { ansi_cp = 437; return; }
        if (w == "uc" && hp) { st.uc = (int)p; return; }
        if (w == "u" && hp) {
            flush();
            long v = p < 0 ? p + 65536 : p;
            std::string u;
            if (st.dest == Dest::Normal || st.dest == Dest::FldRslt) {
                if (v >= 0xD800 && v < 0xDC00) hi_sur = (uint32_t)v;
                else if (v >= 0xDC00 && v < 0xE000 && hi_sur) { put_utf8(u, 0x10000 + ((hi_sur - 0xD800) << 10) + ((uint32_t)v - 0xDC00)); hi_sur = 0; }
                else put_utf8(u, (uint32_t)v);
                add_text_utf8(u);
            } else if (st.dest == Dest::FldInst) { put_utf8(fldinst, (uint32_t)v); }
            skip_after_u = st.uc;
            return;
        }
        if (st.dest == Dest::FldInst) return;
        // character formatting
        auto on = [&]() { return !hp || p != 0; };
        if (w == "b") { flush(); st.b = on(); return; }
        if (w == "i") { flush(); st.i = on(); return; }
        if (w == "ul" || w == "uld" || w == "uldb" || w == "ulw" || w == "uldash") { flush(); st.u = on(); return; }
        if (w == "ulnone") { flush(); st.u = false; return; }
        if (w == "strike" || w == "striked") { flush(); st.s = on(); return; }
        if (w == "fs" && hp) { flush(); st.fs = (int)p; return; }
        if (w == "cf" && hp) { flush(); st.cf = (int)p; return; }
        if (w == "f" && hp) { flush(); st.font = (int)p; return; }
        if (w == "plain") { flush(); bool tb = st.intbl; State keep = st; st = State{}; st.dest = keep.dest; st.uc = keep.uc; st.link = keep.link; st.intbl = tb; return; }
        if (w == "pard") { flush(); st.intbl = false; para.align = 0; para.level = 0; return; }
        if (w == "intbl") { st.intbl = true; return; }
        if (w == "qc") { para.align = 1; return; }
        if (w == "qr") { para.align = 2; return; }
        if (w == "ql" || w == "qj") { para.align = 0; return; }
        if (w == "li" && hp) { para.level = std::max(0, (int)(p / 360)); return; }
        if (w == "par" || w == "sect" || w == "page") { end_para(false); return; }
        if (w == "line" || w == "\n") { flush(); add_text_utf8("\n"); return; }
        if (w == "tab") { flush(); add_text_utf8("\t"); return; }
        if (w == "cell" || w == "nestcell") { end_cell(); return; }
        if (w == "row" || w == "nestrow") { end_row(); return; }
        if (w == "emdash") { flush(); add_text_utf8("\xE2\x80\x94"); return; }
        if (w == "endash") { flush(); add_text_utf8("\xE2\x80\x93"); return; }
        if (w == "bullet") { flush(); add_text_utf8("\xE2\x80\xA2"); return; }
        if (w == "lquote") { flush(); add_text_utf8("\xE2\x80\x98"); return; }
        if (w == "rquote") { flush(); add_text_utf8("\xE2\x80\x99"); return; }
        if (w == "ldblquote") { flush(); add_text_utf8("\xE2\x80\x9C"); return; }
        if (w == "rdblquote") { flush(); add_text_utf8("\xE2\x80\x9D"); return; }
        if (w == "~") { flush(); add_text_utf8("\xC2\xA0"); return; }
        if (w == "_") { flush(); add_text_utf8("\xE2\x80\x91"); return; }
        if (w == "-") return;   // optional hyphen
        if (w == "\\" || w == "{" || w == "}") { add_bytes(w); return; }
    }
    uint32_t hi_sur = 0;
    int listtext_depth = -1;

    void run(const std::string& s) {
        Lexer lx(s);
        int depth = 0;
        for (;;) {
            Tok t = lx.next();
            if (t.k == Tok::End) break;
            if (t.k == Tok::Open) {
                flush();
                stack.push_back(st);
                st.star = false;
                ++depth;
                continue;
            }
            if (t.k == Tok::Close) {
                bool closing_list = in_listtext && (int)stack.size() == listtext_depth;
                group_end();
                if (closing_list) in_listtext = false;
                skip_after_u = 0;
                if (--depth <= 0) break;
                continue;
            }
            if (t.k == Tok::Word) { word(t); continue; }
            if (t.k == Tok::Bin) { if (st.dest == Dest::Pict) pict_bin += t.text; continue; }
            if (t.k == Tok::Hex) {
                if (skip_after_u > 0) { --skip_after_u; continue; }
                if (st.dest == Dest::ColorTbl || st.dest == Dest::Skip) continue;
                add_bytes(t.text);
                continue;
            }
            if (t.k == Tok::Text) {
                std::string txt = t.text;
                if (skip_after_u > 0) { size_t k = std::min<size_t>((size_t)skip_after_u, txt.size()); txt.erase(0, k); skip_after_u -= (int)k; }
                if (st.dest == Dest::ColorTbl) {
                    for (char c : txt) if (c == ';') {
                        colors.push_back(cr < 0 ? 0u : 0xFF000000u | (uint32_t)(cr & 255) << 16 | (uint32_t)(cg & 255) << 8 | (uint32_t)(cbl & 255));
                        cr = cg = cbl = -1;
                    }
                    continue;
                }
                if (st.dest == Dest::FontTbl) { for (char c : txt) if (c != ';') font_name_acc += c; continue; }
                if (st.dest == Dest::Skip) continue;
                add_bytes(txt);
            }
        }
        flush();
        if (!para.runs.empty()) end_para(false);
        if (!row.empty()) end_row();
    }
};

} // namespace

bool is_rtf_bytes(const uint8_t* p, size_t n) { return n >= 5 && std::memcmp(p, "{\\rtf", 5) == 0; }

bool rtf_to_rich(const std::string& rtf, RichDoc& out, std::string& err) {
    if (rtf.size() < 5 || rtf.compare(0, 5, "{\\rtf") != 0) { err = "not an RTF document"; return false; }
    Reader r(out);
    r.run(rtf);
    // trim trailing empty paragraphs
    while (!out.paras.empty() && out.paras.back().table < 0 && out.paras.back().runs.empty() && !out.paras.back().rule) out.paras.pop_back();
    return true;
}

bool rtf_is_fromtext(const std::string& rtf) { return rtf.find("\\fromtext") != std::string::npos && rtf.find("\\fromtext") < 400; }

bool rtf_extract_html(const std::string& rtf, std::string& html) {
    size_t fh = rtf.find("\\fromhtml");
    if (fh == std::string::npos || fh > 1000) return false;
    Lexer lx(rtf);
    struct S { bool skip = false, htmltag = false, star = false; int uc = 1; };
    std::vector<S> stack;
    S st;
    bool htmlrtf = false;
    int ansi = 1252, skip_u = 0;
    std::string pend;
    std::string out;
    auto flush = [&]() { if (!pend.empty()) { out += codepage_to_utf8(pend, ansi); pend.clear(); } };
    auto emit_ok = [&]() { return !st.skip && (st.htmltag || !htmlrtf); };
    int depth = 0;
    for (;;) {
        Tok t = lx.next();
        if (t.k == Tok::End) break;
        if (t.k == Tok::Open) { stack.push_back(st); st.star = false; ++depth; continue; }
        if (t.k == Tok::Close) { if (!stack.empty()) { st = stack.back(); stack.pop_back(); } skip_u = 0; if (--depth <= 0) break; continue; }
        if (t.k == Tok::Word) {
            const std::string& w = t.word;
            if (skip_u > 0 && w != "u") { --skip_u; continue; }
            if (w == "*") { st.star = true; continue; }
            if (w == "htmltag") { st.htmltag = true; st.skip = false; continue; }
            if (w == "mhtmltag") { st.skip = true; continue; }
            if (w == "fonttbl" || w == "colortbl" || w == "stylesheet" || w == "info" || w == "pict" || (st.star && !st.htmltag && w != "htmltag")) { st.skip = true; continue; }
            if (w == "htmlrtf") { flush(); htmlrtf = !t.has_param || t.param != 0; continue; }
            if (w == "ansicpg" && t.has_param) { ansi = (int)t.param; continue; }
            if (w == "uc" && t.has_param) { st.uc = (int)t.param; continue; }
            if (!emit_ok()) continue;
            if (w == "par") { flush(); out += "\r\n"; continue; }
            if (w == "tab") { flush(); out += "\t"; continue; }
            if (w == "line") { flush(); out += "\r\n"; continue; }
            if (w == "u" && t.has_param) { flush(); long v = t.param < 0 ? t.param + 65536 : t.param; put_utf8(out, (uint32_t)v); skip_u = st.uc; continue; }
            if (w == "\\" || w == "{" || w == "}") { pend += w; continue; }
            if (w == "~") { flush(); out += "\xC2\xA0"; continue; }
            continue;
        }
        if (t.k == Tok::Hex) { if (skip_u > 0) { --skip_u; continue; } if (emit_ok()) pend += t.text; continue; }
        if (t.k == Tok::Text) {
            std::string txt = t.text;
            if (skip_u > 0) { size_t k = std::min<size_t>((size_t)skip_u, txt.size()); txt.erase(0, k); skip_u -= (int)k; }
            if (emit_ok()) pend += txt;
        }
    }
    flush();
    html = out;
    return true;
}

// ---------------------------------------------------------------------------
// writer
static void rtf_escape(std::string& o, const std::string& u) {
    for (size_t i = 0; i < u.size();) {
        unsigned char c = u[i];
        uint32_t cp;
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n && i + k < u.size(); ++k) cp = cp << 6 | (u[i + k] & 0x3F);
        i += n;
        if (cp == '\\' || cp == '{' || cp == '}') { o += '\\'; o += (char)cp; }
        else if (cp == '\n') o += "\\line ";
        else if (cp == '\t') o += "\\tab ";
        else if (cp < 0x80) o += (char)cp;
        else if (cp < 0x10000) { o += "\\u" + std::to_string(cp < 0x8000 ? (int)cp : (int)cp - 65536) + "?"; }
        else {
            uint32_t v = cp - 0x10000;
            int hi = 0xD800 + (int)(v >> 10), lo = 0xDC00 + (int)(v & 0x3FF);
            o += "\\u" + std::to_string(hi - 65536) + "?\\u" + std::to_string(lo - 65536) + "?";
        }
    }
}

std::string rich_to_rtf(const RichDoc& d) {
    std::vector<uint32_t> cols;
    auto col_index = [&](uint32_t c) -> int {
        if (!c) return 0;
        for (size_t i = 0; i < cols.size(); ++i) if (cols[i] == c) return (int)i + 1;
        cols.push_back(c);
        return (int)cols.size();
    };
    std::string body;
    auto runs = [&](const std::vector<RichRun>& rr) {
        for (auto& r : rr) {
            if (r.image >= 0 && r.image < (int)d.images.size()) {
                const RichImage& im = d.images[r.image];
                bool png = im.mime == "image/png", jpg = im.mime == "image/jpeg";
                if (png || jpg) {
                    body += std::string("{\\pict") + (png ? "\\pngblip" : "\\jpegblip") + "\n";
                    static const char* H = "0123456789abcdef";
                    for (size_t k = 0; k < im.data.size(); ++k) { body += H[im.data[k] >> 4]; body += H[im.data[k] & 15]; if (k % 64 == 63) body += "\n"; }
                    body += "}";
                }
                continue;
            }
            std::string fmt;
            if (r.b) fmt += "\\b";
            if (r.i) fmt += "\\i";
            if (r.u) fmt += "\\ul";
            if (r.s) fmt += "\\strike";
            if (r.mono) fmt += "\\f1";
            if (r.size > 0) fmt += "\\fs" + std::to_string((int)(r.size * 2 + 0.5f));
            if (r.color) fmt += "\\cf" + std::to_string(col_index(r.color));
            std::string txt;
            rtf_escape(txt, r.text);
            if (!r.link.empty()) {
                std::string l;
                rtf_escape(l, r.link);
                body += "{\\field{\\*\\fldinst HYPERLINK \"" + l + "\"}{\\fldrslt {" + fmt + " " + txt + "}}}";
            } else body += "{" + fmt + (fmt.empty() ? "" : " ") + txt + "}";
        }
    };
    auto para = [&](const RichPara& p, bool in_cell) {
        body += "\\pard";
        if (in_cell) body += "\\intbl";
        if (p.align == 1) body += "\\qc"; else if (p.align == 2) body += "\\qr";
        int indent = p.level * 360 + (p.list ? 360 : 0);
        if (indent) body += "\\li" + std::to_string(indent) + (p.list ? "\\fi-360" : "");
        if (p.heading) body += "\\b\\fs" + std::to_string(p.heading == 1 ? 36 : p.heading == 2 ? 30 : 26);
        body += " ";
        if (p.list == 1) body += "{\\listtext \\u8226?\\tab}";
        else if (p.list == 2) body += "{\\listtext " + std::to_string(p.num ? p.num : 1) + ".\\tab}";
        runs(p.runs);
        if (p.heading) body += "\\b0\\fs22";
    };
    for (auto& p : d.paras) {
        if (p.table >= 0 && p.table < (int)d.tables.size()) {
            for (auto& row : d.tables[p.table].rows) {
                body += "\\trowd\\trgaph108";
                int x = 0;
                for (size_t c = 0; c < row.size(); ++c) { x += 2400; body += "\\clbrdrt\\brdrs\\clbrdrl\\brdrs\\clbrdrb\\brdrs\\clbrdrr\\brdrs\\cellx" + std::to_string(x); }
                body += "\n";
                for (auto& cell : row) {
                    for (size_t k = 0; k < cell.paras.size(); ++k) { para(cell.paras[k], true); if (k + 1 < cell.paras.size()) body += "\\par "; }
                    if (cell.paras.empty()) body += "\\pard\\intbl ";
                    body += "\\cell ";
                }
                body += "\\row\n";
            }
            body += "\\pard\n";
            continue;
        }
        if (p.rule) { body += "\\pard\\brdrb\\brdrs\\brdrw10\\brsp20 \\par\n"; continue; }
        para(p, false);
        body += "\\par\n";
    }
    std::string o = "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fswiss\\fcharset0 Segoe UI;}{\\f1\\fmodern\\fcharset0 Consolas;}}\n{\\colortbl;";
    for (uint32_t c : cols) { char b[64]; std::snprintf(b, sizeof b, "\\red%u\\green%u\\blue%u;", (c >> 16) & 255, (c >> 8) & 255, c & 255); o += b; }
    o += "}\n\\viewkind4\\f0\\fs22\n" + body + "}\n";
    return o;
}

} // namespace pcbp::io
