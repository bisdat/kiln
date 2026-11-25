// SPDX-License-Identifier: MIT
// Host test of kiln::game_call_through_foreign (../abi/kiln_call_site.hpp): the game's call behind another module's code,
// on made-up stacks. tests/test-call-site.sh builds and runs it with ASan/UBSan. No Wine, no game.
//
// The case it is for (Building Tools 0.34.0, src/main.cpp pay_hook): the blueprint program's E8 at BlueprintApplyRet - 5
// now enters Building Tools' relay (a jmp) to pay_hook, which calls pay_body, which calls ApplyMaterials, where Kiln's
// links run. kiln.hooks recorded pay_body's return address as the caller.
#include <cstdio>
#include <cstring>
#include <map>

#include "kiln_call_site.hpp"

static int failures = 0, checks = 0;
#define EXPECT(cond)                                                    \
    do {                                                                \
        ++checks;                                                       \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

constexpr uintptr_t kGameLo = 0x140000000, kGameHi = 0x143000000;
constexpr uintptr_t kApply = 0x140495f20;           // rules/ApplyMaterials
constexpr uintptr_t kBlueprintRet = 0x1402d8bb0;    // build/BlueprintApplyRet: the blueprint program's call returns here
constexpr uintptr_t kPlacePropRet = 0x1400c6be9;    // another building program's call of ApplyMaterials
constexpr uintptr_t kPayBodyRet = 0x7ff810001234;   // in BuildingTools.dll: pay_body's call of ApplyMaterials returns here
constexpr uintptr_t kPayHookRet = 0x7ff810000456;   // pay_hook's call of pay_body returns here
constexpr uintptr_t kItemsRet = 0x7ff820000789;     // kiln-items' link calling the rest of the chain (kiln-rules' link)
std::map<uintptr_t, uintptr_t> original;            // return address -> the target of the game's call there (the exe file)

bool in_game(uintptr_t a) { return a >= kGameLo && a < kGameHi; }
uintptr_t original_call(uintptr_t r) {
    const auto i = original.find(r);
    return i == original.end() ? 0 : i->second;
}
uintptr_t walk(std::initializer_list<uintptr_t> frames, uintptr_t ret, uintptr_t target = kApply, size_t max = 6) {
    return kiln::game_call_through_foreign(frames.begin(), frames.size(), ret, target, in_game, original_call, max);
}

int main() {
    original[kBlueprintRet] = kApply;
    original[kPlacePropRet] = kApply;
    original[0x140300010] = 0x140111111;  // a game call of something else

    // the game's own call: not this case (the hook checks the return address itself)
    EXPECT(walk({0x7ff800000100, kBlueprintRet, 0x140200000}, kBlueprintRet) == 0);
    // Building Tools: hook frame, pay_body's return (recorded), pay_hook's, then the blueprint program's call
    EXPECT(walk({0x7ff800000100, kPayBodyRet, kPayHookRet, kBlueprintRet, 0x140200000}, kPayBodyRet) == kBlueprintRet);
    // the same with kiln-items' link before kiln-rules' on the stack (it calls the rest of the chain)
    EXPECT(walk({0x7ff800000100, kItemsRet, kPayBodyRet, kPayHookRet, kBlueprintRet}, kPayBodyRet) == kBlueprintRet);
    // another building program's call behind it: that call, and the hook decides what it allows there
    EXPECT(walk({0x7ff800000100, kPayBodyRet, kPayHookRet, kPlacePropRet}, kPayBodyRet) == kPlacePropRet);
    // the first game frame is not a call of the target (the module called it on its own, from a game callback, say)
    EXPECT(walk({0x7ff800000100, kPayBodyRet, kPayHookRet, 0x140300010, kBlueprintRet}, kPayBodyRet) == 0);
    EXPECT(walk({0x7ff800000100, kPayBodyRet, kPayHookRet, 0x140300020}, kPayBodyRet) == 0);  // no call there at all
    // the walk ended in foreign code (code without unwind data stops it): nothing certain
    EXPECT(walk({0x7ff800000100, kPayBodyRet, kPayHookRet}, kPayBodyRet) == 0);
    // the recorded return address is not on this stack (a stale record): nothing certain
    EXPECT(walk({0x7ff800000100, kPayHookRet, kBlueprintRet}, kPayBodyRet) == 0);
    // too many foreign frames between them: not this
    EXPECT(walk({kPayBodyRet, 0x7ff8000a0001, 0x7ff8000a0002, kBlueprintRet}, kPayBodyRet, kApply, 2) == 0);
    EXPECT(walk({kPayBodyRet, 0x7ff8000a0001, kBlueprintRet}, kPayBodyRet, kApply, 2) == kBlueprintRet);
    // nothing to go on
    EXPECT(walk({}, kPayBodyRet) == 0);
    EXPECT(walk({kPayBodyRet, kBlueprintRet}, 0) == 0);
    EXPECT(walk({kPayBodyRet, kBlueprintRet}, kPayBodyRet, 0) == 0);
    EXPECT(kiln::game_call_through_foreign(nullptr, 3, kPayBodyRet, kApply, in_game, original_call) == 0);

    // call_target: the E8 rel32 that ends at ret
    uint8_t call[5] = {0xe8};
    const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(kApply) - static_cast<int64_t>(kBlueprintRet));
    std::memcpy(call + 1, &rel, 4);
    EXPECT(kiln::call_target(call, kBlueprintRet) == kApply);
    call[0] = 0xe9;  // a jmp is not a call
    EXPECT(kiln::call_target(call, kBlueprintRet) == 0);

    std::printf("call site: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
