#pragma once
// ============================================================================
// mail_msg.h — Outlook .msg reader (pcbp::io, portable, no Outlook / MAPI).
//   [MS-CFB] compound file (v3/v4 sectors, DIFAT chains, mini stream),
//   [MS-OXMSG] property streams (__substg1.0_XXXXYYYY + __properties_version1.0),
//   recipients, attachments (by value, embedded message -> recursive),
//   [MS-OXRTFCP] LZFu compressed RTF, [MS-OXRTFEX] HTML-in-RTF extraction.
// ============================================================================
#include "mail_eml.h"
#include <string>
#include <vector>

namespace pcbp::io {

// --- compound file (also usable for other OLE2 containers) -------------------
struct CfbEntry {
    std::string name;           // UTF-8
    int type = 0;               // 1 storage, 2 stream, 5 root
    int left = -1, right = -1, child = -1;
    uint32_t start = 0;
    uint64_t size = 0;
};
class Cfb {
public:
    bool open(const std::vector<uint8_t>& data, std::string& err);
    const std::vector<CfbEntry>& entries() const { return dir_; }
    std::vector<int> children(int storage) const;          // direct children of a storage entry
    int find_child(int storage, const std::string& name) const;   // case-insensitive
    bool read_stream(int entry, std::vector<uint8_t>& out) const;
private:
    bool chain(uint32_t start, const std::vector<uint32_t>& fat, std::vector<uint32_t>& out) const;
    const std::vector<uint8_t>* d_ = nullptr;
    uint32_t ssz_ = 512, mssz_ = 64, cutoff_ = 4096;
    std::vector<uint32_t> fat_, minifat_;
    std::vector<uint8_t> ministream_;
    std::vector<CfbEntry> dir_;
};

bool is_cfb_bytes(const uint8_t* p, size_t n);
bool lzfu_decompress(const uint8_t* p, size_t n, std::string& out, std::string& err);
bool msg_parse(const std::vector<uint8_t>& data, MailMessage& out, std::string& err);

} // namespace pcbp::io
