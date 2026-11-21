// SPDX-License-Identifier: MIT
// kiln.items v8: rarity rolls at creation (kiln_items.h "rarity rolls"), the pure part: which creation a rule applies
// to, the weighted pick over the rarities the item type has a perk slot for, the checks of a rule set and the merge of
// every owner's set into the one table createPide's hook reads. No game, no Windows: pide.cpp includes it and the host
// test (tests/pide_test.cpp) checks it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "services/kiln_items.h"

namespace kitems::rolls {

constexpr int kRarities = 5;  // ItemRarity Common 0 .. Legendary 4 (an item type has five perk slots)
constexpr uint32_t kSources = KILN_ITEMS_ROLL_CRAFT | KILN_ITEMS_ROLL_LOOT | KILN_ITEMS_ROLL_GIVE;
constexpr uint32_t kMaxRules = 65536;

struct Rule {
    uint32_t item = 0;
    uint32_t sources = 0;
    float weights[kRarities] = {};
};

// The creation createPide is making, from what it was asked: a give of kiln.items for this very item type (own_give)
// with or without a rarity, or anything else the game creates with or without one. 0: a rule never applies (a give that
// asked for a rarity keeps it). Crafting asks for none (docs/re/items.md section 10); random loot and random drop-table
// stacks ask for the rarity they rolled.
inline uint32_t source_of(bool own_give, bool asked) {
    if (own_give) return asked ? 0u : static_cast<uint32_t>(KILN_ITEMS_ROLL_GIVE);
    return asked ? static_cast<uint32_t>(KILN_ITEMS_ROLL_LOOT) : static_cast<uint32_t>(KILN_ITEMS_ROLL_CRAFT);
}

// A weighted pick among the rarities whose bit is set in `allowed` (bit r: the type has perk slot r, so createPide
// gives the item its enhancements; a rarity without one would come out as another rarity). u in [0, 1). -1: no allowed
// rarity has a weight.
inline int pick(const float (&w)[kRarities], uint32_t allowed, float u) {
    float total = 0;
    for (int r = 0; r < kRarities; ++r)
        if ((allowed >> r) & 1u) total += w[r] > 0 ? w[r] : 0;
    if (!(total > 0)) return -1;
    float at = (u < 0 ? 0 : u) * total;
    int last = -1;
    for (int r = 0; r < kRarities; ++r) {
        if (!((allowed >> r) & 1u) || !(w[r] > 0)) continue;
        last = r;
        if (at < w[r]) return r;
        at -= w[r];
    }
    return last;  // rounding at the very top
}

// One entry of a set as an owner gives it.
inline bool valid(const KilnItemsRarityRoll& r) {
    if (r.item == 0 || r.reserved != 0 || r.sources == 0 || (r.sources & ~kSources) != 0) return false;
    bool any = false;
    for (float f : r.weights) {
        if (!std::isfinite(f) || f < 0) return false;
        any |= f > 0;
    }
    return any;
}

// Every owner's set, merged: for an item several owners name, the owner that set its rules last wins.
class Table {
public:
    // Replaces `owner`'s set (empty: ends it). The caller checked every entry and that no item repeats.
    void set(uint64_t owner, std::vector<Rule> rules) {
        if (rules.empty()) {
            sets_.erase(owner);
        } else {
            auto& s = sets_[owner];
            s.seq = ++seq_;
            s.rules = std::move(rules);
        }
        rebuild();
    }
    // The merged rules, sorted by item (find() reads them); empty when no owner has any.
    const std::vector<Rule>& merged() const { return merged_; }
    uint32_t owners() const { return static_cast<uint32_t>(sets_.size()); }
    uint32_t conflicts() const { return conflicts_; }  // items more than one owner names

    static const Rule* find(const std::vector<Rule>& sorted, uint32_t item) {
        auto it = std::lower_bound(sorted.begin(), sorted.end(), item, [](const Rule& r, uint32_t i) { return r.item < i; });
        return it != sorted.end() && it->item == item ? &*it : nullptr;
    }

private:
    struct Set {
        uint64_t seq = 0;
        std::vector<Rule> rules;
    };
    void rebuild() {
        std::map<uint32_t, std::pair<uint64_t, Rule>> by_item;
        conflicts_ = 0;
        for (const auto& [owner, s] : sets_)
            for (const auto& r : s.rules) {
                auto [it, fresh] = by_item.try_emplace(r.item, s.seq, r);
                if (fresh) continue;
                ++conflicts_;
                if (s.seq > it->second.first) it->second = {s.seq, r};
            }
        merged_.clear();
        merged_.reserve(by_item.size());
        for (const auto& [item, v] : by_item) merged_.push_back(v.second);
    }
    std::map<uint64_t, Set> sets_;
    std::vector<Rule> merged_;
    uint64_t seq_ = 0;
    uint32_t conflicts_ = 0;
};

}  // namespace kitems::rolls
