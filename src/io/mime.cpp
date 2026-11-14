// ============================================================================
// mime.cpp — see mime.h. RFC 5322 / 2045-2049 / 2047 / 2231.
// ============================================================================
#include "mime.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace pcbp::io {

namespace {
#include "mime_charsets.inc"

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}
void put_utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | (cp >> 6 & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | (cp >> 12 & 0x3F)); o += (char)(0x80 | (cp >> 6 & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}
std::string norm_cs(std::string c) {
    c = lower(trim(c));
    if (c.size() >= 2 && c.front() == '"' && c.back() == '"') c = c.substr(1, c.size() - 2);
    std::string o;
    for (char ch : c) if (ch != '_' && ch != ' ') o += ch;
    return o;
}
const CsTable* find_table(const std::string& cs) {
    std::string c = cs;
    // aliases
    if (c.rfind("cp125", 0) == 0) c = "windows-" + c.substr(2);
    if (c.rfind("windows125", 0) == 0) c = "windows-" + c.substr(7);
    if (c.rfind("iso8859-", 0) == 0) c = "iso-" + c.substr(3);
    if (c.rfind("iso8859", 0) == 0 && c.size() > 7 && c[7] != '-') c = "iso-8859-" + c.substr(7);
    if (c.rfind("latin", 0) == 0) {
        static const char* lat[] = {"", "", "iso-8859-2", "iso-8859-3", "iso-8859-4", "iso-8859-9", "iso-8859-10", "iso-8859-13", "iso-8859-14", "iso-8859-15", "iso-8859-16"};
        int n = std::atoi(c.c_str() + 5);
        if (n >= 2 && n <= 10) c = lat[n];
    }
    if (c == "cp437" || c == "437") c = "ibm437";
    if (c == "cp850" || c == "850") c = "ibm850";
    if (c == "cp866") c = "koi8-r";   // not exact; better than mojibake for Cyrillic headers
    for (const CsTable& t : kCs) if (c == t.name) return &t;
    return nullptr;
}
std::string single_byte(const std::string& b, const uint16_t* hi) {
    std::string o;
    o.reserve(b.size() + b.size() / 4);
    for (unsigned char ch : b) {
        if (ch < 0x80) o += (char)ch;
        else put_utf8(o, hi ? hi[ch - 0x80] : ch);
    }
    return o;
}
int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
} // namespace

bool utf8_valid(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || (n == 1 && c < 0xC2)) return false;
        if (i + n >= s.size() && n) return false;
        for (int k = 1; k <= n; ++k) if (((unsigned char)s[i + k] >> 6) != 2) return false;
        i += n + 1;
    }
    return true;
}

std::string utf16le_to_utf8(const uint8_t* p, size_t n) {
    std::string o;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t c = p[i] | p[i + 1] << 8;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
            uint32_t d = p[i + 2] | p[i + 3] << 8;
            if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; }
        }
        put_utf8(o, c);
    }
    return o;
}

std::string charset_to_utf8(const std::string& bytes, const std::string& charset) {
    std::string c = norm_cs(charset);
    if (c.empty() || c == "us-ascii" || c == "ascii" || c == "utf-8" || c == "utf8" || c == "unicode-1-1-utf-8") {
        if (utf8_valid(bytes)) return bytes;
        return single_byte(bytes, find_table("windows-1252")->hi);   // mislabelled 8-bit
    }
    if (c == "iso-8859-1" || c == "latin1" || c == "l1" || c == "iso8859-1" || c == "iso88591") {
        // de-facto: treat as Windows-1252 superset (HTML5 rule) — C1 controls are never meant literally
        return single_byte(bytes, find_table("windows-1252")->hi);
    }
    if (c == "utf-16le" || c == "utf-16" || c == "ucs-2" || c == "unicode") {
        const uint8_t* p = (const uint8_t*)bytes.data();
        size_t n = bytes.size();
        if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {   // BE BOM
            std::string sw(bytes.begin() + 2, bytes.end());
            for (size_t i = 0; i + 1 < sw.size(); i += 2) std::swap(sw[i], sw[i + 1]);
            return utf16le_to_utf8((const uint8_t*)sw.data(), sw.size());
        }
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { p += 2; n -= 2; }
        return utf16le_to_utf8(p, n);
    }
    if (c == "utf-16be") {
        std::string sw = bytes;
        for (size_t i = 0; i + 1 < sw.size(); i += 2) std::swap(sw[i], sw[i + 1]);
        return utf16le_to_utf8((const uint8_t*)sw.data(), sw.size());
    }
    if (const CsTable* t = find_table(c)) return single_byte(bytes, t->hi);
    // unknown: UTF-8 when valid, else Windows-1252
    if (utf8_valid(bytes)) return bytes;
    return single_byte(bytes, find_table("windows-1252")->hi);
}

