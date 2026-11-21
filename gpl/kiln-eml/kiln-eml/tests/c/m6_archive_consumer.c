/* SPDX-License-Identifier: GPL-3.0-or-later
 * A native consumer of kiln-eml's real kiln.eml.archive export, compiled against the MIT header
 * native/core/abi/services/kiln_eml_archive.h (its layout asserts included). It resolves the early getter from the
 * built module by name, as an early native consumer does, then checks only what needs no startup controller grant:
 * getter versions and identity, the table's version/size, KILN_EML_ARCHIVE_HAS for the metadata subset and the error
 * channel (and null for every other slot), and refusals that grant nothing.
 * Usage: m6_archive_consumer <path to kiln-eml.dll / libkiln_eml.so>. Exit 0 when every check passes. */
#include "services/kiln_eml_archive.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int checks, failures;
static void check(int ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; fprintf(stderr, "FAIL %s\n", what); }
}

int main(int argc, char** argv) {
    KilnEmlArchiveServiceFn getter;
    const KilnEmlArchiveApiV1* api;
    KilnEmlArchiveIdentityV1 pins;
    KilnEmlArchiveView view = 0;
    KilnArchiveErrorV1 e;
    size_t required = 0;
    char json[16];
    if (argc != 2) { fprintf(stderr, "usage: %s <module>\n", argv[0]); return 2; }
#ifdef _WIN32
    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) { fprintf(stderr, "cannot load %s (%lu)\n", argv[1], GetLastError()); return 2; }
    getter = (KilnEmlArchiveServiceFn)(void*)GetProcAddress(module, KILN_EML_ARCHIVE_SERVICE_EXPORT);
#else
    void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) { fprintf(stderr, "cannot load %s: %s\n", argv[1], dlerror()); return 2; }
    getter = (KilnEmlArchiveServiceFn)dlsym(module, KILN_EML_ARCHIVE_SERVICE_EXPORT);
#endif
    check(getter != NULL, "the module exports kiln_eml_archive_service");
    if (!getter) return 1;
    api = getter(1);
    check(api != NULL && getter(0) == api, "versions 0 and 1 give the same table");
    check(getter(2) == NULL, "version 2 gives none");
    if (!api) return 1;
    check(api->header.version == KILN_EML_ARCHIVE_VERSION && api->header.size == sizeof(KilnEmlArchiveApiV1),
          "v1 table of the header's 152 bytes");
    check(KILN_EML_ARCHIVE_HAS(api, acquire) && KILN_EML_ARCHIVE_HAS(api, identity) &&
          KILN_EML_ARCHIVE_HAS(api, release) && KILN_EML_ARCHIVE_HAS(api, registry_metadata),
          "the metadata subset is present");
    check(KILN_EML_ARCHIVE_HAS(api, error_copy) && KILN_EML_ARCHIVE_HAS(api, error_release), "the error channel is present");
    check(!KILN_EML_ARCHIVE_HAS(api, resource_json) && !KILN_EML_ARCHIVE_HAS(api, find_resource) &&
          !KILN_EML_ARCHIVE_HAS(api, list_resources) && !KILN_EML_ARCHIVE_HAS(api, read_resource) &&
          !KILN_EML_ARCHIVE_HAS(api, content_read_range) && !KILN_EML_ARCHIVE_HAS(api, list_contents) &&
          !KILN_EML_ARCHIVE_HAS(api, value_decode) && !KILN_EML_ARCHIVE_HAS(api, value_encode) &&
          !KILN_EML_ARCHIVE_HAS(api, loaded_field) && !KILN_EML_ARCHIVE_HAS(api, attribute_catalog) &&
          !KILN_EML_ARCHIVE_HAS(api, attribute_resolve) && !KILN_EML_ARCHIVE_HAS(api, attribute_evaluate),
          "no other slot is filled");
    /* Getter presence grants no view: no generation was authorized in this process. */
    memset(&pins, 0x5a, sizeof pins);
    pins.size = sizeof pins;
    pins.phase = KILN_EML_ARCHIVE_PUBLISHED;
    check(api->acquire(7, &pins, &view) == KILN_ERR_UNAVAILABLE && view == 0, "no authorized generation: UNAVAILABLE");
    check(api->acquire(0, &pins, &view) == KILN_ERR_INVALID_ARGUMENT, "owner 0 refused");
    pins.size = sizeof pins - 1;
    check(api->acquire(7, &pins, &view) == KILN_ERR_INVALID_ARGUMENT, "short identity refused");
    check(api->release(0) == KILN_ERR_INVALID_ARGUMENT, "release of token 0 refused");
    pins.size = sizeof pins; /* a valid record: the refusal is the unknown view's, not the short record's */
    check(api->identity(12345, &pins) == KILN_ERR_INVALID_ARGUMENT, "identity of an unknown view refused");
    memset(&e, 0, sizeof e);
    e.size = sizeof e;
    e.token = 99;
    check(api->registry_metadata(12345, 0, 0, json, sizeof json, &required, &e) == KILN_ERR_INVALID_ARGUMENT &&
          e.result == KILN_ERR_INVALID_ARGUMENT && e.token == 0 && e.message_bytes == 0 && e.size == sizeof e,
          "unknown view: INVALID_ARGUMENT with the error record initialized");
    e.flags = 1;
    e.token = 99;
    check(api->registry_metadata(12345, 0, 0, json, sizeof json, &required, &e) == KILN_ERR_INVALID_ARGUMENT && e.token == 99,
          "an invalid error record is refused before work and not written");
    check(api->error_copy(7, 0, NULL, 0, &required) == KILN_ERR_INVALID_ARGUMENT, "error_copy of token 0 refused");
    check(api->error_release(7, 0) == KILN_ERR_INVALID_ARGUMENT, "error_release of token 0 refused");
    printf("kiln.eml.archive native consumer: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
