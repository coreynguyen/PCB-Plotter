// ============================================================================
// mail_eml.cpp — see mail_eml.h.
// ============================================================================
#include "mail_eml.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace pcbp::io {

namespace {

std::string strip_angle(std::string s) {
    while (!s.empty() && (s.front() == '<' || s.front() == ' ')) s.erase(0, 1);
    while (!s.empty() && (s.back() == '>' || s.back() == ' ')) s.pop_back();
    return s;
}

struct Walker {
    MailMessage& m;
    int depth;
    bool have_text = false, have_html = false;

    void add_attachment(const MimePart& p) {
        MailAttachment a;
        a.name = p.filename();
        a.mime = p.type.value;
        a.cid = strip_angle(header_get(p.headers, "Content-ID"));
        a.inline_part = p.disposition.value == "inline";
        if ((p.type.value == "message/rfc822" || p.type.value == "message/global") && depth < 8) {
            a.data.assign(p.body.begin(), p.body.end());
            auto em = std::make_shared<MailMessage>();
            std::string e;
            if (eml_parse(a.data, *em, e, depth + 1)) {
                a.embedded = em;
                if (a.name.empty()) a.name = (em->subject.empty() ? std::string("message") : em->subject) + ".eml";
            }
        } else a.data.assign(p.body.begin(), p.body.end());
        m.atts.push_back(std::move(a));
    }

    void body_text(const MimePart& p, bool html) {
        std::string t = p.text_utf8();
        if (html) {
            if (have_html) m.html += "<hr>";
            m.html += t;
            have_html = true;
        } else {
            if (have_text && !m.text.empty() && m.text.back() != '\n') m.text += "\n";
            if (have_text) m.text += "\n";
            m.text += t;
            have_text = true;
        }
    }

    bool is_attachment(const MimePart& p) const {
        if (p.disposition.value == "attachment") return true;
        if (!p.filename().empty() && p.type.value.rfind("text/", 0) != 0) return true;
        if (!p.filename().empty() && p.disposition.value != "inline") return true;
        return false;
    }

    void walk(const MimePart& p, bool in_alt) {
        const std::string& t = p.type.value;
        if (p.is_multipart()) {
            if (p.parts.empty()) return;
            if (t == "multipart/alternative") {
                for (auto& c : p.parts) walk(c, true);
            } else if (t == "multipart/related") {
                // root = "start" param or first part
                std::string start = strip_angle(p.type.param("start"));
                size_t root = 0;
                if (!start.empty())
                    for (size_t i = 0; i < p.parts.size(); ++i)
                        if (strip_angle(header_get(p.parts[i].headers, "Content-ID")) == start) { root = i; break; }
                walk(p.parts[root], in_alt);
                for (size_t i = 0; i < p.parts.size(); ++i) if (i != root) {
                    if (p.parts[i].is_multipart()) { walk(p.parts[i], in_alt); continue; }
                    add_attachment(p.parts[i]);
                    m.atts.back().inline_part = true;
                }
            } else if (t == "multipart/signed") {
                walk(p.parts[0], in_alt);   // signature part is metadata
                for (size_t i = 1; i < p.parts.size(); ++i) add_attachment(p.parts[i]);
            } else {
                for (auto& c : p.parts) walk(c, false);
            }
            return;
        }
        if (t == "text/plain" && !is_attachment(p) && (in_alt || !have_text || p.disposition.value == "inline" || p.disposition.value.empty())) {
            if (!in_alt && have_text && !p.filename().empty()) { add_attachment(p); return; }
            body_text(p, false);
            return;
        }
        if (t == "text/html" && !is_attachment(p) && (in_alt || !have_html)) { body_text(p, true); return; }
        if (t == "text/enriched" && !is_attachment(p) && !have_text && in_alt) { body_text(p, false); return; }
        add_attachment(p);
    }
};

} // namespace

bool is_eml_bytes(const uint8_t* p, size_t n) {
    if (n < 8) return false;
    size_t lim = std::min<size_t>(n, 4096);
    std::string s((const char*)p, lim);
    if (s.find('\0') != std::string::npos) return false;
    HeaderList h;
    size_t bo = parse_headers(s, h);
    (void)bo;
    int hits = 0;
    for (const char* k : {"From", "To", "Subject", "Date", "Message-ID", "MIME-Version", "Received", "Return-Path", "Content-Type"})
        if (header_find(h, k)) ++hits;
    return hits >= 2;
}

