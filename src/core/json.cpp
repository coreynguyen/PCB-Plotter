// ============================================================================
// json.cpp — minimal JSON DOM implementation.
// ============================================================================
#include "json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace pcbp::json {

static const Value kNull;

const Value& Value::operator[](size_t i) const {
    if (t_ != Type::Array || i >= a_.size()) return kNull;
    return a_[i];
}

Value& Value::push(Value v) {
    if (t_ != Type::Array) { t_ = Type::Array; a_.clear(); }
    a_.push_back(std::move(v));
    return a_.back();
}

const Value& Value::operator[](const std::string& k) const {
    if (t_ != Type::Object) return kNull;
    for (auto& kv : o_) if (kv.first == k) return kv.second;
    return kNull;
}

Value& Value::set(const std::string& k, Value v) {
    if (t_ != Type::Object) { t_ = Type::Object; o_.clear(); }
    for (auto& kv : o_) if (kv.first == k) { kv.second = std::move(v); return kv.second; }
    o_.emplace_back(k, std::move(v));
    return o_.back().second;
}

bool Value::has(const std::string& k) const {
    if (t_ != Type::Object) return false;
    for (auto& kv : o_) if (kv.first == k) return true;
    return false;
}

std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += (char)c;
        }
    }
    return o;
}

static void put_num(std::string& out, double n) {
    if (!std::isfinite(n)) { out += "0"; return; }
    if (n == std::floor(n) && std::fabs(n) < 9.0e15) {
        char b[32]; std::snprintf(b, sizeof b, "%lld", (long long)n); out += b; return;
    }
    char b[40]; std::snprintf(b, sizeof b, "%.10g", n);
    // ensure round-trip precision for coordinates
    if (std::strtod(b, nullptr) != n) std::snprintf(b, sizeof b, "%.17g", n);
    out += b;
}

void Value::dump_to(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) {
        if (indent < 0) return;
        out += '\n';
        out.append((size_t)(indent * d), ' ');
    };
    switch (t_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Number: put_num(out, n_); break;
    case Type::String: out += '"'; out += escape(s_); out += '"'; break;
    case Type::Array: {
        out += '[';
        // compact arrays of scalars on one line
        bool scalar = true;
        for (auto& v : a_) if (v.t_ == Type::Array || v.t_ == Type::Object) { scalar = false; break; }
        for (size_t i = 0; i < a_.size(); ++i) {
            if (i) out += ',';
            if (!scalar) nl(depth + 1);
            a_[i].dump_to(out, indent, depth + 1);
        }
        if (!scalar && !a_.empty()) nl(depth);
        out += ']';
        break;
    }
    case Type::Object: {
        out += '{';
        for (size_t i = 0; i < o_.size(); ++i) {
            if (i) out += ',';
            nl(depth + 1);
            out += '"'; out += escape(o_[i].first); out += "\":";
            if (indent >= 0) out += ' ';
            o_[i].second.dump_to(out, indent, depth + 1);
        }
        if (!o_.empty()) nl(depth);
        out += '}';
        break;
    }
    }
}

std::string Value::dump(int indent) const {
    std::string s;
    dump_to(s, indent, 0);
    return s;
}

namespace {
struct Parser {
    const char* p;
    const char* e;
    std::string err;
    int depth = 0;

    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }

    static void utf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(uint32_t& v) {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool str(std::string& o) {
        ++p; // opening quote
        while (p < e && *p != '"') {
            char c = *p++;
            if (c != '\\') { o += c; continue; }
            if (p >= e) return fail("bad escape");
            char x = *p++;
            switch (x) {
            case '"': o += '"'; break; case '\\': o += '\\'; break; case '/': o += '/'; break;
            case 'b': o += '\b'; break; case 'f': o += '\f'; break; case 'n': o += '\n'; break;
            case 'r': o += '\r'; break; case 't': o += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return fail("bad \\u escape");
                if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2; uint32_t lo;
                    if (!hex4(lo)) return fail("bad surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(o, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        if (p >= e) return fail("unterminated string");
        ++p;
        return true;
    }
    bool val(Value& v) {
        if (++depth > 512) return fail("nesting too deep");
        ws();
        if (p >= e) return fail("unexpected end");
        bool ok = true;
        if (*p == '{') {
            ++p; v = Value::object(); ws();
            if (p < e && *p == '}') { ++p; }
            else for (;;) {
                ws();
                if (p >= e || *p != '"') { ok = fail("expected key"); break; }
                std::string k;
                if (!str(k)) { ok = false; break; }
                ws();
                if (p >= e || *p != ':') { ok = fail("expected ':'"); break; }
                ++p;
                Value x;
                if (!val(x)) { ok = false; break; }
                v.set(k, std::move(x));
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; break; }
                ok = fail("expected ',' or '}'"); break;
            }
        } else if (*p == '[') {
            ++p; v = Value::array(); ws();
            if (p < e && *p == ']') { ++p; }
            else for (;;) {
                Value x;
                if (!val(x)) { ok = false; break; }
                v.push(std::move(x));
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; break; }
                ok = fail("expected ',' or ']'"); break;
            }
        } else if (*p == '"') {
            std::string s;
            ok = str(s);
            v = Value(std::move(s));
        } else if (e - p >= 4 && !std::strncmp(p, "true", 4)) { p += 4; v = Value(true); }
        else if (e - p >= 5 && !std::strncmp(p, "false", 5)) { p += 5; v = Value(false); }
        else if (e - p >= 4 && !std::strncmp(p, "null", 4)) { p += 4; v = Value(); }
        else {
            char buf[64]; size_t n = 0;
            while (p < e && n < 63 && (std::strchr("+-0123456789.eE", *p))) buf[n++] = *p++;
            buf[n] = 0;
            if (!n) ok = fail("unexpected character");
            else v = Value(std::strtod(buf, nullptr));
        }
        --depth;
        return ok;
    }
};
} // namespace

bool Value::parse(const std::string& text, Value& out, std::string& err) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) ps.p += 3;
    if (!ps.val(out)) { err = ps.err + " at offset " + std::to_string(ps.p - text.data()); return false; }
    ps.ws();
    if (ps.p != ps.e) { err = "trailing data at offset " + std::to_string(ps.p - text.data()); return false; }
    return true;
}

} // namespace pcbp::json
