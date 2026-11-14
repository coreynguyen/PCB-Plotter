// R23a: Windows-only (mingw + Wine) check of crash::guarded_call: hardware faults inside the guard return
// false with the code instead of killing the process; nesting, C++ exceptions and normal returns work.
#include "../src/platform/crash.h"
#include <cstdio>
#include <stdexcept>
#include <windows.h>
#include <string>
namespace pcbp { bool atomic_write_file(const std::string&, const void*, unsigned long long, std::string&) { return false; } }   // link stub (autosave)
static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)
typedef void (*fnp)();
int main() {
    pcbp::crash::install();
    unsigned long code = 1;
    volatile int sink = 0;
    CHECK(pcbp::crash::guarded([&] { sink = 1; }, &code) && code == 0 && sink == 1);
    CHECK(!pcbp::crash::guarded([&] { volatile int* p = nullptr; *p = 5; }, &code) && code == EXCEPTION_ACCESS_VIOLATION);
    // the v22 crash shape: a call to a non-executable address
    CHECK(!pcbp::crash::guarded([&] { volatile fnp f = (fnp)(uintptr_t)0x10000; f(); }, &code) && code == EXCEPTION_ACCESS_VIOLATION);
    // nested: inner fault does not unwind the outer guard
    bool inner = true;
    CHECK(pcbp::crash::guarded([&] { inner = pcbp::crash::guarded([] { volatile int* p = nullptr; *p = 1; }); sink = 7; }, &code) && !inner && sink == 7);
    bool caught = false;
    try { pcbp::crash::guarded([] { throw std::runtime_error("x"); }); } catch (const std::exception&) { caught = true; }
    CHECK(caught);
    for (int i = 0; i < 1000; ++i) CHECK(!pcbp::crash::guarded([] { volatile int z = 0; volatile int* p = (int*)(uintptr_t)(8 + z); *p = 1; }));
    std::printf("test_seh_guard: %s\n", fails ? "FAILED" : "PASS");
    return fails;
}
