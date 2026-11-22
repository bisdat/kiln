// SPDX-License-Identifier: MIT
// kiln.client's local input observation on the Windows code path (run by test-client-input-wine.sh under headless Wine;
// native Windows runs it the same way): client_input.cpp's entry and its SEH-guarded copies (guard.cpp as it is) in
// front of the building-input producer (client_building_input.cpp as it is), on stand-in ClientPlayerInput pages that
// are readable and writable, unreadable, or read-only. A fault must end the request cancelled and leave the process
// running; the Q32.32 and quaternion conversion must give the host's numbers under the Windows ABI. The observation
// point's visit() reads the version as the game does (the client world's step through the input object) on stand-in
// pages too: an unreadable input object or world skips the visit and is counted, never a crash.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include "crash.h"
#include "services/client_input.h"

namespace kiln::crash {
int filter(EXCEPTION_POINTERS*, Capture*, unsigned long*) { return EXCEPTION_EXECUTE_HANDLER; }
void report(const Capture&, const char*, const void*) {}
}  // namespace kiln::crash

namespace bi = kiln::building_input;
namespace ci = kiln::client_input;
namespace {
int passed = 0, failed = 0;
void check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    ok ? ++passed : ++failed;
}
constexpr bi::Layout kLayout{760, 936, 500, 160, 0, 1, 2, 3, 4, 5, 7, 40};
constexpr KilnOwner kOwner = 5;
constexpr KilnEntity kPlayer = 77;
constexpr uint32_t kSession = 3;
const bi::Env kEnv{true, true, true, kSession, nullptr};

