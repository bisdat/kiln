// SPDX-License-Identifier: MIT
// kiln.client.crosshair: a copy of the game's own crosshair hit (abi/services/kiln_client_crosshair.h,
// docs/kiln/services/client-crosshair.md). This part has no game in it: the decoder of the game's 112-byte result,
// the sample store, the session and freshness rules, get(), and the lazy installer's state. The hook and the service
// table are client_crosshair_service.cpp's; host tests: native/core/tests/client_crosshair_test.cpp.
//
// The game side (docs/re/client.md, "The gameplay crosshair trace"): the client world's program client_cursor calls the
// gameplay scene trace once per step with rcx = a 112-byte result in its own frame ([rbp + client/crosshair.hit]). The
// observation is the instruction after that call: the result is complete there and nothing has read it yet. What the
// result holds (client 26430625; the sigdb probes on the trace body pin each offset):
//   +0x00 u8  status: 0 hit, 31 nothing within reach, 21 no usable camera direction
//   +0x08 f32[3] the hit position, world metres (nudged 1 cm back along the normal or the ray, as the game uses it)
//   +0x14 f32[3] the surface normal
//   +0x60 u8  the hit's voxel material id, valid when +0x61 is not 0
//   +0x64 u32 the hit's entity (0 for terrain)
//   +0x68 f32 the squared distance from the trace's start
//
// Threads. observe() runs where the game runs client_cursor (the client step: the main thread or one of the game's job
// threads): it reads atomics, copies the result through the given Memory (a guarded copy in the game) and publishes one
// sample; it allocates nothing, logs nothing, takes no lock that can wait, and skips its visit when another visit is
// publishing (stats().skipped). get() is any thread and never waits for observe(). session_live()/session_ended() come
// from the client world's Pre and its STOPPING.
#pragma once
#include <cstddef>
#include <cstdint>

#include "services/kiln_client_crosshair.h"

namespace kiln::crosshair {

constexpr size_t kResultBytes = 112;  // the game's trace result
constexpr uint32_t kMaxFrameOffset = 0x10000;  // client/crosshair.hit must lie in client_cursor's frame (0x1978 bytes on 26430625)
constexpr float kMaxDistance = 1000.0f;  // a distance past this is not the cursor's (the game clamps its reach to 31 m)

// The value fields of a hit (flags, status, entity, material, position, normal, distance) from the game's result. The
// other fields of *out are left alone. A field the result does not hold as a sane value is left unset (its flag clear).
void decode(const uint8_t raw[kResultBytes], KilnCrosshairHit* out);

// The game's memory, copied under a guard: false when the copy faulted.
struct Memory {
    bool (*read)(void* ctx, void* dst, uintptr_t src, size_t n);
    void* ctx;
};

// The producer's binding: the result's offset in client_cursor's frame (client/crosshair.hit), given when the hook is
// installed. Unbound (0), observe() does nothing.
void bind(uint32_t hit_offset);
// One visit of the observation: `frame` is client_cursor's rbp at the instruction after the trace's call.
void observe(uintptr_t frame, const Memory& m);

// The client world's Pre: its live session and the step about to run (KilnTickInfo.step). A new session drops the
// samples and a fault of the old one.
void session_live(uint32_t session, uint64_t step);
// The client world's STOPPING: no sample of that session (or an older one) is given again.
void session_ended(uint32_t session);

// The facts get() checks, from the live process (client_crosshair_service.cpp).
struct Env {
    bool client_side;  // the game client (a dedicated server has no cursor)
    bool hooks;        // not safe idle
    uint32_t live_session;  // the client session live now (0: none), asked at this get: a sample of another is never given
};
// kiln.client.crosshair's get (kiln_client_crosshair.h). The first call on a ready build installs the hook (the lazy
// installer below) and answers NO_SAMPLE.
KilnResult get(const Env& env, KilnCrosshairHit* out, uint32_t* out_reason);

// The lazy producer, as kiln.client's building input does it (client_building_input.h): on a build where the hook can
// go in, client_crosshair_service.cpp gives an installer at its start and installs nothing; the first get() that passes
// its own checks calls it, once at a time, on the calling thread. Install::Again (kiln.hooks could not check every
// thread and wrote nothing) lets a later get try again, at least kInstallRetryNs later and up to kInstallAttempts in
// all; until then get() answers NO_SAMPLE (the row stays on), and so does a get that waited on such an attempt.
// Anything else, or the last attempt, is final for the process: the installer goes, install_failed() turns true, its
// reason (a static string) becomes usable()'s answer, and get() answers KILN_ERR_UNAVAILABLE.
enum class Install { Bound, Again, Failed };
constexpr uint32_t kInstallAttempts = 3;
constexpr int64_t kInstallRetryNs = 1'000'000'000;  // the least time between two attempts, on the service's clock
using Installer = Install (*)(uint32_t* hit_offset, const char** why);
void set_installer(Installer install);  // nullptr: none (the default); clears install_failed()
// The reason usable() gives while there is neither an installer nor a binding (a static string; nullptr: the default,
// no verified trace on this build).
void unbound_reason(const char* why);
// Whether get() can answer here: bound, or an installer waits for the first get. *why says why not (a static string).
// *installed: the hook is in.
bool usable(const char** why, bool* installed);
bool install_failed();
// Grows at every change of usable()'s answer, for kiln.caps' row client.crosshair.
uint64_t generation();

struct Stats {
    uint64_t samples;   // samples published since boot
    uint64_t skipped;   // visits skipped: another visit was publishing, or no live session
    uint64_t faults;    // guarded copies that faulted
    uint64_t installs;  // installer calls that returned
};
Stats stats();

// Host tests.
void set_clock_for_tests(int64_t (*ns)());  // nullptr: the steady clock (QueryPerformanceCounter on MSVC)
void reset_for_tests();                     // no samples, no session, unbound (default reason), no installer, zero stats
void hold_publish_for_tests(bool held);     // a publish stays half-written: get answers BUSY, observe skips

}  // namespace kiln::crosshair
