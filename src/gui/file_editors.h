#pragma once
// ============================================================================
// file_editors.h — private interface between the Files workspace
// (sheet_editor.cpp: spreadsheet editor + workspace/tab host) and the text
// editor (text_editor.cpp). Public entry points are in gapp.h (files_*).
// ============================================================================
#include "../core/sheet.h"
#include "gapp.h"
#include <memory>
#include <string>
#include <vector>

namespace pcbp::gui {

struct FileDoc {
    std::wstring path;                  // empty = untitled / derived
    std::string name;                   // tab label
    virtual ~FileDoc() = default;
    virtual bool is_sheet() const = 0;
    virtual bool dirty() const = 0;
    virtual void draw(ImVec2 size) = 0;
    virtual bool save(bool save_as) = 0;            // may show dialogs (call outside a frame)
    virtual bool on_command(int id) { (void)id; return false; }
    virtual std::string status() const { return {}; }
    virtual void import_as(bool bom) { (void)bom; }   // send content to the BOM / centroid wizard (deferred)
    // document tab identity (top-level document tabs: icon + type label)
    virtual const char* kind() const { return is_sheet() ? "Spreadsheet" : "Text"; }
    virtual ui::Icon icon() const { return is_sheet() ? ui::Icon::Bom : ui::Icon::Notes; }
};

std::unique_ptr<FileDoc> make_sheet_doc(const std::wstring& path, std::string& err);
std::unique_ptr<FileDoc> make_sheet_doc_from(sheet::Workbook wb, const std::string& name, const std::wstring& src_path);
std::unique_ptr<FileDoc> make_text_doc(const std::wstring& path, std::string& err);
std::unique_ptr<FileDoc> make_text_doc_from(const std::string& utf8, const std::string& name);

// doc_viewer.cpp: rich text documents (.rtf / .docx / .txt as rich) and email (.eml / .msg)
std::unique_ptr<FileDoc> make_rich_doc(const std::wstring& path, std::string& err);
std::unique_ptr<FileDoc> make_mail_doc(const std::wstring& path, std::string& err);
// R20 Files tab preview modal (file_preview.cpp): read-only body for docx / rtf / html (rich view, *zoom set) and
// eml / msg (mail viewer, *zoom null), built from bytes in memory; nothing is written to disk. nullptr + err if unsupported.
std::unique_ptr<FileDoc> make_preview_doc(const std::vector<uint8_t>& data, const std::string& name, std::string& err, float** zoom);
// hex_viewer.cpp: safe binary inspector (hex + text strings), never interprets content
std::unique_ptr<FileDoc> make_hex_doc(const std::wstring& path, std::string& err);
std::unique_ptr<FileDoc> make_hex_doc_from(std::vector<uint8_t> data, const std::string& name);
// What a file is, by content (then extension): 1 sheet, 2 text, 3 rich document, 4 mail, 5 binary
int sniff_document_kind(const std::wstring& path);

void files_add(std::unique_ptr<FileDoc> d);        // adds + activates the Editors workspace
// Run the BOM (bom=true) or centroid import wizard on in-memory bytes (no temp file).
// start_line >= 0 preselects the first table line (header). Must run outside a frame (use defer).
void import_from_bytes(std::string bytes, int start_line, bool bom, const std::wstring& display_path);

// shared small widgets
bool fe_tool(const char* id, ui::Icon ic, const char* tip, bool enabled = true, bool checked = false);
std::string wide_to_u8(const std::wstring& w);
std::wstring file_name_of(const std::wstring& p);

} // namespace pcbp::gui
