// SPDX-License-Identifier: MIT
// kiln.items: custom names of containers (the Rename button), read from the game's string table. shroudlink finds
// them by scanning the heap for the record layout (names.rs, MIT, bjck/shroudlink v0.8.0); Kiln reaches the table
// itself, the `CustomPlayerStringsSystem` singleton's hash map, and checks every record the same way: the text's
// FNV-1a 64 is the id the entity carries.
#pragma once
#include "kiln_api.h"
#include "services/kiln_items.h"

namespace kitems {

// The name of `entity` in the server `world`. KILN_OK with the text in buf (NUL-terminated, cut to capacity - 1) and
// *out_len its full length; KILN_ERR_NOT_FOUND no name; KILN_ERR_BUFFER_TOO_SMALL cut; KILN_ERR_UNAVAILABLE feature
// off, no table, or a table layout this build does not match. Guarded against faults; the tick thread (the table
// singleton is looked up there once per world); other threads get names only once it has been.
KilnResult name_of(uintptr_t world, KilnEntity entity, char* buf, uint32_t capacity, uint32_t* out_len);
void names_world_closed();                      // forget the cached table pointer
uint32_t names_found();                         // lookups that found a name
void names_detail(char* out, uint32_t cap);     // why names are off or what the table looks like

// v9 (since 0.2.0-dev.30.3): the game side of a rename, tick thread, guarded (false: the call could not be made). The
// name component (has it, its id); the string table (holds `id`, records in use, its limit); set_string with the id it
// releases (its error byte and the new id); the tracked write of the component (where its id is) and the id into it;
// AddComponent with the id.
bool name_component(uintptr_t world, KilnEntity entity, bool* has, uint64_t* id);
bool names_table(uintptr_t world, uint64_t id, bool* holds, uint32_t* used, uint32_t* limit);
bool names_set_string(uintptr_t world, uint64_t old_id, const char* text, uint32_t len, uint8_t* error, uint64_t* id);
bool names_track(uintptr_t world, KilnEntity entity, void** slot);
bool names_store(void* slot, uint64_t id);
bool names_add_id(uintptr_t world, KilnEntity entity, uint64_t id);

// Lab: names `entity` through the game's own set_string and writes the id into its component. Returns a message.
bool name_set_for_lab(uintptr_t world, KilnEntity entity, const char* text, char* msg, uint32_t cap);

}  // namespace kitems
