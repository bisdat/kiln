// SPDX-License-Identifier: MIT
#include "services/hooks_code.h"

#include <sanitizer/allocator_interface.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
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
    static_assert(sizeof(KilnHooksApi) == 312);
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
    std::printf("hooks_code_test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
