// SPDX-License-Identifier: MIT
#include "services/kiln_tools.h"
#include <cstddef>
static_assert(sizeof(KilnToolsApi) == 64);
static_assert(sizeof(KilnToolFamilyDef) == 192);
static_assert(offsetof(KilnToolFamilyDef, identity) == 176);
static_assert(sizeof(KilnToolFamilyAction) == 48);
static_assert(sizeof(KilnToolPoolMaterial) == 16);
static_assert(sizeof(KilnToolBrush) == 16);
static_assert(sizeof(KilnToolMaterialRow) == 32);
static_assert(sizeof(KilnToolMaterialSnapshot) == 2088);
static_assert(sizeof(KilnToolPresentationStatus) == 16);
static_assert(offsetof(KilnToolsApi, define_family) == 32);
static_assert(offsetof(KilnToolsApi, remove_family) == 40);
static_assert(offsetof(KilnToolsApi, material_snapshot) == 48);
static_assert(offsetof(KilnToolsApi, presentation_status) == 56);
static_assert(offsetof(KilnToolsApi, status) == 8);
static_assert(offsetof(KilnToolsApi, register_action) == 16);
static_assert(offsetof(KilnToolsApi, unregister_action) == 24);
static_assert(KILN_TOOLS_VERSION == 1);
int main() { return 0; }
