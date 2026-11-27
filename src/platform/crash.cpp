// ============================================================================
// crash.cpp — unhandled exception / terminate / invalid-parameter handler (see crash.h).
// ============================================================================
#include "crash.h"
#include "../core/autosave.h"
#include "../core/fileio.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace pcbp::crash {
namespace {
std::function<void()> g_emergency;
unsigned g_timeout = 8000;
volatile LONG g_in_crash = 0;
std::wstring g_dump, g_log;

typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
struct DumpArgs { EXCEPTION_POINTERS* ep; DWORD tid; };

std::string module_of(void* addr, uintptr_t& off) {
    HMODULE m = nullptr;
    off = (uintptr_t)addr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m) && m) {
        wchar_t n[MAX_PATH] = {};
        GetModuleFileNameW(m, n, MAX_PATH);
        off = (uintptr_t)addr - (uintptr_t)m;
        std::wstring s = n;
        size_t k = s.find_last_of(L"\\/");
        return wide_to_utf8(k == std::wstring::npos ? s : s.substr(k + 1));
    }
    return "?";
}

DWORD WINAPI dump_thread(LPVOID p) {   // separate thread: MiniDumpWriteDump on the faulting thread is unreliable
    DumpArgs* a = (DumpArgs*)p;
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg) return 1;
    auto fn = (MiniDumpWriteDump_t)(void*)GetProcAddress(dbg, "MiniDumpWriteDump");
    if (!fn) return 1;
    HANDLE f = CreateFileW(g_dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 1;
    MINIDUMP_EXCEPTION_INFORMATION mei{a->tid, a->ep, FALSE};
    fn(GetCurrentProcess(), GetCurrentProcessId(), f, (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
       a->ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
    return 0;
}
DWORD WINAPI emergency_thread(LPVOID) {
    try { if (g_emergency) g_emergency(); } catch (...) {}
    return 0;
}

const char* av_kind(ULONG_PTR k) { return k == 0 ? "reading" : k == 1 ? "writing" : k == 8 ? "executing (DEP: call/jump to a bad address)" : "accessing"; }

// Unwinds a copy of the faulting context with the x64 unwind tables. A frame whose RIP has no
// RUNTIME_FUNCTION (leaf, or a call into garbage like address 0) is popped as a plain return
// address at [RSP]; RSP is bounds-checked against this thread's stack so the walk never faults.
USHORT walk_context(const CONTEXT& start, void** out, USHORT max) {
    CONTEXT c = start;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();   // gs:[0x30]; GCC misreads the segment read as a null array
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    const DWORD64 lo = (DWORD64)tib->StackLimit, hi = (DWORD64)tib->StackBase;
    USHORT n = 0;
    while (n < max && c.Rip) {
        out[n++] = (void*)c.Rip;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!fe) {
            if (c.Rsp < lo || c.Rsp + 8 > hi || (c.Rsp & 7)) break;
            c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8;
        } else {
            void* hd = nullptr; DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &ef, nullptr);
        }
        if (c.Rsp < lo || c.Rsp > hi) break;
    }
    return n;
}

void write_all(HANDLE f, const std::string& s) { DWORD w = 0; WriteFile(f, s.data(), (DWORD)s.size(), &w, nullptr); }

