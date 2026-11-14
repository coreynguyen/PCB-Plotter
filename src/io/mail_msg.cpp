// ============================================================================
// mail_msg.cpp — see mail_msg.h.
// ============================================================================
#include "mail_msg.h"
#include "rtf.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>

namespace pcbp::io {

namespace {
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t rd64(const uint8_t* p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
const uint32_t FREESECT = 0xFFFFFFFF, ENDOFCHAIN = 0xFFFFFFFE;
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
} // namespace

bool is_cfb_bytes(const uint8_t* p, size_t n) {
    static const uint8_t sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    return n >= 512 && std::memcmp(p, sig, 8) == 0;
}

bool Cfb::chain(uint32_t s, const std::vector<uint32_t>& fat, std::vector<uint32_t>& out) const {
    out.clear();
    while (s != ENDOFCHAIN && s != FREESECT) {
        if (s >= fat.size() || out.size() > fat.size()) return false;   // bad index / loop
        out.push_back(s);
        s = fat[s];
    }
    return true;
}

bool Cfb::open(const std::vector<uint8_t>& d, std::string& err) {
    d_ = &d;
    if (!is_cfb_bytes(d.data(), d.size())) { err = "not an OLE2 compound file"; return false; }
    const uint8_t* h = d.data();
    uint16_t shift = rd16(h + 30), mshift = rd16(h + 32);
    if (shift != 9 && shift != 12) { err = "bad sector size"; return false; }
    ssz_ = 1u << shift;
    mssz_ = 1u << (mshift > 0 && mshift < 12 ? mshift : 6);
    uint32_t nfat = rd32(h + 44), dir_start = rd32(h + 48);
    cutoff_ = rd32(h + 56) ? rd32(h + 56) : 4096;
    uint32_t mfat_start = rd32(h + 60), nmfat = rd32(h + 64), difat_start = rd32(h + 68), ndifat = rd32(h + 72);
    const size_t nsec = d.size() > ssz_ ? (d.size() - ssz_ + ssz_ - 1) / ssz_ : 0;
    auto sec = [&](uint32_t s) -> const uint8_t* { size_t off = (size_t)(s + 1) * ssz_; return off + ssz_ <= d.size() ? d.data() + off : nullptr; };
    std::vector<uint32_t> difat;
    for (int i = 0; i < 109 && difat.size() < nfat; ++i) difat.push_back(rd32(h + 76 + i * 4));
    uint32_t ds = difat_start;
    for (uint32_t k = 0; k < ndifat && ds != ENDOFCHAIN && ds != FREESECT && difat.size() < nfat; ++k) {
        const uint8_t* p = sec(ds);
        if (!p) { err = "truncated DIFAT"; return false; }
        for (uint32_t i = 0; i < ssz_ / 4 - 1 && difat.size() < nfat; ++i) difat.push_back(rd32(p + i * 4));
        ds = rd32(p + ssz_ - 4);
    }
    fat_.clear();
    for (uint32_t s : difat) {
        const uint8_t* p = sec(s);
        if (!p) { err = "truncated FAT"; return false; }
        for (uint32_t i = 0; i < ssz_ / 4; ++i) fat_.push_back(rd32(p + i * 4));
    }
    if (fat_.size() > nsec + 1024) fat_.resize(nsec + 1024);
    std::vector<uint32_t> ch;
    if (!chain(dir_start, fat_, ch) || ch.empty()) { err = "bad directory chain"; return false; }
    dir_.clear();
    for (uint32_t s : ch) {
        const uint8_t* p = sec(s);
        if (!p) { err = "truncated directory"; return false; }
        for (uint32_t o = 0; o + 128 <= ssz_; o += 128) {
            const uint8_t* e = p + o;
            CfbEntry en;
            int nl = rd16(e + 64);
            nl = std::clamp(nl, 0, 64);
            en.name = utf16le_to_utf8(e, nl >= 2 ? nl - 2 : 0);
            en.type = e[66];
            en.left = (int)rd32(e + 68); en.right = (int)rd32(e + 72); en.child = (int)rd32(e + 76);
            if ((uint32_t)en.left == FREESECT) en.left = -1;
            if ((uint32_t)en.right == FREESECT) en.right = -1;
            if ((uint32_t)en.child == FREESECT) en.child = -1;
            en.start = rd32(e + 116);
            en.size = shift == 9 ? rd32(e + 120) : rd64(e + 120);
            dir_.push_back(en);
        }
    }
    if (dir_.empty() || dir_[0].type != 5) { err = "no root entry"; return false; }
    // mini FAT + mini stream
    minifat_.clear();
    if (nmfat && chain(mfat_start, fat_, ch))
        for (uint32_t s : ch) { const uint8_t* p = sec(s); if (!p) break; for (uint32_t i = 0; i < ssz_ / 4; ++i) minifat_.push_back(rd32(p + i * 4)); }
    ministream_.clear();
    if (chain(dir_[0].start, fat_, ch))
        for (uint32_t s : ch) { const uint8_t* p = sec(s); if (!p) break; ministream_.insert(ministream_.end(), p, p + ssz_); }
    if (ministream_.size() > dir_[0].size) ministream_.resize((size_t)dir_[0].size);
    return true;
}

std::vector<int> Cfb::children(int st) const {
    std::vector<int> out;
    if (st < 0 || st >= (int)dir_.size()) return out;
    std::vector<int> todo{dir_[st].child};
    std::vector<char> seen(dir_.size(), 0);
    while (!todo.empty()) {
        int k = todo.back();
        todo.pop_back();
        if (k < 0 || k >= (int)dir_.size() || seen[k]) continue;
        seen[k] = 1;
        out.push_back(k);
        todo.push_back(dir_[k].left);
        todo.push_back(dir_[k].right);
    }
    return out;
}

int Cfb::find_child(int st, const std::string& name) const {
    std::string n = lower(name);
    for (int k : children(st)) if (lower(dir_[k].name) == n) return k;
    return -1;
}

bool Cfb::read_stream(int e, std::vector<uint8_t>& out) const {
    out.clear();
    if (e < 0 || e >= (int)dir_.size() || dir_[e].type != 2) return false;
    const CfbEntry& en = dir_[e];
    if (en.size > d_->size() * 2 + 4096) return false;
    std::vector<uint32_t> ch;
    if (en.size < cutoff_) {
        if (!chain(en.start, minifat_, ch)) return false;
        for (uint32_t s : ch) {
            size_t off = (size_t)s * mssz_;
            if (off + mssz_ > ministream_.size()) return false;
            out.insert(out.end(), ministream_.begin() + off, ministream_.begin() + off + mssz_);
        }
    } else {
        if (!chain(en.start, fat_, ch)) return false;
        for (uint32_t s : ch) {
            size_t off = (size_t)(s + 1) * ssz_;
            if (off + ssz_ > d_->size()) { if (off < d_->size()) out.insert(out.end(), d_->begin() + off, d_->end()); break; }
            out.insert(out.end(), d_->begin() + off, d_->begin() + off + ssz_);
        }
    }
    if (out.size() < en.size) return false;
    out.resize((size_t)en.size);
    return true;
}

// ---------------------------------------------------------------------------
// LZFu ([MS-OXRTFCP])
static const char kLzfuPre[] =
    "{\\rtf1\\ansi\\mac\\deff0\\deftab720{\\fonttbl;}{\\f0\\fnil \\froman \\fswiss \\fmodern \\fscript \\fdecor MS Sans SerifSymbolArialTimes New RomanCourier"
    "{\\colortbl\\red0\\green0\\blue0\r\n\\par \\pard\\plain\\f0\\fs20\\b\\i\\u\\tab\\tx";

bool lzfu_decompress(const uint8_t* p, size_t n, std::string& out, std::string& err) {
    out.clear();
    if (n < 16) { err = "compressed RTF too short"; return false; }
    uint32_t csize = rd32(p), rsize = rd32(p + 4), magic = rd32(p + 8);
    if (magic == 0x414C454D) {   // "MELA": stored
        size_t m = std::min<size_t>(rsize, n - 16);
        out.assign((const char*)p + 16, m);
        return true;
    }
    if (magic != 0x75465A4C) { err = "unknown compressed RTF type"; return false; }
    size_t end = std::min<size_t>(n, (size_t)csize + 4);
    uint8_t dict[4096];
    const size_t pre = sizeof kLzfuPre - 1;
    std::memset(dict, 0, sizeof dict);
    std::memcpy(dict, kLzfuPre, pre);
    size_t wp = pre;
    size_t i = 16;
    out.reserve(rsize);
    while (i < end) {
        uint8_t ctl = p[i++];
        for (int b = 0; b < 8 && i < end; ++b) {
            if (ctl & (1 << b)) {
                if (i + 1 >= end) { err = "truncated reference"; return false; }
                uint16_t ref = (uint16_t)(p[i] << 8 | p[i + 1]);
                i += 2;
                size_t off = ref >> 4, len = (ref & 15) + 2;
                if (off == (wp & 4095)) { if (rsize && out.size() > rsize) out.resize(rsize); return true; }   // end marker
                for (size_t k = 0; k < len; ++k) {
                    uint8_t c = dict[(off + k) & 4095];
                    out += (char)c;
                    dict[wp & 4095] = c;
                    ++wp;
                }
            } else {
                uint8_t c = p[i++];
                out += (char)c;
                dict[wp & 4095] = c;
                ++wp;
            }
            if (out.size() > (size_t)rsize + 4096 && rsize) { err = "decompressed size overrun"; return false; }
        }
    }
    if (out.size() > rsize && rsize) out.resize(rsize);
    return true;
}

// ---------------------------------------------------------------------------
namespace {

struct Props {
    std::map<uint16_t, std::vector<uint8_t>> fixed;   // id -> 8-byte value (type in ftype)
    std::map<uint16_t, uint16_t> ftype;
};

std::string filetime_str(uint64_t ft) {
    if (!ft) return {};
    int64_t secs = (int64_t)(ft / 10000000ULL) - 11644473600LL;
    int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; --days; }
    // civil from days (H. Hinnant)
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153, dd = doy - (153 * mp + 2) / 5 + 1, mm = mp < 10 ? mp + 3 : mp - 9;
    if (mm <= 2) ++y;
    char b[64];
    std::snprintf(b, sizeof b, "%04d-%02u-%02u %02d:%02d UTC", (int)y, mm, dd, (int)(rem / 3600), (int)(rem / 60 % 60));
    return b;
}

class MsgReader {
public:
    const Cfb& f;
    std::vector<std::string>& warn;
    MsgReader(const Cfb& c, std::vector<std::string>& w) : f(c), warn(w) {}

