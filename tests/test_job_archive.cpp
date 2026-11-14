// ============================================================================
// test_job_archive.cpp — R19b job archive "<Project Name>.zip" (src/core/job_archive.*, specs/DOCUMENTS.md).
//   * index round-trip through project_to_json / project_from_json
//   * add / rename / move / delete rewrites; result passes the system `unzip -t`; one .bak kept
//   * Zip64 records (forced) readable by us and by `unzip -t`
//   * nested zip listing
//   * folder sniffing (Gerber set, BOM, email, PDF, image, panel)
//   * checkpoint snapshot size independent of the attachment bytes
//   * R22: categories, legacy folder / index migration, category proposals (Gerber set heuristics), drop sheet
// Synthetic fixtures only.
// ============================================================================
#include "../src/core/job_archive.h"
#include "../src/core/project_io.h"
#include "../src/core/json.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace pcbp;
using namespace pcbp::io::jobzip;
namespace fs = std::filesystem;

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<uint8_t> B(const std::string& s) { return {s.begin(), s.end()}; }
static const char* kGerber = "%FSLAX24Y24*%\n%MOIN*%\n%ADD10C,0.0100*%\nD10*\nX0Y0D02*\nX10000Y0D01*\nM02*\n";
static const char* kDrill = "M48\nINCH,LZ\nT01C0.0300\n%\nT01\nX010000Y010000\nM30\n";
static const char* kBom = "Item,Qty,Reference,Part Number,Description\n1,2,\"R1,R2\",RC0603-10K,Resistor 10k 0603\n2,1,C1,CC0603-100N,Capacitor 100nF\n3,1,U1,LM358,Op amp\n";
static const char* kEml = "Received: from x\nFrom: a@example.com\nTo: b@example.com\nSubject: rev B gerbers\nMessage-ID: <1@x>\n\nhello\n";

