// SPDX-License-Identifier: MIT
// 16f S3: admission of a mod's authored roster bodies at the native boot build (bootpack.cpp's admit_body), before the
// mod compiles. Portable: the game side binds the reader, the type registry and the client's material reads
// (native_pack.cpp); host tests bind fakes (tests/body_admit_test.cpp).
//
// Two sources of an authored body, both written by Kiln, never by a mod's own quests files:
//  - an NPC block of blocks/*.json: collection (native_pack.cpp npcs_from_blocks) writes {"block": <name>, "spec":
//    {"body", "animation", "motions"}} with the parts resolved as the cooker resolves them. Admission requires this
//    machine's cook cache assets/cooked/npc-bodies.v1.json to describe exactly that spec, re-hashes every authored
//    source and every cooked output it lists, and returns the cooked resources for publication under the mod's own
//    bootpack owner. A server has no renderer output: its model id comes from the cook's RenderModel entry.
//  - an NPC defined in code at an earlier start (Kiln.Blocks NpcFiles, kiln-quests/native/<mod>.blocks.json): its body
//    names cooked ids {model, graph, size}; its resources are the mod's ordinary assets.json entries, which kiln.assets
//    serves already. Admission checks they are served (the model on clients only).
// Every admitted body becomes {"model", "graph", "size": {radius, height}, "bindings", "closure"} for compile_pack.
// A refusal names the NPC and the reason and the whole mod is refused (nothing of it is published).
#pragma once
#include <functional>
#include <string>

#include "bodypack.h"
#include "questpack.h"

namespace kiln::qb::bodyadmit {
inline constexpr const char* kCache = "assets/cooked/npc-bodies.v1.json";
inline constexpr size_t kMaxCache = 4u << 20;
inline constexpr size_t kMaxFile = 64u << 20;
inline constexpr const char* kModelType = "keen::RenderModel";
inline constexpr const char* kGraphType = "keen::anim_graph::runtime_graph::AnimationGraphResource2_0";

struct Env {
    bool client = false;
    // One mod-relative file (forward slashes, no "." or ".." parts, under the mod's folder), at most `max` bytes.
    std::function<bool(const std::string& file, size_t max, Bytes* out, std::string* why)> read;
    // The running game's type registry still has the verified body layout (bodypack.h check_body_layout).
    std::function<bool(std::string* why)> layout;
    // Clients: kiln.vfx's material build report and kiln.assets' served resources (bodypack.h check_body_materials).
    BodyMaterialContext materials;
    // A resource the game is served now (a code-defined body's ordinary assets.json entries).
    std::function<bool(const Guid& guid, uint32_t type)> served;
};

// A mod-relative path the cooker may name: forward slashes, no empty, "." or ".." part, no drive or root.
bool contained(const std::string& file);
// The resolved parts of one blocks NPC as the cooker resolves them (tools/animcook/anicook/blocks.py npc_bodies):
// a part is an inline object or the name of a block of this mod ("<mod>:<name>" or "<name>"); later files win.
// Returns null when the body is not an authored body (no "model").
ojson resolve_spec(const std::string& mod, const ojson& npc, const ojson& bodies, const ojson& animations, const ojson& motions);
bool admit(ModInput* mod, const Env& env, ojson* assets, std::string* why);
}  // namespace kiln::qb::bodyadmit