    Props props(int st, size_t header) {
        Props p;
        int k = f.find_child(st, "__properties_version1.0");
        std::vector<uint8_t> b;
        if (k < 0 || !f.read_stream(k, b)) return p;
        for (size_t o = header; o + 16 <= b.size(); o += 16) {
            uint16_t type = rd16(&b[o]), id = rd16(&b[o + 2]);
            p.ftype[id] = type;
            p.fixed[id].assign(b.begin() + o + 8, b.begin() + o + 16);
        }
        return p;
    }
    bool stream(int st, uint16_t id, uint16_t type, std::vector<uint8_t>& out) {
        char nm[40];
        std::snprintf(nm, sizeof nm, "__substg1.0_%04X%04X", id, type);
        int k = f.find_child(st, nm);
        return k >= 0 && f.read_stream(k, out);
    }
    int cpid = 0;
    std::string str(int st, uint16_t id) {
        std::vector<uint8_t> b;
        if (stream(st, id, 0x001F, b)) {
            std::string s = utf16le_to_utf8(b.data(), b.size());
            while (!s.empty() && s.back() == 0) s.pop_back();
            return s;
        }
        if (stream(st, id, 0x001E, b)) {
            std::string s(b.begin(), b.end());
            while (!s.empty() && s.back() == 0) s.pop_back();
            return codepage_to_utf8(s, cpid ? cpid : 1252);
        }
        return {};
    }
    static int64_t ival(const Props& p, uint16_t id, int64_t def = 0) {
        auto it = p.fixed.find(id);
        if (it == p.fixed.end()) return def;
        return (int32_t)rd32(it->second.data());
    }
    static uint64_t u64(const Props& p, uint16_t id) {
        auto it = p.fixed.find(id);
        return it == p.fixed.end() ? 0 : rd64(it->second.data());
    }

