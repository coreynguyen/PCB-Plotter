/* ===========================================================================
 * pcbp_plugin.h — PCB Plotter plugin SDK (stable C ABI, version 1)
 *
 * A plugin is a DLL placed in "<exe dir>\plugins\". It exports ONE function:
 *
 *     PCBP_EXPORT int pcbp_plugin_enumerate(uint32_t host_api_version,
 *                                           const pcbp_plugin_desc** list,
 *                                           uint32_t* count);
 *
 * returning a static array of descriptors. Each descriptor is an importer
 * (file -> project data) or an exporter (job/project -> file bytes).
 *
 * All data crosses the boundary as UTF-8 JSON (schema = the native .pcbp
 * project format, see docs/PLUGINS.md), so plugins can be written in any
 * language that can produce a C-callable DLL (C, C++, Rust, Zig, ...).
 * Memory returned by a plugin is released by the host through the
 * descriptor's free_mem callback.
 *
 * Units in JSON: millimetres, degrees CCW.
 * ======================================================================== */
#ifndef PCBP_PLUGIN_H
#define PCBP_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define PCBP_EXPORT __declspec(dllexport)
#  define PCBP_CALL __cdecl
#else
#  define PCBP_EXPORT __attribute__((visibility("default")))
#  define PCBP_CALL
#endif

#define PCBP_PLUGIN_API_VERSION 1u

enum pcbp_plugin_kind {
    PCBP_KIND_IMPORTER = 1,
    PCBP_KIND_EXPORTER = 2
};

/* What an importer produces. */
enum pcbp_import_mode {
    PCBP_IMPORT_MERGE   = 0,   /* data merged into the open project (CAD, centroid, BOM) */
    PCBP_IMPORT_PROJECT = 1    /* a complete project, opened as a new document (.cpf)    */
};

/* What an exporter consumes. */
enum pcbp_export_input {
    PCBP_EXPORT_MACHINE_JOB = 1,   /* machine program: MachineJob JSON (per model point) */
    PCBP_EXPORT_PROJECT     = 2    /* whole project JSON (CAD / report exports)          */
};

enum pcbp_log_level { PCBP_LOG_INFO = 0, PCBP_LOG_WARN = 1, PCBP_LOG_ERROR = 2 };

typedef struct pcbp_host {
    uint32_t api_version;          /* PCBP_PLUGIN_API_VERSION of the host */
    void*    ctx;
    void (PCBP_CALL *log)(void* ctx, int level, const char* utf8_msg);
    void (PCBP_CALL *progress)(void* ctx, double fraction, const char* utf8_msg);
} pcbp_host;

typedef struct pcbp_plugin_desc {
    uint32_t    struct_size;       /* sizeof(pcbp_plugin_desc) */
    uint32_t    api_version;       /* PCBP_PLUGIN_API_VERSION   */
    const char* id;                /* unique, e.g. "acme.fuji-nxt" */
    const char* name;              /* menu text, e.g. "Fuji NXT program" */
    const char* vendor;
    uint32_t    kind;              /* pcbp_plugin_kind */
    const char* file_filter;       /* "*.ab;*.cd" */
    const char* default_ext;       /* "ab" (no dot) */
    /* JSON array of option descriptors:
       [{"key":"units","label":"Units","type":"enum","choices":["mm","inch"],"default":"mm"},
        {"key":"header","label":"Write header","type":"bool","default":true}]          */
    const char* options_schema_json;

    /* ---- importer ---- */
    uint32_t import_mode;          /* pcbp_import_mode */
    /* return 0..100 confidence that this file is ours (NULL = extension match only) */
    int (PCBP_CALL *sniff)(const char* path_utf8, const uint8_t* head, size_t head_size);
    /* returns 1 on success; *out_json = project JSON (may be partial for MERGE) */
    int (PCBP_CALL *import_file)(const pcbp_host* host, const char* path_utf8,
                                 const uint8_t* data, size_t size,
                                 const char* options_json,
                                 char** out_json, char** out_error);

    /* ---- exporter ---- */
    uint32_t export_input;         /* pcbp_export_input */
    int (PCBP_CALL *export_file)(const pcbp_host* host, const char* input_json,
                                 const char* options_json,
                                 uint8_t** out_data, size_t* out_size, char** out_error);

    void (PCBP_CALL *free_mem)(void* p);
} pcbp_plugin_desc;

typedef int (PCBP_CALL *pcbp_plugin_enumerate_fn)(uint32_t host_api_version,
                                                  const pcbp_plugin_desc** list,
                                                  uint32_t* count);

#ifdef __cplusplus
}
#endif
#endif /* PCBP_PLUGIN_H */