std::string codepage_to_utf8(const std::string& bytes, int cp) {
    switch (cp) {
    case 65001: return charset_to_utf8(bytes, "utf-8");
    case 1200: return charset_to_utf8(bytes, "utf-16le");
    case 1201: return charset_to_utf8(bytes, "utf-16be");
    case 20127: return charset_to_utf8(bytes, "us-ascii");
    case 20866: return charset_to_utf8(bytes, "koi8-r");
    case 21866: return charset_to_utf8(bytes, "koi8-u");
    case 437: return charset_to_utf8(bytes, "ibm437");
    case 850: return charset_to_utf8(bytes, "ibm850");
    default: break;
    }
    if (cp >= 1250 && cp <= 1258) return charset_to_utf8(bytes, "windows-" + std::to_string(cp));
    if (cp >= 28591 && cp <= 28606) return charset_to_utf8(bytes, "iso-8859-" + std::to_string(cp - 28590));
    return charset_to_utf8(bytes, "");
}

std::vector<uint8_t> base64_decode(const std::string& s) {
    std::vector<uint8_t> o;
    o.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back((uint8_t)(acc >> bits)); }
    }
    return o;
}

std::string base64_encode(const uint8_t* p, size_t n, int line_len) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    int col = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o += T[v >> 18 & 63]; o += T[v >> 12 & 63];
        o += i + 1 < n ? T[v >> 6 & 63] : '=';
        o += i + 2 < n ? T[v & 63] : '=';
        col += 4;
        if (line_len > 0 && col >= line_len) { o += "\r\n"; col = 0; }
    }
    return o;
}

std::vector<uint8_t> qp_decode(const std::string& s, bool hdr) {
    std::vector<uint8_t> o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '=') {
            if (i + 2 < s.size() + 1 && i + 1 < s.size()) {
                if (s[i + 1] == '\r' || s[i + 1] == '\n') {   // soft break
                    ++i;
                    if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ++i;
                    continue;
                }
                if (i + 2 < s.size()) {
                    int a = hexv(s[i + 1]), b = hexv(s[i + 2]);
                    if (a >= 0 && b >= 0) { o.push_back((uint8_t)(a << 4 | b)); i += 2; continue; }
                }
                // "=  \r\n" (trailing whitespace before soft break)
                size_t j = i + 1;
                while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
                if (j < s.size() && (s[j] == '\r' || s[j] == '\n')) {
                    i = j;
                    if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ++i;
                    continue;
                }
            }
            o.push_back('=');
        } else if (hdr && c == '_') o.push_back(' ');
        else o.push_back((uint8_t)c);
    }
    return o;
}