    bool message(int st, size_t header, MailMessage& m, int depth) {
        m.format = "MSG";
        m.depth = depth;
        Props P = props(st, header);
        cpid = (int)ival(P, 0x3FDE, 0);
        if (!cpid) cpid = (int)ival(P, 0x3FFD, 0);
        m.subject = str(st, 0x0037);
        std::string sname = str(st, 0x0C1A), smail = str(st, 0x5D01);
        if (smail.empty()) smail = str(st, 0x0C1F);
        if (smail.find('@') == std::string::npos) { std::string s2 = str(st, 0x5D02); if (s2.find('@') != std::string::npos) smail = s2; }
        if (smail.find('@') == std::string::npos) { std::string s2 = str(st, 0x0065); if (s2.find('@') != std::string::npos) smail = s2; }   // sent representing
        m.from = sname.empty() ? smail : smail.empty() || smail == sname ? sname : sname + " <" + smail + ">";
        m.to = str(st, 0x0E04);
        m.cc = str(st, 0x0E03);
        m.bcc = str(st, 0x0E02);
        m.raw_headers = str(st, 0x007D);
        m.message_id = str(st, 0x1035);
        uint64_t t = u64(P, 0x0039);
        if (!t) t = u64(P, 0x0E06);
        m.date = filetime_str(t);
        if (!m.raw_headers.empty()) {   // prefer the original Date header
            HeaderList h;
            parse_headers(m.raw_headers + "\r\n\r\n", h);
            std::string d = header_get(h, "Date");
            if (!d.empty()) m.date = d;
        }
        m.text = str(st, 0x1000);
        std::vector<uint8_t> b;
        if (stream(st, 0x1013, 0x0102, b)) {
            std::string h(b.begin(), b.end());
            std::string cs = html_meta_charset(h);
            m.html = cs.empty() ? codepage_to_utf8(h, cpid ? cpid : 65001) : charset_to_utf8(h, cs);
        } else {
            std::string h = str(st, 0x1013);
            if (!h.empty()) m.html = h;
        }
        if (stream(st, 0x1009, 0x0102, b)) {
            std::string e;
            if (!lzfu_decompress(b.data(), b.size(), m.rtf, e)) warn.push_back("RTF body: " + e);
            else if (m.html.empty()) {
                std::string html;
                if (rtf_extract_html(m.rtf, html)) m.html = html;
            }
        }
        // recipients (fill To/Cc when the display props are missing)
        std::string to, cc, bcc;
        for (int k : f.children(st)) {
            const CfbEntry& e = f.entries()[k];
            if (e.type != 1) continue;
            if (lower(e.name).rfind("__recip_version1.0_", 0) == 0) {
                Props R = props(k, 8);
                std::string nm = str(k, 0x3001), ad = str(k, 0x39FE);
                if (ad.empty()) ad = str(k, 0x3003);
                std::string one = nm.empty() ? ad : (ad.empty() || ad == nm || ad.find('@') == std::string::npos) ? nm : nm + " <" + ad + ">";
                int type = (int)ival(R, 0x0C15, 1);
                std::string& dst = type == 2 ? cc : type == 3 ? bcc : to;
                if (!one.empty()) dst += (dst.empty() ? "" : "; ") + one;
            }
        }
        if (!to.empty()) m.to = to;
        if (!cc.empty()) m.cc = cc;
        if (!bcc.empty()) m.bcc = bcc;
        // attachments
        std::vector<std::pair<std::string, int>> at;
        for (int k : f.children(st)) {
            const CfbEntry& e = f.entries()[k];
            if (e.type == 1 && lower(e.name).rfind("__attach_version1.0_", 0) == 0) at.emplace_back(e.name, k);
        }
        std::sort(at.begin(), at.end());
        for (auto& a : at) attachment(a.second, m, depth);
        for (auto& a : m.atts)
            if (!a.cid.empty() && m.html.find("cid:" + a.cid) != std::string::npos) a.inline_part = true;
        return true;
    }

