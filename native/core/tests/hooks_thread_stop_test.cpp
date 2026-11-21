// Host model of the production stop loop and position reads, extracted by test-hooks-thread-stop.sh.
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <vector>
using DWORD = uint32_t;
constexpr DWORD ERROR_SUCCESS = 0;
constexpr DWORD STILL_ACTIVE = 259, WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xffffffff;
using LONG = int32_t;
using ULONG = uint32_t;
#define NTAPI
struct FakeThread { DWORD id, exit_code, wait; bool stop, query; unsigned waits = 0; bool started = true; bool basic = true;
                    uintptr_t rip = 0; unsigned contexts = 0; };
using HANDLE = FakeThread*;
using HMODULE = void*;
using FARPROC = void (*)();
HMODULE GetModuleHandleW(const wchar_t*) { return nullptr; }
FARPROC GetProcAddress(HMODULE, const char*) { return nullptr; }
constexpr DWORD CONTEXT_CONTROL = 1;
struct CONTEXT { DWORD ContextFlags; uint64_t Rip; };
// Under Wine, GetThreadContext on a thread that has not started waits forever: here it fails the test instead.
bool g_blocked = false;
bool GetThreadContext(HANDLE h, CONTEXT* c) {
    ++h->contexts;
    if (!h->started) { g_blocked = true; return false; }
    c->Rip = h->rip;
    return true;
}
// NtQueryInformationThread(ThreadBasicInformation): the TEB is the second field; a thread that has not started has none
LONG NTAPI FakeQuery(HANDLE h, ULONG cls, void* info, ULONG size, ULONG*) {
    if (cls != 0 || size < 16 || !h->basic) return LONG(0xC0000022);  // STATUS_ACCESS_DENIED
    static_cast<void**>(info)[1] = h->started ? reinterpret_cast<void*>(0x7ff000) : nullptr;
    return 0;
}
DWORD GetThreadId(HANDLE h) { return h->id; }
DWORD GetLastError() { return 5; }
bool GetExitCodeThread(HANDLE h, DWORD* code) { if (!h->query) return false; *code = h->exit_code; return true; }
DWORD SuspendThread(HANDLE h) { return h->stop ? 0 : DWORD(-1); }
DWORD WaitForSingleObject(HANDLE h, DWORD) { ++h->waits; return h->wait; }
#include "hooks_stop_loop.g.h"
int main() {
    unsigned pass = 0, fail = 0;
    // Query status must never substitute for object termination, including return code 259.
    for (bool unknown : {false, true}) for (bool checked : {false, true})
        for (DWORD wait : {WAIT_OBJECT_0, WAIT_TIMEOUT, WAIT_FAILED})
            for (DWORD code : {DWORD(0), STILL_ACTIVE, DWORD(0xc0000005)}) for (bool query : {false, true}) {
                FakeThread h{unknown ? 0u : 42u, code, wait, false, query};
                std::vector<Stopped> th(1);
                th[0].h = &h; th[0].unknown_id = unknown; th[0].skip = unknown;
                FreezeReport rep;
                uint32_t unchecked = 0;
                stop_loop(th, rep, checked ? &unchecked : nullptr);
                bool ended = wait == WAIT_OBJECT_0;
                bool ok = rep.exited == size_t(ended) && rep.stopped == 0 &&
                    rep.unstopped == size_t(!unknown && !ended) &&
                    rep.no_context == size_t(unknown && checked && !ended);
                if (ok) ++pass; else ++fail;
            }
    // The calling thread is never suspended or waited; successful stops need no wait.
    for (bool self : {false, true}) {
        FakeThread h{42, 0, WAIT_TIMEOUT, true, true};
        std::vector<Stopped> th(1); th[0].h = &h; th[0].skip = self;
        FreezeReport rep; stop_loop(th, rep, nullptr);
        if (rep.stopped == size_t(!self) && rep.exited == 0 && h.waits == 0) ++pass; else ++fail;
    }
    // Positions: a thread that has not started is never asked for its context (it would wait forever under Wine) and
    // counts as not started, not as unchecked; a failed query falls back to GetThreadContext; without the query
    // function (an ntdll without it) every stopped thread is asked, as before.
    for (int q = 0; q < 2; ++q) {
        FakeThread a{1, 0, WAIT_TIMEOUT, true, true}, b{2, 0, WAIT_TIMEOUT, true, true}, c{3, 0, WAIT_TIMEOUT, true, true},
            d{4, 0, WAIT_TIMEOUT, true, true};
        a.rip = 0x1000; b.started = false; c.basic = false; c.rip = 0x2000; d.started = false;
        std::vector<Stopped> th(4);
        th[0].h = &a; th[1].h = &b; th[2].h = &c; th[3].h = &d;
        for (int i = 0; i < 3; ++i) th[i].suspended = true;  // d was not stopped: never touched
        FreezeReport rep;
        g_blocked = false;
        read_stopped_positions(th, rep, q ? FakeQuery : nullptr);
        const bool ok = q ? (!g_blocked && b.contexts == 0 && rep.unstarted == 1 && rep.no_context == 0 &&
                             th[0].has_rip && th[0].rip == 0x1000 && !th[1].has_rip && th[2].has_rip && th[2].rip == 0x2000 &&
                             d.contexts == 0)
                          : (g_blocked && b.contexts == 1 && rep.unstarted == 0 && rep.no_context == 1 && d.contexts == 0);
        if (ok) ++pass; else ++fail, std::printf("FAIL positions (query %d)\n", q);
    }
    if (!thread_not_started(nullptr, nullptr) && query_thread_fn() == nullptr) ++pass; else ++fail;
    FreezeReport rep;
    for (unsigned i = 0; i < 100; ++i) rep.note(i, 5, STILL_ACTIVE, false);
    if (rep.why_n == 4 && rep.why[3].tid == 3) ++pass; else ++fail;
    std::printf("hooks_thread_stop_test: %u PASS, %u FAIL\n", pass, fail);
    return fail != 0;
}
