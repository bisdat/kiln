// SPDX-License-Identifier: MIT
#include "services/kiln_tools.h"
#include <cstddef>
static_assert(sizeof(KilnToolsApi) == 32);
static_assert(offsetof(KilnToolsApi, status) == 8);
static_assert(offsetof(KilnToolsApi, register_action) == 16);
static_assert(offsetof(KilnToolsApi, unregister_action) == 24);
static_assert(KILN_TOOLS_VERSION == 1);
int main() { return 0; }
