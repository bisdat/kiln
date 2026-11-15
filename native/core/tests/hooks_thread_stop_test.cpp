// Host model of the production stop loop, extracted by test-hooks-thread-stop.sh.
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <vector>
using DWORD = uint32_t;
constexpr DWORD ERROR_SUCCESS = 0;
constexpr DWORD STILL_ACTIVE = 259, WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xffffffff;
struct FakeThread { DWORD id, exit_code, wait; bool stop, query; unsigned waits = 0; };
using HANDLE = FakeThread*;
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
    FreezeReport rep;
    for (unsigned i = 0; i < 100; ++i) rep.note(i, 5, STILL_ACTIVE, false);
    if (rep.why_n == 4 && rep.why[3].tid == 3) ++pass; else ++fail;
    std::printf("hooks_thread_stop_test: %u PASS, %u FAIL\n", pass, fail);
    return fail != 0;
}
