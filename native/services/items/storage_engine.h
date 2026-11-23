// SPDX-License-Identifier: MIT
#pragma once
#include "engine.h"
#include "storage_logic.h"
namespace kitems::storage {
struct Result8 {
    uint8_t error = 0, pad[3]{};
    uint32_t value = 0;
};
// Two pointers, never interchangeable (#135): Access::storage is the world's entity storage, at world + world.storage
// (E.world_storage); a World is what api->server_world() returns. Game calls that add world.storage themselves take the
// World: GetComponent, WriteComponentTick, items/CreateEntity, PideFromRecord's context +0 and an EntityRef. Calls that
// take the storage: the program accessors (read/write_other), spawn/DestroyEntity, gameplay/DeliverEvents.
inline uintptr_t world_of(const void* storage) {
    return storage ? reinterpret_cast<uintptr_t>(storage) - static_cast<uintptr_t>(E.world_storage) : 0;
}
// The game's entity reference. +8 is the World, not its entity storage: characters/CapturesDone fills it with the world
// it also looks player rows up in, and RecordFromStack resolves a stack's PIDE row through it at
// world + world.storage + storage.entities (client 0x9b2780, server 0x64e610).
struct EntityRef {
    uint32_t id;
    uint8_t kind, pad;
    uint16_t family;
    uintptr_t world;
    void* row;
};
struct ComponentDesc {
    uint32_t hash, pad;
    const void* bytes;
    uint64_t size;
};
struct CreateDesc {
    uint32_t requested;
    uint8_t guid[16];
    uint32_t pad;
    ComponentDesc* components;
    uint64_t count;
    uint16_t reserved, unknown;
    uint8_t flags, padding[3];
};
static_assert(sizeof(EntityRef) == 24 && sizeof(CreateDesc) == 48 && offsetof(CreateDesc, flags) == 44 &&
              sizeof(ComponentDesc) == 24);
using ConvertFn = Result8*(__fastcall*)(Result8*, void*, const void*, uint8_t, void*, void*, EntityRef*);
using WriteFn = void(__fastcall*)(void*, const void*);
using ReadFn = void(__fastcall*)(void*, void*);
using WriteTickFn = PtrSize*(__fastcall*)(PtrSize*, uintptr_t, uint32_t, uint32_t);
struct ItemTraits {
    uint16_t max_stack = 0;
    uint8_t category = 0, equip = 0;
    bool pide = false;
};
bool item_traits(void* registry, uint32_t item, ItemTraits*);
using RebuildFn = uint32_t(__fastcall*)(const void*, uint32_t, const void*, const void*);
// items/CreateEntity is the World wrapper (client 0x984e50, server 0x620ce0): mov r8, rdx; add rcx, world.storage;
// mov edx, [.rdata 0]; xor r9d, r9d; jmp spawn/CreateEntity(storage, 0, desc, nullptr). Handed the entity storage, the
// game reads world + 2 * world.storage, past the end of the 0x15fd3c0-byte World: the access violation of #135.
using CreateFn = uint32_t(__fastcall*)(uintptr_t world, const CreateDesc*);
// spawn/DestroyEntity takes the entity storage itself (lea rdi, [rcx + storage.entities]).
using DestroyFn = uint8_t(__fastcall*)(void* storage, uint32_t, uint8_t, uint8_t);
struct EngineStorage {
    Layout layout{};
    ConvertFn convert = nullptr;
    WriteFn write = nullptr;
    ReadFn read = nullptr;
    WriteTickFn tick_write = nullptr;
    RebuildFn rebuild = nullptr;
    CreateFn create = nullptr;
    DestroyFn destroy = nullptr;
    bool (*structural_gate)() = nullptr;
    uintptr_t (*server_world)() = nullptr; // the live server World: create_holder hands the game only this one
};
extern EngineStorage S;
inline constexpr uint8_t default_inventory_guid[]{0x6d, 0x71, 0xdf, 0xcd, 0x7c, 0xde, 0x73, 0x4a,
                                                  0x8f, 0x5a, 0x3d, 0xef, 0xf2, 0xde, 0x67, 0x82};
// These wrappers never let an engine exception cross the service boundary. Returned item bytes use the game writer.
bool record_from_memory(const uint8_t*, Record*);
bool memory_from_record(const Record&, uint8_t out[56]);
bool inventory_from_character(const void*, Inventory*);
bool character_witness(const void*, uint64_t*, uint32_t baseline = 5, uint32_t* flags = nullptr);
// Internal diagnostics only, never exposed in the service ABI. A reported destroy success is NOT rollback proof:
// lookup faults can look like missing components, and a failed create may have allocated an unknown entity.
struct NativeCall {
    uint32_t value = 0, error = 0;
    bool called = false;
};
struct HolderCleanup {
    bool attempted = false, reported_ok = false;
    uint32_t calls = 0, succeeded = 0, failed_entity = 0;
    NativeCall last{}, first_failed{};
};
enum class HolderStage { None, Preconditions, World, Gate, RootCreate, PageCreate, SetupLookup, SetupCopy };
struct HolderBuild {
    HolderStage stage = HolderStage::None;
    uint32_t page = UINT32_MAX, root = 0, pages = 0, error = 0;
    uint64_t setup_size = 0;
    NativeCall native{};
    HolderCleanup cleanup{};
    bool retryable() const { return stage == HolderStage::Gate && !native.called; }
};
bool create_holder(const Access&, uint32_t player, uint32_t slots, Holder*, HolderBuild* = nullptr);
bool destroy_holder(const Access&, const Holder*, HolderCleanup* = nullptr);
bool insert_record(const Access&, const Holder&, const KilnStorageRecord&, uint32_t* reason);
bool freeze_holder(const Holder&, void* attributes, void* bindings, EntityRef*, Entry*);
bool clear_holder_references(uintptr_t world, uint32_t player, const Holder&);
// only_empty: write only when the slot points nowhere; only_from: write only when it points into one of these pages.
bool reference_write(const Access&, uint32_t player, uint32_t equip, uint64_t slot, bool only_empty = false,
                     const std::vector<uint32_t>* only_from = nullptr);
bool component_copy(uintptr_t world, uint32_t hash, uint32_t entity, void* out, size_t size);
} // namespace kitems::storage
