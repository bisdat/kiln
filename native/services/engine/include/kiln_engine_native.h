// SPDX-License-Identifier: MIT
// kiln.internal.engine: Kiln-internal engine primitives for Kiln's own native services. NOT a public ABI: not in
// native/core/abi, not in kiln-abi, not in the SDK, and it may change between Kiln builds. The core resolves the name
// only through the KilnApi it hands to DLLs of kiln/services and accepts its publication only from such a DLL's owner
// (native/core/src/api.cpp; docs/kiln/services/README.md "Kiln-internal tables"). Plugins, managed mods and
// foreign DLLs never see it: a plugin holding the engine's Scene word would freeze the server.
//
// Consumers check header.version == KILN_ENGINE_NATIVE_VERSION, header.size covering every member they use and
// layout_size == sizeof(kiln::engine::Layout) (both sides are built from one tree); a mismatch is refused loudly by
// kiln_engine_native() below, and the consumer reports its own row unavailable with that reason.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "kiln_api.h"
#include "kiln_engine_layout.h"

#define KILN_ENGINE_NATIVE_SERVICE "kiln.internal.engine"
#define KILN_ENGINE_NATIVE_VERSION 1u

extern "C" {
struct KilnEngineNativeApi {
    KilnServiceHeader header;
    std::uint32_t layout_size, reserved; /* sizeof(kiln::engine::Layout) of the provider */
    /* The selected exact profile's validated layout, immutable for the process; NULL without an exact profile. */
    const kiln::engine::Layout*(KILN_CALL* layout)(void);
};
}
static_assert(offsetof(KilnEngineNativeApi, layout) == 16, "kiln.internal.engine v1 prefix");

// The table, or nullptr with a reason in `why` (cap bytes, NUL-terminated) and one log line through api->log for
// `owner`. `api` must be the KilnApi a kiln/services DLL received in kiln_plugin_main; any other KilnApi never resolves
// it. Looked up with min_version 1, so an older or newer provider is named "another Kiln build", not "missing".
inline const KilnEngineNativeApi* kiln_engine_native(const KilnApi* api, KilnOwner owner, std::size_t need, char* why,
                                                     std::size_t cap) {
    const auto say = [&](const char* text) {
        if (why && cap) std::snprintf(why, cap, "%s", text);
        if (api && api->log) {
            char line[256];
            const int n = std::snprintf(line, sizeof line, "kiln.internal.engine refused: %s", text);
            api->log(owner, KILN_LOG_WARN, {line, n > 0 ? static_cast<std::size_t>(n) : 0});
        }
        return static_cast<const KilnEngineNativeApi*>(nullptr);
    };
    if (!api || !api->service) return say("no KilnApi");
    const auto* t = static_cast<const KilnEngineNativeApi*>(
        api->service({KILN_ENGINE_NATIVE_SERVICE, sizeof KILN_ENGINE_NATIVE_SERVICE - 1}, 1));
    if (!t)
        return say("kiln.internal.engine is not here (kiln-engine.dll missing, of another build, or not a Kiln "
                   "service caller)");
    if (t->header.version != KILN_ENGINE_NATIVE_VERSION || t->header.size < need ||
        t->header.size < offsetof(KilnEngineNativeApi, layout) + sizeof t->layout ||
        t->layout_size != sizeof(kiln::engine::Layout))
        return say("kiln.internal.engine is from another Kiln build (version or layout size mismatch)");
    return t;
}
