// SPDX-License-Identifier: MIT
// The iterator layout of an ECS program: which qword of the engine's iterator holds which name of the program's
// descriptor (re_custom_programs.md section 6.1; docs/re/world.md). Kiln's own programs (programs.cpp) and the game's
// programs that kiln.world wraps (wraps.cpp, through layout.cpp's game_layout) use the same algorithm:
//   [0] ctx | TPL (tpl, tpl_opt) | READ (read, read_opt) | WRITE (write, write_opt; 6 qwords each) |
//   SINGLETON (singletons read, then write, descriptor order) | OTHER_TPL | OTHER_READ | OTHER_WRITE | EVENTS_IN |
//   EVENTS_OUT (descriptor order)
// Inside TPL, READ, WRITE, OTHER_* and EVENTS_IN the engine orders by type index (ties: list order). Header-only so the
// host tests that compile programs.cpp alone keep compiling it alone.
#pragma once
#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include "world_internal.h"

namespace kw {

enum ListId {
    L_TPL, L_TPL_OPT, L_READ, L_READ_OPT, L_WRITE, L_WRITE_OPT, L_SREAD, L_SWRITE,
    L_OTPL, L_OREAD, L_OWRITE, L_WITH, L_WITHOUT, L_ANY, L_EIN, L_EOUT, L_COUNT
};
enum MapKind { M_DYN, M_TPL, M_SINGLETON, M_EVENT };
inline constexpr MapKind kListMap[L_COUNT] = {M_TPL, M_TPL, M_DYN, M_DYN, M_DYN, M_DYN, M_SINGLETON, M_SINGLETON,
                                              M_TPL, M_DYN, M_DYN, M_TPL, M_TPL, M_TPL, M_EVENT, M_EVENT};
inline constexpr const char* kListName[L_COUNT] = {"tpl", "tpl_optional", "read", "read_optional", "write",
                                                   "write_optional", "singletons_read", "singletons_write", "other_tpl",
                                                   "other_read", "other_write", "with", "without", "any_of",
                                                   "events_in", "events_out"};

struct List {
    std::vector<std::string> names;
    std::vector<NameRef> refs;
    std::vector<int32_t> slot;   // qword index into the iterator per name (-1 = none)
    std::vector<uint16_t> index; // engine type index (dyn/tpl lists) after validation
};

// What a layout needs of a program: its name (for messages), its lists, the singleton optional masks, and what the
// layout computes (group counts and the iterator's size).
struct ProgramLayout {
    std::string name;
    List lists[L_COUNT];
    uint64_t sread_opt = 0, swrite_opt = 0;
    uint32_t T = 0, R = 0, W = 0, S = 0, OT = 0, O = 0, EI = 0, EO = 0, iter_size = 0;
};

inline std::string qualified(const std::string& n) { return "keen::ecs::" + n; }

// Assigns slots to a group of lists: sorted by key across the lists (ties: list order), consecutive from `base`.
template <class Key>
uint32_t assign(ProgramLayout& p, std::initializer_list<int> ids, uint32_t base, uint32_t stride, bool sort, Key key) {
    struct Item {
        int list;
        size_t i;
    };
    std::vector<Item> items;
    for (int id : ids)
        for (size_t i = 0; i < p.lists[id].names.size(); ++i) items.push_back({id, i});
    if (sort) std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        return key(a.list, a.i) < key(b.list, b.i);
    });
    for (size_t k = 0; k < items.size(); ++k) p.lists[items[k].list].slot[items[k].i] = static_cast<int32_t>(base + k * stride);
    return static_cast<uint32_t>(items.size());
}

// Slot layout of the engine iterator (re_custom_programs.md §6.1; checked on 1285601 against the game's teleport
// program, which iterates [ctx][R:NpcCommon][W x8 by name][S x3][O:PlayerSpawnPoint] in 0x1B0 bytes).
template <class Key>
void layout(ProgramLayout& p, Key key) {
    for (auto& l : p.lists) l.slot.assign(l.names.size(), -1);
    uint32_t at = 1;
    p.T = assign(p, {L_TPL, L_TPL_OPT}, at, 1, true, key);
    at += p.T;
    p.R = assign(p, {L_READ, L_READ_OPT}, at, 1, true, key);
    at += p.R;
    p.W = assign(p, {L_WRITE, L_WRITE_OPT}, at, 6, true, key);
    at += 6 * p.W;
    p.S = assign(p, {L_SREAD, L_SWRITE}, at, 1, false, key);
    at += p.S;
    p.OT = assign(p, {L_OTPL}, at, 1, true, key);
    at += p.OT;
    const uint32_t oread = assign(p, {L_OREAD}, at, 1, true, key);
    at += oread;
    const uint32_t owrite = assign(p, {L_OWRITE}, at, 1, true, key);
    at += owrite;
    p.O = oread + owrite;
    p.EI = assign(p, {L_EIN}, at, 1, true, key);
    at += p.EI;
    p.EO = assign(p, {L_EOUT}, at, 1, false, key);
    at += p.EO;
    p.iter_size = at * 8;
}

inline std::vector<std::vector<int32_t>> slots_of(const ProgramLayout& p) {
    std::vector<std::vector<int32_t>> v;
    for (auto& l : p.lists) v.push_back(l.slot);
    return v;
}

