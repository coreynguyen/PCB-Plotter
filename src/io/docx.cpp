// ============================================================================
// docx.cpp — see docx.h.
// ============================================================================
#include "docx.h"
#include "archive.h"
#include "xml_lite.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

namespace pcbp::io {

namespace {

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

const ArchiveEntry* find_entry(const std::vector<ArchiveEntry>& es, const std::string& name) {
    std::string n = lower(name);
    for (auto& e : es) if (lower(e.name) == n) return &e;
    return nullptr;
}

bool on_val(const XmlNode* n) {
    if (!n) return false;
    std::string v = lower(n->attr("w:val", "true"));
    return !(v == "0" || v == "false" || v == "none" || v == "off");
}

struct Ctx {
    const std::vector<ArchiveEntry>& zip;
    RichDoc& doc;
    std::map<std::string, std::string> rels;           // rId -> target
    std::map<std::string, int> num_fmt;                 // numId -> 1 bullet / 2 numbered (level 0)
    std::map<std::string, int> style_heading;           // styleId -> heading level
    std::map<std::string, int> counters;                // numId:ilvl -> count
    std::map<std::string, int> image_of_target;

    int image(const std::string& rid) {
        auto it = rels.find(rid);
        if (it == rels.end()) return -1;
        std::string t = it->second;
        if (t.rfind("http", 0) == 0) return -1;   // external: never fetched
        std::string path = t[0] == '/' ? t.substr(1) : "word/" + t;
        auto f = image_of_target.find(path);
        if (f != image_of_target.end()) return f->second;
        const ArchiveEntry* e = find_entry(zip, path);
        if (!e) return -1;
        RichImage im;
        im.name = path.substr(path.find_last_of('/') + 1);
        std::string ext = lower(im.name.substr(im.name.find_last_of('.') + 1));
        im.mime = ext == "png" ? "image/png" : (ext == "jpg" || ext == "jpeg") ? "image/jpeg" : ext == "gif" ? "image/gif" : ext == "bmp" ? "image/bmp" : "application/octet-stream";
        im.data = e->data;
        int idx = (int)doc.images.size();
        doc.images.push_back(std::move(im));
        image_of_target[path] = idx;
        return idx;
    }

    void run_props(const XmlNode* rp, RichRun& r) {
        if (!rp) return;
        if (const XmlNode* n = rp->child("b")) r.b = on_val(n);
        if (const XmlNode* n = rp->child("i")) r.i = on_val(n);
        if (const XmlNode* n = rp->child("u")) r.u = on_val(n);
        if (const XmlNode* n = rp->child("strike")) r.s = on_val(n);
        if (const XmlNode* n = rp->child("dstrike")) r.s = r.s || on_val(n);
        if (const XmlNode* n = rp->child("sz")) { double hp = std::atof(n->attr("w:val").c_str()); if (hp > 0) r.size = (float)std::min(144.0, hp / 2); }
        if (const XmlNode* n = rp->child("color")) {
            std::string v = n->attr("w:val");
            if (v.size() == 6 && lower(v) != "auto") { char* e; unsigned long x = std::strtoul(v.c_str(), &e, 16); if (!*e) r.color = 0xFF000000u | (uint32_t)x; }
        }
        if (const XmlNode* n = rp->child("rFonts")) {
            std::string f = lower(n->attr("w:ascii") + n->attr("w:hAnsi"));
            if (f.find("courier") != std::string::npos || f.find("consol") != std::string::npos || f.find("mono") != std::string::npos) r.mono = true;
        }
        if (const XmlNode* n = rp->child("vanish")) if (on_val(n)) r.text = "\x01";   // hidden marker
    }

    void runs(const XmlNode& p, RichPara& out, const std::string& link) {
        for (const XmlNode& c : p.children) {
            std::string_view ln = c.local();
            if (ln == "r") {
                RichRun proto;
                run_props(c.child("rPr"), proto);
                if (proto.text == "\x01") continue;   // hidden
                proto.link = link;
                if (!link.empty()) proto.u = true;
                for (const XmlNode& k : c.children) {
                    std::string_view kl = k.local();
                    RichRun r = proto;
                    if (kl == "t") r.text = k.text;
                    else if (kl == "tab") r.text = "\t";
                    else if (kl == "br" || kl == "cr") r.text = "\n";
                    else if (kl == "noBreakHyphen") r.text = "-";
                    else if (kl == "sym") { r.text = "?"; }
                    else if (kl == "drawing" || kl == "pict") {
                        std::vector<const XmlNode*> blips;
                        k.find_all("blip", blips);
                        std::vector<const XmlNode*> vimg;
                        k.find_all("imagedata", vimg);
                        for (auto* v : vimg) blips.push_back(v);
                        for (const XmlNode* b : blips) {
                            std::string rid = b->attr("r:embed");
                            if (rid.empty()) rid = b->attr("r:id");
                            int im = image(rid);
                            RichRun ir = proto;
                            if (im >= 0) { ir.image = im; ir.text = "[image]"; }
                            else { ir.text = "[linked image \xE2\x80\x94 not loaded]"; ir.i = true; }
                            out.runs.push_back(ir);
                        }
                        continue;
                    } else continue;
                    if (r.text.empty()) continue;
                    if (!out.runs.empty() && out.runs.back().same_style(r)) out.runs.back().text += r.text;
                    else out.runs.push_back(r);
                }
            } else if (ln == "hyperlink") {
                std::string l;
                std::string rid = c.attr("r:id");
                auto it = rels.find(rid);
                if (it != rels.end()) l = it->second;
                else if (!c.attr("w:anchor").empty()) l = "#" + c.attr("w:anchor");
                runs(c, out, l);
            } else if (ln == "smartTag" || ln == "ins" || ln == "sdt" || ln == "sdtContent" || ln == "fldSimple" || ln == "customXml") {
                runs(c, out, link);   // container: tracked insertions shown as text
            }
            // w:del (tracked deletions), w:proofErr, bookmarks: ignored
        }
    }