uint8_t* page(DWORD protect) {
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD old = 0;
    if (p && protect != PAGE_READWRITE) VirtualProtect(p, 4096, protect, &old);
    return p;
}
uint32_t submit(uint32_t action) {
    KilnBuildingInput in{};
    in.size = sizeof in;
    in.action = action;
    in.player = kPlayer;
    in.position[0] = 1.5;
    in.position[1] = -0.25;
    in.position[2] = 1.0 / 8589934592.0;  // 2^-33: half a Q32.32 unit
    for (double& r : in.rotation) r = 3.0;
    in.scale[0] = in.scale[1] = in.scale[2] = 1.0;
    uint32_t id = 0;
    return bi::submit(kEnv, kOwner, 0, &in, &id) == KILN_OK ? id : 0;
}
uint32_t state(uint32_t id) {
    uint32_t s = 0;
    bi::status(kEnv, kOwner, id, &s);
    return s;
}
uint64_t digital(const uint8_t* r) {
    uint64_t v;
    std::memcpy(&v, r + kLayout.digital, 8);
    return v;
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered (the MSVC CRT refuses a line-buffered stream without a buffer)
    bi::reset_for_tests();
    bi::bind(&kLayout);

    uint8_t* rw = page(PAGE_READWRITE);
    uint8_t* none = page(PAGE_NOACCESS);
    uint8_t* ro = page(PAGE_READONLY);
    check(rw && none && ro, "pages");
    const bi::Memory m = ci::guarded_memory();
    uint64_t v = 0;
    check(m.read(m.ctx, &v, reinterpret_cast<uintptr_t>(rw), 8) && m.write(m.ctx, reinterpret_cast<uintptr_t>(rw), &v, 8),
          "guarded copies: a readable, writable page");
    check(!m.read(m.ctx, &v, reinterpret_cast<uintptr_t>(none), 8) && !m.write(m.ctx, reinterpret_cast<uintptr_t>(none), &v, 8) &&
              m.read(m.ctx, &v, reinterpret_cast<uintptr_t>(ro), 8) && !m.write(m.ctx, reinterpret_cast<uintptr_t>(ro), &v, 8),
          "guarded copies: an access violation answers false (no access; write to read-only)");

    // The selection: only the published local player's input reaches the producer.
    uint32_t id = submit(KILN_BUILDING_ACTION_PLACE);
    ci::observe(reinterpret_cast<uintptr_t>(rw), kPlayer, 1);
    check(state(id) == KILN_BUILDING_INPUT_QUEUED && digital(rw) == 0, "selection: nothing reaches the producer before a selection");
    ci::publish(kPlayer, kSession);
    ci::observe(reinterpret_cast<uintptr_t>(rw), kPlayer + 1, 1);
    check(state(id) == KILN_BUILDING_INPUT_QUEUED && digital(rw) == 0, "selection: another entity's input is not the local one");
    ci::observe(reinterpret_cast<uintptr_t>(rw), kPlayer, 1);
    int64_t pos[3];
    float rot[4];
    std::memcpy(pos, rw + kLayout.cursor, sizeof pos);
    std::memcpy(rot, rw + kLayout.cursor + 24, sizeof rot);
    check(state(id) == KILN_BUILDING_INPUT_PRESSED && digital(rw) == 3, "observe: the local input is pressed");
    check(pos[0] == 6442450944ll && pos[1] == -1073741824ll && pos[2] == 1 && rot[0] == 0.5f && rot[3] == 0.5f,
          "observe: Q32.32 and the normalized quaternion as on the host");
    ci::observe(reinterpret_cast<uintptr_t>(rw), kPlayer, 2);
    check(state(id) == KILN_BUILDING_INPUT_DISPATCHED && digital(rw) == 8, "observe: released at the next version");

    const auto faults0 = bi::stats().faults;
    id = submit(KILN_BUILDING_ACTION_PLACE);
    ci::observe(reinterpret_cast<uintptr_t>(none), kPlayer, 3);
    check(state(id) == KILN_BUILDING_INPUT_CANCELLED && bi::stats().faults == faults0 + 1,
          "fault: an unreadable input ends the request cancelled, the process runs on");
    id = submit(KILN_BUILDING_ACTION_PLACE);
    ci::observe(reinterpret_cast<uintptr_t>(ro), kPlayer, 4);
    check(state(id) == KILN_BUILDING_INPUT_CANCELLED && bi::stats().faults == faults0 + 2,
          "fault: a read-only input (the write faults) ends cancelled");

    std::memset(rw, 0, 4096);
    id = submit(KILN_BUILDING_ACTION_PLACE);
    ci::observe(reinterpret_cast<uintptr_t>(rw), kPlayer, 5);
    DWORD old = 0;
    VirtualProtect(rw, 4096, PAGE_READONLY, &old);
    ci::observe(reinterpret_cast<uintptr_t>(rw), kPlayer, 6);
    check(state(id) == KILN_BUILDING_INPUT_CANCELLED && bi::stats().faults == faults0 + 3 && digital(rw) == 3,
          "fault: a release that faults ends cancelled and frees the slot");
    check(submit(KILN_BUILDING_ACTION_UNDO) != 0, "fault: the slot is free");

    // visit(): the observation point's handler. Stand-ins: the client game's input object (the client world's pointer
    // at +8, as client/input.world fixes it), the client world (its step at client/input.step), the ClientPlayerInput.
    {
        const ci::Site site{8, 0x6c0};
        uint8_t* input = page(PAGE_READWRITE);
        uint8_t* world = page(PAGE_READWRITE);
        uint8_t* rec = page(PAGE_READWRITE);
        check(input && world && rec, "visit: pages");
        const uintptr_t wp = reinterpret_cast<uintptr_t>(world);
        std::memcpy(input + 8, &wp, sizeof wp);
        uint32_t step = 41;
        std::memcpy(world + 0x6c0, &step, 4);
        bi::reset_for_tests();  // the slot holds the queued undo above: start from an empty one
        bi::bind(&kLayout);
        ci::reset_stats_for_tests();
        ci::publish(kPlayer, kSession);
        id = submit(KILN_BUILDING_ACTION_PLACE);
        ci::visit(reinterpret_cast<uintptr_t>(input), reinterpret_cast<uintptr_t>(rec), kPlayer, site);
        check(state(id) == KILN_BUILDING_INPUT_PRESSED && digital(rec) == 3, "visit: the local input is pressed at step 41");
        ci::visit(reinterpret_cast<uintptr_t>(input), reinterpret_cast<uintptr_t>(rec), kPlayer, site);
        check(state(id) == KILN_BUILDING_INPUT_PRESSED && digital(rec) == 3, "visit: the same step does not release it");
        step = 42;
        std::memcpy(world + 0x6c0, &step, 4);
        ci::visit(reinterpret_cast<uintptr_t>(input), reinterpret_cast<uintptr_t>(rec), kPlayer, site);
        check(state(id) == KILN_BUILDING_INPUT_DISPATCHED && digital(rec) == 8, "visit: released at the next step");
        check(ci::stats().visits == 3 && ci::stats().faults == 0, "visit: three visits, no fault");

        std::memset(rec, 0, 4096);
        id = submit(KILN_BUILDING_ACTION_PLACE);
        ci::visit(reinterpret_cast<uintptr_t>(none), reinterpret_cast<uintptr_t>(rec), kPlayer, site);
        const uintptr_t gone = reinterpret_cast<uintptr_t>(none);
        std::memcpy(input + 8, &gone, sizeof gone);
        ci::visit(reinterpret_cast<uintptr_t>(input), reinterpret_cast<uintptr_t>(rec), kPlayer, site);
        const uintptr_t null_world = 0;
        std::memcpy(input + 8, &null_world, sizeof null_world);
        ci::visit(reinterpret_cast<uintptr_t>(input), reinterpret_cast<uintptr_t>(rec), kPlayer, site);
        check(state(id) == KILN_BUILDING_INPUT_QUEUED && digital(rec) == 0 && ci::stats().faults == 3 && ci::stats().visits == 6,
              "visit: an unreadable input object, an unreadable world or no world skips the visit (counted); the request waits");
        std::memcpy(input + 8, &wp, sizeof wp);
        ci::visit(reinterpret_cast<uintptr_t>(input), reinterpret_cast<uintptr_t>(rec), kPlayer + 1, site);
        check(state(id) == KILN_BUILDING_INPUT_QUEUED && digital(rec) == 0, "visit: another entity's input is not the local one");
        ci::visit(reinterpret_cast<uintptr_t>(input), 0, kPlayer, site);
        check(state(id) == KILN_BUILDING_INPUT_QUEUED, "visit: no record, nothing done");
        ci::visit(reinterpret_cast<uintptr_t>(input), reinterpret_cast<uintptr_t>(rec), kPlayer, site);
        check(state(id) == KILN_BUILDING_INPUT_PRESSED && digital(rec) == 3, "visit: then the local input is pressed");
        uint32_t cancelled = 0;
        bi::cancel(kEnv, kOwner, id, &cancelled);
        step = 43;
        std::memcpy(world + 0x6c0, &step, 4);
        ci::visit(reinterpret_cast<uintptr_t>(input), reinterpret_cast<uintptr_t>(rec), kPlayer, site);
        check(cancelled == 1 && state(id) == KILN_BUILDING_INPUT_CANCELLED && digital(rec) == 8,
              "visit: a cancelled press is still released at the next step");
    }

    // The lazy producer on Windows threads: a stand-in game thread observes the input only once the installer has
    // hooked it (a link runs only once placed), all the time; a mod thread's first submit installs, binds, and its
    // request is pressed and released by that thread's visits.
    {
        static std::atomic<bool> hooked{false};
        static std::atomic<int> calls{0};
        bi::reset_for_tests();
        ci::publish(kPlayer, kSession);
        bi::set_installer(+[](bi::Layout* l, const char**) {
            calls.fetch_add(1);
            Sleep(20);  // an install takes a while: the game thread runs on meanwhile
            *l = kLayout;
            hooked.store(true);
            return bi::Install::Bound;
        });
        uint8_t* rec = page(PAGE_READWRITE);
        std::atomic<bool> stop{false};
        std::atomic<uint32_t> version{100};
        std::atomic<uint64_t> visits{0};
        std::thread game([&] {
            while (!stop.load()) {
                if (hooked.load()) {
                    ci::observe(reinterpret_cast<uintptr_t>(rec), kPlayer, version.load());
                    visits.fetch_add(1);
                }
                Sleep(0);
            }
        });
        Sleep(30);
        check(calls.load() == 0 && !bi::bound(nullptr) && visits.load() == 0, "lazy: nothing hooked before the first submit");
        id = submit(KILN_BUILDING_ACTION_PLACE);
        auto wait_for = [&](uint32_t want) {
            for (int i = 0; i < 2000 && state(id) != want; ++i) Sleep(1);
            return state(id) == want;
        };
        const bool pressed = id != 0 && wait_for(KILN_BUILDING_INPUT_PRESSED);
        version.store(101);
        const bool released = wait_for(KILN_BUILDING_INPUT_DISPATCHED);
        check(pressed && released && calls.load() == 1 && bi::bound(nullptr) && digital(rec) == 8,
              "lazy: the first submit installs once; the game thread's visits press and release it");
        check(submit(KILN_BUILDING_ACTION_UNDO) != 0 && calls.load() == 1, "lazy: a later submit installs nothing");
        stop.store(true);
        game.join();
        bi::set_installer(nullptr);
    }

    ci::publish(0, 0);
    std::printf("client input (Windows): %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
