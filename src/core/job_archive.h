#pragma once
// ============================================================================
// job_archive.h (core: uses content_detect) — the project's job archive "<Project Name>.zip" (specs/DOCUMENTS.md), portable.
//
// A plain PKWARE zip: readable and writable by any zip tool. Reader handles Zip64 (EOCD64, per-entry
// extra 0x0001), stored and deflated entries, and works on a file on disk (only the central
// directory is loaded; entries read on demand) or on bytes in memory (nested zips).
// Edits are applied by rewrite(): unchanged / renamed entries are copied raw (no recompression),
// new entries are stored (already-compressed types) or deflated (text), the result goes to a temp
// file that replaces the archive; the previous archive is kept as "<archive>.bak" (one generation).
// Zip64 records are written only when needed (or when forced, for tests).
//
// Also: R22 categories (= top folders) with the R19-R21 folder migration, category proposals for dropped files
// (content_detect + Gerber layer heuristics) and the drop confirm sheet model,
// "import as" kinds per entry, and the light index the .pcbp stores (core/project.h DocumentRef).
// ============================================================================
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "project.h"

namespace pcbp::io::jobzip {

struct Item {
    std::string name;               // '/'-separated; folders end with '/'
    uint16_t flags = 0, method = 0, mtime = 0, mdate = 0;
    uint32_t crc = 0;
    uint64_t csize = 0, usize = 0, lho = 0;   // lho: local header offset
    bool dir() const { return !name.empty() && name.back() == '/'; }
};

class Zip {
public:
    bool open_file(const std::string& path, std::string& err);       // UTF-8 path
    bool open_memory(std::vector<uint8_t> bytes, std::string& err);  // nested zip
    const std::vector<Item>& items() const { return items_; }
    int find(const std::string& name) const;                         // exact, then case-insensitive; -1
    bool read(const Item& it, std::vector<uint8_t>& out, std::string& err) const;      // decoded, CRC checked
    bool read_raw(const Item& it, std::vector<uint8_t>& raw, std::string& err) const;  // compressed payload
    bool read(const std::string& name, std::vector<uint8_t>& out, std::string& err) const;
    bool zip64() const { return zip64_; }
    const std::string& path() const { return path_; }
private:
    bool read_at(uint64_t off, size_t n, std::vector<uint8_t>& out) const;
    bool parse(std::string& err);
    std::string path_;
    std::vector<uint8_t> mem_;
    bool in_mem_ = false, zip64_ = false;
    uint64_t size_ = 0;
    std::vector<Item> items_;
};

// One pending change set. Paths are archive names ('/'). Order applied: remove, rename, add.
struct Edit {
    std::set<std::string> remove;                       // exact names (folder "X/" removes everything below)
    std::map<std::string, std::string> rename;          // old -> new (folder "A/" -> "B/" moves the subtree)
    struct Add { std::string name; std::vector<uint8_t> data; std::string disk; };   // data, or a disk file (UTF-8)
    std::vector<Add> add;                               // replaces an existing entry of the same name
    std::vector<std::string> add_dirs;                  // empty folders ("Drawing/")
    bool force_zip64 = false;                           // tests: write Zip64 records even when small
    int level = 6;
};
// Rewrite (or create) `zip_path` atomically: temp file -> replace; old archive -> zip_path + ".bak".
bool rewrite(const std::string& zip_path, const Edit& e, std::string& err);

// Store already-compressed types (zip, images, pdf, office, media), deflate the rest.
bool should_store(const std::string& name);

// --- R22 categories (specs/REVIEW_V21.md §12) ---------------------------------
// A category is the top folder of an entry inside the job archive. Files outside a category folder (top level,
// custom folders) take the category stored in the index ("Other" when none).
// R25: CAD (ODB++ / CAD / placement data), BOM (bills of materials) and Machine programs added; categories are stored
// by label, so the numbering may change.
enum class Category : int { Original = 0, Panel, Stencil, Cad, Bom, Programs, Schematic, Drawing, Image, Email, Notes, Other };
constexpr int kCategoryCount = 12;
const char* category_label(Category c);     // "Original Circuit", "Panel Gerbers", ..., "Email / Correspondence", ...
const char* category_folder(Category c);    // archive top folder; = label except Email ("Email": no '/' in a folder)
// Label, folder, or a legacy (R19-R21) folder / category name -> category. known = false: not recognised (-> Other).
Category category_from_name(const std::string& s, bool* known = nullptr);
// R19-R21 standard folders and what they became. tag != nullptr: the folder has no category of its own, its files
// go to `to` and get that tag (decided in R22: BOM -> Other + "BOM", Datasheets -> Other + "Datasheet").
struct LegacyFolder { const char* name; Category to; const char* tag; };
extern const LegacyFolder kLegacyFolders[8];
const LegacyFolder* legacy_folder(const std::string& top);   // nullptr when `top` is not a legacy folder name
std::string top_folder(const std::string& path);              // "A/b/c.txt" -> "A"; "" for a top-level file
// Category of an archive path from its top folder (current or legacy). known = false: not in a category folder.
Category category_of_path(const std::string& path, bool* known = nullptr);
// `path` moved to category c: a (current or legacy) category top folder is replaced, anything else goes below the
// category folder by its base name ("Single Circuit/g/top.gtl" -> "Panel Gerbers/g/top.gtl", "x.pdf" -> "Drawing/x.pdf").
std::string path_in_category(const std::string& path, Category c);
// Archive folders still named the R19-R21 way (top folders, in archive order).
std::vector<std::string> legacy_folders_in(const Zip& z);
// Collision-free per-entry renames moving every legacy top folder to its new folder (needs the user's OK: it
// rewrites the archive). Files that would collide get "name (2)"; nothing is removed.
// Returns false when nothing needs to move.
bool plan_folder_migration(const Zip& z, Edit& e);

// --- R22 sniffing: proposed category for a dropped file / folder --------------
struct GerberTraits {
    int layers = 0, drill = 0, copper = 0, paste = 0, mask = 0, silk = 0, outline = 0, other = 0;
    int outline_loops = 0, repeated_outlines = 0;   // closed contours on outline layers; size of the largest group of equal ones
    bool name_panel = false, name_stencil = false;
};
struct Proposal {
    Category cat = Category::Other;
    bool gerber_set = false;            // Gerber / drill data: the sheet asks Original vs Panel vs Stencil
    GerberTraits g;
    std::vector<std::string> tags;      // suggested tags ("BOM", "Datasheet")
    std::string why;                    // short reason shown in the confirm sheet
};
Proposal propose_category(const std::string& name, const std::vector<uint8_t>& bytes);
// A dropped folder (an extracted Gerber directory, ...): name + (relative name, first bytes) of its files.
Proposal propose_category_set(const std::string& folder_name, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files);
// Closed contours of an outline layer (Gerber text) and the size of the largest group of equal-size ones (rotation
// by 90 deg allowed). Contours smaller than 10 % of the layer extent are ignored (holes, slots, fiducials).
int count_outline_loops(const std::string& gerber_text, int* repeated);
// Compatibility (R19-R21 callers): the archive top folder of propose_category().
std::string sniff_folder(const std::string& name, const std::vector<uint8_t>& bytes);
// First bytes of a disk file used for sniffing (64 KiB; a whole .zip under 256 MiB to look inside).
std::vector<uint8_t> sniff_head(const std::string& disk_path);

// --- R22 drop confirm sheet (model; the Files tab draws it) --------------------
struct DropRow {
    std::string disk;                   // dropped path (file or folder), UTF-8
    std::string name;                   // file / folder name
    bool folder = false;
    std::vector<std::pair<std::string, std::string>> members;   // folder: (disk path, relative '/' name)
    Proposal p;
    Category cat = Category::Other;     // the user's choice (starts as the proposal / the drop target)
    bool include = true;
    int files() const { return folder ? (int)members.size() : 1; }
};
struct DropSheet {
    std::vector<DropRow> rows;
    int target = -1;                    // category the files were dropped on (-1: the list / anywhere)
    // R25 versions: a file whose archive name exists already replaces it when its content differs, and the old one
    // moves to ".versions/" (version_path); identical content is skipped. false: the new file gets "name (2)".
    bool keep_versions = true;
    int included_files() const;
    void set_all(Category c);
    // Archive names "<category folder>/<name>" (folders keep their name + tree), unique against `z` and each other.
    // tags: archive name -> suggested tags to put in the index after the rewrite. per_cat: files per category.
    // skipped (optional): archive names left out because the archive holds the same content there already.
    bool build(const Zip* z, Edit& e, std::map<std::string, std::vector<std::string>>* tags = nullptr,
               std::map<int, int>* per_cat = nullptr, std::vector<std::string>* skipped = nullptr) const;
};

// --- R25 versions -----------------------------------------------------------------------------------------------
// Old versions live under ".versions/" with the date the replaced copy was archived:
// "Drawing/assy.pdf" (archived 2026-11-19 10:12) -> ".versions/Drawing/assy @2026-11-19 1012.pdf".
constexpr const char* kVersionsFolder = ".versions/";
bool is_version_path(const std::string& path);
std::string version_path(const Item& old, const Zip* z, const std::set<std::string>& taken = {});
// The current file a version belongs to ("" when `path` is not a version) and the stored versions of `path`, newest first.
std::string version_of(const std::string& path);
std::vector<const Item*> versions_of(const Zip& z, const std::string& path);
// Restore: the version becomes the current file; the current one becomes a version.
bool plan_restore(const Zip& z, const std::string& version, Edit& e);
// Versions follow their file: a removed file / folder takes its versions with it, a renamed / moved one moves them
// along (folder prefixes too). Renames from or to a version (restore) are left as they are. Call before rewrite().
void carry_versions(const Zip& z, Edit& e);
// Size + CRC-32 of a disk file (streamed); false when it cannot be read.
bool file_crc(const std::string& disk, uint64_t& size, uint32_t& crc);

// --- R25 pack the project folder ------------------------------------------------------------------------------
// Everything in the folder of the .pcbp goes into the job archive except what must stay next to it:
//   * machine programs, wherever their name (.h8h / .h7h / .h5h / .h4h, splits .h81.., .iss, splits .i01.., .e41..)
//   * project sidecars by name: the M1 work order / traveller, the AOI picture, machine side files (.cpf .cmp .osl
//     .mmd .err .tbl), which TrayTagPrinter and the machines read from the folder
//   * the project file and its backups / autosaves, the archive and its .bak, Office lock files, Thumbs.db /
//     desktop.ini, hidden folders, the "_packed" folder.
// Files whose content the archive already has (same size + CRC anywhere in it) are listed as already archived.
struct PackSkip { std::string disk, name, why; };
struct PackPlan {
    DropSheet sheet;                    // top-level files and folders to pack, with proposed categories
    std::vector<PackSkip> stays;        // stays next to the archive
    std::vector<PackSkip> already;      // the archive holds the same content
    int files() const { return sheet.included_files(); }
};
PackPlan plan_pack(const std::string& dir, const std::vector<std::string>& projects, const std::string& archive_name, const Zip* z);
constexpr const char* kPackedFolder = "_packed";
// After the rewrite: every disk file the edit added is in `z` with its size and CRC. false + err names the first miss.
bool verify_packed(const Zip& z, const Edit& e, std::string& err);
// Move packed originals to dir/_packed/ keeping their folders below dir (never overwrites: "name (2)"); empty source
// folders are removed. Returns the number moved; `failed` lists what could not be moved.
int move_packed(const std::string& dir, const std::vector<std::string>& disk_paths, std::vector<std::string>& failed);

// --- R25 hashes (DocumentRef::sha256) -----------------------------------------------------------------------------
// SHA-256 of what an edit adds (disk files streamed), by archive name; put into the index after the rewrite.
std::map<std::string, std::string> hash_added(const Edit& e);
std::string entry_sha256(const Zip& z, const Item& it);   // "" when the entry cannot be read
// Verify / complete the index: entries in `known` take that hash, others are hashed from the archive (all of them:
// a full check). bad: entries that cannot be read or whose stored hash no longer matches. Returns hashes added.
int fill_hashes(std::vector<DocumentRef>& idx, const Zip& z, const std::map<std::string, std::string>& known = {},
                std::vector<std::string>* bad = nullptr);
// Rows for dropped / picked disk paths (reads the first bytes of each file). target >= 0 preselects that category.
DropSheet make_drop_sheet(const std::vector<std::string>& disk_paths, int target = -1);

// Import kinds offered for an entry: bit 1 Gerbers, 2 CAD, 4 BOM, 8 Centroid.
enum : int { ImpGerber = 1, ImpCad = 2, ImpBom = 4, ImpCentroid = 8 };
int import_kinds(const std::string& name, const std::vector<uint8_t>& bytes);
bool is_zip_name(const std::string& name);
bool is_image_name(const std::string& name);

// Archive file for a project: dir(pcbp) / sanitized(project_name or pcbp stem) + ".zip". Empty when no pcbp path.
std::string archive_path_for(const std::string& pcbp_path, const std::string& project_name);
std::string sanitize_file_name(const std::string& s);
// Unique archive name for `name` under `folder` ("Drawing/x.pdf", "Drawing/x (2).pdf", ...).
std::string unique_name(const Zip* z, const std::string& folder, const std::string& file_name, const std::set<std::string>& taken = {});

// Index <-> archive: rebuild `idx` from the archive listing (files only, not the .versions/ copies), keeping tags / notes / category of
// entries that survive; `renames` (old -> new, folder prefixes allowed) carries metadata across rename / move.
// Category (stored as its label) = the top folder's category when it is a category folder (current or legacy),
// else the stored one migrated to the R22 names ("Other" when unknown). R22 index migration: legacy category names
// become the new labels and BOM / Datasheet files get their tag (idempotent).
void sync_index(std::vector<DocumentRef>& idx, const Zip& z, const std::map<std::string, std::string>& renames = {});
DocumentRef* index_find(std::vector<DocumentRef>& idx, const std::string& path);

// --- R20 Files tab explorer helpers ----------------------------------------
// File class from the name (extension, "bom" in the name): drives the per-type icon and the Type column.
enum class FileClass { Generic, Pdf, Doc, Sheet, Csv, Zip, Gerber, Drill, Image, Email, Cad, Bom, Text };
FileClass file_class(const std::string& name);
const char* file_class_label(FileClass c);          // "PDF document", "Gerber layer", ...
// "YYYY-MM-DD HH:MM" from a zip entry's DOS date / time ("" when unset).
std::string dos_datetime_text(uint16_t date, uint16_t time);

// --- R23 job folder sidecars (specs/DOCUMENTS.md R23) --------------------------
// Files that live NEXT TO the .pcbp / "<Project>.zip" (not inside the zip) and belong to the project by name:
// machine programs (HLC master .h8h + per-machine splits .H81/.H82..; ISS master .iss + splits .i01..), the AOI
// board picture (.jpg / .png), work orders dumped from M1 ("<project> <job id> x<qty>.xlsx"), machine side files
// (.OSL .mmd .err .tbl .cpf .cmp ..) and the job archive itself.
enum class SideKind : int { HlcMaster = 0, HlcSplit, IssMaster, IssSplit, AoiImage, WorkOrder, Archive, Other };
const char* side_kind_label(SideKind k);            // "HLC program", "HLC machine split", ...
enum class SideGroup : int { Programs = 0, AoiImage, WorkOrders, Machine, Archive };
constexpr int kSideGroupCount = 5;
const char* side_group_label(SideGroup g);          // "Programs", "AOI image", "Work orders", "Machine / other files", "Job archive"
SideGroup side_group(SideKind k);
struct SideFile {
    std::string path;            // UTF-8 disk path ("" in name-only tests)
    std::string name;            // file name
    std::string stem;            // name without the extension
    std::string side;            // "SIDE1" from "[SIDE1]" / "(Side1)" after the project name ("" when none)
    SideKind kind = SideKind::Other;
    uint64_t size = 0;
    int64_t mtime = 0;           // seconds since 1970 (0 unknown)
    int master = -1;             // splits: index of their master in the list (-1: master not present)
    std::string job_id;          // work orders
    int qty = -1;                // work orders
};
// `file_name` starts with one of `projects` (case-insensitive; '.', '_' and '-' compare equal, so
// "I-1.00-2 (Side1).tbl" matches "I-1-00-2") followed by the end of the stem or a separator (space . - _ [ ( ).
// rest: what follows the matched prefix in the original name.
bool side_name_match(const std::string& file_name, const std::vector<std::string>& projects, std::string* rest = nullptr);
// "<project> <job id> x<qty>.xlsx" (also .xls / .xlsm / .csv; 'X' accepted). false: not a work-order name.
bool parse_work_order(const std::string& file_name, const std::vector<std::string>& projects, std::string& job_id, int& qty);
// Kind of a matched file from its extension (and the work-order pattern). false: excluded (.pcbp, autosave, .bak, temp).
bool classify_side(const std::string& file_name, const std::vector<std::string>& projects, const std::string& archive_name,
                   SideFile& out);
// Classify + order a folder listing: groups in SideGroup order, masters followed by their splits (split.master set),
// names otherwise sorted case-insensitively. Files not matching the project are left out.
std::vector<SideFile> build_job_folder(std::vector<SideFile> listing, const std::vector<std::string>& projects,
                                       const std::string& archive_name);
// Directory scan (one level, no recursion; cheap) + build_job_folder. Empty when `dir` is empty / missing.
std::vector<SideFile> scan_job_folder(const std::string& dir, const std::vector<std::string>& projects,
                                      const std::string& archive_name);
// The AOI picture for a program: same stem, else same side tag, else the only image. -1: none.
int aoi_for(const std::vector<SideFile>& files, int program);

} // namespace pcbp::io::jobzip