    RichPara para(const XmlNode& p) {
        RichPara out;
        if (const XmlNode* pp = p.child("pPr")) {
            if (const XmlNode* s = pp->child("pStyle")) {
                std::string id = s->attr("w:val");
                auto it = style_heading.find(id);
                if (it != style_heading.end()) out.heading = it->second;
                else {
                    std::string l = lower(id);
                    if (l.rfind("heading", 0) == 0 && l.size() > 7) out.heading = std::clamp(std::atoi(l.c_str() + 7), 1, 6);
                    else if (l == "title") out.heading = 1;
                }
                if (lower(id).find("listbullet") != std::string::npos) out.list = 1;
                if (lower(id).find("listnumber") != std::string::npos) out.list = 2;
            }
            if (const XmlNode* np = pp->child("numPr")) {
                std::string id = np->child("numId") ? np->child("numId")->attr("w:val") : "";
                int lvl = np->child("ilvl") ? std::atoi(np->child("ilvl")->attr("w:val").c_str()) : 0;
                if (!id.empty() && id != "0") {
                    auto it = num_fmt.find(id + ":" + std::to_string(lvl));
                    if (it == num_fmt.end()) it = num_fmt.find(id + ":0");
                    out.list = it == num_fmt.end() ? 1 : it->second;
                    out.level = lvl;
                    if (out.list == 2) out.num = ++counters[id + ":" + std::to_string(lvl)];
                }
            }
            if (const XmlNode* jc = pp->child("jc")) { std::string v = jc->attr("w:val"); out.align = v == "center" ? 1 : (v == "right" || v == "end") ? 2 : 0; }
            if (const XmlNode* ind = pp->child("ind")) {
                int left = std::atoi((ind->attr("w:left").empty() ? ind->attr("w:start") : ind->attr("w:left")).c_str());
                if (!out.list && left >= 360) out.level = std::min(6, left / 720);
            }
        }
        runs(p, out, {});
        if (out.heading) for (auto& r : out.runs) { r.b = true; if (r.size == 0) r.size = out.heading == 1 ? 18.f : out.heading == 2 ? 15.f : 13.f; }
        return out;
    }

    void table_cells(const XmlNode& tc, RichCell& cell) {
        for (const XmlNode& c : tc.children) {
            std::string_view ln = c.local();
            if (ln == "p") cell.paras.push_back(para(c));
            else if (ln == "tbl") {   // nested table: flatten rows into cell paragraphs
                for (const XmlNode* tr : c.kids("tr")) {
                    RichPara p;
                    for (const XmlNode* tcc : tr->kids("tc")) {
                        RichCell inner;
                        table_cells(*tcc, inner);
                        if (!p.runs.empty()) { RichRun sep; sep.text = " | "; p.runs.push_back(sep); }
                        for (auto& ip : inner.paras) for (auto& r : ip.runs) p.runs.push_back(r);
                    }
                    cell.paras.push_back(std::move(p));
                }
            } else if (ln == "sdt") { if (const XmlNode* sc = c.child("sdtContent")) table_cells(*sc, cell); }
        }
    }

