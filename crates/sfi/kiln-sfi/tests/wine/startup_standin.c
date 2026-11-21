/* SPDX-License-Identifier: MIT
 * A stand-in for kiln-eml.dll's early export (kiln.eml.startup v1, native/core/abi/services/kiln_eml_startup.h) for
 * kiln-sfi's Windows resolution test (tests/startup_seam_windows.rs). Synthetic: no game, no kiln-eml. Built twice by
 * the runner: as kiln-eml.dll and as other-name.dll (the same export in a module of another name). attach records
 * its key and issues session 0x51; every other member is UNAVAILABLE. Link it with /EXPORT:kiln_eml_startup_service.
 */
#include "kiln_eml_startup.h"

static uint64_t g_launch_id;
static uint32_t g_planned = 0xffffffffu;

static KilnResult KILN_CALL attach(const KilnEmlStartupKeyV1* key, uint32_t planned_dlls, uint64_t* session) {
    if (!key || !session || key->size < KILN_EML_STARTUP_KEY_SIZE || key->flags || key->session) return KILN_ERR_INVALID_ARGUMENT;
    g_launch_id = key->launch_id;
    g_planned = planned_dlls;
    *session = 0x51;
    return KILN_OK;
}
static KilnResult KILN_CALL witness_wait(const KilnEmlStartupKeyV1* key, uint32_t timeout_ms, KilnEmlStartupWitnessV1* out,
                                         char* reason, size_t capacity, size_t* required) {
    (void)key; (void)timeout_ms; (void)out; (void)reason; (void)capacity; (void)required;
    return KILN_ERR_UNAVAILABLE;
}
static KilnResult KILN_CALL witness_recheck(const KilnEmlStartupKeyV1* key, const KilnEmlStartupWitnessV1* witness,
                                            char* reason, size_t capacity, size_t* required) {
    (void)key; (void)witness; (void)reason; (void)capacity; (void)required;
    return KILN_ERR_UNAVAILABLE;
}
static KilnResult KILN_CALL native_terminal(const KilnEmlStartupKeyV1* key, KilnStr json) {
    (void)key; (void)json;
    return KILN_ERR_UNAVAILABLE;
}
static KilnResult KILN_CALL outcome(const KilnEmlStartupKeyV1* key, KilnEmlArchiveIdentityV1* identity, char* json,
                                    size_t capacity, size_t* required) {
    (void)key; (void)identity; (void)json; (void)capacity; (void)required;
    return KILN_ERR_UNAVAILABLE;
}

static const KilnEmlStartupApiV1 g_table = {
    {sizeof(KilnEmlStartupApiV1), KILN_EML_STARTUP_VERSION}, attach, witness_wait, witness_recheck, native_terminal, outcome,
    0, /* published_identity: kiln.boot's read, which kiln-sfi never makes */
};

/* Exported by the link (/EXPORT:kiln_eml_startup_service): the header declares it without dllexport. */
const KilnEmlStartupApiV1* KILN_CALL kiln_eml_startup_service(uint32_t min_version) {
    return min_version <= KILN_EML_STARTUP_VERSION ? &g_table : 0;
}
/* What the last attach saw (the test reads it back). */
__declspec(dllexport) uint64_t KILN_CALL standin_launch_id(void) { return g_launch_id; }
__declspec(dllexport) uint32_t KILN_CALL standin_planned(void) { return g_planned; }

int __stdcall DllMain(void* module, unsigned long reason, void* reserved) {
    (void)module; (void)reason; (void)reserved;
    return 1;
}
