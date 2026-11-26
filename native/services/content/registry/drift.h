// SPDX-License-Identifier: MIT
// kiln.content registry: the best-effort drift probe's names (commitment 7: other loaders' DLL patches and hooks are
// best effort and labelled so).
//
// At each world load and each pack reload the live back-end (registry_live.cpp live_row) compares, for every field it
// binds, the game's value just before Kiln's first write in this world (the store slots' originals) with what the stage
// read at resources-ready and the writers Kiln knows below the packs (archive-writer packages, old boot patches). When they
// differ and none of them explains it, something outside Kiln changed the value (another DLL engine, a hook, a memory
// write): a writer row of kind "drift", certainty "bestEffort", naming the suspects Kiln can see, never a refusal. A slot
// another writer changed after Kiln's layer (the store's compare-before-write) gets the same row. An entry an old boot
// document wrote (an OldPatch row or entry mark in RegistryLive::old) is not checked: the game holds Kiln's own patch there
// and many of its paths have no row to explain it. Kiln cannot see a writer that hooks nothing (a thread writing memory),
// so a suspect list is a hint, not proof, and an empty one proves nothing.
//
// Suspects: the modules the hook map (kiln.hooks map_json, state/hooks.json) lists as having patched game code before
// Kiln hooked it ("foreign", not Kiln's own), and hosted ShroudForge when it runs this start (its run-time mods write game
// memory Kiln does not see as writes). Read once per world, outside the store's lock.
// Header-only, plain data: host-tested (tests/registry_writers_test.cpp).
#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace kc::registry {

// The suspects in a hook map document (state/hooks.json's form): every "foreign" row's module that is not Kiln's own,
// each once, in the map's order (at most 8; "+N more" after them). hosted_sf: hosted or installed ShroudForge has this start.
inline std::vector<std::string> drift_suspects(const nlohmann::json& map, bool hosted_sf) {
    std::vector<std::string> out;
    if (hosted_sf) out.push_back("ShroudForge (hosted)");
    size_t more = 0;
    if (map.is_object() && map.contains("foreign") && map["foreign"].is_array())
        for (const auto& f : map["foreign"]) {
            if (!f.is_object() || !f.contains("module") || !f["module"].is_string()) continue;
            if (f.contains("own") && f["own"].is_boolean() && f["own"].get<bool>()) continue;
            std::string m = f["module"].get<std::string>();
            if (m.empty()) m = "code outside any module";
            if (std::find(out.begin(), out.end(), m) != out.end()) continue;
            if (out.size() >= 8) { ++more; continue; }
            out.push_back(std::move(m));
        }
    if (more) out.push_back("+" + std::to_string(more) + " more");
    return out;
}

// What a drift row's writer is called (registry.json "who"; no mod).
inline std::string drift_who(const std::vector<std::string>& suspects) {
    std::string s = "outside Kiln (best effort; ";
    if (suspects.empty()) return s + "no other loader's hook seen)";
    s += "suspects: ";
    for (size_t i = 0; i < suspects.size(); ++i) s += (i ? ", " : "") + suspects[i];
    return s + ")";
}

}  // namespace kc::registry
