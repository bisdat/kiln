// SPDX-License-Identifier: MIT
// kiln.content registry stage: the game's bytes through its reflection, for the model (model.cpp) and the boot back-end.
//
// The registry's own headers (namespace kc::reg) cannot share a translation unit with content.h, whose kc::reg() looks an
// entry up by its 32-bit id: so the model reaches gamedata's walker (gd::resolve, gd::node_json) through these plain
// functions, implemented in model_bytes.cpp next to the walker. A node is an opaque type and an address inside bytes the
// caller holds (a copy of a resource: every offset in it is self-relative).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace kc::rbytes {

struct Node {
    const void* type = nullptr;  // gd::TType of the node
    uintptr_t addr = 0;
    bool tmpl = false;  // a keen::ecs::TemplateResource root: a path may start at a component by its type
};
// The root of a resource's bytes of the root type named type. false + *why: no such struct type on this build, or the bytes
// are shorter than it.
bool root(const void* data, size_t len, const std::string& type, Node* out, std::string* why);
// The elements of the array member at path below at.
bool elements(const Node& at, const std::string& path, std::vector<Node>* out, std::string* why);
// The member at path below at ("" the node itself) as node_json gives it (several, for a "[*]" path: a list). false + *why.
bool member(const Node& at, const std::string& path, nlohmann::json* out, std::string* why);
// A member name that identifies things (ids, GUIDs): read-only (gd::identity_field).
bool identity(const std::string& name);

}  // namespace kc::rbytes
