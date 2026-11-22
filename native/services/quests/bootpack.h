// SPDX-License-Identifier: MIT
// INTERNAL before-world publication, using the existing ResourceInit/OnBoot startup barriers.
// This is deliberately not a service table or a mod API: the per-mod boundary uses owner-isolated rollback.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "questpack.h"

namespace kiln::qb::bootpack {
using Owner = uint64_t;
struct Before {
    Guid guid{};
    uint32_t type = 0;
    bool present = false;
    Bytes bytes;
};
struct Resource {
    Guid guid{};
    uint32_t type = 0;
    bool is_new = false;
    const Bytes* bytes = nullptr;
    std::string name;
};
struct Backend {
    GameData game;
    std::function<Owner(const std::string&)> owner;
    std::function<bool(Owner, const std::string&, std::string*)> begin;
    std::function<bool(Owner, const Resource&, std::string*)> put;
    std::function<bool(Owner, bool item, const std::string&, std::string*)> define;
    std::function<bool(Owner, const TextOut&, std::string*)> text;
    std::function<bool(Owner, std::string*)> finish;
    std::function<bool(Owner, std::string*)> rollback;
};
struct Applied {
    std::string mod;
    Owner owner = 0;
    std::vector<Before> before;
};
struct Result {
    ojson manifest = {{"mods", ojson::array()}, {"errors", ojson::array()}};
    std::vector<Applied> applied;
    std::vector<std::string> errors, warnings;
    bool stopped = false, roster_refused = false;
    std::string reason;
    std::vector<std::string> stopped_mods;
    std::vector<uint32_t> roster_boot_ids, roster_kiln_ids;
    size_t knowledge = 0, quests = 0, npcs = 0, templates = 0, texts = 0;
};

Result build(const std::vector<ModInput>& mods, const Backend& backend);
// Reverse order only: an earlier shared-table layer is not withdrawn from under a later committed layer.
bool withdraw(const Applied& applied, const Backend& backend, std::string* why);
}  // namespace kiln::qb::bootpack
