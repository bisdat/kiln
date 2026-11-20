// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstring>
#include <vector>
#include "services/kiln_hooks.h"
namespace kiln::hooks::chain {
// Portable half of native typed fault recovery. Storage is prepared before entering the handler.
struct TypedResult {
    bool called = false;
    uint64_t rax = 0;
    uint8_t xmm0[16]{};
    void* buffer = nullptr;
    uint32_t size = 0;
    std::array<uint8_t, 64> inline_bytes{};
    std::vector<uint8_t> large;
    void prepare(void* out, uint32_t count) {
        buffer = out; size = count;
        if (size > inline_bytes.size()) large.resize(size);
    }
    uint8_t* data() { return size > inline_bytes.size() ? large.data() : inline_bytes.data(); }
    const uint8_t* data() const { return size > inline_bytes.size() ? large.data() : inline_bytes.data(); }
    void capture(const KilnCallFrame& f) {
        rax = f.rax;
        std::memcpy(xmm0, f.xmm0r, 16);
        if (buffer && size) std::memcpy(data(), buffer, size);
    }
    void restore(KilnCallFrame& f) const {
        f.rax = rax;
        std::memcpy(f.xmm0r, xmm0, 16);
        if (buffer && size) std::memcpy(buffer, data(), size);
    }
    template<class Original> void original(KilnCallFrame& f, Original&& run) {
        if (called) return;
        called = true;
        run();
        capture(f);
    }
};
}
