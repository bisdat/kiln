// SPDX-License-Identifier: MIT
// The production outline_info entry point, with the exact mod API 0.2.13 record (including 4-byte alignment).
// No game, hooks or renderer are started. This translation unit includes the Windows glue so the bounded copy cannot
// silently drift from the test. Build target: ui-outline-abi-test.
#include "../outline.cpp"
#include <cstdio>

namespace kui {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
void logf(int, const char*, ...) {}
KilnHandle next_handle() { return 1; }
}

struct ReleasedOutlineInfo013 {
    uint32_t Size, State, Reason, Entity, Group, Merged;
};
static_assert(sizeof(ReleasedOutlineInfo013) == 24 && alignof(ReleasedOutlineInfo013) == 4);
struct alignas(8) LegacyBuffer {
    uint32_t before;
    ReleasedOutlineInfo013 value;
    uint32_t after[9];
};
static_assert(offsetof(LegacyBuffer, value) == 4);

int main() {
    int passed = 0, failed = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        (ok ? passed : failed)++;
    };
    LegacyBuffer legacy{};
    auto reset = [&](uint32_t size) {
        std::memset(&legacy, 0xA5, sizeof legacy);
        legacy.value.Size = size;
    };
    auto untouched = [&] {
        if (legacy.before != 0xA5A5A5A5u) return false;
        for (auto v : legacy.after) if (v != 0xA5A5A5A5u) return false;
        return true;
    };
    check(reinterpret_cast<uintptr_t>(&legacy.value) % 8 == 4, "legacy outline: caller is 4-aligned, not 8-aligned");
    KilnRenderOutline options{};
    options.size = sizeof options; options.rgba = 0x46FF6EFFu; options.flags = KILN_RENDER_OUTLINE_THROUGH_WALLS;
    check(g_book.add(11, 4100, options, 0, 713) == KILN_OK, "legacy outline: request accepted without starting game hooks");
    const double position[3] = {512, 41, 2048};
    g_book.set_position(4100, true, position, 7);
    reset(24);
    auto* out = reinterpret_cast<KilnRenderOutlineInfo*>(&legacy.value);
    check(kui::outline_info(713, out) == KILN_OK && legacy.value.Size == 24 && legacy.value.Entity == 4100 &&
              legacy.value.State == KILN_RENDER_OUTLINE_WAITING && legacy.value.Reason == KILN_RENDER_OUTLINE_PENDING,
          "legacy outline: released record receives its unchanged fields");
    check(untouched(), "legacy outline: no write before or beyond the caller's 24 bytes, even with a valid position");
    reset(24);
    check(kui::outline_info(999, out) == KILN_ERR_NOT_FOUND && legacy.value.Size == 24 && untouched(),
          "legacy outline: a missing handle is also a bounded 24-byte copy");
    reset(20);
    check(kui::outline_info(713, out) == KILN_ERR_INVALID_ARGUMENT && legacy.value.Size == 20 && untouched(),
          "legacy outline: an undersized caller is rejected without a write");
    std::printf("outline_abi_test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