bool eml_parse(const std::vector<uint8_t>& data, MailMessage& m, std::string& err, int depth) {
    if (data.empty()) { err = "empty file"; return false; }
    std::string raw(data.begin(), data.end());
    MimePart root;
    parse_mime(raw, root);
    if (root.headers.empty()) { err = "no message headers"; return false; }
    m.format = "EML";
    m.depth = depth;
    m.from = header_get(root.headers, "From");
    m.to = header_get(root.headers, "To");
    m.cc = header_get(root.headers, "Cc");
    m.bcc = header_get(root.headers, "Bcc");
    m.reply_to = header_get(root.headers, "Reply-To");
    m.date = header_get(root.headers, "Date");
    m.subject = header_get(root.headers, "Subject");
    m.message_id = header_get(root.headers, "Message-ID");
    for (auto& kv : root.headers) m.raw_headers += kv.first + ": " + decode_encoded_words(kv.second) + "\n";
    Walker w{m, depth};
    w.walk(root, false);
    // inline flag for anything referenced by cid: in the HTML
    for (auto& a : m.atts)
        if (!a.cid.empty() && m.html.find("cid:" + a.cid) != std::string::npos) a.inline_part = true;
    return true;
}

std::string safe_attachment_name(const MailAttachment& a, int index) {
    std::string n;
    for (unsigned char c : a.name) {
        if (c < 32 || std::strchr("<>:\"/\\|?*", c)) n += '_';
        else n += (char)c;
    }
    while (!n.empty() && (n.back() == '.' || n.back() == ' ')) n.pop_back();
    while (!n.empty() && n.front() == '.') n.erase(0, 1);
    if (n.empty()) n = "attachment" + std::to_string(index + 1);
    if (n.find('.') == std::string::npos) {
        static const std::pair<const char*, const char*> ext[] = {
            {"image/png", ".png"}, {"image/jpeg", ".jpg"}, {"image/gif", ".gif"}, {"image/bmp", ".bmp"}, {"text/plain", ".txt"},
            {"text/html", ".html"}, {"application/pdf", ".pdf"}, {"application/zip", ".zip"}, {"message/rfc822", ".eml"},
            {"text/csv", ".csv"}, {"application/rtf", ".rtf"}, {"text/rtf", ".rtf"},
            {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", ".xlsx"},
            {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", ".docx"}};
        for (auto& e : ext) if (a.mime == e.first) { n += e.second; break; }
        if (a.embedded && n.find('.') == std::string::npos) n += ".eml";
    }
    // reserved device names
    std::string stem = n.substr(0, n.find('.'));
    for (auto& c : stem) c = (char)std::toupper((unsigned char)c);
    for (const char* r : {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3"})
        if (stem == r) { n = "_" + n; break; }
    if (n.size() > 180) n = n.substr(0, 120) + n.substr(n.size() - 40);
    return n;
}

static std::string enc_header(const std::string& v) {
    bool ascii = true;
    for (unsigned char c : v) if (c >= 0x80 || c < 0x20) { ascii = false; break; }
    if (ascii) return v;
    return "=?UTF-8?B?" + base64_encode((const uint8_t*)v.data(), v.size(), 0) + "?=";
}

std::string eml_write(const MailMessage& m) {
    std::string o;
    auto h = [&](const char* k, const std::string& v) { if (!v.empty()) o += std::string(k) + ": " + enc_header(v) + "\r\n"; };
    h("From", m.from); h("To", m.to); h("Cc", m.cc); h("Date", m.date); h("Subject", m.subject);
    if (!m.message_id.empty()) o += "Message-ID: " + m.message_id + "\r\n";
    o += "MIME-Version: 1.0\r\n";
    const std::string B = "=_pcbp_mixed_boundary_7f3a";
    const std::string A = "=_pcbp_alt_boundary_7f3a";
    o += "Content-Type: multipart/mixed; boundary=\"" + B + "\"\r\n\r\n";
    o += "--" + B + "\r\nContent-Type: multipart/alternative; boundary=\"" + A + "\"\r\n\r\n";
    std::string text = m.text;
    o += "--" + A + "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n" + base64_encode((const uint8_t*)text.data(), text.size()) + "\r\n";
    if (!m.html.empty()) o += "--" + A + "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n" + base64_encode((const uint8_t*)m.html.data(), m.html.size()) + "\r\n";
    o += "--" + A + "--\r\n";
    int i = 0;
    for (auto& a : m.atts) {
        std::string nm = safe_attachment_name(a, i++);
        std::vector<uint8_t> data = a.data;
        std::string mime = a.mime.empty() ? "application/octet-stream" : a.mime;
        if (a.embedded && data.empty()) { std::string e = eml_write(*a.embedded); data.assign(e.begin(), e.end()); mime = "message/rfc822"; }
        if (a.embedded) mime = "message/rfc822";
        o += "--" + B + "\r\nContent-Type: " + mime + "; name=\"" + nm + "\"\r\n";
        if (!a.cid.empty()) o += "Content-ID: <" + a.cid + ">\r\n";
        if (mime == "message/rfc822") {
            o += "Content-Disposition: attachment; filename=\"" + nm + "\"\r\n\r\n";
            o.append(data.begin(), data.end());
            o += "\r\n";
        } else {
            o += "Content-Transfer-Encoding: base64\r\nContent-Disposition: " + std::string(a.inline_part ? "inline" : "attachment") + "; filename=\"" + nm + "\"\r\n\r\n";
            o += base64_encode(data.data(), data.size()) + "\r\n";
        }
    }
    o += "--" + B + "--\r\n";
    return o;
}

} // namespace pcbp::io
