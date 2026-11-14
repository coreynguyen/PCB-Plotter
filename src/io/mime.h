#pragma once
// ============================================================================
// mime.h — Internet message primitives (pcbp::io, portable, no deps).
//   * RFC 5322 header block parsing (unfolding), RFC 2047 encoded-words,
//     RFC 2045 Content-Type / Content-Disposition params incl. RFC 2231
//     (charset'lang'value, continuations name*0*=...)
//   * Content-Transfer-Encoding: base64, quoted-printable, 7bit/8bit/binary
//   * charset -> UTF-8: UTF-8, US-ASCII, ISO-8859-1..16, Windows-1250..1258,
//     KOI8-R/U, IBM437/850, UTF-16LE/BE; unknown -> UTF-8 if valid, else 1252
//   * MIME tree (multipart/*, message/rfc822 nested parts)
// ============================================================================
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pcbp::io {

std::string charset_to_utf8(const std::string& bytes, const std::string& charset);
bool utf8_valid(const std::string& s);
std::string utf16le_to_utf8(const uint8_t* p, size_t n_bytes);
std::string codepage_to_utf8(const std::string& bytes, int codepage);   // Windows code page number (1252, 65001, 28591, ...)

std::vector<uint8_t> base64_decode(const std::string& s);
std::string base64_encode(const uint8_t* p, size_t n, int line_len = 76);
std::vector<uint8_t> qp_decode(const std::string& s, bool header_mode = false);   // header_mode: '_' = space (RFC 2047 Q)

std::string decode_encoded_words(const std::string& raw);   // RFC 2047 header value -> UTF-8

using HeaderList = std::vector<std::pair<std::string, std::string>>;   // raw (undecoded) values, unfolded
// Splits a message into header block + body. Returns body offset.
size_t parse_headers(const std::string& msg, HeaderList& out);
const std::string* header_find(const HeaderList& h, const std::string& name);   // case-insensitive
std::string header_get(const HeaderList& h, const std::string& name);           // decoded (RFC 2047), "" if missing

struct HeaderValue {
    std::string value;                                          // lower-case token (e.g. "text/html", "attachment")
    std::vector<std::pair<std::string, std::string>> params;    // lower-case name -> UTF-8 value (2231 / 2047 decoded)
    std::string param(const std::string& name) const;
};
HeaderValue parse_header_value(const std::string& raw);

struct MimePart {
    HeaderList headers;
    HeaderValue type;           // content-type (default text/plain; charset=us-ascii)
    HeaderValue disposition;
    std::string body;           // transfer-decoded bytes (not charset-converted)
    std::vector<MimePart> parts;   // multipart children / message/rfc822 (one child = the message)
    bool is_multipart() const { return type.value.rfind("multipart/", 0) == 0; }
    std::string filename() const;  // disposition filename / type name, decoded
    std::string text_utf8() const; // body converted from its charset
};
// depth cap guards against hostile nesting
void parse_mime(const std::string& raw, MimePart& out, int depth = 0);

} // namespace pcbp::io
