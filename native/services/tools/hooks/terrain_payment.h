// SPDX-License-Identifier: MIT
// Private implementation of tool placement costs inside the game's building operation.
#pragma once
#include "kiln_api.h"
#include <cstddef>
#include <cstdint>

namespace kiln_tools_payment {
// The game's material list; independently qualified on both exact builds. The first 16 bytes of a plain item cost
// are zero. The same 24-byte record is paid and copied to the native building history; never a mining-drop receipt.
struct Material { uint64_t reserved[2]; uint32_t item, count; };
struct MaterialList { Material* data; uint64_t count, capacity; void* grow; };
static_assert(sizeof(Material) == 24 && offsetof(Material, item) == 16 && offsetof(Material, count) == 20);
static_assert(sizeof(MaterialList) == 32 && offsetof(MaterialList, count) == 8);
using CostFn = void (*)(void* list, void* operation, void* description, float volume);
using WrapperFn = float (*)(void* list, void* operation, void* actor, void* inventories, void* items,
                            void* description, float volume, void* sources);
using AppendFn = Material* (*)(void* list, const Material* record);
}

// Called once by the tools service after service discovery. COST is reported only when both hooks are installed.
extern "C" bool kiln_tools_payment_install(const KilnApi* api, KilnOwner owner);
