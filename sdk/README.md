# PCB Plotter plugin SDK

External importers and exporters are plain DLLs placed in `<PCBPlotter.exe dir>\plugins\`.
They are written against **`sdk/pcbp_plugin.h` only** (stable C ABI, version 1) and never
link against PCB Plotter itself. Any language that can produce a C-callable DLL works.

## ABI

A plugin DLL exports exactly one function:

```c
PCBP_EXPORT int PCBP_CALL pcbp_plugin_enumerate(uint32_t host_api_version,
                                                const pcbp_plugin_desc** list,
                                                uint32_t* count);
```

Return `1` and point `*list` at a **static** array of `*count` descriptors (they must stay
valid while the DLL is loaded; the host never unloads plugin DLLs). Return `0` to decline,
e.g. when `host_api_version < PCBP_PLUGIN_API_VERSION`.

Each `pcbp_plugin_desc`:

| field | meaning |
|---|---|
| `struct_size` | `sizeof(pcbp_plugin_desc)`; also the array stride |
| `api_version` | `PCBP_PLUGIN_API_VERSION` (1). Other values are rejected |
| `id` | unique id, **vendor-prefixed** (`acme.fuji-nxt`). Ids that collide with a built-in (`juki.h8h`, `gencad`, ...) are rejected |
| `name`, `vendor` | menu text / plugin manager |
| `kind` | `PCBP_KIND_IMPORTER` or `PCBP_KIND_EXPORTER` |
| `file_filter`, `default_ext` | `"*.ab;*.cd"`, `"ab"` |
| `options_schema_json` | JSON array of options shown in the options dialog (see below) |
| `import_mode` | `PCBP_IMPORT_MERGE` (merged into the open project) or `PCBP_IMPORT_PROJECT` (opens a new document) |
| `sniff` | optional; return 0..100 confidence from path + first 8 KiB. NULL = extension match (50) |
| `import_file` | importer entry point, returns 1 on success, `*out_json` = project JSON |
| `export_input` | `PCBP_EXPORT_MACHINE_JOB` or `PCBP_EXPORT_PROJECT` |
| `export_file` | exporter entry point, returns 1 on success, `*out_data/*out_size` = file bytes |
| `free_mem` | frees every buffer the plugin returned (`out_json`, `out_data`, `out_error`). Required: the host never calls `free()` on plugin memory (different CRTs) |

`pcbp_host` (passed to import/export calls) gives `log(ctx, level, utf8)` (shown in the
import report / export error) and `progress(ctx, fraction, utf8)` (status bar).

Errors: return 0 and optionally set `*out_error` to a UTF-8 message allocated by the plugin.
Calls are guarded with SEH in MSVC builds, so a crash is reported instead of killing the
app — but do not rely on it.

Plugins can be disabled per id in the plugin manager (`disabled_plugins` in PCBPlotter.ini);
load diagnostics appear there too.

### Options

```json
[{"key":"units","label":"Units","type":"enum","choices":["mm","inch"],"default":"mm"},
 {"key":"header","label":"Write header","type":"bool","default":true},
 {"key":"decimals","label":"Decimals","type":"int","default":3}]
```

Types: `bool`, `int`, `double`, `string`, `enum`, `path`; optional `tooltip`. The chosen values
arrive as `options_json`, a JSON object: bools as `true/false`, int/double as numbers,
everything else as strings.

## JSON payloads

All text is UTF-8. Lengths are **millimetres**, angles **degrees CCW**, +Y up.

**Project** (importer output, `PCBP_EXPORT_PROJECT` input) — the native `.pcbp` format,
written/read by `src/core/project_io.cpp` (`project_to_json` / `project_from_json`), which is
the authoritative field list. Top level:

```
{ "format":"PCBPlotter.Project", "version":1,
  "assembly":{name, number, description, customer, assembly_rev, ...},
  "origin":[x,y], "board_frame":[[x0,y0,x1,y1,...], ...], "panel_frame":[...],
  "packages":[{name, desc, smt, leads, height, pads:[{at:[x,y], w, h, rot, shape, drill, pin}], body:[gprim...]}],
  "parts":[{number, desc, package, polarized, manf_pn, vend_pn, process, height, ...}],
  "components":[{ref, at:[x,y], rot, side:"T"|"B", number, package, feature, dnp, point, ...}],
  "images":[{name, offset:[x,y], rot, root, xout}], "layers":[...], "factory_model":{...} }
```

Importers may return a **partial** project: `format`/`version` are added by the host if
missing. In MERGE mode the host merges: components by `ref` (update or add), packages and
parts by name, layers appended, `board_frame` replaced if present, non-empty `assembly`
fields copied. `shape`: 0 round, 1 square, 2 rect, 3 oblong, 4 octagon. `feature`: 0
component, 1 fiducial, 2 bad mark, 3 hole.

**MachineJob** (`PCBP_EXPORT_MACHINE_JOB` input) — `machine_job_to_json` in
`src/plugins/plugin_registry.cpp`, coordinates already in machine space:

```
{ "format":"PCBPlotter.MachineJob", "version":1, "program_name", "assembly", "assembly_rev",
  "customer", "point_name", "machine", "board_w", "board_h", "board_t", "bottom_side", "single_board",
  "placements":[{ref, part, package, desc, x, y, rot, circuit, skip, polarized, smt, leads, height}],
  "parts":[{part, package, desc, length, width, height, leads, polarized, smt, count}],
  "fiducials":[{name, x, y, diameter, circuit, local}],
  "circuits":[{index, name, x, y, rot, skip}],
  "options":{ "key":"value", ... } }   // machine-setup options of the model point
```

## Sample

`sample_plugin/sample_plugin.cpp` (self-contained, includes a ~150-line JSON reader):

* exporter `sample.semicolon` — MachineJob → `REF;X;Y;ROT;PART` lines (options `decimals`,
  `include_skipped`)
* importer `sample.simplexy` — lines `REF X Y ROT [PART]` in mm → components (MERGE);
  `#` / `;` comment lines; malformed lines fail with a line number.

### Build

mingw-w64:

```sh
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static -I sdk sdk/sample_plugin/sample_plugin.cpp -o sample_plugin.dll
```

MSVC (Developer prompt):

```bat
cl /O2 /LD /EHsc /I sdk sdk\sample_plugin\sample_plugin.cpp /Fe:sample_plugin.dll
```

A VS2026 DLL project for the sample will be added to the solution (Configuration type
*Dynamic Library*, include dir `sdk`, no extra libs). Copy the DLL to `plugins\` next to
`PCBPlotter.exe` and restart; the plugin manager lists what was loaded.

On Linux the same source builds as a shared object for testing
(`g++ -std=c++17 -shared -fPIC -I sdk ... -o sample_plugin.so`); `tests/test_plugins.cpp`
loads it with `dlopen` and exercises both entry points.
