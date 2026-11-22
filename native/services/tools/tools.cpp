// SPDX-License-Identifier: MIT
#include "kiln_api.h"
#include "services/kiln_caps.h"
#include "services/kiln_tools.h"
#include <cstring>

namespace {
KilnResult KILN_CALL status(KilnToolsStatus* out) {
    if (!out || out->size < sizeof(KilnToolsStatus)) return KILN_ERR_INVALID_ARGUMENT;
    *out = {};
    out->size = sizeof(*out);
    out->reason = KILN_CAP_REASON_SYMBOL_MISSING;
    return KILN_OK;
}
KilnResult KILN_CALL register_action(KilnOwner, const KilnToolActionDef*, KilnHandle* out) {
    if (out) *out = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL unregister_action(KilnOwner, KilnHandle) { return KILN_ERR_NOT_FOUND; }
const KilnToolsApi table = {{sizeof(KilnToolsApi), KILN_TOOLS_VERSION}, &status, &register_action, &unregister_action};
}
extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info) {
    if (!api || api->abi_version != KILN_ABI_VERSION || api->size < sizeof(KilnApi) || !info)
        return KILN_ERR_UNAVAILABLE;
    info->id = {KILN_TOOLS_SERVICE, sizeof(KILN_TOOLS_SERVICE) - 1};
    info->version = {"1", 1};
    const void* p = api->service({KILN_CAPS_SERVICE, sizeof(KILN_CAPS_SERVICE) - 1}, KILN_CAPS_VERSION);
    if (p) {
        const auto* caps = static_cast<const KilnCapsApi*>(p);
        constexpr char reason[] = "authoritative tool action context and command backend are not mapped on this build";
        caps->report(owner, {"tools.actions", 13}, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SYMBOL_MISSING,
                     {reason, sizeof(reason) - 1});
    }
    return api->register_service(owner, {KILN_TOOLS_SERVICE, sizeof(KILN_TOOLS_SERVICE) - 1}, &table);
}