    void body(const XmlNode& b, std::vector<RichPara>& out) {
        for (const XmlNode& c : b.children) {
            std::string_view ln = c.local();
            if (ln == "p") out.push_back(para(c));
            else if (ln == "tbl") {
                RichTable t;
                for (const XmlNode* tr : c.kids("tr")) {
                    std::vector<RichCell> row;
                    bool hdr = false;
                    if (const XmlNode* trp = tr->child("trPr")) hdr = trp->child("tblHeader") != nullptr;
                    for (const XmlNode* tc : tr->kids("tc")) {
                        RichCell cell;
                        cell.header = hdr;
                        table_cells(*tc, cell);
                        row.push_back(std::move(cell));
                    }
                    if (!row.empty()) t.rows.push_back(std::move(row));
                }
                if (t.rows.empty()) continue;
                RichPara blk;
                blk.table = (int)doc.tables.size();
                doc.tables.push_back(std::move(t));
                out.push_back(std::move(blk));
            } else if (ln == "sdt") {
                if (const XmlNode* sc = c.child("sdtContent")) body(*sc, out);
            } else if (ln == "customXml") body(c, out);
        }
    }
};

} // namespace

bool is_docx_bytes(const std::vector<uint8_t>& data) {
    if (data.size() < 30 || data[0] != 'P' || data[1] != 'K') return false;
    std::string head(data.begin(), data.begin() + std::min<size_t>(data.size(), 65536));
    if (head.find("word/") != std::string::npos) return true;
    // central directory is at the end
    std::string tail(data.end() - std::min<size_t>(data.size(), 65536), data.end());
    return tail.find("word/document.xml") != std::string::npos;
}

bool docx_read(const std::vector<uint8_t>& data, RichDoc& out, std::string& err) {
    std::vector<ArchiveEntry> zip;
    if (!read_zip(data.data(), data.size(), zip, err)) return false;
    // main part: from [Content_Types] / _rels/.rels, default word/document.xml
    std::string main = "word/document.xml";
    if (const ArchiveEntry* r = find_entry(zip, "_rels/.rels")) {
        XmlNode x;
        std::string e;
        if (xml_parse(std::string(r->data.begin(), r->data.end()), x, e)) {
            std::vector<const XmlNode*> rs;
            x.find_all("Relationship", rs);
            for (auto* n : rs) if (n->attr("Type").find("/officeDocument") != std::string::npos) { std::string t = n->attr("Target"); if (!t.empty() && t[0] == '/') t.erase(0, 1); main = t; }
        }
    }
    const ArchiveEntry* doc = find_entry(zip, main);
    if (!doc) { err = "no " + main + " in the package (not a Word document?)"; return false; }
    Ctx c{zip, out};
    std::string dir = main.substr(0, main.find_last_of('/') + 1), base = main.substr(dir.size());
    if (const ArchiveEntry* r = find_entry(zip, dir + "_rels/" + base + ".rels")) {
        XmlNode x;
        std::string e;
        if (xml_parse(std::string(r->data.begin(), r->data.end()), x, e)) {
            std::vector<const XmlNode*> rs;
            x.find_all("Relationship", rs);
            for (auto* n : rs) c.rels[n->attr("Id")] = n->attr("Target");
        }
    }
    if (const ArchiveEntry* n = find_entry(zip, dir + "numbering.xml")) {
        XmlNode x;
        std::string e;
        if (xml_parse(std::string(n->data.begin(), n->data.end()), x, e)) {
            std::map<std::string, std::map<int, int>> abs;   // abstractNumId -> lvl -> kind
            std::vector<const XmlNode*> an;
            x.find_all("abstractNum", an);
            for (auto* a : an)
                for (auto* l : a->kids("lvl")) {
                    const XmlNode* f = l->child("numFmt");
                    std::string v = f ? f->attr("w:val") : "bullet";
                    abs[a->attr("w:abstractNumId")][std::atoi(l->attr("w:ilvl").c_str())] = (v == "bullet" || v == "none") ? 1 : 2;
                }
            std::vector<const XmlNode*> nums;
            x.find_all("num", nums);
            for (auto* nm : nums) {
                const XmlNode* ai = nm->child("abstractNumId");
                if (!ai) continue;
                for (auto& kv : abs[ai->attr("w:val")]) c.num_fmt[nm->attr("w:numId") + ":" + std::to_string(kv.first)] = kv.second;
            }
        }
    }
    if (const ArchiveEntry* s = find_entry(zip, dir + "styles.xml")) {
        XmlNode x;
        std::string e;
        if (xml_parse(std::string(s->data.begin(), s->data.end()), x, e)) {
            std::vector<const XmlNode*> st;
            x.find_all("style", st);
            for (auto* n : st) {
                const XmlNode* nm = n->child("name");
                std::string l = lower(nm ? nm->attr("w:val") : "");
                if (l.rfind("heading ", 0) == 0) c.style_heading[n->attr("w:styleId")] = std::clamp(std::atoi(l.c_str() + 8), 1, 6);
                else if (l == "title") c.style_heading[n->attr("w:styleId")] = 1;
            }
        }
    }
    XmlNode x;
    if (!xml_parse(std::string(doc->data.begin(), doc->data.end()), x, err)) return false;
    const XmlNode* body = x.find("body");
    if (!body) { err = "document has no body"; return false; }
    c.body(*body, out.paras);
    if (const ArchiveEntry* core = find_entry(zip, "docProps/core.xml")) {
        XmlNode cx;
        std::string e;
        if (xml_parse(std::string(core->data.begin(), core->data.end()), cx, e)) if (const XmlNode* t = cx.find("title")) out.title = t->text;
    }
    return true;
}

} // namespace pcbp::io
