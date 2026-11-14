#pragma once
// ============================================================================
// xlsx_xml.h — tiny streaming (pull) XML reader + writer helpers shared by the
// spreadsheet readers/writers (xlsx.cpp, ods.cpp). Internal to src/io.
// Pull parsing keeps memory flat for 1M-cell sheets (no DOM).
// ============================================================================
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace pcbp::io::xmlp {

inline void utf8_put(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
}

inline void decode_into(std::string& o, std::string_view s) {
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '&') { o += c; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 12) { o += c; continue; }
        std::string_view e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") o += '&'; else if (e == "lt") o += '<'; else if (e == "gt") o += '>';
        else if (e == "quot") o += '"'; else if (e == "apos") o += '\'';
        else if (!e.empty() && e[0] == '#') {
            uint32_t cp = (e.size() > 1 && (e[1] == 'x' || e[1] == 'X')) ? (uint32_t)strtoul(std::string(e.substr(2)).c_str(), nullptr, 16)
                                                                         : (uint32_t)strtoul(std::string(e.substr(1)).c_str(), nullptr, 10);
            utf8_put(o, cp);
        } else { o += '&'; o.append(e); o += ';'; }
        i = semi;
    }
}

struct Attr { std::string_view name; std::string value; };

class Pull {
public:
    enum Ev { Start, End, Text, Eof };
    explicit Pull(std::string_view s) : s_(s) {
        // skip BOM
        if (s_.size() >= 3 && (uint8_t)s_[0] == 0xEF && (uint8_t)s_[1] == 0xBB && (uint8_t)s_[2] == 0xBF) p_ = 3;
    }
    // next event; for Start, name()/attrs() are valid; empty elements yield Start then End
    Ev next() {
        if (pending_end_) { pending_end_ = false; return End; }
        text_.clear();
        while (p_ < s_.size()) {
            if (s_[p_] != '<') {
                size_t lt = s_.find('<', p_);
                if (lt == std::string_view::npos) lt = s_.size();
                decode_into(text_, s_.substr(p_, lt - p_));
                p_ = lt;
                return Text;
            }
            if (s_.compare(p_, 4, "<!--") == 0) { size_t e = s_.find("-->", p_); p_ = e == std::string_view::npos ? s_.size() : e + 3; continue; }
            if (s_.compare(p_, 9, "<![CDATA[") == 0) {
                size_t e = s_.find("]]>", p_);
                if (e == std::string_view::npos) e = s_.size();
                text_.assign(s_.substr(p_ + 9, e - p_ - 9));
                p_ = e + 3;
                return Text;
            }
            if (s_.compare(p_, 2, "<?") == 0) { size_t e = s_.find("?>", p_); p_ = e == std::string_view::npos ? s_.size() : e + 2; continue; }
            if (s_.compare(p_, 2, "<!") == 0) { size_t e = s_.find('>', p_); p_ = e == std::string_view::npos ? s_.size() : e + 1; continue; }
            if (s_.compare(p_, 2, "</") == 0) {
                size_t e = s_.find('>', p_);
                if (e == std::string_view::npos) e = s_.size();
                name_ = trim(s_.substr(p_ + 2, e - p_ - 2));
                p_ = e + 1;
                return End;
            }
            // start tag
            size_t i = p_ + 1;
            size_t n0 = i;
            while (i < s_.size() && !isspace_((unsigned char)s_[i]) && s_[i] != '>' && s_[i] != '/') ++i;
            name_ = s_.substr(n0, i - n0);
            attrs_.clear();
            bool selfclose = false;
            while (i < s_.size()) {
                while (i < s_.size() && isspace_((unsigned char)s_[i])) ++i;
                if (i >= s_.size()) break;
                if (s_[i] == '>') { ++i; break; }
                if (s_[i] == '/') { selfclose = true; ++i; continue; }
                size_t a0 = i;
                while (i < s_.size() && s_[i] != '=' && !isspace_((unsigned char)s_[i]) && s_[i] != '>' && s_[i] != '/') ++i;
                std::string_view an = s_.substr(a0, i - a0);
                while (i < s_.size() && isspace_((unsigned char)s_[i])) ++i;
                Attr a{an, {}};
                if (i < s_.size() && s_[i] == '=') {
                    ++i;
                    while (i < s_.size() && isspace_((unsigned char)s_[i])) ++i;
                    if (i < s_.size() && (s_[i] == '"' || s_[i] == '\'')) {
                        char q = s_[i++];
                        size_t e = s_.find(q, i);
                        if (e == std::string_view::npos) e = s_.size();
                        decode_into(a.value, s_.substr(i, e - i));
                        i = e + 1;
                    }
                }
                attrs_.push_back(std::move(a));
            }
            p_ = i;
            pending_end_ = selfclose;
            return Start;
        }
        return Eof;
    }
    std::string_view name() const { return name_; }
    std::string_view local() const { size_t k = name_.find(':'); return k == std::string_view::npos ? name_ : name_.substr(k + 1); }
    const std::vector<Attr>& attrs() const { return attrs_; }
    const std::string* attr(std::string_view n) const {
        for (auto& a : attrs_) if (a.name == n) return &a.value;
        for (auto& a : attrs_) { size_t k = a.name.find(':'); if (k != std::string_view::npos && a.name.substr(k + 1) == n) return &a.value; }
        return nullptr;
    }
    std::string attr_or(std::string_view n, const std::string& def = {}) const { const std::string* v = attr(n); return v ? *v : def; }
    double attr_num(std::string_view n, double def = 0) const { const std::string* v = attr(n); return v && !v->empty() ? atof(v->c_str()) : def; }
    bool attr_bool(std::string_view n, bool def = false) const { const std::string* v = attr(n); if (!v) return def; return *v == "1" || *v == "true"; }
    const std::string& text() const { return text_; }
    // Collect all character data up to the end of the current element (call right after Start).
    std::string read_text() {
        if (pending_end_) { pending_end_ = false; return {}; }
        std::string o;
        int depth = 1;
        while (depth > 0) {
            Ev e = next();
            if (e == Eof) break;
            if (e == Start) ++depth;
            else if (e == End) --depth;
            else if (e == Text) o += text_;
        }
        return o;
    }
    void skip_element() { read_text(); }
    bool empty_element() const { return pending_end_; }

private:
    static bool isspace_(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
    static std::string_view trim(std::string_view v) { while (!v.empty() && isspace_((unsigned char)v.back())) v.remove_suffix(1); return v; }
    std::string_view s_;
    size_t p_ = 0;
    std::string_view name_;
    std::vector<Attr> attrs_;
    std::string text_;
    bool pending_end_ = false;
};

inline void esc(std::string& o, std::string_view s, bool attr = false) {
    for (unsigned char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': if (attr) o += "&quot;"; else o += '"'; break;
        case '\n': if (attr) o += "&#10;"; else o += '\n'; break;
        case '\t': if (attr) o += "&#9;"; else o += '\t'; break;
        case '\r': o += "&#13;"; break;
        default: if (c < 0x20) break; o += (char)c;   // illegal in XML 1.0
        }
    }
}
inline std::string esc(std::string_view s, bool attr = true) { std::string o; esc(o, s, attr); return o; }

} // namespace pcbp::io::xmlp