void handle(EXCEPTION_POINTERS* ep, const char* what) {
    if (InterlockedExchange(&g_in_crash, 1)) { TerminateProcess(GetCurrentProcess(), 0xC0DE); }
    // R23a: walk the FAULTING context (ep->ContextRecord), not the handler's own stack. v22 used
    // CaptureStackBackTrace(0, ...), so frames #00/#01 were crash.cpp itself and the real faulting frames
    // (here: a call into address 0 from pdf_winrt) were hidden behind the SEH dispatcher.
    void* frames[62] = {};
    USHORT nf = 0;
    if (ep && ep->ContextRecord) nf = walk_context(*ep->ContextRecord, frames, 62);
    if (!nf) nf = CaptureStackBackTrace(0, 62, frames, nullptr);
    char ts[32];
    SYSTEMTIME st; GetLocalTime(&st);
    std::snprintf(ts, sizeof ts, "%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::string dir = recovery::crash_dir();
    const std::string base = dir + "\\crash-" + ts + "-" + std::to_string(GetCurrentProcessId());
    g_dump = utf8_to_wide(base + ".dmp");
    g_log = utf8_to_wide(base + ".txt");
    // 1) text log first (cheap, survives a failing dump)
    std::string log = "PCB Plotter crash report\r\nTime: " + std::string(ts) + "\r\nReason: " + what + "\r\n";
    if (ep && ep->ExceptionRecord) {
        char b[160];
        uintptr_t off = 0;
        std::string mod = module_of(ep->ExceptionRecord->ExceptionAddress, off);
        std::snprintf(b, sizeof b, "Exception 0x%08lX at %s+0x%llx\r\n", (unsigned long)ep->ExceptionRecord->ExceptionCode, mod.c_str(), (unsigned long long)off);
        log += b;
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            std::snprintf(b, sizeof b, "Access violation %s address 0x%llx\r\n", av_kind(ep->ExceptionRecord->ExceptionInformation[0]),
                          (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
            log += b;
        }
    }
    log += "Stack (module+offset):\r\n";
    for (USHORT i = 0; i < nf; ++i) {
        uintptr_t off = 0;
        std::string mod = module_of(frames[i], off);
        char b[160]; std::snprintf(b, sizeof b, "  #%02u %s+0x%llx\r\n", (unsigned)i, mod.c_str(), (unsigned long long)off);
        log += b;
    }
    HANDLE lf = CreateFileW(g_log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lf != INVALID_HANDLE_VALUE) { write_all(lf, log); FlushFileBuffers(lf); }
    // 2) minidump on a helper thread
    DumpArgs a{ep, GetCurrentThreadId()};
    if (HANDLE t = CreateThread(nullptr, 0, dump_thread, &a, 0, nullptr)) { WaitForSingleObject(t, 20000); CloseHandle(t); }
    // 3) emergency autosave on a helper thread, bounded (the heap may be damaged)
    std::string saved = "not attempted";
    if (g_emergency) {
        if (HANDLE t = CreateThread(nullptr, 0, emergency_thread, nullptr, 0, nullptr)) {
            DWORD r = WaitForSingleObject(t, g_timeout);
            saved = r == WAIT_OBJECT_0 ? "completed" : "timed out";
            CloseHandle(t);
        }
    }
    if (lf != INVALID_HANDLE_VALUE) { write_all(lf, "Emergency autosave: " + saved + "\r\n"); CloseHandle(lf); }
    // 4) marker for the next launch
    std::string marker = "log=" + base + ".txt\r\ndump=" + base + ".dmp\r\nrecovery=" + recovery::root() + "\r\nreason=" + what + "\r\nautosave=" + saved + "\r\n";
    std::string err;
    write_file(dir + "\\last_crash.txt", marker.data(), marker.size(), err);
}

LONG WINAPI seh_filter(EXCEPTION_POINTERS* ep) { handle(ep, "unhandled exception"); return EXCEPTION_EXECUTE_HANDLER; }
void raise_with(const char* what) {
    // produce EXCEPTION_POINTERS for the dump

    {
        CONTEXT ctx{}; RtlCaptureContext(&ctx);
        EXCEPTION_RECORD er{}; er.ExceptionCode = 0xE0000001; er.ExceptionAddress = (void*)&raise_with;
        EXCEPTION_POINTERS ep{&er, &ctx};
        handle(&ep, what);
    }
    TerminateProcess(GetCurrentProcess(), 3);
}
void on_terminate() { raise_with("std::terminate (uncaught C++ exception)"); }
void on_invalid_param(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { raise_with("CRT invalid parameter"); }
void on_purecall() { raise_with("pure virtual call"); }
void on_abort(int) { raise_with("abort()"); }
} // namespace

namespace {
DWORD g_tls_ctx = TLS_OUT_OF_INDEXES, g_tls_code = TLS_OUT_OF_INDEXES;
bool recoverable(DWORD c) {
    return c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_PRIV_INSTRUCTION ||
           c == EXCEPTION_INT_DIVIDE_BY_ZERO || c == EXCEPTION_IN_PAGE_ERROR || c == EXCEPTION_ARRAY_BOUNDS_EXCEEDED ||
           c == EXCEPTION_DATATYPE_MISALIGNMENT || c == EXCEPTION_INT_OVERFLOW;
}
LONG WINAPI guard_veh(EXCEPTION_POINTERS* ep) {
    if (g_tls_ctx == TLS_OUT_OF_INDEXES || !ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* saved = (CONTEXT*)TlsGetValue(g_tls_ctx);
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!saved || !recoverable(code)) return EXCEPTION_CONTINUE_SEARCH;
    TlsSetValue(g_tls_code, (void*)(uintptr_t)code);
    *ep->ContextRecord = *saved;          // resume right after RtlCaptureContext in guarded_call
    return EXCEPTION_CONTINUE_EXECUTION;
}
void guard_init() {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        g_tls_ctx = TlsAlloc(); g_tls_code = TlsAlloc();
        AddVectoredExceptionHandler(1, guard_veh);
        return TRUE;
    }, nullptr, nullptr);
}
} // namespace