std::string decode_encoded_words(const std::string& raw) {
    std::string out;
    size_t i = 0;
    bool last_was_word = false;
    std::string pending_ws;
    while (i < raw.size()) {
        if (raw.compare(i, 2, "=?") == 0) {
            size_t q1 = raw.find('?', i + 2);
            size_t q2 = q1 == std::string::npos ? q1 : raw.find('?', q1 + 1);
            size_t end = q2 == std::string::npos ? q2 : raw.find("?=", q2 + 1);
            if (end != std::string::npos && q2 == q1 + 2) {
                std::string cs = raw.substr(i + 2, q1 - i - 2);
                size_t star = cs.find('*');   // RFC 2231 language suffix
                if (star != std::string::npos) cs.resize(star);
                char enc = (char)std::toupper((unsigned char)raw[q1 + 1]);
                std::string txt = raw.substr(q2 + 1, end - q2 - 1);
                std::vector<uint8_t> b;
                bool ok = true;
                if (enc == 'B') b = base64_decode(txt);
                else if (enc == 'Q') b = qp_decode(txt, true);
                else ok = false;
                if (ok) {
                    if (!last_was_word) out += pending_ws;   // whitespace between adjacent encoded words is dropped
                    pending_ws.clear();
                    out += charset_to_utf8(std::string(b.begin(), b.end()), cs);
                    last_was_word = true;
                    i = end + 2;
                    continue;
                }
            }
        }
        char c = raw[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { pending_ws += (c == '\r' || c == '\n') ? ' ' : c; ++i; continue; }
        out += pending_ws;
        pending_ws.clear();
        last_was_word = false;
        // raw 8-bit in headers (RFC 6532 UTF-8 or legacy): pass through, fixed below
        out += c;
        ++i;
    }
    out += pending_ws;
    if (!utf8_valid(out)) out = charset_to_utf8(out, "windows-1252");
    return trim(out);
}

size_t parse_headers(const std::string& m, HeaderList& out) {
    size_t i = 0, n = m.size();
    // skip an mbox "From " line
    if (m.compare(0, 5, "From ") == 0) { size_t e = m.find('\n'); i = e == std::string::npos ? n : e + 1; }
    while (i < n) {
        size_t e = m.find('\n', i);
        if (e == std::string::npos) e = n;
        std::string line = m.substr(i, e - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return e < n ? e + 1 : n;
        if ((line[0] == ' ' || line[0] == '\t') && !out.empty()) {
            out.back().second += line;   // unfold (keep the WSP)
        } else {
            size_t c = line.find(':');
            if (c == std::string::npos || c == 0) {
                if (out.empty()) return i;   // not a header block at all: whole thing is body
            } else {
                out.emplace_back(trim(line.substr(0, c)), trim(line.substr(c + 1)));
            }
        }
        i = e + 1;
    }
    return n;
}

const std::string* header_find(const HeaderList& h, const std::string& name) {
    for (auto& kv : h) if (kv.first.size() == name.size() && lower(kv.first) == lower(name)) return &kv.second;
    return nullptr;
}
std::string header_get(const HeaderList& h, const std::string& name) {
    const std::string* v = header_find(h, name);
    return v ? decode_encoded_words(*v) : std::string();
}

std::string HeaderValue::param(const std::string& name) const {
    for (auto& kv : params) if (kv.first == name) return kv.second;
    return {};
}

HeaderValue parse_header_value(const std::string& raw) {
    HeaderValue hv;
    // strip comments (...) outside quotes
    std::string s;
    { int depth = 0; bool q = false;
      for (size_t i = 0; i < raw.size(); ++i) {
          char c = raw[i];
          if (q) { s += c; if (c == '\\' && i + 1 < raw.size()) s += raw[++i]; else if (c == '"') q = false; continue; }
          if (c == '"' && !depth) { q = true; s += c; continue; }
          if (c == '(') { ++depth; continue; }
          if (c == ')' && depth) { --depth; continue; }
          if (!depth) s += c;
      } }
    size_t i = s.find(';');
    hv.value = lower(trim(s.substr(0, i)));
    struct Piece { std::string base; int idx; bool ext; std::string val; };
    std::vector<Piece> pieces;
    while (i != std::string::npos && i < s.size()) {
        ++i;
        while (i < s.size() && (unsigned char)s[i] <= ' ') ++i;
        size_t eq = s.find('=', i);
        if (eq == std::string::npos) break;
        std::string name = lower(trim(s.substr(i, eq - i)));
        size_t j = eq + 1;
        while (j < s.size() && (unsigned char)s[j] <= ' ') ++j;
        std::string val;
        if (j < s.size() && s[j] == '"') {
            ++j;
            while (j < s.size() && s[j] != '"') { if (s[j] == '\\' && j + 1 < s.size()) ++j; val += s[j++]; }
            i = s.find(';', j);
        } else {
            i = s.find(';', j);
            val = trim(s.substr(j, i == std::string::npos ? std::string::npos : i - j));
        }
        Piece p{name, -1, false, val};
        size_t st = name.find('*');
        if (st != std::string::npos) {
            p.base = name.substr(0, st);
            std::string rest = name.substr(st + 1);   // "", "0", "0*", "1*"
            if (rest.empty()) { p.ext = true; p.idx = 0; }
            else {
                p.idx = std::atoi(rest.c_str());
                p.ext = rest.back() == '*';
            }
        }
        pieces.push_back(p);
    }
    // assemble RFC 2231 continuations
    std::vector<std::string> done;
    for (size_t k = 0; k < pieces.size(); ++k) {
        const Piece& p = pieces[k];
        if (p.idx < 0) {
            if (std::find(done.begin(), done.end(), p.base) != done.end()) continue;
            hv.params.emplace_back(p.base, decode_encoded_words(p.val));   // non-standard but common: 2047 in params
            continue;
        }
        if (std::find(done.begin(), done.end(), p.base) != done.end()) continue;
        std::vector<const Piece*> seg;
        for (auto& q : pieces) if (q.base == p.base && q.idx >= 0) seg.push_back(&q);
        std::sort(seg.begin(), seg.end(), [](const Piece* a, const Piece* b) { return a->idx < b->idx; });
        std::string cs, bytes;
        for (size_t z = 0; z < seg.size(); ++z) {
            std::string v = seg[z]->val;
            if (seg[z]->ext) {
                if (z == 0) {
                    size_t a = v.find('\''), b = a == std::string::npos ? a : v.find('\'', a + 1);
                    if (b != std::string::npos) { cs = v.substr(0, a); v = v.substr(b + 1); }
                }
                for (size_t w = 0; w < v.size(); ++w) {
                    if (v[w] == '%' && w + 2 < v.size() && hexv(v[w + 1]) >= 0 && hexv(v[w + 2]) >= 0) { bytes += (char)(hexv(v[w + 1]) << 4 | hexv(v[w + 2])); w += 2; }
                    else bytes += v[w];
                }
            } else bytes += v;
        }
        // drop any plain duplicate already added
        hv.params.erase(std::remove_if(hv.params.begin(), hv.params.end(), [&](auto& kv) { return kv.first == p.base; }), hv.params.end());
        hv.params.emplace_back(p.base, charset_to_utf8(bytes, cs));
        done.push_back(p.base);
    }
    return hv;
}

std::string MimePart::filename() const {
    std::string f = disposition.param("filename");
    if (f.empty()) f = type.param("name");
    // strip any path the sender supplied
    size_t k = f.find_last_of("/\\");
    if (k != std::string::npos) f = f.substr(k + 1);
    return f;
}

std::string MimePart::text_utf8() const { return charset_to_utf8(body, type.param("charset")); }

void parse_mime(const std::string& raw, MimePart& out, int depth) {
    size_t bo = parse_headers(raw, out.headers);
    const std::string* ct = header_find(out.headers, "Content-Type");
    out.type = parse_header_value(ct ? *ct : "text/plain; charset=us-ascii");
    if (out.type.value.empty() || out.type.value.find('/') == std::string::npos) out.type = parse_header_value("text/plain; charset=us-ascii");
    if (const std::string* cd = header_find(out.headers, "Content-Disposition")) out.disposition = parse_header_value(*cd);
    std::string body = raw.substr(std::min(bo, raw.size()));
    std::string cte = lower(trim(header_get(out.headers, "Content-Transfer-Encoding")));
    if (out.is_multipart() && depth < 40) {
        std::string bnd = out.type.param("boundary");
        if (bnd.empty()) { out.body = body; return; }
        std::string d = "--" + bnd;
        size_t pos = 0;
        // find first delimiter at line start
        auto find_delim = [&](size_t from) -> size_t {
            for (size_t p = body.find(d, from); p != std::string::npos; p = body.find(d, p + 1))
                if (p == 0 || body[p - 1] == '\n') return p;
            return std::string::npos;
        };
        pos = find_delim(0);
        if (pos == std::string::npos) { out.body = body; return; }
        while (pos != std::string::npos) {
            size_t ls = pos + d.size();
            if (body.compare(ls, 2, "--") == 0) break;   // close delimiter
            size_t le = body.find('\n', ls);
            if (le == std::string::npos) break;
            size_t next = find_delim(le + 1);
            size_t pe = next == std::string::npos ? body.size() : next;
            // the CRLF before the delimiter belongs to the delimiter
            size_t ce = pe;
            if (next != std::string::npos) { if (ce > le + 1 && body[ce - 1] == '\n') --ce; if (ce > le + 1 && body[ce - 1] == '\r') --ce; }
            MimePart ch;
            parse_mime(body.substr(le + 1, ce - (le + 1)), ch, depth + 1);
            if (out.type.value == "multipart/digest" && !header_find(ch.headers, "Content-Type")) ch.type = parse_header_value("message/rfc822");
            out.parts.push_back(std::move(ch));
            pos = next;
        }
        return;
    }
    if (cte == "base64") { auto v = base64_decode(body); out.body.assign(v.begin(), v.end()); }
    else if (cte == "quoted-printable") { auto v = qp_decode(body); out.body.assign(v.begin(), v.end()); }
    else out.body = body;
    if ((out.type.value == "message/rfc822" || out.type.value == "message/global") && depth < 40) {
        MimePart m;
        parse_mime(out.body, m, depth + 1);
        out.parts.push_back(std::move(m));
    }
}

} // namespace pcbp::io
