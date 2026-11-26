// SPDX-License-Identifier: MIT
// kiln.registry.live v1's C table (native/core/abi/services/kiln_registry_live.h) over registry_live.h: the argument and
// owner checks, the result struct and the copies out. stage_service.cpp binds it to the core (owner ids through
// kiln.internal.owners, the stage's generation) and registers it; on the host, registry_live_test binds fakes (review2 S5:
// these wrappers compiled only into the Windows DLL, so no test reached them).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "services/kiln_registry_live.h"

namespace kc::registry::stage {

struct LiveTableEnv {
    // The mod id Kiln binds an owner to (empty: no such owner, or no mod id); *revoked: revoked or never made.
    std::string (*bound_mod)(KilnOwner owner, bool* revoked) = nullptr;
    // The registry stage's generation (KilnRegistryLiveResult.generation, KilnRegistryLiveStatus.generation).
    uint64_t (*generation)() = nullptr;
};
// Binds the table to env (once, before it is registered; a later call replaces the env) and returns it.
const KilnRegistryLiveApi* live_table(const LiveTableEnv& env);
// The bound env's check of an owner: KILN_OK and *mod (may be null) its mod id; REVOKED; INVALID_ARGUMENT (0, no mod).
KilnResult bound_owner(KilnOwner owner, std::string* mod = nullptr);

// The C side's text helpers, shared with stage_service.cpp's other tables.
inline std::string str(KilnStr s) { return s.ptr ? std::string(s.ptr, s.len) : std::string(); }
inline bool str_ok(KilnStr s, size_t max) { return (s.ptr || !s.len) && s.len <= max; }
// text into buf (always 0-terminated when cap > 0); *out_len: its whole length. BUFFER_TOO_SMALL: cut.
inline KilnResult copy_out(const std::string& text, char* buf, size_t cap, size_t* out_len) {
    if (!buf && cap) return KILN_ERR_INVALID_ARGUMENT;
    if (out_len) *out_len = text.size();
    if (cap) {
        const size_t n = text.size() < cap - 1 ? text.size() : cap - 1;
        std::memcpy(buf, text.data(), n);
        buf[n] = 0;
    }
    return text.size() + 1 > cap ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}
// The same for a report beside a result: no error of its own (a cut report still returns the call's result).
inline void report_out(const std::string& text, char* buf, size_t cap, size_t* out_len) {
    if (out_len) *out_len = text.size();
    if (buf && cap) {
        const size_t n = text.size() < cap - 1 ? text.size() : cap - 1;
        std::memcpy(buf, text.data(), n);
        buf[n] = 0;
    }
}

}  // namespace kc::registry::stage