// Must never be inlined: the VEH resumes at the RtlCaptureContext return inside THIS frame.
#if defined(_MSC_VER)
#define PCBP_NOINLINE __declspec(noinline)
#else
#define PCBP_NOINLINE __attribute__((noinline))
#endif
PCBP_NOINLINE bool guarded_call(void (*fn)(void*), void* arg, unsigned long* code) {
    guard_init();
    if (g_tls_ctx == TLS_OUT_OF_INDEXES || g_tls_code == TLS_OUT_OF_INDEXES) { fn(arg); return true; }
    CONTEXT* volatile prev = (CONTEXT*)TlsGetValue(g_tls_ctx);   // nesting: restore the outer guard
    alignas(16) CONTEXT ctx;
    TlsSetValue(g_tls_code, nullptr);
    RtlCaptureContext(&ctx);                   // a fault inside fn() resumes HERE with code set
    const DWORD c = (DWORD)(uintptr_t)TlsGetValue(g_tls_code);
    if (c) {
        TlsSetValue(g_tls_ctx, prev);
        TlsSetValue(g_tls_code, nullptr);
        if (code) *code = c;
        return false;
    }
    TlsSetValue(g_tls_ctx, &ctx);
    try { fn(arg); } catch (...) { TlsSetValue(g_tls_ctx, prev); throw; }
    TlsSetValue(g_tls_ctx, prev);
    if (code) *code = 0;
    return true;
}

std::string exception_name(unsigned long c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error (file or network read failed)";
    default: { char b[32]; std::snprintf(b, sizeof b, "exception 0x%08lX", c); return b; }
    }
}

void install() {
    guard_init();
    SetUnhandledExceptionFilter(seh_filter);
    std::set_terminate(on_terminate);
    _set_invalid_parameter_handler(on_invalid_param);
    _set_purecall_handler(on_purecall);
    signal(SIGABRT, on_abort);
}
void set_emergency_save(std::function<void()> fn, unsigned timeout_ms) { g_emergency = std::move(fn); g_timeout = timeout_ms; }
void test_crash() { volatile int* p = nullptr; *p = 42; std::abort(); }

} // namespace pcbp::crash
#else
namespace pcbp::crash {
void install() {}
void set_emergency_save(std::function<void()>, unsigned) {}
void test_crash() { volatile int* p = nullptr; *p = 42; __builtin_trap(); }
bool guarded_call(void (*fn)(void*), void* arg, unsigned long* code) { fn(arg); if (code) *code = 0; return true; }
std::string exception_name(unsigned long c) { return "exception " + std::to_string(c); }
}
#endif
