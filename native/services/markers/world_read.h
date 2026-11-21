// SPDX-License-Identifier: MIT
// kiln.markers' explicit-world reader (chapter 04 §2.6, §4.2): the game's GetComponent called with an explicit world
// pointer (never kiln.world's cached server world, so it works on a joined client), under a structured-exception guard,
// the bytes copied at once and never kept. The position of a server world's entity is its CurrentTransform; a client
// world's is the first WorldTransform of its RenderTransform (not the interpolation source or target). layout_check
// compares both with the game's own reflection (kiln.types) before anchors turn on: a layout that differs turns anchors
// off, it is never followed.
#ifndef KILN_MARKERS_WORLD_READ_H
#define KILN_MARKERS_WORLD_READ_H
#include <cstdint>
#include <string>

#include "kiln_api.h"
#include "services/kiln_types.h"

namespace kmk {

// The engine's component reference (16 bytes): the component's bytes in the world's storage, or null.
struct ComponentSlice {
    const void* data;
    uint64_t size;
};
using GetComponentFn = ComponentSlice* (*)(ComponentSlice* out, uintptr_t world, uint32_t hash, uint32_t entity);

// Where a world kind's position comes from.
struct TransformSource {
    uint32_t hash;     // FNV-1a-32 of the qualified name: the component key
    uint32_t size;     // its reflected size
    const char* name;  // the qualified name kiln.types answers for the hash
};
inline constexpr TransformSource kServerTransform{0x027ac564u, 56, "keen::ecs::CurrentTransform"};
inline constexpr TransformSource kClientTransform{0x3b8476ddu, 176, "keen::ecs::RenderTransform"};
inline const TransformSource& transform_source(uint32_t world_kind) {
    return world_kind == KILN_WORLD_CLIENT ? kClientTransform : kServerTransform;
}

enum class ReadKind : uint8_t {
    Ok,           // position holds the entity's position
    Absent,       // no such entity, or it has no transform (not created yet, not replicated here, destroyed)
    WrongSize,    // the component's size is not the checked one (the layout moved under us)
    Fault,        // the call or the copy faulted
    BadPosition,  // read, but not finite or outside the world
};

struct TransformRead {
    ReadKind kind = ReadKind::Absent;
    KilnVec3 position{};
};

// World bounds of every marker position (chapter 04 §4.2): x and z in [-1000, 12000], y in [-1000, 5000].
bool position_ok(const KilnVec3& p);
// The engine's 32.32 fixed-point metres.
KilnVec3 fixed_to_metres(const int64_t p[3]);
// A transform component's bytes (its first 24 bytes are the position's s64 x, y, z): Ok, WrongSize or BadPosition.
TransformRead decode_transform(const void* data, uint64_t size, const TransformSource& src);

// One guarded read of entity's transform in world (the world's thread, between its steps): fn(&slice, world, hash,
// entity), the bytes copied at once. The host build catches a C++ exception from a fake as the fault.
TransformRead read_transform(GetComponentFn fn, uintptr_t world, uint32_t world_kind, uint32_t entity);

// The exact reflected layout of world_kind's transform through kiln.types (any thread: its table is immutable): the
// component's name, hash and size, the WorldTransform at +0 (position +0 as three s64, orientation +24, scale +40, 56
// bytes), and for RenderTransform also its interpolation fields. False with the first difference in *why.
bool layout_check(const KilnTypesApi* types, uint32_t world_kind, std::string* why);

}  // namespace kmk
#endif
