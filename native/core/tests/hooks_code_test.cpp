// SPDX-License-Identifier: MIT
#include "services/hooks_code.h"

#include <sanitizer/allocator_interface.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace kiln::hooks::code;
namespace {
int passed = 0, failed = 0;
void check(bool ok, const char* text, const std::string& detail = "") {
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", text, detail.empty() ? "" : ": ", detail.c_str());
    ok ? ++passed : ++failed;
}
std::array<uint8_t, 128> file{}, live{};
bool disk = true, readable = true, torn = false;
std::barrier<>* entered = nullptr;
std::barrier<>* resume = nullptr;
constexpr uintptr_t base = 0x10000, text = base + 16;
bool file_copy(uintptr_t a, uint8_t* out, size_t n) {
    if (!disk) { if (n) out[0] = 0xff; return false; } // short file copy must not leak partial output
    std::memcpy(out, file.data() + a - base, n);
    return true;
}
bool live_copy(uintptr_t a, uint8_t* out, size_t n) {
    if (entered) { entered->arrive_and_wait(); resume->arrive_and_wait(); entered = nullptr; }
    if (!readable) return false;
    std::memcpy(out, live.data() + a - base, n);
    if (torn) live[a - base] ^= 1;
    return true;
}
Image image{base, file.size(), text, 96, file_copy, live_copy};

// For the range limit: a .text two bytes longer than KILN_HOOKS_CODE_RANGE_MAX. Its file reads are counted, and so are
// the allocations larger than the limit while `counting` is set (the sanitizer's malloc hook sees operator new too).
constexpr uintptr_t big_base = 0x200000;
std::array<uint8_t, KILN_HOOKS_CODE_RANGE_MAX + 2> big{};
std::atomic<unsigned> big_reads{0}, big_allocs{0};
std::atomic<bool> counting{false};
bool big_copy(uintptr_t a, uint8_t* out, size_t n) {
    std::memcpy(out, big.data() + (a - big_base), n);
    return true;
}
bool big_file(uintptr_t a, uint8_t* out, size_t n) {
    ++big_reads;
    return big_copy(a, out, n);
}
__attribute__((no_sanitize("address", "thread", "undefined"))) void on_malloc(const volatile void*, size_t n) {
    if (counting.load(std::memory_order_relaxed) && n > KILN_HOOKS_CODE_RANGE_MAX)
        big_allocs.fetch_add(1, std::memory_order_relaxed);
}
void on_free(const volatile void*) {}
Image big_image{big_base, big.size(), big_base, big.size(), big_file, big_copy};

// ---- protected ranges (kiln.internal.hooks, hooks_protect_internal.h) ----
std::atomic<uint64_t> dead_owner{0};
bool owner_alive(uint64_t o) { return o != dead_owner.load(); }
const Registry::Protection* overlap(Registry& r, uintptr_t a, size_t n) {
    auto guard = r.lock();
    return r.protected_overlap_locked(a, n);
}

void protected_ranges() {
    file.fill(0x90); live = file; disk = true; readable = true; torn = false;
    Registry r;
    ProtectOutcome o;
    auto protect = [&](uint64_t owner, std::initializer_list<KilnCodeRange> list, const char* why = "engine call A") {
        return r.protect(image, owner, list.size() ? list.begin() : nullptr, list.size(), why, &owner_alive, &o);
    };
    uint32_t s = 99; uint64_t g0 = 0, g1 = 0;
    r.code_state(image, text + 20, 10, &s, &g0);
    check(protect(1, {{text + 20, 10}}) == KILN_OK && o.added == 1 && o.added_bytes == 10 && r.protected_count() == 1 &&
              r.protected_bytes() == 10,
          "protect: an ORIGINAL range is protected");
    r.code_state(image, text + 20, 10, &s, &g1);
    check(g1 == g0, "protect moves no generation (no code changed)");
    const auto* p = overlap(r, text + 20, 1);
    check(p && p->owner == 1 && p->address == text + 20 && p->size == 10 && p->reason == "engine call A",
          "the protection keeps its owner, range and reason");
    check(!overlap(r, text + 10, 10) && !overlap(r, text + 30, 5) && !overlap(r, text + 19, 1) && !overlap(r, text + 30, 1),
          "adjacent ranges before and after do not overlap");
    check(overlap(r, text + 20, 1) && overlap(r, text + 29, 1) && overlap(r, text + 10, 11) && overlap(r, text + 29, 10) &&
              overlap(r, text + 15, 30) && overlap(r, text + 22, 2),
          "a range over either edge, inside or around it overlaps");
    const uintptr_t top = std::numeric_limits<uintptr_t>::max();
    check(!overlap(r, text + 25, 0) && !overlap(r, top - 1, 2) && !overlap(r, top, 1) && overlap(r, 0, top),
          "size 0 overlaps nothing; ranges near the top of the address space neither wrap nor match");
    check(protect(1, {{text + 20, 10}}) == KILN_OK && o.added == 0 && r.protected_count() == 1,
          "protect again by the same owner: KILN_OK, nothing added (idempotent)");
    check(protect(2, {{text + 20, 10}}, "engine call B") == KILN_OK && o.added == 1 && r.protected_count() == 2,
          "another owner protecting the same range gets its own entry");
    check(protect(3, {{text + 50, 4}, {text + 50, 4}}) == KILN_OK && o.added == 1 && r.protected_count() == 3,
          "the same range twice in one call is protected once");
    check(r.release_owner(3) == 1 && r.protected_count() == 2, "release_owner(3) drops its range");

    // Arguments: refused before anything is read or protected.
    const size_t before = r.protected_count();
    std::string long_reason(KILN_HOOKS_PROTECT_REASON_MAX + 1, 'x'), max_reason(KILN_HOOKS_PROTECT_REASON_MAX, 'x');
    KilnCodeRange many[KILN_HOOKS_PROTECT_MAX_PER_CALL + 1];
    for (size_t i = 0; i < std::size(many); ++i) many[i] = {text + i, 1};
    const KilnCodeRange one{text + 60, 4};
    bool all = true;
    all = all && r.protect(image, 0, &one, 1, "x", &owner_alive, &o) == KILN_ERR_INVALID_ARGUMENT;
    all = all && r.protect(image, 5, nullptr, 1, "x", &owner_alive, &o) == KILN_ERR_INVALID_ARGUMENT;
    all = all && r.protect(image, 5, &one, 0, "x", &owner_alive, &o) == KILN_ERR_INVALID_ARGUMENT;
    all = all && r.protect(image, 5, many, std::size(many), "x", &owner_alive, &o) == KILN_ERR_INVALID_ARGUMENT;
    all = all && r.protect(image, 5, &one, 1, "", &owner_alive, &o) == KILN_ERR_INVALID_ARGUMENT;
    all = all && r.protect(image, 5, &one, 1, long_reason, &owner_alive, &o) == KILN_ERR_INVALID_ARGUMENT;
    all = all && r.protect(image, 5, &one, 1, "x", nullptr, &o) == KILN_ERR_INVALID_ARGUMENT;
    for (auto bad : {KilnCodeRange{text + 60, 0}, KilnCodeRange{base, 4}, KilnCodeRange{text + 95, 2},
                     KilnCodeRange{base + 128, 1}, KilnCodeRange{top - 1, 8}})
        all = all && r.protect(image, 5, &bad, 1, "x", &owner_alive, &o) == KILN_ERR_INVALID_ARGUMENT;
    check(all && r.protected_count() == before,
          "owner 0, no ranges, more than 32, an empty or over-long reason, no alive test, a range of 0 bytes, outside .text "
          "or wrapping: KILN_ERR_INVALID_ARGUMENT, nothing protected");
    check(r.protect(image, 5, many, KILN_HOOKS_PROTECT_MAX_PER_CALL, max_reason, &owner_alive, &o) == KILN_OK && o.added == 32 &&
              r.release_owner(5) == 32,
          "exactly 32 ranges and a 256-byte reason are accepted");
    {   // The ranges of one call come to at most 64 KiB together.
        Registry big_r;
        const KilnCodeRange halves[2] = {{big_base, KILN_HOOKS_CODE_RANGE_MAX / 2}, {big_base + KILN_HOOKS_CODE_RANGE_MAX / 2, KILN_HOOKS_CODE_RANGE_MAX / 2 + 1}};
        const KilnCodeRange whole{big_base, KILN_HOOKS_CODE_RANGE_MAX + 1};
        check(big_r.protect(big_image, 1, halves, 2, "x", &owner_alive, &o) == KILN_ERR_INVALID_ARGUMENT &&
                  big_r.protect(big_image, 1, &whole, 1, "x", &owner_alive, &o) == KILN_ERR_INVALID_ARGUMENT &&
                  big_r.protected_count() == 0,
              "a call whose ranges pass 64 KiB together, or one range over 64 KiB, is refused");
        // The process-wide bounds: 512 KiB (here 8 owners x 64 KiB of the same bytes), then nothing more.
        const KilnCodeRange cap{big_base, KILN_HOOKS_CODE_RANGE_MAX};
        bool fits = true;
        for (uint64_t owner = 1; owner <= 8; ++owner)
            fits = fits && big_r.protect(big_image, owner, &cap, 1, "x", &owner_alive, &o) == KILN_OK && o.added == 1;
        check(fits && big_r.protected_bytes() == KILN_HOOKS_PROTECT_MAX_BYTES, "512 KiB of protections fit");
        const KilnCodeRange more{big_base + 7, 1};
        check(big_r.protect(big_image, 9, &more, 1, "x", &owner_alive, &o) == KILN_ERR_UNAVAILABLE && o.full &&
                  big_r.protected_bytes() == KILN_HOOKS_PROTECT_MAX_BYTES,
              "one byte more is KILN_ERR_UNAVAILABLE (full), nothing added");
        check(big_r.protect(big_image, 3, &cap, 1, "x", &owner_alive, &o) == KILN_OK && o.added == 0,
              "...while an owner's own range again is still KILN_OK (nothing added)");
    }
    {   // 256 ranges at most: 1-byte ranges of 3 owners over the 96 bytes of .text, 32 per call.
        Registry count_r;
        size_t calls_ok = 0;
        KilnResult last = KILN_OK;
        for (uint64_t owner = 1; owner <= 3; ++owner)
            for (size_t at = 0; at < 96; at += 32) {
                KilnCodeRange batch[32];
                for (size_t i = 0; i < 32; ++i) batch[i] = {text + at + i, 1};
                last = count_r.protect(image, owner, batch, 32, "x", &owner_alive, &o);
                if (last == KILN_OK) ++calls_ok;
            }
        check(calls_ok == 8 && count_r.protected_count() == KILN_HOOKS_PROTECT_MAX_RANGES && last == KILN_ERR_UNAVAILABLE && o.full,
              "256 ranges fit; the call that would pass them is refused whole (full)");
    }

    // The owner gone, the state not ORIGINAL: nothing is protected, and nothing of a mixed call either.
    dead_owner = 7;
    check(protect(7, {{text + 60, 4}}) == KILN_ERR_REVOKED && r.protected_count() == before,
          "an owner that is not alive: KILN_ERR_REVOKED, nothing protected");
    dead_owner = 0;
    uint64_t token;
    {
        auto guard = r.lock();
        token = r.add_locked(text + 40, {0x90, 0x90}, {0xeb, 0xfe}, true);
        live[56] = 0xeb; live[57] = 0xfe;
    }
    check(protect(4, {{text + 40, 8}}) == KILN_ERR_UNAVAILABLE && o.state == KILN_CODE_MANAGED && o.address == text + 40 &&
              !overlap(r, text + 40, 8),
          "a range Kiln already hooked or patched (MANAGED first) is not protected");
    check(protect(4, {{text + 60, 4}, {text + 38, 4}}) == KILN_ERR_UNAVAILABLE && !overlap(r, text + 60, 4) &&
              r.protected_count() == before,
          "one MANAGED range refuses the whole call: its ORIGINAL range is not protected either");
    {
        auto guard = r.lock();
        live[56] = 0x90; live[57] = 0x90;
        r.remove_locked(token);
    }
    check(protect(4, {{text + 40, 8}}) == KILN_OK && overlap(r, text + 40, 8), "after the patch is gone it is protected");
    r.release_owner(4);
    live[text + 68 - base] ^= 1;
    check(protect(4, {{text + 66, 8}}) == KILN_ERR_UNAVAILABLE && o.state == KILN_CODE_FOREIGN && o.address == text + 66,
          "a FOREIGN range is not protected");
    live[text + 68 - base] ^= 1;
    readable = false;
    check(protect(4, {{text + 66, 8}}) == KILN_ERR_UNAVAILABLE && o.state == KILN_CODE_UNKNOWN, "an unreadable one neither");
    readable = true; torn = true;
    check(protect(4, {{text + 66, 8}}) == KILN_ERR_UNAVAILABLE && o.state == KILN_CODE_UNKNOWN, "nor one that changes between the reads");
    torn = false; live = file;  // torn flipped a byte
    disk = false;
    check(protect(4, {{text + 66, 8}}) == KILN_ERR_UNAVAILABLE && o.address == 0 && r.protected_count() == before,
          "without the exe file's code: KILN_ERR_UNAVAILABLE");
    disk = true;

    // Release: only the retiring owner's ranges go.
    check(r.release_owner(2) == 1 && overlap(r, text + 20, 10) && overlap(r, text + 20, 10)->owner == 1,
          "owner 2 retires: owner 1's protection of the same range stays");
    check(r.release_owner(1) == 1 && !overlap(r, text + 20, 10) && r.protected_count() == 0 && r.protected_bytes() == 0,
          "owner 1 retires: the range is free again");
    check(r.release_owner(42) == 0, "an owner with nothing protected releases nothing");
}

// A Kiln detour or patch recorded over a range counts even when the live bytes are the file's: here a patch that put
// the original bytes back over another one. unpatch, revoke and revoke_owner are not checked, so undoing the upper
// patch would write into a protected range; protect refuses such a range as MANAGED instead.
void protected_records() {
    file.fill(0x90); live = file; disk = true; readable = true; torn = false;
    Registry r;
    ProtectOutcome o;
    uint64_t lower = 0, upper = 0;
    {
        auto guard = r.lock();
        lower = r.add_locked(text + 42, {0x90, 0x90}, {0xeb, 0xfe}, true);  // a patch
        upper = r.add_locked(text + 42, {0xeb, 0xfe}, {0x90, 0x90}, true);  // a patch over it with the file's bytes
    }
    uint32_t s = 99; uint64_t g = 0;
    check(r.code_state(image, text + 40, 8, &s, &g) == KILN_OK && s == KILN_CODE_ORIGINAL,
          "two stacked patches that put the file's bytes back read ORIGINAL");
    const KilnCodeRange over{text + 40, 8}, inside{text + 43, 1}, before{text + 38, 4}, after{text + 44, 4};
    check(r.protect(image, 1, &over, 1, "x", &owner_alive, &o) == KILN_ERR_UNAVAILABLE && o.state == KILN_CODE_MANAGED &&
              o.address == text + 40 && r.protect(image, 1, &inside, 1, "x", &owner_alive, &o) == KILN_ERR_UNAVAILABLE &&
              r.protected_count() == 0,
          "...but a range under a Kiln patch record is not protected (MANAGED): undoing the upper patch would change it");
    check(r.protect(image, 1, &before, 1, "x", &owner_alive, &o) == KILN_OK && r.protect(image, 1, &after, 1, "x", &owner_alive, &o) == KILN_OK,
          "ranges right before and right after the records are protected");
    {
        auto guard = r.lock();
        r.remove_locked(upper);
        r.remove_locked(lower);
    }
    check(r.protect(image, 1, &over, 1, "x", &owner_alive, &o) == KILN_OK && r.release_owner(1) == 3,
          "with both records gone the range is protected");
}

// kiln.log's rate limit for refusals: per requesting owner and protected range, the 1st, 2nd, 4th, 8th, ... is logged.
void refusal_counts() {
    Registry::Protection p{1, text, 4, "x"}, q{1, text + 8, 4, "x"};
    bool counts = true;
    for (uint64_t i = 1; i <= 20; ++i) counts = counts && Registry::count_refusal_locked(p, 6) == i;
    check(counts, "one requester's refusals over a protected range count 1, 2, 3, ...");
    check(Registry::count_refusal_locked(p, 7) == 1 && Registry::count_refusal_locked(p, 6) == 21 &&
              Registry::count_refusal_locked(p, 0) == 1 && Registry::count_refusal_locked(p, ~uint64_t{0}) == 1,
          "another requester (owner 0 and the unowned write too) counts on its own");
    check(Registry::count_refusal_locked(q, 6) == 1, "...and so does another protected range");
    for (uint64_t req = 100; req < 104; ++req) Registry::count_refusal_locked(p, req);  // 8 requesters now
    check(Registry::count_refusal_locked(p, 200) == 1 && Registry::count_refusal_locked(p, 201) == 2 &&
              Registry::count_refusal_locked(p, 103) == 2 && Registry::count_refusal_locked(p, 6) == 22,
          "past 8 requesters the later ones share one count; the first 8 keep theirs");
    std::string logged;
    for (uint64_t n = 1; n <= 40; ++n)
        if (Registry::log_refusal(n)) logged += std::to_string(n) + " ";
    check(logged == "1 2 4 8 16 32 " && Registry::refusal_tail(1).empty() &&
              Registry::refusal_tail(8).find("[refusal 8 ") != std::string::npos,
          "a refusal is logged at the 1st, 2nd, 4th, 8th, ... time; from the 2nd on its line says which", logged);
    {   // the counts live in the registry's protection, under its lock
        file.fill(0x90); live = file; disk = true; readable = true; torn = false;
        Registry r;
        const KilnCodeRange range{text + 4, 4};
        r.protect(image, 1, &range, 1, "x", &owner_alive);
        auto guard = r.lock();
        Registry::Protection* hit = r.protected_overlap_locked(text + 5, 1);
        const bool first = hit && Registry::count_refusal_locked(*hit, 6) == 1;
        hit = r.protected_overlap_locked(text + 7, 4);
        check(first && hit && Registry::count_refusal_locked(*hit, 6) == 2, "the registry's protection keeps the count");
    }
}

// An installer (kiln.hooks' writers: the overlap check, the record and the write under one lock) racing a caller that
// follows the engine call's order: code_state ORIGINAL, capture, the capture equal to the file's bytes, protect, then the
// "call" (the live bytes compared with the capture, again and again), then retire. Without the capture check an install
// that came and went between the capture and protect would leave a stale capture (engine-call-recipes.md, step 5). Either the install lands first (protect refuses: MANAGED) or protect does
// (the install is refused); the live bytes never change while protected.
void protected_race() {
    file.fill(0x90); live = file;
    Registry r;
    constexpr uintptr_t at = text + 64;
    constexpr size_t size = 16;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> installed{0}, refused{0}, rounds{0}, protected_rounds{0}, managed_first{0}, changed{0}, stale{0};
    std::thread installer([&] {
        for (unsigned i = 0; !stop.load(); ++i) {
            const uintptr_t a = at + (i % size);  // every byte of the range, one at a time
            uint64_t token = 0;
            {
                auto guard = r.lock();
                if (r.protected_overlap_locked(a, 2)) { ++refused; continue; }
                token = r.add_locked(a, {0x90, 0x90}, {0xcc, 0xcc}, true);
                live[a - base] = 0xcc; live[a - base + 1] = 0xcc;
                ++installed;
            }
            std::this_thread::yield();
            auto guard = r.lock();
            live[a - base] = 0x90; live[a - base + 1] = 0x90;
            r.remove_locked(token);
        }
    });
    const KilnCodeRange range{at, size};
    // Bounded by time, not rounds: on a loaded machine the installer can be descheduled while its bytes are in, and
    // rounds that only see MANAGED would use up a round count without either order happening.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (int i = 0; i < 4000 || (protected_rounds.load() < 50 || managed_first.load() < 50); ++i) {
        if (std::chrono::steady_clock::now() > deadline) break;  // the counts below then fail the check
        ++rounds;
        uint32_t s = 99; uint64_t g = 0;
        if (r.code_state(image, at, size, &s, &g) != KILN_OK || s != KILN_CODE_ORIGINAL) {
            std::this_thread::yield();  // let the installer take its bytes out
            continue;
        }
        std::array<uint8_t, size> capture;
        { auto guard = r.lock(); std::memcpy(capture.data(), live.data() + (at - base), size); }
        if (std::memcmp(capture.data(), file.data() + (at - base), size)) { ++stale; continue; }  // engine-code-patched
        std::this_thread::yield();  // widens the window an install can land in
        if (r.protect(image, 9, &range, 1, "race", &owner_alive) != KILN_OK) { ++managed_first; continue; }
        ++protected_rounds;
        for (int k = 0; k < 20; ++k) {
            auto guard = r.lock();
            if (std::memcmp(capture.data(), live.data() + (at - base), size)) ++changed;
        }
        r.release_owner(9);
    }
    stop = true;
    installer.join();
    check(changed == 0, "racing installs never change protected bytes", std::to_string(changed.load()) + " changes");
    check(protected_rounds >= 50 && managed_first >= 50 && installed > 0 && refused > 0,
          "both orders happened: protect first (install refused) and install first (protect refused)",
          std::to_string(protected_rounds.load()) + " protected, " + std::to_string(managed_first.load()) +
              " refused as MANAGED, " + std::to_string(installed.load()) + " installs, " + std::to_string(refused.load()) +
              " refused installs, " + std::to_string(stale.load()) + " captures not the file's, " +
              std::to_string(rounds.load()) + " rounds");
}

