#pragma once
// ============================================================================
// m1_odbc.h — read-only live BOM fetch from the M1 ERP SQL Server by Job ID
// (ported from the author's TrayTagPrinter m1_live, adapted: no credentials or
// host names in source, odbc32 / crypt32 loaded at run time so the exe has
// no extra link dependencies). See specs/M1_BOM.md.
// ============================================================================
#include <string>
#include <vector>

namespace pcbp::platform {

struct M1Conn {
    std::wstring server, database;     // from Settings, never defaults
    bool windows_auth = false;         // Trusted_Connection=yes
    std::wstring user, password;       // SQL login (password only in memory)
    int login_timeout_s = 8, query_timeout_s = 30;
};

struct M1FetchResult {
    bool ok = false;
    std::string error;                 // user-facing, ODBC diagnostics appended
    std::vector<std::string> headers;  // result column names (M1 export headers)
    std::vector<std::vector<std::string>> rows;
};

// Connect (read-only, READ UNCOMMITTED), run the one parameterised SELECT, disconnect.
M1FetchResult m1_fetch_bom(const M1Conn& c, const std::string& job_id);

// The only data SQL this module sends (exposed for the select-only guard / docs).
const wchar_t* m1_bom_query();
bool m1_sql_is_select_only(const wchar_t* sql);

// DPAPI (CryptProtectData, current user) <-> hex string for the settings file.
bool dpapi_protect_hex(const std::wstring& plain, std::wstring& hex_out);
bool dpapi_unprotect_hex(const std::wstring& hex, std::wstring& plain_out);

} // namespace pcbp::platform
