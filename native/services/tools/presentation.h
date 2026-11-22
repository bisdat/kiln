// SPDX-License-Identifier: MIT
#pragma once
// Owned, bounded policy values. Engine adapters must copy source records while their views are valid;
// no game view or pointer is kept by the policy, selection or preview. This is not an engine adapter.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kiln::tools {
constexpr uint32_t material_capacity = 64;
struct Material { uint32_t item, material, voxels_per_item; };
struct Brush { uint32_t item, shape; double radius; };
struct Family {
    uint32_t tool;
    std::string name, description;
    std::vector<Material> materials;
    std::vector<Brush> brushes;
};
struct Stock { uint32_t item; uint64_t backpack, magic_chests; };
struct Selection { uint64_t session{}; uint32_t actor{}, tool{}, item{}; };
struct MaterialRow {
    uint32_t item{}, material{}, voxels_per_item{}, reserved{};
    uint64_t backpack{}, magic_chests{};
};
struct Snapshot {
    uint32_t size{sizeof(Snapshot)}, count{};
    uint64_t session{}, revision{};
    uint32_t actor{}, tool{}, selected_item{}, selected_material{};
    std::array<MaterialRow, material_capacity> rows{};
};
inline uint64_t saturated_add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
class Registry {
    struct Owned { uint64_t owner; std::shared_ptr<const Family> value; };
    mutable std::mutex gate_;
    std::unordered_map<uint32_t, Owned> families_;
public:
    static bool valid(const Family& value) {
        if (!value.tool || value.name.empty() || value.name.size() > 1024 || value.description.size() > 16384 ||
            value.name.find('\0') != std::string::npos || value.description.find('\0') != std::string::npos ||
            value.materials.size() > material_capacity ||
            value.brushes.empty() || value.brushes.size() > material_capacity) return false;
        std::unordered_set<uint32_t> items{value.tool}, materials;
        for (const auto& m : value.materials)
            if (!m.item || !m.material || m.material >= 128 || !m.voxels_per_item || m.voxels_per_item > 1000000 ||
                !items.insert(m.item).second || !materials.insert(m.material).second) return false;
        for (const auto& b : value.brushes)
            if (!b.item || b.shape != 0 || !std::isfinite(b.radius) || b.radius < 0.25 || b.radius > 8 ||
                !items.insert(b.item).second) return false;
        return true;
    }
    bool add(uint64_t owner, Family value) {
        if (!owner || !valid(value)) return false;
        auto owned = std::make_shared<const Family>(std::move(value));
        std::lock_guard lock(gate_);
        std::unordered_set<uint32_t> ids{owned->tool};
        for (const auto& b : owned->brushes) ids.insert(b.item);
        for (const auto& [_, entry] : families_) {
            if (ids.contains(entry.value->tool)) return false;
            for (const auto& b : entry.value->brushes) if (ids.contains(b.item)) return false;
        }
        const auto tool = owned->tool;
        families_.emplace(tool, Owned{owner, std::move(owned)});
        return true;
    }
    bool remove(uint64_t owner,uint32_t tool) {
        std::lock_guard lock(gate_);auto i=families_.find(tool);
        if(i==families_.end()||i->second.owner!=owner)return false;
        families_.erase(i);return true;
    }
    void remove_owner(uint64_t owner) {
        std::lock_guard lock(gate_);
        for (auto i = families_.begin(); i != families_.end();) {
            if (i->second.owner == owner) i = families_.erase(i); else ++i;
        }
    }
    std::shared_ptr<const Family> family(uint32_t tool) const {
        std::lock_guard lock(gate_);
        auto i = families_.find(tool);
        return i == families_.end() ? nullptr : i->second.value;
    }
    uint32_t root(uint32_t item) const {
        std::lock_guard lock(gate_);
        for (const auto& [tool, owned] : families_) {
            if (item == tool) return tool;
            for (const auto& b : owned.value->brushes) if (item == b.item) return tool;
        }
        return 0; // No override; the adapter must retain the vanilla result.
    }
    std::vector<uint32_t> brushes(uint32_t tool) const {
        std::vector<uint32_t> result;
        if (auto f = family(tool)) for (const auto& b : f->brushes) result.push_back(b.item);
        return result;
    }
    std::optional<Material> resolve(const Selection& selection, uint32_t actor, uint32_t tool, uint64_t session) const {
        if (!actor || !session || selection.actor != actor || selection.tool != tool || selection.session != session) return {};
        if (auto f = family(tool)) for (const auto& m : f->materials) if (m.item == selection.item) return m;
        return {};
    }
    Snapshot snapshot(uint32_t actor, uint32_t tool, uint64_t session, uint64_t revision,
                      const Selection& selection, const std::vector<Stock>& stock) const {
        Snapshot out;
        if (!actor || !session || !revision || stock.size() > 4096) return out;
        auto f = family(tool); if (!f) return out;
        out.session = session; out.revision = revision; out.actor = actor; out.tool = tool;
        out.count = static_cast<uint32_t>(f->materials.size());
        for (uint32_t i = 0; i < out.count; ++i) {
            const auto& m = f->materials[i]; auto& row = out.rows[i];
            row.item = m.item; row.material = m.material; row.voxels_per_item = m.voxels_per_item;
            for (const auto& s : stock) if (s.item == m.item) {
                row.backpack = saturated_add(row.backpack, s.backpack);
                row.magic_chests = saturated_add(row.magic_chests, s.magic_chests);
            }
            if (selection.session == session && selection.actor == actor && selection.tool == tool &&
                selection.item == m.item && (row.backpack || row.magic_chests)) {
                out.selected_item = m.item; out.selected_material = m.material;
            }
        }
        return out;
    }
    // A local preview choice is not edit/payment authority. The execution adapter must resolve again
    // against the invocation's pinned policy and the game's current eligible inventories.
    bool select(const Snapshot& snapshot, uint32_t item, Selection& out) const {
        if (!snapshot.actor || !snapshot.session || !snapshot.revision || snapshot.count > material_capacity) return false;
        auto f = family(snapshot.tool); if (!f) return false;
        for (uint32_t i = 0; i < snapshot.count; ++i) {
            const auto& row = snapshot.rows[i];
            if (row.item != item || !(row.backpack || row.magic_chests)) continue;
            for (const auto& m : f->materials) if (m.item == item && m.material == row.material) {
                out = {snapshot.session, snapshot.actor, snapshot.tool, item}; return true;
            }
        }
        return false;
    }
};
// Independently recovered client layouts; no adapter may use them on another unqualified build.
// Selector builder: 7 groups, each 0x648 bytes. Counts aggregate the game's eligible sources.
struct SelectorRow { uint32_t item{}; std::array<uint8_t, 12> source{}; uint64_t count{}; };
struct SelectorGroup {
    SelectorRow* data{};
    uint64_t count{}, capacity{};
    void* grow{};
    uint64_t allocation{};
    SelectorRow inline_rows[64]{};
    uint64_t selected{};
    uint8_t has_selected{}, padding[7]{};
    uint8_t mode{}, tail[15]{};
};
struct PreviewRow { std::array<uint8_t, 16> presentation{}; uint32_t item{}, count{}; };
struct PreviewArray {
    PreviewRow* data{};
    uint64_t count{}, capacity{};
    void* grow{};
    uint64_t allocation{};
    PreviewRow inline_rows[64]{};
};
struct PreviewOwner {
    std::array<uint8_t, 32> prefix{};
    uint32_t selected_item{}, selected_count{};
    PreviewRow* view{};
    uint64_t view_count{};
    uint8_t flag{}, padding[7]{};
    PreviewArray owned;
};
static_assert(sizeof(SelectorRow) == 24 && sizeof(SelectorGroup) == 0x648);
static_assert(offsetof(SelectorGroup, selected) == 0x628 && offsetof(SelectorGroup, has_selected) == 0x630);
static_assert(sizeof(PreviewRow) == 24 && offsetof(PreviewOwner, view) == 0x28 && offsetof(PreviewOwner, owned) == 0x40);
inline bool pool_contains(const std::vector<Material>& pool, uint32_t item) {
    for (const auto& m : pool) if (m.item == item) return true;
    return false;
}
inline bool filter_selector(SelectorGroup& group, const std::vector<Material>& pool) {
    if (pool.size() > material_capacity || group.count > material_capacity || group.count > group.capacity || (group.count && !group.data)) {
        group.count = 0; group.selected = 0; group.has_selected = 0; return false;
    }
    const uint32_t selected = group.has_selected && group.selected < group.count ? group.data[group.selected].item : 0;
    uint64_t keep = 0; group.selected = 0; group.has_selected = 0;
    for (uint64_t i = 0; i < group.count; ++i) if (pool_contains(pool, group.data[i].item)) {
        group.data[keep] = group.data[i];
        if (group.data[i].item == selected && group.data[i].count) { group.selected = keep; group.has_selected = 1; }
        ++keep;
    }
    group.count = keep;
    return true;
}
inline bool filter_preview(PreviewOwner& out, const std::vector<Material>& pool) {
    // The original swaps/copies its local SmallDynamicArray into owned, but leaves view naming the
    // old local buffer. Repair from the destination even when its filtered count becomes zero.
    out.view = out.owned.data;
    if (pool.size() > material_capacity || out.owned.count > material_capacity || out.owned.count > out.owned.capacity || (out.owned.count && !out.owned.data)) {
        out.view_count = 0; out.owned.count = 0; out.selected_item = 0; out.selected_count = 0; return false;
    }
    uint64_t keep = 0; bool selected = false;
    for (uint64_t i = 0; i < out.owned.count; ++i) if (pool_contains(pool, out.owned.data[i].item)) {
        out.owned.data[keep] = out.owned.data[i];
        if (out.owned.data[i].item == out.selected_item && out.owned.data[i].count) selected = true;
        ++keep;
    }
    out.owned.count = out.view_count = keep;
    if (!selected) { out.selected_item = 0; out.selected_count = 0; }
    return true;
}
} // namespace kiln::tools
