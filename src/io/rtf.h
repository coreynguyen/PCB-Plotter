#pragma once
// ============================================================================
// rtf.h — RTF reader / writer for the rich-text model (pcbp::io, portable).
//   Reader: groups, \ansicpg / \'hh / \uN (+\ucN), font table charsets,
//   colour table, \b \i \ul \strike \fs \cf, \par \line \tab, \pard \plain,
//   HYPERLINK fields, \pict (png / jpeg blips -> images), basic tables
//   (\trowd \cell \row), \listtext bullets, ignorable destinations.
//   Encapsulated HTML (\fromhtml1, Outlook): rtf_extract_html().
//   Writer: paragraphs, B/I/U/strike, size, colour, lists, links, tables.
// No field code is ever executed (only HYPERLINK targets are recorded).
// ============================================================================
#include "richdoc.h"
#include <string>

namespace pcbp::io {

bool is_rtf_bytes(const uint8_t* p, size_t n);
bool rtf_to_rich(const std::string& rtf, RichDoc& out, std::string& err);
// true when the RTF carries \fromhtml (HTML encapsulated by Outlook); html receives UTF-8 HTML.
bool rtf_extract_html(const std::string& rtf, std::string& html);
// true when the RTF carries \fromtext (plain text encapsulated by Outlook)
bool rtf_is_fromtext(const std::string& rtf);
std::string rich_to_rtf(const RichDoc& d);

} // namespace pcbp::io
