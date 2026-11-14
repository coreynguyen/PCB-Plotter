#pragma once
// ============================================================================
// crash.h — crash handler (REVIEW_V11 §4, R12). Windows only.
//   install(): SetUnhandledExceptionFilter + std::set_terminate + CRT invalid-parameter / purecall / SIGABRT
//   On a crash: minidump (MiniDumpWriteDump via dynamically loaded dbghelp.dll) + text log (exception, module
//   offsets, CaptureStackBackTrace frames) into Recovery\crash\, then the emergency callback (autosave of
//   unsaved documents) on a separate thread with a timeout, then "last_crash.txt" for the next launch.
// ============================================================================
#include <functional>
#include <string>

namespace pcbp::crash {
void install();
// Called from the crash handler on a helper thread (bounded by timeout_ms). Must not take UI locks.
void set_emergency_save(std::function<void()> fn, unsigned timeout_ms = 8000);
// PCBP_TEST_CRASH=1 automation: deliberately crash (access violation) to test the handler.
[[noreturn]] void test_crash();

// R23a: runs fn(arg) on the calling thread; a hardware fault inside it (access violation, illegal
// instruction, divide by zero, in-page error, DEP) is caught by a vectored handler that resumes at the
// call site instead of reaching the crash handler. Returns false and the exception code in *code on a
// fault. C++ exceptions are NOT caught here (wrap fn in try/catch). Destructors of the abandoned frames do
// not run (memory leaks; a lock held by the faulting code stays held), so use it only around
// self-contained work such as previews/decoders whose result can be discarded. No-op guard off Windows.
bool guarded_call(void (*fn)(void*), void* arg, unsigned long* code = nullptr);
template <class F> bool guarded(F&& f, unsigned long* code = nullptr) {
    return guarded_call([](void* p) { (*static_cast<F*>(p))(); }, (void*)&f, code);
}
// "access violation" / "illegal instruction" / ... for an exception code
std::string exception_name(unsigned long code);
} // namespace pcbp::crash
