/* SPDX-License-Identifier: MIT
 * A stand-in for kiln-eml.dll's early exports kiln_eml_archive_service and kiln_eml_startup_service
 * (boot_archive_eml_windows_host.cpp), built as kiln-eml.dll, as other-name.dll and, with STANDIN_OLD_STARTUP, as a
 * kiln-eml.dll from before published_identity (a 48-byte kiln.eml.startup table without published_identity). Its archive table is a
 * header only: the host test checks who finds it, not its members. Its published_identity hands a fixed identity once
 * standin_grant(1) was called, and refuses (UNAVAILABLE) before; it keeps the key it was called with.
 * PRIVATE (tools/sfi/public_cut.py). */
#include "services/kiln_eml_startup.h"

static unsigned g_asked;
static KilnEmlArchiveApiV1 g_table; /* zero: every member NULL */
static unsigned g_startup_asked;
static int g_grant;
static KilnEmlStartupKeyV1 g_key;

/* exported by the link (/EXPORT in test-boot-archive-eml-wine.sh): the header declares it without dllexport */
const KilnEmlArchiveApiV1* KILN_CALL kiln_eml_archive_service(uint32_t min_version) {
    g_asked = min_version;
    g_table.header.size = sizeof g_table;
    g_table.header.version = KILN_EML_ARCHIVE_VERSION;
    return min_version <= KILN_EML_ARCHIVE_VERSION ? &g_table : NULL;
}

#ifndef STANDIN_OLD_STARTUP
static KilnResult KILN_CALL published_identity(const KilnEmlStartupKeyV1* key, KilnEmlArchiveIdentityV1* identity) {
    uint32_t size;
    int i;
    if (!key || !identity || key->size < KILN_EML_STARTUP_KEY_SIZE || key->flags || key->session ||
        identity->size < sizeof *identity)
        return KILN_ERR_INVALID_ARGUMENT;
    g_key = *key;
    if (!g_grant) return KILN_ERR_UNAVAILABLE;
    size = identity->size;
    for (i = 0; i < (int)sizeof *identity; ++i) ((unsigned char*)identity)[i] = 0;
    identity->size = size;
    identity->phase = KILN_EML_ARCHIVE_PUBLISHED;
    for (i = 0; i < 8; ++i) identity->launch_id[8 + i] = (uint8_t)(key->launch_id >> (56 - 8 * i));
    for (i = 0; i < 32; ++i) identity->plan_sha256[i] = key->plan_sha256[i];
    identity->generation = 77;
    return KILN_OK;
}
#endif

static KilnEmlStartupApiV1 g_startup;
const KilnEmlStartupApiV1* KILN_CALL kiln_eml_startup_service(uint32_t min_version) {
    g_startup_asked = min_version;
#ifdef STANDIN_OLD_STARTUP
    g_startup.header.size = 48; /* attach .. outcome only (all NULL here): no published_identity */
#else
    g_startup.header.size = sizeof g_startup;
    g_startup.published_identity = published_identity;
#endif
    g_startup.header.version = KILN_EML_STARTUP_VERSION;
    return min_version <= KILN_EML_STARTUP_VERSION ? &g_startup : NULL;
}

__declspec(dllexport) unsigned __cdecl standin_asked(void) { return g_asked; }
__declspec(dllexport) unsigned __cdecl standin_startup_asked(void) { return g_startup_asked; }
__declspec(dllexport) void __cdecl standin_grant(int grant) { g_grant = grant; }
__declspec(dllexport) int __cdecl standin_granted(void) { return g_grant; }
__declspec(dllexport) uint64_t __cdecl standin_key_launch(void) { return g_key.launch_id; }
