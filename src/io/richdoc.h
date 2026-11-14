#pragma once
// ============================================================================
// richdoc.h — small rich-text document model shared by the email, RTF, DOCX
// and HTML readers and the GUI document viewer (pcbp::io, portable).
//
// A document is a flat list of blocks (paragraphs). A paragraph with
// table >= 0 stands for a table block (RichDoc::tables[table]); table cells
// hold their own paragraphs. Images are stored once (RichDoc::images) and
// referenced from runs. Links are kept as data only: nothing ever opens them.
// ============================================================================
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pcbp::io {

struct RichRun {
    std::string text;           // UTF-8
    bool b = false, i = false, u = false, s = false, mono = false;
    float size = 0;             // points; 0 = default
    uint32_t color = 0;         // 0 = default, else 0xFFRRGGBB
    std::string link;           // href (shown, never followed)
    int image = -1;             // index into RichDoc::images (text = alt)
    bool same_style(const RichRun& o) const {
        return b == o.b && i == o.i && u == o.u && s == o.s && mono == o.mono && size == o.size && color == o.color && link == o.link && image < 0 && o.image < 0;
    }
};

struct RichPara {
    std::vector<RichRun> runs;
    int list = 0;               // 0 none, 1 bullet, 2 numbered
    int level = 0;              // list / quote indent level
    int num = 0;                // number for numbered lists
    int align = 0;              // 0 left, 1 centre, 2 right
    int heading = 0;            // 1..6
    bool rule = false;          // horizontal rule
    bool quote = false;
    int table = -1;             // >= 0: this block is RichDoc::tables[table]
    std::string text() const;   // concatenated run text
};

struct RichCell { std::vector<RichPara> paras; bool header = false; };
struct RichTable { std::vector<std::vector<RichCell>> rows; };

struct RichImage {
    std::string name, mime, cid;
    std::vector<uint8_t> data;  // encoded (png / jpeg / gif / bmp)
};

struct RichDoc {
    std::vector<RichPara> paras;
    std::vector<RichTable> tables;
    std::vector<RichImage> images;
    std::string title;
    std::string plain() const;  // text rendering (tables tab-separated)
    bool empty() const { return paras.empty(); }
};

RichDoc rich_from_plain(const std::string& utf8);

// HTML -> rich text. Safe: scripts / styles / forms / iframes / objects are dropped, nothing is
// fetched. <img src="cid:..."> is resolved through `cid_image` (returns an image index into
// out.images or -1); every other image becomes an "[image: alt]" placeholder.
void html_to_rich(const std::string& html_utf8, RichDoc& out, const std::function<int(const std::string& cid)>& cid_image = {});
// Decode HTML character references (&amp; &#233; &#x2014; and the common named set).
std::string html_unescape(const std::string& s);
// Best-effort charset from <meta charset=...> / <meta http-equiv content="...charset=...">.
std::string html_meta_charset(const std::string& html);

} // namespace pcbp::io
