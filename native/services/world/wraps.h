// SPDX-License-Identifier: MIT
// kiln.world v4 internals between the program directory (game_programs.cpp) and the wraps (wraps.cpp). The rules the
// wrap core keeps are chapter 06a section 9 (normative): 9.8 per-world lifetime and the prediction forward, 9.9
// balanced in-flight counts, 9.10 faults and budgets, 9.11 queued install and remove in the world's own Pre.
#pragma once
#include "world_internal.h"

namespace kw {

using UpdateFn = void (*)(void* ctx);

constexpr int kWrapPools = 2;      // one per world kind with a program table: server, client
constexpr int kWrapSlots = 896;    // a table's capacity: every program of a world can be wrapped at once
constexpr uint32_t kEntryUpdateAt = 0x28;  // the update pointer of a table entry (what ProgramRunner calls)

inline int wrap_pool(uint32_t kind) {
    return kind == KILN_WORLD_SERVER ? 0 : kind == KILN_WORLD_CLIENT ? 1 : -1;
}

// One program of the current directory of a world kind.
struct ProgramRef {
    int32_t index = -1;
    const std::string* name = nullptr;  // interned, life of the process
    uintptr_t entry = 0, desc = 0;
    bool no_update = true;
};

// ---- game_programs.cpp ----
bool game_program_lookup(uint32_t kind, std::string_view name, ProgramRef* out);  // the live directory, by name
bool game_program_at(uint32_t kind, int32_t index, ProgramRef* out);              // the live directory, by index
bool game_programs_has_directory(uint32_t kind);
uint32_t current_thread_id();
std::string module_name(uintptr_t address);  // the file name of the module that holds an address ("" when none)
const std::string* game_program_intern(std::string_view name);

// ---- wraps.cpp ----
void wraps_client_ready(bool tick, bool events);  // boot: enable only with lifecycle and both subscriptions
void wraps_define_row(bool programs);              // boot: systems.define follows kiln.world's own programs
void wraps_init();                                       // boot: the trampoline map, capability rows
void wraps_pre(uint32_t kind, uintptr_t world);          // that world's thread, Pre, after its directory is current
void wraps_world_event(const KilnWorldEvent* event);     // a world of a kind starts or stops
bool game_in_slice();                                    // inside a wrap stage on this thread
uintptr_t wraps_resolve(uintptr_t entry, uintptr_t current);  // the game's update behind a trampoline, else current
void game_program_wrap_stats(uint32_t kind, const std::string* name, KilnGameProgramInfo* out);
void game_wraps_status(KilnGameProgramsStatus* out);
const char* game_wraps_off_reason();                     // "" while wraps work

KilnResult KILN_CALL game_program_wrap(KilnOwner owner, uint32_t world_kind, int32_t index, int32_t stage,
                                       int32_t priority, KilnWrapStageFn fn, void* user, KilnHandle* out);
KilnResult KILN_CALL game_program_unwrap(KilnHandle wrap);
KilnResult KILN_CALL game_wrap_info(KilnHandle wrap, KilnGameWrapInfo* out);
KilnResult KILN_CALL game_wraps_release(KilnOwner owner, uint32_t* out_pending);
KilnResult KILN_CALL game_slice_run_original(KilnGameSlice* slice);
uint64_t KILN_CALL game_world_session(uint32_t kind);
KilnResult KILN_CALL game_program_contract(uint32_t kind, int32_t index, uint32_t* out_contract, char* text, uint32_t capacity,
                                           uint32_t* out_size);
// ---- foreign.cpp (since 0.2.0-dev.29, chapter 06c 9.1) ----
KilnResult KILN_CALL foreign_layout(uint32_t world_kind, KilnStr name, KilnForeignLayout* out);
int32_t KILN_CALL foreign_slot(const KilnForeignLayout* layout, int32_t list, KilnStr name);
KilnResult KILN_CALL foreign_check(const KilnForeignLayout* layout, const uint64_t* iter, uint32_t* out_counts);
const void* KILN_CALL foreign_read(const KilnForeignLayout* layout, const uint64_t* iter, KilnStr component, uint32_t entity,
                                   uint32_t* size, KilnResult* rc);
void* KILN_CALL foreign_write(const KilnForeignLayout* layout, const uint64_t* iter, KilnStr component, uint32_t entity,
                              uint32_t* size, KilnResult* rc);
KilnResult KILN_CALL foreign_attr_changed(const KilnForeignLayout* layout, const uint64_t* iter, void* component);
uint64_t KILN_CALL wrap_original(uintptr_t entry);
uint32_t KILN_CALL in_program();  // the table's in_program: a Kiln program's update, a wrap stage or a kiln.types
                                  // system_hook callback on this thread
void in_program_resolve(const KilnTypesApi* types);  // on_ready: kiln.types' in_system_callback (null: that part is 0)
KilnResult KILN_CALL game_slice_iterate(KilnGameSlice* slice, KilnSliceEntityFn each, void* user);
void* KILN_CALL game_slice_slot(KilnGameSlice* slice, const uint64_t* iter, int32_t list, int32_t i, uint32_t* size,
                                uint8_t* writable);
KilnResult KILN_CALL game_slice_mark(KilnGameSlice* slice, const uint64_t* iter, int32_t list, int32_t i);
KilnResult KILN_CALL game_slice_archetype(KilnGameSlice* slice, uintptr_t* out_layout);
int32_t KILN_CALL game_archetype_has(uint32_t world_kind, uintptr_t layout, uint32_t component_hash);
KilnResult KILN_CALL game_slice_attr_validate(KilnGameSlice* slice, const uint64_t* iter, int32_t list, int32_t i);
KilnResult KILN_CALL game_slice_attr_changed(KilnGameSlice* slice, const uint64_t* iter, int32_t list, int32_t i);
int32_t KILN_CALL game_program_list(uint32_t world_kind, int32_t index, int32_t list, KilnStr* names, uint32_t capacity);

// ---- seh.cpp ----
bool seh_wrap_stage(KilnWrapStageFn fn, KilnGameSlice* slice, void* user, unsigned long* code);
// compare-exchange of a table entry's update pointer; false when the address faulted (*seen untouched)
bool seh_cas_ptr(uintptr_t address, uintptr_t expected, uintptr_t desired, uintptr_t* seen);

// ---- host tests ----
void wraps_test_budget_ns(uint64_t ns);
uint32_t wraps_test_in_flight(int pool);  // the sum over a pool's slots
}  // namespace kw
