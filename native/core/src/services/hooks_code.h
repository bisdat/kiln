// SPDX-License-Identifier: MIT
// kiln.hooks' read_original and code_state: the exe file's code, and a record of exactly what Kiln's own detours and
// patches wrote over it. Kiln's writers of game code (hooks.cpp: patch, unpatch, write, revoke_owner; hooks_chain.cpp:
// install, revoke, revoke_owner, sweep) hold lock() while they read the old bytes, write and record what they wrote.
// Lock order: the chain or patch mutex, then this one. code_state takes only this one, and holds it for two guarded
// copies of the live bytes and the comparison: no callback, no log, no wait. hooks.json's foreignBefore and
// foreignOver are never used here.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include "services/kiln_hooks.h"

namespace kiln::hooks::code {
struct Image {
    uintptr_t base;
    size_t size;
    uintptr_t text;
    size_t text_size;
    bool (*file)(uintptr_t, uint8_t*, size_t);
    bool (*live)(uintptr_t, uint8_t*, size_t);
};
inline bool valid_range(const Image& image, uintptr_t address, size_t size) {
    // Subtraction-based bounds checks also refuse wraparound in malformed image metadata. The size limit comes first:
    // both members check the range before they allocate or read anything sized by it.
    const auto max = std::numeric_limits<uintptr_t>::max();
    return size && size <= KILN_HOOKS_CODE_RANGE_MAX && image.base && image.size <= max - image.base &&
        image.text >= image.base && image.text - image.base <= image.size &&
        image.text_size <= image.size - (image.text - image.base) && address >= image.text &&
        address - image.text < image.text_size && size <= image.text_size - (address - image.text);
}
inline KilnResult read_original(const Image& image, uintptr_t address, void* out, size_t size) {
    if (!out || !valid_range(image, address, size)) return KILN_ERR_INVALID_ARGUMENT;
    // Stage the whole copy: a short/unavailable file reader must not publish a partial result.
    std::vector<uint8_t> bytes(size);
    if (!image.file || !image.file(address, bytes.data(), size)) return KILN_ERR_UNAVAILABLE;
    std::memcpy(out, bytes.data(), size);
    return KILN_OK;
}

class Registry {
    struct Replacement {
        uint64_t token;
        uintptr_t address;
        std::vector<uint8_t> original, installed;
        bool continuation;
    };
    std::mutex mutex_;
    std::vector<Replacement> replacements_;
    uint64_t next_token_ = 1;
    std::atomic<uint64_t> generation_{1};
    // Only the last range asked about is kept, so a query for another range moves the generation (kiln_hooks.h says
    // so). A map of every range callers ever asked about would grow for the life of the process.
    uintptr_t observed_address_ = 0;
    size_t observed_size_ = 0;
    uint32_t observed_state_ = KILN_CODE_UNKNOWN;
    bool observed_readable_ = false;
    std::vector<uint8_t> observed_;

    uint32_t classify(uintptr_t address, const std::vector<uint8_t>& file, const std::vector<uint8_t>& live) const {
        bool managed = false, foreign = false;
        for (size_t i = 0; i < live.size(); ++i) {
            if (live[i] == file[i]) continue;
            const uintptr_t at = address + i;
            uint8_t value = live[i];
            // Unwind overlapping managed layers, newest first. An entry's range alone cannot bless foreign bytes
            // written over it, or a foreign prologue that Kiln's trampoline merely chained over.
            for (auto it = replacements_.rbegin(); it != replacements_.rend(); ++it) {
                if (at < it->address || at - it->address >= it->installed.size()) continue;
                if (!it->continuation || it->original.size() != it->installed.size()) return KILN_CODE_UNKNOWN;
                const size_t offset = at - it->address;
                if (value != it->installed[offset]) break;
                value = it->original[offset];
            }
            if (value == file[i]) managed = true;
            else foreign = true;
        }
        return foreign ? KILN_CODE_FOREIGN : managed ? KILN_CODE_MANAGED : KILN_CODE_ORIGINAL;
    }
public:
    auto lock() { return std::unique_lock(mutex_); }
    // Writers call these ONLY while holding lock(), including the physical write. Prepare all allocations before
    // changing code. Tokens are never reused; a detour's fact is process-lifetime, as is its trampoline.
    uint64_t add_locked(uintptr_t address, const std::vector<uint8_t>& original,
                        const std::vector<uint8_t>& installed, bool continuation) {
        const auto token = next_token_++;
        replacements_.push_back({token, address, original, installed, continuation});
        touch_locked();
        return token;
    }
    void remove_locked(uint64_t token) {
        const auto it = std::find_if(replacements_.begin(), replacements_.end(),
                                    [=](const auto& r) { return r.token == token; });
        if (it != replacements_.end()) { replacements_.erase(it); touch_locked(); }
    }
    void touch_locked() { generation_.fetch_add(1, std::memory_order_relaxed); }
    // Dispatch cannot take a mutex. Disabling a faulted link changes no physical replacement facts; it only
    // invalidates cached ownership observations. Its later sweep publishes the new chain under lock().
    void note_chain_fault() { generation_.fetch_add(1, std::memory_order_relaxed); }

    KilnResult code_state(const Image& image, uintptr_t address, size_t size, uint32_t* state, uint64_t* generation) {
        if (!state || !generation || !valid_range(image, address, size)) return KILN_ERR_INVALID_ARGUMENT;
        std::vector<uint8_t> file(size), live(size), second(size);
        if (!image.file || !image.file(address, file.data(), size)) return KILN_ERR_UNAVAILABLE;
        auto guard = lock();
        const bool readable = image.live && image.live(address, live.data(), size) &&
            image.live(address, second.data(), size) && live == second;
        const uint32_t result = readable ? classify(address, file, live) : KILN_CODE_UNKNOWN;
        if (observed_address_ != address || observed_size_ != size || observed_readable_ != readable ||
            observed_state_ != result || (readable && observed_ != live)) touch_locked();
        observed_address_ = address;
        observed_size_ = size;
        observed_readable_ = readable;
        observed_state_ = result;
        if (readable) observed_ = std::move(live);
        else observed_.clear();
        *state = result;
        *generation = generation_.load(std::memory_order_relaxed);
        return KILN_OK;
    }
};
inline Registry& registry() {
    static auto& instance = *new Registry; // same lifetime as the registry's detours
    return instance;
}
} // namespace kiln::hooks::code
