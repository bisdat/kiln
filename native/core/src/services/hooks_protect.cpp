// SPDX-License-Identifier: MIT
// kiln.internal.hooks v1 (../hooks_protect_internal.h): code ranges Kiln's own services protect for their engine calls.
// The ranges live in kiln.hooks' code registry (hooks_code.h) under the lock kiln.hooks' writers of game code hold:
// hooks.cpp's patch and write and hooks_chain.cpp's first detour on a target refuse an overlap there, before they read
// the old bytes, write or stop a thread. api.cpp serves this table through the services' KilnApi only.
//
// In-core: picked up by the CMake glob with the other services, but no KILN_SERVICE of its own (an internal table gets
// no capability row).
#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>

#include "../common.h"
#include "../hooks_protect_internal.h"
#include "../image.h"
#include "../log.h"
#include "../registry.h"
#include "../sigdb.h"
#include "hooks_code.h"
#include "hooks_internal.h"

namespace kiln::hooks {
namespace seh {
bool copy(void* dst, const void* src, size_t n);
}  // namespace seh
namespace {

// The owner-retiring notification releases a retired owner's protections. Subscribed once, at the first call: until a
// service protects something there is nothing to release.
std::mutex g_observer_mutex;
bool g_observing = false;

void KILN_CALL owner_retiring(KilnOwner owner, void*) {
    const size_t released = code::registry().release_owner(owner);
    if (released)
        KLOG_I("hooks: %zu protected code range(s) of owner %llu released (the owner retired)", released,
               static_cast<unsigned long long>(owner));
}

bool observe_retirement() {
    std::lock_guard lock(g_observer_mutex);
    if (!g_observing) g_observing = reg::add_owner_retiring(0, &owner_retiring, nullptr, nullptr) == KILN_OK;
    return g_observing;
}

bool owner_alive(uint64_t owner) { return reg::owner_alive(owner); }

// The exe's code as read_original and code_state see it (hooks.cpp code_image()).
code::Image code_image() {
    const auto& i = image();
    return {i.base, i.size, reinterpret_cast<uintptr_t>(i.text), i.text_size, &sigdb::file_bytes,
        +[](uintptr_t a, uint8_t* out, size_t n) { return seh::copy(out, reinterpret_cast<const void*>(a), n); }};
}

const char* state_name(uint32_t s) {
    switch (s) {
        case KILN_CODE_ORIGINAL: return "original";
        case KILN_CODE_MANAGED: return "changed by a Kiln hook or patch";
        case KILN_CODE_FOREIGN: return "changed by another module";
        default: return "unknown (unreadable, or changing)";
    }
}

KilnResult protect(uint64_t owner, const KilnCodeRange* ranges, size_t n, KilnStr reason) {
    const auto& img = image();
    if (!img.text || img.text_size == 0) return KILN_ERR_UNAVAILABLE;
    if (!observe_retirement()) {
        static std::atomic<bool> once{false};
        if (!once.exchange(true))
            KLOG_E("hooks: cannot observe owner retirement; code ranges cannot be protected for engine calls");
        return KILN_ERR_UNAVAILABLE;
    }
    code::ProtectOutcome o;
    const KilnResult rc = code::registry().protect(code_image(), owner, ranges, n, sv(reason), &owner_alive, &o);
    const std::string why(sv(reason));
    if (rc == KILN_OK && o.added)
        KLOG_I("hooks: %s protected %zu code range(s) (%zu bytes) for an engine call (%s): kiln.hooks refuses installs, "
               "patches and writes over them",
               reg::owner_name(owner).c_str(), o.added, o.added_bytes, why.c_str());
    else if (rc == KILN_ERR_UNAVAILABLE && o.full)
        KLOG_W("hooks: %s cannot protect more code for an engine call (%s): at most %u ranges and %u bytes are protected",
               reg::owner_name(owner).c_str(), why.c_str(), KILN_HOOKS_PROTECT_MAX_RANGES, KILN_HOOKS_PROTECT_MAX_BYTES);
    else if (rc == KILN_ERR_UNAVAILABLE && o.address)
        KLOG_D("hooks: %s: code at rva 0x%llx not protected for an engine call (%s): it is %s",
               reg::owner_name(owner).c_str(), static_cast<unsigned long long>(o.address - img.base), why.c_str(),
               state_name(o.state));
    return rc;
}

KilnResult KILN_CALL h_protect_code_ranges(uint64_t owner, const KilnCodeRange* ranges, size_t n, KilnStr reason) {
    try {
        return protect(owner, ranges, n, reason);
    } catch (const std::exception& e) {
        static std::atomic<bool> once{false};
        if (!once.exchange(true)) KLOG_E("hooks: protect_code_ranges failed: %s", e.what());
    } catch (...) {
        static std::atomic<bool> once{false};
        if (!once.exchange(true)) KLOG_E("hooks: protect_code_ranges failed (unknown exception)");
    }
    return KILN_ERR_INTERNAL;
}

const KilnHooksInternalApi kInternalTable = {
    {sizeof(KilnHooksInternalApi), KILN_HOOKS_INTERNAL_VERSION},
    &h_protect_code_ranges,
};

}  // namespace

const KilnHooksInternalApi* internal_table() { return &kInternalTable; }

}  // namespace kiln::hooks
