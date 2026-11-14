// m1_import.cpp — BOM > "Import from M1 by Job ID..." (R14d, specs/M1_BOM.md).
// Fetches the job's BOM rows from the M1 ERP database (read-only ODBC), turns
// them into the same tab-separated table an M1 export would give, and opens the
// regular BOM wizard on it; the built-in "M1 BOM" header profile is matched
// automatically, so the wizard starts on mapped columns.
#include "../app/app.h"
#include "../core/fileio.h"
#include "../io/m1_bom.h"
#include "../platform/m1_odbc.h"
#include "../ui/form.h"
#include "gapp.h"
#include <cstring>
#include <memory>

namespace pcbp::app {
using namespace pcbp::ui;

namespace {
struct M1Dlg {
    char pass[256] = {};
    bool remember = true;
};
}

bool dlg_import_bom_m1(HWND owner, Doc& d) {
    if (gui::in_frame()) {   // modal dialogs only between frames
        DocPtr dp;
        for (auto& p : open_docs()) if (p.get() == &d) dp = p;
        gui::defer([owner, dp]() { if (dp) dlg_import_bom_m1(owner, *dp); });
        return false;
    }
    Settings& s = settings();
    auto st = std::make_unique<M1Dlg>();
    {
        std::wstring pw;
        if (platform::dpapi_unprotect_hex(s.m1_pass_dpapi, pw) && !pw.empty()) {
            std::snprintf(st->pass, sizeof st->pass, "%s", U(pw).c_str());
            SecureZeroMemory(pw.data(), pw.size() * sizeof(wchar_t));
        }
        st->remember = !s.m1_pass_dpapi.empty() || s.m1_user.empty();
    }
    static std::wstring s_last_job;
    std::wstring job = s_last_job;
    if (const wchar_t* e = _wgetenv(L"PCBP_TEST_M1_JOB")) job = e;   // automation
    const std::vector<std::wstring> auths{L"Windows (this user)", L"SQL Server login"};
    Form f(L"Import from M1", 520);
    f.icon((int)Icon::ImportBom).header(L"Import BOM from M1 by Job ID")
        .subtitle(L"Reads the job's material list (read-only) and opens the BOM wizard with the M1 BOM profile.");
    f.text("job", L"Job ID", job).tip(L"For example 123456-01-01");
    f.separator();
    f.heading(L"Connection");
    f.text("server", L"Server", s.m1_server).tip(L"SQL Server name, e.g. HOST\\INSTANCE or HOST,PORT");
    f.text("db", L"Database", s.m1_database);
    f.combo("auth", L"Sign-in", auths, auths[s.m1_auth == 0 ? 0 : 1]);
    f.text("user", L"User", s.m1_user);
    M1Dlg* S = st.get();
    f.custom("pass", [S](Form& ff, float w) {
        const bool sql = ff.index("auth") == 1;
        ImGui::BeginDisabled(!sql);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Password");
        ImGui::SameLine(gui::S(130));
        ImGui::SetNextItemWidth(std::max(gui::S(120), w - gui::S(140)));
        ImGui::InputText("##m1pw", S->pass, sizeof S->pass, ImGuiInputTextFlags_Password);
        ImGui::Checkbox("Remember password (encrypted for this Windows user)", &S->remember);
        ImGui::EndDisabled();
    });
    f.ok_text(L"Fetch");
    f.validate([](Form& ff, std::wstring& e) {
        std::string j;
        if (!io::m1_normalize_job_id(ff.u8("job"), j)) { e = L"Enter a Job ID (letters, digits and - _ . / only)."; return false; }
        if (ff.str("server").empty() || ff.str("db").empty()) { e = L"Enter the M1 server and database."; return false; }
        return true;
    });
    if (!_wgetenv(L"PCBP_TEST_M1_TSV") && !f.run(owner)) { SecureZeroMemory(S->pass, sizeof S->pass); return false; }

    std::string jobid;
    io::m1_normalize_job_id(f.u8("job").empty() ? U(job) : f.u8("job"), jobid);
    std::string tsv;
    if (const wchar_t* tf = _wgetenv(L"PCBP_TEST_M1_TSV")) {   // automation: canned result instead of a server
        std::string err;
        if (!read_text_file(U(tf), tsv, err)) { warning(owner, L"Import from M1", L"Could not read the test table.\n" + W(err)); return false; }
        if (jobid.empty()) jobid = "TEST";
    } else {
        s.m1_server = f.str("server"); s.m1_database = f.str("db"); s.m1_user = f.str("user");
        s.m1_auth = f.index("auth") == 0 ? 0 : 1;
        std::wstring pw = W(S->pass);
        SecureZeroMemory(S->pass, sizeof S->pass);
        std::wstring enc;
        if (s.m1_auth == 1 && S->remember && !pw.empty()) {
            if (platform::dpapi_protect_hex(pw, enc)) s.m1_pass_dpapi = enc;
            else warning(owner, L"Import from M1", L"The password could not be encrypted, so it was not saved.");
        } else s.m1_pass_dpapi.clear();
        s.save();
        s_last_job = W(jobid);
        platform::M1Conn c;
        c.server = s.m1_server; c.database = s.m1_database;
        c.windows_auth = s.m1_auth == 0;
        c.user = s.m1_user; c.password = pw;
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        platform::M1FetchResult r = platform::m1_fetch_bom(c, jobid);
        SetCursor(old);
        SecureZeroMemory(pw.data(), pw.size() * sizeof(wchar_t));
        SecureZeroMemory(c.password.data(), c.password.size() * sizeof(wchar_t));
        if (!r.ok) { warning(owner, L"Import from M1", L"Job " + W(jobid) + L": " + W(r.error)); return false; }
        tsv = io::m1_rows_to_tsv(r.headers, r.rows);
        gui::toast((std::to_string(r.rows.size()) + " BOM rows fetched from M1 for job " + jobid).c_str(), gui::ToastKind::Info, 3.f);
    }
    gui::set_import_source(std::move(tsv), 0);
    const bool ok = dlg_import_bom(owner, d, L"M1 job " + W(jobid));
    if (ok) d.changed(ui::CH_ALL);
    return ok;
}

} // namespace pcbp::app
