// ============================================================================
// xml_lite.cpp — see xml_lite.h.
// ============================================================================
#include "xml_lite.h"
#include "io_util.h"

#include <cstdlib>

namespace pcbp::io {

std::string_view XmlNode::local() const {
    std::string_view n = name;
    auto p = n.find(':');
    return p == std::string_view::npos ? n : n.substr(p + 1);
}

const std::string* XmlNode::find_attr(std::string_view k) const {
    for (auto& a : attrs) if (a.first == k) return &a.second;
    for (auto& a : attrs) {
        std::string_view an = a.first;
        auto p = an.find(':');
        if (p != std::string_view::npos) an = an.substr(p + 1);
        if (ieq(an, k)) return &a.second;
    }
    return nullptr;
}

std::string XmlNode::attr(std::string_view k, const std::string& def) const {
    const std::string* s = find_attr(k);
    return s ? *s : def;
}

double XmlNode::attr_num(std::string_view k, double def) const {
    const std::string* s = find_attr(k);
    if (!s) return def;
    auto v = parse_num(*s);
    return v ? *v : def;
}

const XmlNode* XmlNode::child(std::string_view lname) const {
    for (auto& c : children) if (ieq(c.local(), lname)) return &c;
    return nullptr;
}

std::vector<const XmlNode*> XmlNode::kids(std::string_view lname) const {
    std::vector<const XmlNode*> v;
    for (auto& c : children) if (ieq(c.local(), lname)) v.push_back(&c);
    return v;
}

const XmlNode* XmlNode::find(std::string_view lname) const {
    for (auto& c : children) {
        if (ieq(c.local(), lname)) return &c;
        if (auto r = c.find(lname)) return r;
    }
    return nullptr;
}

void XmlNode::find_all(std::string_view lname, std::vector<const XmlNode*>& out) const {
    for (auto& c : children) {
        if (ieq(c.local(), lname)) out.push_back(&c);
        c.find_all(lname, out);
    }
}

namespace {

void put_utf8(std::string& o, unsigned long cp) {
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x110000) {
        o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F));
        o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F));
    }
}

std::string decode(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string_view::npos || e - i > 12) { o += '&'; continue; }
        std::string_view ent = s.substr(i + 1, e - i - 1);
        if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "amp") o += '&';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            std::string num(ent.substr(1));
            unsigned long cp = (!num.empty() && (num[0] == 'x' || num[0] == 'X')) ? std::strtoul(num.c_str() + 1, nullptr, 16)
                                                                                 : std::strtoul(num.c_str(), nullptr, 10);
            put_utf8(o, cp);
        } else { o += '&'; continue; }
        i = e;
    }
    return o;
}

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string err;
    int max_depth = 512;

    bool sp(char c) const { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
    void skip_ws() { while (i < s.size() && sp(s[i])) ++i; }
    bool starts(std::string_view t) const { return s.substr(i, t.size()) == t; }
    bool skip_past(std::string_view t) {
        size_t p = s.find(t, i);
        if (p == std::string_view::npos) { err = "unterminated construct"; return false; }
        i = p + t.size();
        return true;
    }
    bool skip_misc() {  // comments, PIs, doctype
        for (;;) {
            skip_ws();
            if (starts("<!--")) { if (!skip_past("-->")) return false; }
            else if (starts("<?")) { if (!skip_past("?>")) return false; }
            else if (starts("<!DOCTYPE") || starts("<!doctype")) {
                int depth = 0;
                for (; i < s.size(); ++i) {
                    if (s[i] == '[') ++depth;
                    else if (s[i] == ']') --depth;
                    else if (s[i] == '>' && depth <= 0) { ++i; break; }
                }
            } else return true;
        }
    }
    bool name(std::string& out) {
        size_t b = i;
        while (i < s.size() && !sp(s[i]) && s[i] != '>' && s[i] != '/' && s[i] != '=' && s[i] != '<') ++i;
        out.assign(s.substr(b, i - b));
        return !out.empty();
    }
    bool element(XmlNode& n, int depth) {
        if (depth > max_depth) { err = "nesting too deep"; return false; }
        if (i >= s.size() || s[i] != '<') { err = "expected '<'"; return false; }
        ++i;
        if (!name(n.name)) { err = "bad element name"; return false; }
        for (;;) {
            skip_ws();
            if (i >= s.size()) { err = "unexpected end in tag"; return false; }
            if (s[i] == '/') {
                if (i + 1 < s.size() && s[i + 1] == '>') { i += 2; return true; }
                err = "bad '/'"; return false;
            }
            if (s[i] == '>') { ++i; break; }
            std::string an;
            if (!name(an)) { err = "bad attribute in <" + n.name + ">"; return false; }
            skip_ws();
            if (i >= s.size() || s[i] != '=') { n.attrs.emplace_back(an, ""); continue; }
            ++i; skip_ws();
            if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) { err = "unquoted attribute"; return false; }
            const char q = s[i++];
            size_t e = s.find(q, i);
            if (e == std::string_view::npos) { err = "unterminated attribute"; return false; }
            n.attrs.emplace_back(an, decode(s.substr(i, e - i)));
            i = e + 1;
        }
        // content
        for (;;) {
            size_t lt = s.find('<', i);
            if (lt == std::string_view::npos) { err = "missing </" + n.name + ">"; return false; }
            if (lt > i) n.text += decode(s.substr(i, lt - i));
            i = lt;
            if (starts("</")) {
                i += 2;
                std::string en;
                name(en);
                skip_ws();
                if (i < s.size() && s[i] == '>') ++i;
                if (en != n.name) { err = "mismatched </" + en + "> for <" + n.name + ">"; return false; }
                return true;
            }
            if (starts("<!--")) { if (!skip_past("-->")) return false; continue; }
            if (starts("<?")) { if (!skip_past("?>")) return false; continue; }
            if (starts("<![CDATA[")) {
                size_t b = i + 9, e = s.find("]]>", b);
                if (e == std::string_view::npos) { err = "unterminated CDATA"; return false; }
                n.text.append(s.substr(b, e - b));
                i = e + 3;
                continue;
            }
            n.children.emplace_back();
            if (!element(n.children.back(), depth + 1)) return false;
        }
    }
};

} // namespace

bool xml_parse(std::string_view text, XmlNode& root, std::string& err, int max_depth) {
    Parser p;
    p.s = text;
    p.max_depth = max_depth;
    if (p.s.size() >= 3 && (unsigned char)p.s[0] == 0xEF && (unsigned char)p.s[1] == 0xBB && (unsigned char)p.s[2] == 0xBF) p.i = 3;
    root = XmlNode{};
    if (!p.skip_misc() || !p.element(root, 0)) {
        err = "XML: " + (p.err.empty() ? std::string("parse error") : p.err) + " (offset " + std::to_string(p.i) + ")";
        return false;
    }
    return true;
}

} // namespace pcbp::io
