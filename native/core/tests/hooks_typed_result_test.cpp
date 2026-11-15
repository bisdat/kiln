// SPDX-License-Identifier: MIT
#include <cassert>
#include <cstdio>
#include "services/hooks_typed_result.h"
int main() {
    using kiln::hooks::chain::TypedResult;
    int assertions = 0;
    for (bool after : {false, true}) for (uint32_t size : {8u, 16u, 128u, 4096u}) {
        std::vector<uint8_t> out(size, 3);
        KilnCallFrame f{}; f.gpr[0] = reinterpret_cast<uint64_t>(out.data());
        TypedResult state; state.prepare(out.data(), size);
        int calls = 0;
        auto original = [&] { ++calls; std::fill(out.begin(), out.end(), 42); f.rax = f.gpr[0]; f.xmm0r[0] = 17; };
        // Inject a handler fault before or after CallOriginal; no Windows exception machinery in this test.
        if (after) state.original(f, original);
        std::fill(out.begin(), out.end(), 99); f.rax = 123; f.xmm0r[0] = 2;
        const bool fault = true;
        if (fault && state.called) state.restore(f); else state.original(f, original);
        state.original(f, original); // duplicate CallOriginal is a no-op
        assert(calls == 1); ++assertions;
        assert(f.rax == f.gpr[0]); ++assertions;
        assert(f.xmm0r[0] == 17); ++assertions;
        for (auto byte : out) { assert(byte == 42); ++assertions; }
    }
    std::printf("typed result: %d assertions passed\n", assertions);
}
