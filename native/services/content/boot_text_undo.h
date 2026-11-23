// SPDX-License-Identifier: MIT
// Text-tag before-images for one private native boot pack. Unrelated tags/writers are never overwritten on abort.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace kc::bootpack {
template <class Entry>
class TextUndo {
public:
    struct Change { Entry before{}, after{}; };
    std::map<uint32_t, Change> changes;
    bool recording = true;

    bool record(uint32_t tag, Entry before, Entry after) {
        if (!recording) return false;
        auto [it, fresh] = changes.try_emplace(tag, Change{before, after});
        if (fresh) return true;
        if (it->second.after != before) return false;
        it->second.after = after;
        return true;
    }

    template <class Map, class Select>
    bool restore(const Map& current, Map* restored, Select selected, std::string* why) const {
        // Validate every selected tag before returning a new map; a conflict cannot partially restore a shard.
        for (const auto& [tag, change] : changes) {
            if (!selected(tag)) continue;
            const auto found = current.find(tag);
            const Entry entry = found == current.end() ? Entry{} : found->second;
            if (entry != change.after) {
                *why = "text tag " + std::to_string(tag) + " changed after this boot pack's write";
                return false;
            }
        }
        Map next = current;
        for (const auto& [tag, change] : changes) {
            if (!selected(tag)) continue;
            if (change.before != Entry{}) next[tag] = change.before;
            else next.erase(tag);
        }
        *restored = std::move(next);
        return true;
    }
};
}  // namespace kc::bootpack