inline uintptr_t map_base(uintptr_t registry, MapKind k) {
    switch (k) {
        case M_DYN: return L.reg_components >= 0 ? registry + L.reg_components : 0;
        case M_TPL: return L.reg_templates >= 0 ? registry + L.reg_templates : 0;
        case M_SINGLETON: return F.singletons ? registry + L.reg_singletons : 0;
        case M_EVENT: return F.events ? registry + L.reg_events : 0;
    }
    return 0;
}

// Each name's engine type index from the world's maps (a singleton may be missing: optional, or the engine skips the
// program later). False with `why` for a name the world does not know.
inline bool resolve_indices(ProgramLayout& p, uintptr_t world, std::string& why) {
    const uintptr_t registry = registry_of(world);
    for (int id = 0; id < L_COUNT; ++id) {
        List& l = p.lists[id];
        l.index.assign(l.names.size(), 0);
        if (l.names.empty()) continue;
        const MapKind mk = kListMap[id];
        const uintptr_t map = map_base(registry, mk);
        if (!map) {
            why = std::string(kListName[id]) + ": cannot check these names on this build";
            return false;
        }
        for (size_t i = 0; i < l.names.size(); ++i) {
            const std::string& n = l.names[i];
            const uint32_t key = mk == M_SINGLETON ? fnv1a(n) : fnv1a(n, fnv1a("keen::ecs::"));
            uint16_t v = 0;
            if (map_find_u16(map, key, &v)) {
                l.index[i] = v;
                continue;
            }
            if (mk == M_SINGLETON) {
                const uint64_t opt = id == L_SREAD ? p.sread_opt : p.swrite_opt;
                if (!(opt >> i & 1))
                    KW_W("program %s: singleton '%s' is not registered yet; if it is still missing when the world "
                         "starts, the engine skips the program",
                         p.name.c_str(), n.c_str());
                continue;
            }
            why = std::string(kListName[id]) + ": unknown " +
                  (mk == M_EVENT ? "event" : mk == M_TPL ? "template component" : "component") + " '" + n + "'";
            return false;
        }
    }
    return true;
}

// Kiln's own programs: laid out by name at registration; the engine lays them out by type index. Both must agree.
inline bool validate(ProgramLayout& p, uintptr_t world, std::string& why) {
    if (!resolve_indices(p, world, why)) return false;
    // The engine orders TPL/READ/WRITE/OTHER slots by type index; we laid them out by name. They must agree.
    const auto provisional = slots_of(p);
    layout(p, [&](int list, size_t i) { return static_cast<uint32_t>(p.lists[list].index[i]); });
    if (slots_of(p) != provisional) {
        why = "the engine's type order differs from name order for these components";
        layout(p, [&](int list, size_t i) { return qualified(p.lists[list].names[i]); });
        return false;
    }
    return true;
}

// The group counts the engine computed for a job's iterator (docs/re/world.md "job"); a layout must match them.
struct JobCounts {
    uint32_t T = 0, R = 0, W = 0, S = 0, OT = 0, O = 0, EI = 0, EO = 0;
    uint32_t iter_size() const { return 8 * (1 + T + R + 6 * W + S + OT + O + EI + EO); }
};
inline JobCounts job_counts(const uint8_t* j) {
    JobCounts c;
    c.T = j[L.job_tpl_count];
    c.R = *reinterpret_cast<const uint16_t*>(j + L.job_read_count);
    c.W = *reinterpret_cast<const uint16_t*>(j + L.job_write_count);
    const auto* singletons = *reinterpret_cast<const uint8_t* const*>(j + L.job_singletons);
    c.S = singletons ? singletons[L.singletons_count] : 0;
    c.OT = j[L.job_other_tpl_count];
    c.O = j[L.job_other_count];
    c.EI = static_cast<uint32_t>(*reinterpret_cast<const uint64_t*>(j + L.job_events_in_count));
    c.EO = *reinterpret_cast<const uint32_t*>(j + L.job_events_out_count);
    return c;
}
inline bool same_counts(const ProgramLayout& p, const JobCounts& c) {
    return c.T == p.T && c.R == p.R && c.W == p.W && c.S == p.S && c.OT == p.OT && c.O == p.O && c.EI == p.EI &&
           c.EO == p.EO;
}

// layout.cpp: the layout of one of the game's programs from its descriptor (names read through SEH-guarded copies),
// laid out by the world's type indices. False with `why` when a name is unknown or the descriptor uses a list the
// algorithm does not model (other_write_unser, all_read, all_write: chapter 06a section 9.3).
bool game_layout(const EngineDesc* desc, uintptr_t world, ProgramLayout& out, std::string& why);
// The descriptor's name, lists (names only) and singleton masks; *no_view: it uses a list the layout does not model.
bool read_desc(const EngineDesc* desc, ProgramLayout& out, bool* no_view, std::string& why);
// The descriptor's contract text (chapter 06a 9.7; tools/typegen/catalog.py writes the same): for each of its 21 lists in
// descriptor order "<list>=" + its names joined by "," + "\n", then the three masks, flags and interval. Its FNV-1a-32 is
// the contract (game_program_contract). False with `why` when the descriptor cannot be read.
bool contract_text(const EngineDesc* desc, std::string& out, std::string& why);

}  // namespace kw