    void attachment(int st, MailMessage& m, int depth) {
        Props P = props(st, 8);
        MailAttachment a;
        a.name = str(st, 0x3707);
        if (a.name.empty()) a.name = str(st, 0x3704);
        if (a.name.empty()) a.name = str(st, 0x3001);
        size_t k = a.name.find_last_of("/\\");
        if (k != std::string::npos) a.name = a.name.substr(k + 1);
        a.mime = lower(str(st, 0x370E));
        a.cid = str(st, 0x3712);
        if (!a.cid.empty() && a.cid.front() == '<') { a.cid.erase(0, 1); if (!a.cid.empty() && a.cid.back() == '>') a.cid.pop_back(); }
        int method = (int)ival(P, 0x3705, 1);
        int flags = (int)ival(P, 0x3714, 0);
        a.inline_part = (flags & 4) != 0;   // ATT_MHTML_REF
        int obj = f.find_child(st, "__substg1.0_3701000D");
        if (method == 5 || (obj >= 0 && f.entries()[obj].type == 1)) {
            if (obj >= 0 && depth < 8) {
                auto em = std::make_shared<MailMessage>();
                MsgReader sub(f, em->warnings);
                if (sub.message(obj, 24, *em, depth + 1)) {
                    a.embedded = em;
                    a.mime = "message/rfc822";
                    if (a.name.empty() || a.name.find('.') == std::string::npos) a.name = (em->subject.empty() ? a.name.empty() ? std::string("message") : a.name : em->subject) + ".msg";
                    std::string e = eml_write(*em);
                    a.data.assign(e.begin(), e.end());   // savable form
                }
            }
        } else if (!stream(st, 0x3701, 0x0102, a.data)) {
            if (method == 6) { warn.push_back("OLE object attachment '" + a.name + "' not extracted"); }
            else if (method == 2 || method == 3 || method == 4) { warn.push_back("attachment '" + a.name + "' is a link to a file, not included"); }
        }
        m.atts.push_back(std::move(a));
    }
};

} // namespace

bool msg_parse(const std::vector<uint8_t>& data, MailMessage& m, std::string& err) {
    Cfb f;
    if (!f.open(data, err)) return false;
    // sanity: an Outlook message has property streams at the root
    if (f.find_child(0, "__properties_version1.0") < 0 && f.find_child(0, "__substg1.0_0037001F") < 0 && f.find_child(0, "__substg1.0_0037001E") < 0) {
        err = "OLE2 file is not an Outlook message (no MAPI property streams)";
        return false;
    }
    MsgReader r(f, m.warnings);
    return r.message(0, 32, m, 0);
}

} // namespace pcbp::io
