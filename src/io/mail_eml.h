#pragma once
// ============================================================================
// mail_eml.h — email message model + RFC 5322 / MIME (.eml) reader and a
// simple .eml writer (used to save embedded Outlook messages). pcbp::io,
// portable. The Outlook .msg reader (mail_msg.h) fills the same model.
// ============================================================================
#include "mime.h"
#include <memory>
#include <string>
#include <vector>

namespace pcbp::io {

struct MailMessage;

struct MailAttachment {
    std::string name;                   // file name (sanitised: no path)
    std::string mime;                   // content type, lower case
    std::string cid;                    // Content-ID without <>
    std::vector<uint8_t> data;
    bool inline_part = false;           // referenced from the HTML body (cid:) / disposition inline
    std::shared_ptr<MailMessage> embedded;   // message/rfc822 or embedded .msg (data = .eml bytes)
};

struct MailMessage {
    std::string format;                 // "EML" / "MSG"
    std::string from, to, cc, bcc, reply_to, date, subject, message_id;
    std::string text;                   // text/plain body, UTF-8
    std::string html;                   // text/html body, UTF-8
    std::string rtf;                    // RTF body (MSG PR_RTF_COMPRESSED, decompressed) when present
    std::string raw_headers;            // header block as text (transport headers for MSG)
    std::vector<MailAttachment> atts;
    std::vector<std::string> warnings;
    int depth = 0;
};

bool is_eml_bytes(const uint8_t* p, size_t n);   // header-block sniff
bool eml_parse(const std::vector<uint8_t>& data, MailMessage& out, std::string& err, int depth = 0);
std::string eml_write(const MailMessage& m);     // multipart/mixed with base64 attachments
// Unique-ish safe file name for saving (strips reserved chars, ensures an extension from mime).
std::string safe_attachment_name(const MailAttachment& a, int index);

} // namespace pcbp::io
