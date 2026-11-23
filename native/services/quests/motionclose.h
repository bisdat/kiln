// SPDX-License-Identifier: MIT
// 16f S4: the NPC motion closure of the Fisher chassis (chapter 16f sections 2.2 and 5.1). G3: a bounded fixed parser
// over owned sequence and template bytes, its layouts verified against the running type registry with the body's
// (bodypack.h body_layout / check_body_layout), never a live game-memory walk. No resource is published here.
//
// The 21 sequences the Fisher's ActiveNpcState, NpcState, NpcActionSequences, NpcDialog and PathWalker name are cloned
// under ids of the mod, each SetAnimationEvent bound in serialized order to the authored state of its role and variant
// (the admitted 27-row binding vector), every whitelisted event's local times retimed (motionpack.h), and every
// template reference replaced. An unknown event type, a changed event size, another number of animation events than
// the manifest, an unknown nonzero sequence reference or a source id left anywhere in the template refuses.
#pragma once
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "bodypack.h"
#include "motionpack.h"
#include "nlohmann/json.hpp"

namespace kiln::qb::motionclose {
inline constexpr const char* kSequenceType = "keen::actor::ActorSequenceResource";
inline constexpr size_t kMaxSequenceBytes = 16u << 20;

struct Source {
    const char* guid;
    const char* role;      // the binding vector's role name (tools/animcook/anicook/blocks.py MOTION_ROLES)
    uint32_t variants;     // SetAnimationEvents bound in serialized order: Sit/Sleep enter/loop/exit, Door's three branches
};
std::span<const Source> fisher_closure();

struct Bound {
    Guid state;
    motionpack::Time duration = 0;
    bool loop = false, clamp = false;
};
using Bindings = std::map<std::string, std::vector<Bound>>;  // role -> variant -> bound state
// The admitted vector: exactly the manifest's 27 role/variant rows, each with a nonzero state GUID and a positive
// durationNs. Refuses with the row's path.
bool parse_bindings(const nlohmann::ordered_json& rows, Bindings* out, std::string* err);

// One clone: the source bytes with `clone` as resourceId, each nonzero sourceSubSequenceId through `sub_id`, retimed
// local times, SubSequence lengths and bound animation names. Failure leaves `out` unchanged.
bool clone_sequence(const uint8_t* res, size_t len, const Guid& clone, const Source& source, const std::vector<Bound>& variants,
                    const std::function<Guid(const Guid&)>& sub_id, Bytes* out, std::string* err);

// Replace every sequence reference of the five verified components through `map` (source id text -> clone id). A zero
// reference stays zero. A nonzero reference outside the map refuses, and so does any source id still present anywhere in
// the bytes afterwards (a reference path a game update added). Failure leaves `tpl` unchanged. `replaced`: how many.
bool remap_template(Bytes* tpl, const std::map<std::string, Guid>& map, size_t* replaced, std::string* err);
}  // namespace kiln::qb::motionclose