bool has_original(const void* raw) {
    const auto* h = static_cast<const KilnHooksApi*>(raw);
    return raw && static_cast<const KilnServiceHeader*>(raw)->size >= offsetof(KilnHooksApi, read_original) + sizeof(h->read_original) && h->read_original;
}
bool has_state(const void* raw) {
    const auto* h = static_cast<const KilnHooksApi*>(raw);
    return raw && static_cast<const KilnServiceHeader*>(raw)->size >= offsetof(KilnHooksApi, code_state) + sizeof(h->code_state) && h->code_state;
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // a hang (test-hooks-code.sh's time limit) still shows how far it got
    static_assert(offsetof(KilnHooksApi, read_original) == 296);
    static_assert(offsetof(KilnHooksApi, code_state) == 304);
    static_assert(sizeof(KilnHooksApi) >= 312);  // 328 since open_module / module_scan were appended after code_state
    // A real 296-byte allocation, not a newer object with a short size; ASan catches premature slot loads.
    alignas(KilnHooksApi) std::array<uint8_t, 296> old{};
    reinterpret_cast<KilnServiceHeader*>(old.data())->size = old.size();
    check(!has_original(reinterpret_cast<const KilnHooksApi*>(old.data())) &&
          !has_state(reinterpret_cast<const KilnHooksApi*>(old.data())), "old 296-byte table is refused before loading either tail slot");
    KilnHooksApi absent{}; absent.header.size = sizeof absent;
    check(!has_original(nullptr) && !has_state(nullptr) && !has_original(&absent) && !has_state(&absent), "null table and null members refuse");
    absent.header.size = 304;
    absent.read_original = +[](uintptr_t, void*, size_t) -> KilnResult { return KILN_OK; };
    check(has_original(&absent) && !has_state(&absent), "intermediate 304-byte table exposes only read_original");

    file.fill(0x90); live = file;
    // Relocated pointer bytes are already in sigdb's verified file view.
    const uintptr_t relocated = base + 72;
    std::memcpy(file.data() + 24, &relocated, sizeof relocated); live = file;
    uintptr_t output = 0;
    check(read_original(image, base + 24, &output, sizeof output) == KILN_OK && output == relocated, "original copy preserves relocated file bytes");
    std::array<uint8_t, 8> sentinel; sentinel.fill(0x55);
    for (auto [a, n] : {std::pair<uintptr_t, size_t>{text, 0}, {base, 1}, {base + 111, 2},
                       {base + 128, 1}, {std::numeric_limits<uintptr_t>::max() - 1, 8}}) {
        check(read_original(image, a, sentinel.data(), n) == KILN_ERR_INVALID_ARGUMENT && sentinel[0] == 0x55,
              "invalid/zero/out-of-section/overflow range writes nothing");
    }
    check(read_original(image, text, nullptr, 1) == KILN_ERR_INVALID_ARGUMENT, "null output is invalid");
    disk = false;
    check(read_original(image, text, sentinel.data(), sentinel.size()) == KILN_ERR_UNAVAILABLE && sentinel[0] == 0x55,
          "absent/short verified file never falls back to live and writes nothing");
    disk = true;

    {   // The range limit: exactly KILN_HOOKS_CODE_RANGE_MAX (64 KiB) is accepted. One byte more is refused before
        // anything sized by it is allocated or read, writes nothing and leaves the generation and the remembered range.
        static_assert(KILN_HOOKS_CODE_RANGE_MAX == 65536);
        constexpr size_t cap = KILN_HOOKS_CODE_RANGE_MAX;
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 7 + 1);
        __sanitizer_install_malloc_and_free_hooks(on_malloc, on_free);
        std::vector<uint8_t> copy(cap + 1, 0x55);
        const auto untouched = [&] {
            return std::all_of(copy.begin(), copy.end(), [](uint8_t b) { return b == 0x55; });
        };
        check(read_original(big_image, big_base + 1, copy.data(), cap) == KILN_OK &&
                  !std::memcmp(copy.data(), big.data() + 1, cap) && copy[cap] == 0x55,
              "range limit: read_original accepts exactly 64 KiB");
        std::fill(copy.begin(), copy.end(), 0x55);
        const unsigned reads = big_reads.load();
        counting = true;
        const KilnResult over = read_original(big_image, big_base, copy.data(), cap + 1);
        counting = false;
        check(over == KILN_ERR_INVALID_ARGUMENT && untouched(),
              "range limit: read_original refuses 64 KiB + 1 and writes nothing");
        check(big_reads.load() == reads && big_allocs.load() == 0,
              "range limit: read_original refuses before a read or an allocation of that size",
              std::to_string(big_reads.load() - reads) + " reads, " + std::to_string(big_allocs.load()) +
                  " allocations");
        Registry limited;
        uint32_t s = 99;
        uint64_t g = 0;
        check(limited.code_state(big_image, big_base + 1, cap, &s, &g) == KILN_OK && s == KILN_CODE_ORIGINAL,
              "range limit: code_state accepts exactly 64 KiB");
        const uint64_t kept = g;
        uint32_t s2 = 77;
        uint64_t g2 = 88;
        const unsigned reads_before = big_reads.load();
        counting = true;
        const KilnResult state_over = limited.code_state(big_image, big_base, cap + 1, &s2, &g2);
        counting = false;
        check(state_over == KILN_ERR_INVALID_ARGUMENT && s2 == 77 && g2 == 88,
              "range limit: code_state refuses 64 KiB + 1 and leaves state and generation alone");
        check(big_reads.load() == reads_before && big_allocs.load() == 0,
              "range limit: code_state refuses before a read or an allocation of that size",
              std::to_string(big_reads.load() - reads_before) + " reads, " + std::to_string(big_allocs.load()) +
                  " allocations");
        uint64_t again = 0;
        check(limited.code_state(big_image, big_base + 1, cap, &s, &again) == KILN_OK && again == kept,
              "range limit: the refused call left the remembered range alone (the same range keeps the generation)");
    }

    Registry r;
    uint32_t state = 99; uint64_t generation = 0;
    auto query = [&](uintptr_t a = text, size_t n = 64) { return r.code_state(image, a, n, &state, &generation); };
    check(query() == KILN_OK && state == 0 && generation != 0, "equal live and file bytes are state 0");
    const auto first = generation;
    check(query() == KILN_OK && generation == first, "identical repeat keeps generation");
    {   // The generation is one counter for the process: only the last range asked about is kept, so two callers that
        // take turns with two ranges see it move on every call although nothing changed.
        Registry two;
        uint32_t s = 99;
        uint64_t g = 0, previous = 0;
        bool states = two.code_state(image, text, 8, &s, &previous) == KILN_OK && s == KILN_CODE_ORIGINAL, moved = true;
        for (int i = 0; i < 8; ++i) {
            const uintptr_t at = i % 2 ? text : text + 40;  // B, A, B, A, ...
            states = states && two.code_state(image, at, 8, &s, &g) == KILN_OK && s == KILN_CODE_ORIGINAL;
            moved = moved && g > previous;
            previous = g;
        }
        check(states && moved, "two ranges taking turns: the generation moves on every call though nothing changed");
        uint64_t again = 0;
        check(two.code_state(image, text, 8, &s, &again) == KILN_OK && again == g,
              "the same range asked again right after keeps the generation");
    }
    state = 77; generation = 88;
    check(query(base, 1) == KILN_ERR_INVALID_ARGUMENT && state == 77 && generation == 88,
          "code_state invalid range preserves both outputs");
    query();
    uint64_t token;
    const std::vector<uint8_t> original(7, 0x90), replacement{0xe9, 1, 2, 3, 4, 0x90, 0x90};
    {
        auto guard = r.lock();
        token = r.add_locked(text + 32, original, replacement, true);
        std::memcpy(live.data() + 48, replacement.data(), replacement.size());
    }
    check(query() == KILN_OK && state == 1 && generation > first, "exact owned detour range with continuation is state 1");
    check(query(text + 39, 8) == KILN_OK && state == 0, "outside the actual instruction replacement is state 0");
    const auto installed = generation;
    { auto guard = r.lock(); r.touch_locked(); }
    check(query() == KILN_OK && state == 1 && generation > installed, "chain owner change bumps generation without changing code");
    live[49] ^= 1;
    check(query() == KILN_OK && state == 2, "foreign overwrite INSIDE owned range is not laundered by ownership");
    const auto foreign = generation;
    live[49] ^= 1;
    check(query() == KILN_OK && state == 1 && generation > foreign, "fresh observed byte change bumps generation even if ownership did not change");
    live[79] ^= 1;
    check(query() == KILN_OK && state == 2, "foreign byte outside detour within query is state 2");
    live[79] ^= 1;
    { auto guard = r.lock(); std::memcpy(live.data() + 48, original.data(), original.size()); r.remove_locked(token); }
    check(query() == KILN_OK && state == 0, "physical patch removal returns state 0");

    // Stacked managed patches retain original provenance; foreign pre-existing code does not.
    uint64_t lower, upper;
    { auto guard = r.lock();
      lower = r.add_locked(text, {0x90}, {0x91}, true); live[16] = 0x91;
      upper = r.add_locked(text, {0x91}, {0x92}, true); live[16] = 0x92; }
    check(query() == KILN_OK && state == 1, "stacked managed patches reconstruct original continuation");
    { auto guard = r.lock(); r.remove_locked(upper); r.remove_locked(lower); live[16] = 0x93;
      token = r.add_locked(text, {0x93}, {0x94}, true); live[16] = 0x94; }
    check(query() == KILN_OK && state == 2, "hook over foreign bytes is still foreign, not an original continuation");
    { auto guard = r.lock(); r.remove_locked(token); token = r.add_locked(text, {0x90}, {0x94}, false); }
    check(query() == KILN_OK && state == 3, "missing original continuation makes registry evidence unknown");
    { auto guard = r.lock(); r.remove_locked(token); live[16] = 0x90; }
    readable = false;
    check(query() == KILN_OK && state == 3, "unreadable live page returns unknown, not a file fallback");
    readable = true; torn = true;
    check(query() == KILN_OK && state == 3, "unequal guarded live copies return unknown");
    torn = false;
    check(query() == KILN_OK && state == 0, "recovering readable original is observed");
    disk = false; state = 77; generation = 88;
    check(query() == KILN_ERR_UNAVAILABLE && state == 77 && generation == 88, "missing file leaves state and generation unchanged");
    disk = true;
    check(r.code_state(image, text, 1, nullptr, &generation) == KILN_ERR_INVALID_ARGUMENT &&
          r.code_state(image, text, 1, &state, nullptr) == KILN_ERR_INVALID_ARGUMENT, "null state/generation outputs refuse");

    // Hold a query inside its actual live-copy callback while a writer attempts the registry lock.
    std::barrier a(2), b(2); entered = &a; resume = &b;
    std::atomic<bool> attempting{false}, acquired{false};
    std::thread reader([&] { check(query() == KILN_OK && state == 0, "in-flight query sees a consistent pre-install snapshot"); });
    a.arrive_and_wait();
    std::thread writer([&] {
        attempting.store(true);
        auto guard = r.lock(); acquired.store(true);
        token = r.add_locked(text, {0x90}, {0xcc}, true); live[16] = 0xcc;
    });
    while (!attempting.load()) std::this_thread::yield();
    check(!acquired.load(), "install cannot alter registry or bytes during in-flight query");
    b.arrive_and_wait(); reader.join(); writer.join();
    check(query() == KILN_OK && state == 1, "post-overlap query sees completed install");
    std::atomic<bool> done{false}; std::atomic<unsigned> queries{0}, bad{0};
    std::thread poller([&] {
        uint64_t previous = 0;
        do { uint32_t s; uint64_t g;
             if (r.code_state(image, text, 64, &s, &g) != KILN_OK || s > 1 || g < previous) ++bad;
             previous = g;
             ++queries;
        } while (!done.load());
    });
    std::thread faults([&] { for (int i = 0; i < 2000; ++i) r.note_chain_fault(); });
    for (int i = 0; i < 2000; ++i) {
        auto guard = r.lock(); r.remove_locked(token); live[16] = 0x90;
        token = r.add_locked(text, {0x90}, {0xcc}, true); live[16] = 0xcc;
        r.touch_locked(); // chain retirement/replacement under the same publication lock
    }
    done.store(true); poller.join(); faults.join();
    check(queries > 0 && bad == 0, "concurrent poll/install/remove/owner-change/fault stress remains coherent");
    protected_ranges();
    protected_records();
    refusal_counts();
    protected_race();
    std::printf("hooks_code_test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
