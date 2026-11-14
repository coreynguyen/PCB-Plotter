// m1_odbc.cpp — read-only M1 BOM fetch over ODBC + DPAPI helpers (see m1_odbc.h)
#include "m1_odbc.h"
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <wincrypt.h>
#include <cwchar>

namespace pcbp::platform {

namespace {

const wchar_t* kBomQuery =
    L"SELECT "
        L"jm.jmmJobID, jm.jmmJobAssemblyID, jm.jmmJobMaterialID, "
        L"jm.jmmPartRevisionID, jm.jmmPartWarehouseLocationID, jm.jmmPartBinID, "
        L"jm.jmmPartID, p.impPartClassID, "
        L"jm.jmmPartShortDescription, jm.ujmmPCRefText, "
        L"jm.jmmQuantityPerAssembly, jm.jmmUnitOfMeasure, "
        L"p.uimpMfgPartID, p.uimpManufacturer, "
        L"jm.jmmCreatedBy, jm.jmmCreatedDate "
    L"FROM dbo.JobMaterials jm "
    L"LEFT JOIN dbo.Parts p ON p.impPartID = jm.jmmPartID "
    L"WHERE jm.jmmJobID = ? "
    L"ORDER BY jm.jmmJobAssemblyID, jm.jmmJobMaterialID";
const wchar_t* kSetIsolation = L"SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED";

std::string u8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// odbc32.dll entry points, resolved at run time (no import-library dependency)
struct Odbc {
    HMODULE h = nullptr;
    decltype(&SQLAllocHandle) AllocHandle = nullptr;
    decltype(&SQLFreeHandle) FreeHandle = nullptr;
    decltype(&SQLSetEnvAttr) SetEnvAttr = nullptr;
    decltype(&SQLSetConnectAttrW) SetConnectAttrW = nullptr;
    decltype(&SQLDriverConnectW) DriverConnectW = nullptr;
    decltype(&SQLDisconnect) Disconnect = nullptr;
    decltype(&SQLExecDirectW) ExecDirectW = nullptr;
    decltype(&SQLSetStmtAttrW) SetStmtAttrW = nullptr;
    decltype(&SQLPrepareW) PrepareW = nullptr;
    decltype(&SQLBindParameter) BindParameter = nullptr;
    decltype(&SQLExecute) Execute = nullptr;
    decltype(&SQLNumResultCols) NumResultCols = nullptr;
    decltype(&SQLDescribeColW) DescribeColW = nullptr;
    decltype(&SQLFetch) Fetch = nullptr;
    decltype(&SQLGetData) GetData = nullptr;
    decltype(&SQLGetDiagRecW) GetDiagRecW = nullptr;
    bool load() {
        if (h) return true;
        h = LoadLibraryW(L"odbc32.dll");
        if (!h) return false;
        auto P = [&](auto& f, const char* n) { f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(reinterpret_cast<void*>(GetProcAddress(h, n))); return f != nullptr; };
        bool ok = P(AllocHandle, "SQLAllocHandle") && P(FreeHandle, "SQLFreeHandle") && P(SetEnvAttr, "SQLSetEnvAttr") &&
                  P(SetConnectAttrW, "SQLSetConnectAttrW") && P(DriverConnectW, "SQLDriverConnectW") && P(Disconnect, "SQLDisconnect") &&
                  P(ExecDirectW, "SQLExecDirectW") && P(SetStmtAttrW, "SQLSetStmtAttrW") && P(PrepareW, "SQLPrepareW") &&
                  P(BindParameter, "SQLBindParameter") && P(Execute, "SQLExecute") && P(NumResultCols, "SQLNumResultCols") &&
                  P(DescribeColW, "SQLDescribeColW") && P(Fetch, "SQLFetch") && P(GetData, "SQLGetData") && P(GetDiagRecW, "SQLGetDiagRecW");
        if (!ok) { FreeLibrary(h); h = nullptr; }
        return ok;
    }
};
Odbc& odbc() { static Odbc o; return o; }

bool OK(SQLRETURN r) { return r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO; }

std::string diag(SQLSMALLINT type, SQLHANDLE h, std::string* state0 = nullptr) {
    std::string out;
    for (SQLSMALLINT i = 1; i <= 4; ++i) {
        SQLWCHAR st[6] = {}, msg[512] = {};
        SQLINTEGER native = 0;
        SQLSMALLINT len = 0;
        if (!OK(odbc().GetDiagRecW(type, h, i, st, &native, msg, 512, &len))) break;
        if (i == 1 && state0) *state0 = u8(std::wstring((wchar_t*)st));
        if (!out.empty()) out += " | ";
        out += u8(std::wstring((wchar_t*)st)) + ": " + u8(std::wstring((wchar_t*)msg));
    }
    return out;
}

// Connection-string value: braces protect ';' / '}' inside values.
std::wstring cs_val(const std::wstring& v) {
    std::wstring o = L"{";
    for (wchar_t c : v) { o += c; if (c == L'}') o += L'}'; }
    return o + L"}";
}

} // namespace

const wchar_t* m1_bom_query() { return kBomQuery; }

bool m1_sql_is_select_only(const wchar_t* sql) {
    if (!sql) return false;
    std::wstring up;
    for (const wchar_t* p = sql; *p; ++p) up += (*p >= L'a' && *p <= L'z') ? (wchar_t)(*p - 32) : *p;
    if (up.rfind(L"SELECT ", 0) != 0 && up.rfind(L"SET ", 0) != 0) return false;
    auto word = [](wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_'; };
    for (const wchar_t* b : {L"INSERT", L"UPDATE", L"DELETE", L"DROP", L"TRUNCATE", L"MERGE", L"ALTER", L"EXEC", L"EXECUTE",
                             L"CREATE", L"GRANT", L"REVOKE"}) {
        const size_t n = wcslen(b);
        for (size_t pos = up.find(b); pos != std::wstring::npos; pos = up.find(b, pos + n))
            if ((pos == 0 || !word(up[pos - 1])) && (pos + n >= up.size() || !word(up[pos + n]))) return false;
    }
    for (const wchar_t* b : {L"SP_", L"XP_"})
        for (size_t pos = up.find(b); pos != std::wstring::npos; pos = up.find(b, pos + 3))
            if (pos == 0 || !word(up[pos - 1])) return false;
    return true;
}

M1FetchResult m1_fetch_bom(const M1Conn& c, const std::string& job_id) {
    M1FetchResult res;
    if (c.server.empty() || c.database.empty()) { res.error = "M1 connection is not configured (server and database are required)."; return res; }
    if (job_id.empty()) { res.error = "Enter a Job ID."; return res; }
    if (!c.windows_auth && c.user.empty()) { res.error = "SQL Server login selected but no user name is set."; return res; }
    if (!m1_sql_is_select_only(kBomQuery) || !m1_sql_is_select_only(kSetIsolation)) { res.error = "internal: query guard failed"; return res; }
    Odbc& O = odbc();
    if (!O.load()) { res.error = "ODBC (odbc32.dll) is not available on this computer."; return res; }

    SQLHENV env = nullptr; SQLHDBC dbc = nullptr; SQLHSTMT st = nullptr;
    bool connected = false;
    auto cleanup = [&]() {
        if (st) O.FreeHandle(SQL_HANDLE_STMT, st);
        if (dbc) { if (connected) O.Disconnect(dbc); O.FreeHandle(SQL_HANDLE_DBC, dbc); }
        if (env) O.FreeHandle(SQL_HANDLE_ENV, env);
    };
    if (!OK(O.AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) { res.error = "ODBC: could not allocate an environment."; return res; }
    O.SetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
    if (!OK(O.AllocHandle(SQL_HANDLE_DBC, env, &dbc))) { res.error = "ODBC: could not allocate a connection."; cleanup(); return res; }
    O.SetConnectAttrW(dbc, SQL_ATTR_ACCESS_MODE, (SQLPOINTER)SQL_MODE_READ_ONLY, 0);
    O.SetConnectAttrW(dbc, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)(uintptr_t)c.login_timeout_s, 0);

    // Driver preference as in TrayTagPrinter; the legacy "SQL Server" driver ships with Windows.
    struct Drv { const wchar_t* name; bool tls; };
    static const Drv drivers[] = {{L"ODBC Driver 18 for SQL Server", true}, {L"ODBC Driver 17 for SQL Server", true},
                                  {L"SQL Server Native Client 11.0", true}, {L"SQL Server", false}};
    std::string errs;
    bool network = false;
    for (const Drv& d : drivers) {
        std::wstring cs = L"DRIVER={" + std::wstring(d.name) + L"};SERVER=" + cs_val(c.server) + L";DATABASE=" + cs_val(c.database) + L";";
        if (c.windows_auth) cs += L"Trusted_Connection=yes;";
        else cs += L"UID=" + cs_val(c.user) + L";PWD=" + cs_val(c.password) + L";";
        cs += L"APP=PCB Plotter;";
        if (d.tls) cs += L"Encrypt=yes;TrustServerCertificate=yes;";
        SQLSMALLINT outlen = 0;
        SQLRETURN r = O.DriverConnectW(dbc, nullptr, (SQLWCHAR*)cs.c_str(), SQL_NTS, nullptr, 0, &outlen, SQL_DRIVER_NOPROMPT);
        SecureZeroMemory(cs.data(), cs.size() * sizeof(wchar_t));
        if (OK(r)) { connected = true; break; }
        std::string state;
        std::string dg = diag(SQL_HANDLE_DBC, dbc, &state);
        if (state == "IM002") continue;   // driver not installed: try the next one
        if (state.rfind("08", 0) == 0 || state == "HYT00") network = true;
        errs += "[" + u8(d.name) + "] " + dg + "\n";
        if (state == "28000") break;      // login failed: other drivers will not help
    }
    if (!connected) {
        if (errs.empty()) res.error = "No SQL Server ODBC driver is installed.";
        else res.error = std::string(network ? "Could not reach the M1 server (offline, wrong server name, or blocked).\n\n"
                                             : "Could not connect to the M1 database.\n\n") + errs;
        cleanup();
        return res;
    }
    {
        SQLHSTMT iso = nullptr;
        if (OK(O.AllocHandle(SQL_HANDLE_STMT, dbc, &iso))) { O.ExecDirectW(iso, (SQLWCHAR*)kSetIsolation, SQL_NTS); O.FreeHandle(SQL_HANDLE_STMT, iso); }
    }
    if (!OK(O.AllocHandle(SQL_HANDLE_STMT, dbc, &st))) { res.error = "ODBC: could not allocate a statement."; cleanup(); return res; }
    O.SetStmtAttrW(st, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)(uintptr_t)c.query_timeout_s, 0);
    if (!OK(O.PrepareW(st, (SQLWCHAR*)kBomQuery, SQL_NTS))) { res.error = "M1 query rejected: " + diag(SQL_HANDLE_STMT, st); cleanup(); return res; }
    std::wstring wjob(job_id.begin(), job_id.end());   // validated ASCII
    SQLLEN cb = SQL_NTS;
    if (!OK(O.BindParameter(st, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR, 40, 0, (SQLPOINTER)wjob.c_str(), 0, &cb))) {
        res.error = "ODBC bind: " + diag(SQL_HANDLE_STMT, st); cleanup(); return res;
    }
    if (!OK(O.Execute(st))) { res.error = "M1 query failed: " + diag(SQL_HANDLE_STMT, st); cleanup(); return res; }
    SQLSMALLINT ncols = 0;
    O.NumResultCols(st, &ncols);
    for (SQLSMALLINT i = 1; i <= ncols; ++i) {
        SQLWCHAR nm[256] = {}; SQLSMALLINT nl = 0;
        O.DescribeColW(st, i, nm, 256, &nl, nullptr, nullptr, nullptr, nullptr);
        res.headers.push_back(u8(std::wstring((wchar_t*)nm, (size_t)(nl > 0 ? nl : 0))));
    }
    for (;;) {
        SQLRETURN fr = O.Fetch(st);
        if (fr == SQL_NO_DATA) break;
        if (!OK(fr)) { res.error = "M1 fetch failed: " + diag(SQL_HANDLE_STMT, st); cleanup(); return res; }
        std::vector<std::string> row;
        for (SQLSMALLINT i = 1; i <= ncols; ++i) {
            std::wstring v;
            wchar_t buf[1024];
            for (;;) {
                SQLLEN ind = 0;
                SQLRETURN gr = O.GetData(st, i, SQL_C_WCHAR, buf, sizeof buf, &ind);
                if (gr == SQL_NO_DATA || ind == SQL_NULL_DATA || !OK(gr)) break;
                v += buf;
                if (gr == SQL_SUCCESS) break;   // SUCCESS_WITH_INFO = truncated, more to read
            }
            row.push_back(u8(v));
        }
        res.rows.push_back(std::move(row));
        if (res.rows.size() > 100000) break;
    }
    cleanup();
    if (res.rows.empty()) { res.error = "Job " + job_id + " was not found in M1 (no BOM rows)."; return res; }
    res.ok = true;
    return res;
}

// ---- DPAPI (crypt32 loaded at run time) ----
namespace {
using ProtectFn = BOOL(WINAPI*)(DATA_BLOB*, LPCWSTR, DATA_BLOB*, PVOID, PVOID, DWORD, DATA_BLOB*);
using UnprotectFn = BOOL(WINAPI*)(DATA_BLOB*, LPWSTR*, DATA_BLOB*, PVOID, PVOID, DWORD, DATA_BLOB*);
HMODULE crypt32() { static HMODULE h = LoadLibraryW(L"crypt32.dll"); return h; }
}

bool dpapi_protect_hex(const std::wstring& plain, std::wstring& hex) {
    hex.clear();
    if (plain.empty()) return true;
    auto fn = crypt32() ? (ProtectFn)(void*)GetProcAddress(crypt32(), "CryptProtectData") : nullptr;
    if (!fn) return false;
    DATA_BLOB in{(DWORD)(plain.size() * sizeof(wchar_t)), (BYTE*)plain.data()}, out{};
    if (!fn(&in, L"PCB Plotter M1", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    static const wchar_t* hx = L"0123456789ABCDEF";
    for (DWORD i = 0; i < out.cbData; ++i) { hex += hx[out.pbData[i] >> 4]; hex += hx[out.pbData[i] & 15]; }
    LocalFree(out.pbData);
    return true;
}

bool dpapi_unprotect_hex(const std::wstring& hex, std::wstring& plain) {
    plain.clear();
    if (hex.empty()) return true;
    if (hex.size() % 2) return false;
    auto fn = crypt32() ? (UnprotectFn)(void*)GetProcAddress(crypt32(), "CryptUnprotectData") : nullptr;
    if (!fn) return false;
    std::vector<BYTE> b(hex.size() / 2);
    auto nib = [](wchar_t c) { return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'A' && c <= L'F' ? c - L'A' + 10 : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1; };
    for (size_t i = 0; i < b.size(); ++i) {
        int h = nib(hex[2 * i]), l = nib(hex[2 * i + 1]);
        if (h < 0 || l < 0) return false;
        b[i] = (BYTE)(h * 16 + l);
    }
    DATA_BLOB in{(DWORD)b.size(), b.data()}, out{};
    if (!fn(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    plain.assign((wchar_t*)out.pbData, out.cbData / sizeof(wchar_t));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

} // namespace pcbp::platform