static bool unzip_ok(const std::string& p) {
    std::string cmd = "unzip -tqq \"" + p + "\" >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}
static std::vector<std::string> names(const Zip& z) { std::vector<std::string> v; for (auto& i : z.items()) v.push_back(i.name); return v; }
static bool has(const Zip& z, const std::string& n) { for (auto& i : z.items()) if (i.name == n) return true; return false; }

int main() {
    const fs::path dir = fs::temp_directory_path() / "pcbp_test_job_archive";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string pcbp = (dir / "Test1.pcbp").string();

    // ---- archive name
    CHECK(archive_path_for(pcbp, "J-0000-00-00-01") == (dir / "J-0000-00-00-01.zip").string());
    CHECK(archive_path_for(pcbp, "") == (dir / "Test1.zip").string());
    CHECK(archive_path_for(pcbp, "a/b:c") == (dir / "a_b_c.zip").string());
    CHECK(archive_path_for("", "x").empty());
    const std::string zp = archive_path_for(pcbp, "Job 7");

    // ---- create with content (archive missing -> created)
    std::vector<uint8_t> big_text;
    for (int i = 0; i < 4000; ++i) { std::string l = "X" + std::to_string(i * 10) + "Y" + std::to_string(i) + "D01*\n"; big_text.insert(big_text.end(), l.begin(), l.end()); }
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    {
        Edit e;
        e.add_dirs = {"Datasheets/"};
        e.add.push_back({"Single Circuit/top.gtl", big_text, ""});
        e.add.push_back({"Images/board.png", png, ""});
        e.add.push_back({"Bill of Materials/bom.csv", B(kBom), ""});
        std::string err;
        CHECK(rewrite(zp, e, err));
        if (!err.empty()) std::printf("  err: %s\n", err.c_str());
    }
    CHECK(fs::exists(zp));
    CHECK(!fs::exists(zp + ".bak"));   // nothing to back up on create
    CHECK(unzip_ok(zp));
    {
        Zip z; std::string err;
        CHECK(z.open_file(zp, err));
        CHECK(z.items().size() == 4);
        CHECK(has(z, "Datasheets/"));
        int g = z.find("single circuit/TOP.GTL");   // case-insensitive
        CHECK(g >= 0);
        if (g >= 0) { CHECK(z.items()[g].method == 8); CHECK(z.items()[g].csize < z.items()[g].usize); }
        int p = z.find("Images/board.png");
        CHECK(p >= 0 && z.items()[p].method == 0);   // already-compressed type stored
        std::vector<uint8_t> d;
        CHECK(z.read("Single Circuit/top.gtl", d, err) && d == big_text);
        CHECK(!z.zip64());
    }

    // ---- rename, move, delete (+ .bak)
    {
        Edit e;
        e.rename["Bill of Materials/bom.csv"] = "Bill of Materials/BOM rev B.csv";
        e.rename["Single Circuit/"] = "Panelized Circuit/";   // move a folder subtree
        e.remove.insert("Images/board.png");
        e.add.push_back({"Emails/note.eml", B(kEml), ""});
        std::string err;
        CHECK(rewrite(zp, e, err));
        CHECK(fs::exists(zp + ".bak"));
        CHECK(unzip_ok(zp));
        CHECK(unzip_ok(zp + ".bak"));
        Zip z; CHECK(z.open_file(zp, err));
        CHECK(has(z, "Bill of Materials/BOM rev B.csv"));
        CHECK(!has(z, "Bill of Materials/bom.csv"));
        CHECK(has(z, "Panelized Circuit/top.gtl"));
        CHECK(!has(z, "Single Circuit/top.gtl"));
        CHECK(!has(z, "Images/board.png"));
        CHECK(has(z, "Emails/note.eml"));
        std::vector<uint8_t> d;
        CHECK(z.read("Panelized Circuit/top.gtl", d, err) && d == big_text);   // raw-copied, still valid
        Zip bak; CHECK(bak.open_file(zp + ".bak", err) && has(bak, "Images/board.png"));
        // replace an entry by adding the same name
        Edit r; r.add.push_back({"Emails/note.eml", B("From: x\nTo: y\nSubject: z\n\nnew\n"), ""});
        CHECK(rewrite(zp, r, err));
        Zip z2; CHECK(z2.open_file(zp, err));
        CHECK(z2.items().size() == z.items().size());
        CHECK(z2.read("Emails/note.eml", d, err) && std::string(d.begin(), d.end()).find("new") != std::string::npos);
        CHECK(!fs::exists(zp + ".tmp"));
    }
    // ---- unsafe names refused, archive untouched
    {
        Edit e; e.add.push_back({"../evil.txt", B("x"), ""});
        std::string err;
        CHECK(!rewrite(zp, e, err));
        CHECK(unzip_ok(zp));
    }

    // ---- Zip64
    const std::string z64 = (dir / "z64.zip").string();
    {
        Edit e; e.force_zip64 = true;
        e.add.push_back({"Single Circuit/top.gtl", big_text, ""});
        e.add.push_back({"Images/board.png", png, ""});
        e.add_dirs = {"Other/"};
        std::string err;
        CHECK(rewrite(z64, e, err));
        CHECK(unzip_ok(z64));
        Zip z; CHECK(z.open_file(z64, err));
        CHECK(z.zip64());
        CHECK(z.items().size() == 3);
        std::vector<uint8_t> d;
        CHECK(z.read("Single Circuit/top.gtl", d, err) && d == big_text);
        // rewrite a Zip64 archive normally: copies raw, drops Zip64 records
        Edit r; r.rename["Images/board.png"] = "Images/b.png";
        CHECK(rewrite(z64, r, err));
        Zip z2; CHECK(z2.open_file(z64, err) && !z2.zip64() && has(z2, "Images/b.png"));
        CHECK(unzip_ok(z64));
    }
    // a Zip64 archive written by the system zip tool (streamed: zip -fz forces Zip64)
    {
        fs::path src = dir / "src"; fs::create_directories(src / "g");
        { FILE* f = std::fopen((src / "g" / "a.gbr").string().c_str(), "wb"); std::fputs(kGerber, f); std::fclose(f); }
        std::string cmd = "cd \"" + src.string() + "\" && zip -qr -fz \"" + (dir / "sys64.zip").string() + "\" g >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0) {
            Zip z; std::string err;
            CHECK(z.open_file((dir / "sys64.zip").string(), err));
            std::vector<uint8_t> d;
            CHECK(z.read("g/a.gbr", d, err) && std::string(d.begin(), d.end()) == kGerber);
        }
    }

    // ---- nested zip listing
    {
        const std::string inner = (dir / "inner.zip").string();
        Edit e;
        e.add.push_back({"gerbers/top.gtl", B(kGerber), ""});
        e.add.push_back({"gerbers/drill.drl", B(kDrill), ""});
        std::string err;
        CHECK(rewrite(inner, e, err));
        std::vector<uint8_t> ib;
        { FILE* f = std::fopen(inner.c_str(), "rb"); int c; while ((c = std::fgetc(f)) != EOF) ib.push_back((uint8_t)c); std::fclose(f); }
        Edit o; o.add.push_back({"Single Circuit/Gerbers rev A.zip", ib, ""});
        CHECK(rewrite(zp, o, err));
        Zip z; CHECK(z.open_file(zp, err));
        std::vector<uint8_t> nb;
        CHECK(z.read("Single Circuit/Gerbers rev A.zip", nb, err));
        CHECK(z.items()[z.find("Single Circuit/Gerbers rev A.zip")].method == 0);   // zip stored
        Zip n; CHECK(n.open_memory(nb, err));
        auto nn = names(n);
        CHECK(nn.size() == 2 && nn[0] == "gerbers/top.gtl");
        std::vector<uint8_t> d;
        CHECK(n.read("gerbers/drill.drl", d, err) && std::string(d.begin(), d.end()) == kDrill);
        // sniffing the nested zip finds the Gerber set
        CHECK(sniff_folder("Gerbers rev A.zip", nb) == "Original Circuit");
        CHECK(sniff_folder("Panel array gerbers.zip", nb) == "Panel Gerbers");
        CHECK(propose_category("Gerbers rev A.zip", nb).gerber_set);
        CHECK((import_kinds("x.zip", nb) & ImpGerber) != 0);
        CHECK(unzip_ok(zp));
    }

    // ---- category sniffing (R22 categories)
    CHECK(sniff_folder("top.gtl", B(kGerber)) == "Original Circuit");
    CHECK(sniff_folder("1.l2t", B(kGerber)) == "Original Circuit");     // unknown extension, content wins
    CHECK(sniff_folder("drill.drl", B(kDrill)) == "Original Circuit");
    CHECK(sniff_folder("bom.csv", B(kBom)) == "Other");
    CHECK(propose_category("bom.csv", B(kBom)).tags == std::vector<std::string>{"BOM"});
    CHECK((import_kinds("bom.csv", B(kBom)) & ImpBom) != 0);
    CHECK(sniff_folder("BOM GRU6.xlsx", B("PK\x03\x04....")) == "Other");
    CHECK(sniff_folder("re rev B.eml", B(kEml)) == "Email");
    CHECK(sniff_folder("mail.txt", B(kEml)) == "Email");
    CHECK(sniff_folder("Outlook.msg", B("\xD0\xCF\x11\xE0")) == "Email");
    CHECK(sniff_folder("EQ1.png", png) == "Image");
    CHECK(sniff_folder("photo", std::vector<uint8_t>{0xFF, 0xD8, 0xFF, 0xE0, 0, 0}) == "Image");
    CHECK(sniff_folder("LM358 datasheet.pdf", B("%PDF-1.7\n")) == "Other");
    CHECK(propose_category("LM358 datasheet.pdf", B("%PDF-1.7\n")).tags == std::vector<std::string>{"Datasheet"});
    CHECK(sniff_folder("Sten1446592_PasteReport.pdf", B("%PDF-1.7\n")) == "Drawing");
    CHECK(sniff_folder("assy drawing.pdf", B("%PDF-1.4\n")) == "Drawing");
    CHECK(sniff_folder("outline.dxf", B("0\nSECTION\n")) == "Drawing");
    CHECK(sniff_folder("db.mdb", B("random")) == "Other");
    CHECK(should_store("a.ZIP") && should_store("x.jpg") && !should_store("x.gbr") && !should_store("x.csv"));
    CHECK(unique_name(nullptr, "Drawing", "a.pdf") == "Drawing/a.pdf");
    {
        Zip z; std::string err; z.open_file(zp, err);
        CHECK(unique_name(&z, "Emails", "note.eml") == "Emails/note (2).eml");
    }

    // ---- index sync + round-trip through the project file
    {
        Zip z; std::string err; CHECK(z.open_file(zp, err));
        Project p;
        p.documents.push_back({"Emails/note.eml", 0, 0, "", {"customer", "rev B"}, "first contact"});
        sync_index(p.documents, z);
        CHECK(p.documents.size() == 4);   // files only, no folder entries
        DocumentRef* r = index_find(p.documents, "Emails/note.eml");
        CHECK(r && r->tags.size() == 2 && r->note == "first contact" && r->category == "Email / Correspondence" && r->size > 0 && r->crc != 0);
        // move carries metadata
        Edit e; e.rename["Emails/"] = "Other/Mail/";
        CHECK(rewrite(zp, e, err));
        Zip z2; CHECK(z2.open_file(zp, err));
        sync_index(p.documents, z2, e.rename);
        r = index_find(p.documents, "Other/Mail/note.eml");
        CHECK(r && r->note == "first contact" && r->category == "Other");
        p.documents_archive = "../elsewhere/Job 7.zip";
        std::string js = project_to_json(p).dump(1);
        json::Value v; CHECK(json::Value::parse(js, v, err));
        Project q; CHECK(project_from_json(v, q, err));
        CHECK(q.documents.size() == p.documents.size());
        CHECK(q.documents_archive == p.documents_archive);
        bool same = q.documents.size() == p.documents.size();
        for (size_t i = 0; same && i < q.documents.size(); ++i) {
            auto &a = p.documents[i], &b = q.documents[i];
            same = a.path == b.path && a.size == b.size && a.crc == b.crc && a.category == b.category && a.tags == b.tags && a.note == b.note;
        }
        CHECK(same);
        CHECK(js.find("hello") == std::string::npos && js.find("D01*") == std::string::npos);   // no bytes in the project
    }

    // ==== R22 (REVIEW_V21 §12): categories, migration, sniffing proposals, drop confirm sheet ====
    {   // category names
        CHECK(kCategoryCount == 9);
        CHECK(std::string(category_label(Category::Email)) == "Email / Correspondence");
        CHECK(std::string(category_folder(Category::Email)) == "Email");
        CHECK(std::string(category_folder(Category::Original)) == "Original Circuit");
        bool k = false;
        CHECK(category_from_name("Single Circuit", &k) == Category::Original && k);
        CHECK(category_from_name("Panelized Circuit") == Category::Panel);
        CHECK(category_from_name("Bill of Materials") == Category::Other);
        CHECK(category_from_name("Datasheets") == Category::Other);
        CHECK(category_from_name("Drawings") == Category::Drawing);
        CHECK(category_from_name("Emails") == Category::Email);
        CHECK(category_from_name("Email / Correspondence") == Category::Email);
        CHECK(category_from_name("whatever", &k) == Category::Other && !k);
        CHECK(category_of_path("Stencil/top.gtp", &k) == Category::Stencil && k);
        CHECK(category_of_path("Single Circuit/g/top.gtl", &k) == Category::Original && k);
        CHECK(category_of_path("Customer/x.txt", &k) == Category::Other && !k);
        CHECK(category_of_path("loose.pdf", &k) == Category::Other && !k);
        CHECK(path_in_category("Single Circuit/g/top.gtl", Category::Panel) == "Panel Gerbers/g/top.gtl");
        CHECK(path_in_category("Drawing/a.pdf", Category::Schematic) == "Schematic/a.pdf");
        CHECK(path_in_category("Original Circuit/rev B/", Category::Stencil) == "Stencil/rev B/");
        CHECK(path_in_category("loose.pdf", Category::Drawing) == "Drawing/loose.pdf");
        CHECK(path_in_category("Customer/sub/x.txt", Category::Notes) == "Notes/x.txt");
        CHECK(path_in_category("x.eml", Category::Email) == "Email/x.eml");
    }
    {   // index migration (no archive change) + folder migration (on the user's OK)
        const std::string mz = (dir / "legacy.zip").string();
        Edit e;
        e.add_dirs = {"Single Circuit/", "Emails/", "Original Circuit/"};
        e.add.push_back({"Single Circuit/top.gtl", B(kGerber), ""});
        e.add.push_back({"Single Circuit/gerbers/bot.gbl", B(kGerber), ""});
        e.add.push_back({"Original Circuit/top.gtl", B("other top"), ""});      // collides with the moved one
        e.add.push_back({"Panelized Circuit/panel.gko", B(kGerber), ""});
        e.add.push_back({"Bill of Materials/bom.csv", B(kBom), ""});
        e.add.push_back({"Other/bom.csv", B("an older bom"), ""});             // collides with the folded BOM
        e.add.push_back({"Datasheets/LM358.pdf", B("%PDF-1.7\n"), ""});
        e.add.push_back({"Images/board.png", png, ""});
        e.add.push_back({"Drawings/assy.pdf", B("%PDF-1.7\n"), ""});
        e.add.push_back({"Customer/x.txt", B("note"), ""});
        std::string err;
        CHECK(rewrite(mz, e, err));
        Zip z; CHECK(z.open_file(mz, err));
        size_t files0 = 0; for (auto& it : z.items()) files0 += !it.dir();
        std::vector<DocumentRef> idx = {
            {"Single Circuit/top.gtl", 0, 0, "Single Circuit", {"rev A"}, "keep me"},
            {"Bill of Materials/bom.csv", 0, 0, "Bill of Materials", {}, "bom note"},
            {"Datasheets/LM358.pdf", 0, 0, "Datasheets", {}, ""},
            {"Customer/x.txt", 0, 0, "Emails", {}, ""},   // stored legacy category outside a category folder
        };
        sync_index(idx, z);
        CHECK(idx.size() == files0);
        DocumentRef* r = index_find(idx, "Single Circuit/top.gtl");
        CHECK(r && r->category == "Original Circuit" && r->note == "keep me" && r->tags == std::vector<std::string>{"rev A"});
        r = index_find(idx, "Bill of Materials/bom.csv");
        CHECK(r && r->category == "Other" && r->tags == std::vector<std::string>{"BOM"} && r->note == "bom note");
        r = index_find(idx, "Datasheets/LM358.pdf");
        CHECK(r && r->category == "Other" && r->tags == std::vector<std::string>{"Datasheet"});
        CHECK(index_find(idx, "Panelized Circuit/panel.gko")->category == "Panel Gerbers");
        CHECK(index_find(idx, "Images/board.png")->category == "Image");
        CHECK(index_find(idx, "Drawings/assy.pdf")->category == "Drawing");
        CHECK(index_find(idx, "Customer/x.txt")->category == "Email / Correspondence");
        sync_index(idx, z);   // idempotent: no duplicate tags
        CHECK(index_find(idx, "Bill of Materials/bom.csv")->tags.size() == 1);
        auto lf = legacy_folders_in(z);
        CHECK(lf.size() == 7);   // all but Other
        Edit m;
        CHECK(plan_folder_migration(z, m));
        CHECK(m.rename.at("Single Circuit/top.gtl") == "Original Circuit/top (2).gtl");
        CHECK(m.rename.at("Single Circuit/gerbers/bot.gbl") == "Original Circuit/gerbers/bot.gbl");
        CHECK(m.rename.at("Bill of Materials/bom.csv") == "Other/bom (2).csv");
        CHECK(m.rename.at("Datasheets/LM358.pdf") == "Other/LM358.pdf");
        CHECK(m.rename.at("Emails/") == "Email/");
        CHECK(m.remove.empty());                     // a folder remove would take its files with it
        CHECK(rewrite(mz, m, err));
        CHECK(unzip_ok(mz));
        Zip z2; CHECK(z2.open_file(mz, err));
        size_t files1 = 0; for (auto& it : z2.items()) files1 += !it.dir();
        CHECK(files1 == files0);                       // nothing lost
        CHECK(legacy_folders_in(z2).empty());
        CHECK(has(z2, "Original Circuit/top.gtl") && has(z2, "Original Circuit/top (2).gtl") && has(z2, "Panel Gerbers/panel.gko"));
        CHECK(has(z2, "Image/board.png") && has(z2, "Drawing/assy.pdf") && has(z2, "Email/") && has(z2, "Customer/x.txt"));
        std::vector<uint8_t> d;
        CHECK(z2.read("Original Circuit/top (2).gtl", d, err) && std::string(d.begin(), d.end()) == kGerber);
        CHECK(z2.read("Other/bom.csv", d, err) && std::string(d.begin(), d.end()) == "an older bom");
        sync_index(idx, z2, m.rename);
        r = index_find(idx, "Original Circuit/top (2).gtl");
        CHECK(r && r->note == "keep me" && r->category == "Original Circuit");
        r = index_find(idx, "Other/bom (2).csv");
        CHECK(r && r->note == "bom note" && r->tags == std::vector<std::string>{"BOM"} && r->category == "Other");
        CHECK(index_find(idx, "Other/LM358.pdf")->tags == std::vector<std::string>{"Datasheet"});
        Edit none; CHECK(!plan_folder_migration(z2, none));
    }
    // synthetic Gerber outlines (metric 4.6)
    auto mm = [](double v) { return std::to_string((long long)std::llround(v * 1e6)); };
    auto rect = [&](double x, double y, double w, double h) {
        return "X" + mm(x) + "Y" + mm(y) + "D02*\nX" + mm(x + w) + "Y" + mm(y) + "D01*\nX" + mm(x + w) + "Y" + mm(y + h) + "D01*\nX" + mm(x) + "Y" + mm(y + h) + "D01*\nX" + mm(x) + "Y" + mm(y) + "D01*\n";
    };
    const std::string hdr = "%FSLAX46Y46*%\n%MOMM*%\n%ADD10C,0.100000*%\nD10*\n";
    const std::string single = hdr + rect(0, 0, 50, 30) + "X" + mm(3) + "Y" + mm(3) + "D02*\nX" + mm(4) + "Y" + mm(3) + "D01*\n" + rect(5, 5, 2, 2) + rect(40, 5, 2, 2) + "M02*\n";   // + 2 small cut-outs, a stray line
    std::string panel = hdr + rect(0, 0, 120, 80);
    for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) panel += rect(5 + i * 55, 5 + j * 35, 50, 30);
    panel += "M02*\n";
    const std::string rotated = hdr + rect(0, 0, 100, 70) + rect(5, 5, 50, 30) + rect(60, 5, 30, 50) + "M02*\n";   // 2 circuits, one at 90 deg
    {
        int rep = -1;
        CHECK(count_outline_loops(single, &rep) == 1 && rep == 0);
        CHECK(count_outline_loops(panel, &rep) == 5 && rep == 4);
        CHECK(count_outline_loops(rotated, &rep) == 3 && rep == 2);
        CHECK(count_outline_loops("garbage", &rep) == 0 && rep == 0);
    }
    auto mkzip = [&](std::vector<std::pair<std::string, std::string>> f) {
        const std::string zpth = (dir / "mk.zip").string();
        fs::remove(zpth);
        Edit e; for (auto& [n, c] : f) e.add.push_back({n, B(c), ""});
        std::string err; rewrite(zpth, e, err);
        std::vector<uint8_t> b; FILE* fp = std::fopen(zpth.c_str(), "rb"); int ch; while ((ch = std::fgetc(fp)) != EOF) b.push_back((uint8_t)ch); std::fclose(fp);
        return b;
    };
    {   // proposals: Gerber sets -> Original / Panel / Stencil, never a silent Other
        const auto one = mkzip({{"job.gtl", kGerber}, {"job.gts", kGerber}, {"job.gto", kGerber}, {"job.gtp", kGerber}, {"job.gko", single}, {"job.drl", kDrill}});
        Proposal p = propose_category("Job 1234 rev B.zip", one);
        CHECK(p.gerber_set && p.cat == Category::Original);
        CHECK(p.g.copper == 1 && p.g.paste == 1 && p.g.outline == 1 && p.g.drill == 1 && p.g.repeated_outlines == 0);
        const auto pan = mkzip({{"job.gtl", kGerber}, {"job.gts", kGerber}, {"job.gko", panel}, {"job.drl", kDrill}});
        p = propose_category("Job 1234.zip", pan);
        CHECK(p.gerber_set && p.cat == Category::Panel && p.g.repeated_outlines == 4);
        CHECK(p.why.find("4 circuits") != std::string::npos);
        const auto st = mkzip({{"job.gtp", kGerber}, {"job.gbp", kGerber}, {"job.gko", single}});
        p = propose_category("Job 1234.zip", st);
        CHECK(p.gerber_set && p.cat == Category::Stencil && p.why.find("paste") != std::string::npos);
        CHECK(propose_category("Job 1234 STENCIL.zip", one).cat == Category::Stencil);
        CHECK(propose_category("Job 1234 panel.zip", one).cat == Category::Panel);
        CHECK(propose_category("top.gtp", B(kGerber)).cat == Category::Stencil);        // a lone paste layer
        CHECK(propose_category("board.gko", B(panel)).cat == Category::Panel);
        CHECK(propose_category("board.gko", B(single)).cat == Category::Original);
        CHECK(propose_category("NC Drill.txt", B(kDrill)).gerber_set);                  // Altium drill export (.txt)
        // documents
        CHECK(propose_category("XYZ-100 Schematic rev2.pdf", B("%PDF-1.7\n")).cat == Category::Schematic);
        CHECK(propose_category("XYZ-100_SCH.pdf", B("%PDF-1.7\n")).cat == Category::Schematic);
        CHECK(propose_category("XYZ-100.pdf", B("%PDF-1.7\n1 0 obj << /Title (XYZ-100 Schematic) >>")).cat == Category::Schematic);
        CHECK(propose_category("XYZ-100 fab.pdf", B("%PDF-1.7\n")).cat == Category::Drawing);
        CHECK(propose_category("XYZ-100.pdf", B("%PDF-1.7\n")).cat == Category::Drawing);
        CHECK(propose_category("build notes.md", B("# Notes\n")).cat == Category::Notes);
        CHECK(propose_category("readme.txt", B("Run on line 2\n")).cat == Category::Notes);
        CHECK(propose_category("re rev B.eml", B(kEml)).cat == Category::Email);
        CHECK(propose_category("Outlook.msg", B("\xD0\xCF\x11\xE0")).cat == Category::Email);
        CHECK(propose_category("IMG_0001.JPG", std::vector<uint8_t>{0xFF, 0xD8, 0xFF, 0xE0, 0, 0}).cat == Category::Image);
        CHECK(propose_category("instructions.docx", B("PK\x03\x04")).cat == Category::Notes);
        CHECK(!propose_category("db.mdb", B("random")).why.empty());   // Other, but with a reason shown
        // a dropped folder: an extracted Gerber directory
        std::vector<std::pair<std::string, std::vector<uint8_t>>> fset = {{"g/top.gtl", B(kGerber)}, {"g/board.gko", B(panel)}, {"readme.txt", B("hi")}};
        p = propose_category_set("Gerbers", fset);
        CHECK(p.gerber_set && p.cat == Category::Panel);
        fset = {{"a.pdf", B("%PDF-1.4\n")}, {"b.pdf", B("%PDF-1.4\n")}, {"c.png", png}};
        p = propose_category_set("Docs", fset);
        CHECK(!p.gerber_set && p.cat == Category::Drawing);
    }
    {   // drop confirm sheet
        const fs::path dd = dir / "drop";
        fs::create_directories(dd / "Panel gerbers" / "sub");
        auto put = [](const fs::path& p, const std::string& c) { FILE* f = std::fopen(p.string().c_str(), "wb"); std::fwrite(c.data(), 1, c.size(), f); std::fclose(f); };
        put(dd / "top.gtl", kGerber);
        put(dd / "Panel gerbers" / "a.gtl", kGerber);
        put(dd / "Panel gerbers" / "sub" / "a.gko", panel);
        put(dd / "notes.md", "# Build\n- **bake** first\n");
        put(dd / "assy.pdf", "%PDF-1.4\n");
        put(dd / "mail.eml", kEml);
        put(dd / "bom.csv", kBom);
        std::vector<std::string> paths;
        for (const char* n : {"top.gtl", "Panel gerbers", "notes.md", "assy.pdf", "mail.eml", "bom.csv", "missing.txt"}) paths.push_back((dd / n).string());
        DropSheet sh = make_drop_sheet(paths);
        CHECK(sh.rows.size() == 6);   // the missing path is skipped
        CHECK(sh.rows[0].cat == Category::Original && sh.rows[0].p.gerber_set);
        CHECK(sh.rows[1].folder && sh.rows[1].files() == 2 && sh.rows[1].cat == Category::Panel && sh.rows[1].p.gerber_set);
        CHECK(sh.rows[2].cat == Category::Notes && sh.rows[3].cat == Category::Drawing && sh.rows[4].cat == Category::Email);
        CHECK(sh.rows[5].cat == Category::Other && sh.rows[5].p.tags == std::vector<std::string>{"BOM"});
        CHECK(sh.included_files() == 7);
        int others = 0; for (auto& r : sh.rows) others += r.cat == Category::Other && r.p.why.empty();
        CHECK(others == 0);   // every proposal carries a reason
        // the user changes one row and drops another
        sh.rows[0].cat = Category::Stencil;
        sh.rows[4].include = false;
        CHECK(sh.included_files() == 6);
        const std::string sz = (dir / "sheet.zip").string();
        { Edit pre; pre.add.push_back({"Drawing/assy.pdf", B("%PDF-old"), ""}); std::string err; CHECK(rewrite(sz, pre, err)); }
        Zip z; std::string err; CHECK(z.open_file(sz, err));
        Edit e; std::map<std::string, std::vector<std::string>> tags; std::map<int, int> per;
        CHECK(sh.build(&z, e, &tags, &per));
        std::set<std::string> added; for (auto& a : e.add) added.insert(a.name);
        CHECK(added.count("Stencil/top.gtl"));
        CHECK(added.count("Panel Gerbers/Panel gerbers/a.gtl") && added.count("Panel Gerbers/Panel gerbers/sub/a.gko"));
        CHECK(added.count("Notes/notes.md"));
        CHECK(added.count("Drawing/assy (2).pdf"));   // unique against the archive
        CHECK(added.count("Other/bom.csv"));
        CHECK(added.size() == 6);                     // the e-mail row is excluded
        CHECK(tags.size() == 1 && tags["Other/bom.csv"] == std::vector<std::string>{"BOM"});
        CHECK(per[(int)Category::Panel] == 2 && per[(int)Category::Stencil] == 1 && !per.count((int)Category::Email));
        CHECK(rewrite(sz, e, err));
        CHECK(unzip_ok(sz));
        Zip z2; CHECK(z2.open_file(sz, err));
        std::vector<DocumentRef> idx;
        sync_index(idx, z2);
        CHECK(index_find(idx, "Panel Gerbers/Panel gerbers/sub/a.gko")->category == "Panel Gerbers");
        CHECK(index_find(idx, "Notes/notes.md")->category == "Notes");
        // dropped on a category: every row preselects it, the proposal is still shown
        DropSheet t = make_drop_sheet({(dd / "notes.md").string(), (dd / "assy.pdf").string()}, (int)Category::Schematic);
        CHECK(t.target == (int)Category::Schematic && t.rows.size() == 2);
        CHECK(t.rows[0].cat == Category::Schematic && t.rows[0].p.cat == Category::Notes);
        t.set_all(Category::Image);
        CHECK(t.rows[1].cat == Category::Image);
        DropSheet empty = make_drop_sheet({});
        Edit e3; CHECK(!empty.build(nullptr, e3));
    }

    // ---- checkpoint size unaffected by attachment size
    {
        Project p1, p2;
        p1.documents.push_back({"Single Circuit/top.gtl", 1000, 0x12345678u, "Single Circuit", {}, ""});
        p2.documents.push_back({"Single Circuit/top.gtl", 4000000000ull, 0x12345678u, "Single Circuit", {}, ""});
        // checkpoint snapshot = project_to_json without layer content (output_views.cpp checkpoint_snapshot)
        const size_t s1 = project_to_json(p1, false).dump(-1).size(), s2 = project_to_json(p2, false).dump(-1).size();
        CHECK(s2 >= s1 && s2 - s1 <= 8);   // only the size digits differ
        Project p0;
        CHECK(project_to_json(p1, false).dump(-1).size() - project_to_json(p0, false).dump(-1).size() < 200);
    }

    fs::remove_all(dir);
    std::printf("%d/%d checks passed\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
