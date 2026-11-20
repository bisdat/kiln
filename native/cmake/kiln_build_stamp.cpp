// SPDX-License-Identifier: MIT
// Compiled into every DLL built with kiln_service/kiln_plugin (native/CMakeLists.txt): the Kiln build it belongs to.
// The core compares a kiln/services DLL's stamp with its own (api.cpp load_one) before that DLL may publish or resolve
// Kiln-internal tables (docs/kiln/services/README.md "Kiln-internal tables"). KILN_BUILD comes from
// cmake/kiln_build_stamp.sh; a DLL built without it exports the version only, which no core trusts.
#include "kiln_api.h"
#include "kiln_version.h"
#if __has_include("kiln_build.h")
#include "kiln_build.h"
#endif
#ifndef KILN_BUILD
#define KILN_BUILD ""
#endif

extern "C" KILN_EXPORT const char* KILN_CALL kiln_build_stamp(void) { return KILN_VERSION KILN_BUILD; }
