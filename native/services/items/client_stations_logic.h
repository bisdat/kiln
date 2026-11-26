// SPDX-License-Identifier: MIT
// kiln.stations.client (native/core/abi/services/kiln_stations_client.h, since 0.2.0-dev.31.13): the pure part, host-tested
// (tests/client_stations_test.cpp): one ClientFactoryInfo row decoded with the layout sigdb captured, the game HUD's
// remaining-time arithmetic (client 26430625 0xb2d6e0, saturating at the i64 extremes), the scaled craft duration
// (FactorySpeed's float factor, as factory_station), the view a row gives, and the bounded caller buffer.
// client_stations.cpp reads the game's memory on the client world's thread and hands the bytes here.
// Evidence: docs/re/stations.md section 14.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "../../core/abi/services/kiln_stations_client.h"

namespace kitems::client_view {

constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
constexpr int64_t kMin = std::numeric_limits<int64_t>::min();

// Row offsets in ClientFactoryInfo's 200-byte rows. The defaults are client 26430625's; client_stations.cpp takes the
// values sigdb captured (constants stations/cfi.*) and layout_ok refuses a set that does not fit.
struct RowLayout {
    int64_t size = 0xc8, entity = 0x00, workshop = 0x04, recipe = 0x08, position = 0x0c, start = 0x28, sel_category = 0x30,
            sel_index = 0x34, crafts = 0xa0, water_max = 0xa4, state = 0xa6, missing = 0xa7, water = 0xa8, use_all = 0xaa,
            energy = 0xab, base = 0xb0;
};

inline bool layout_ok(const RowLayout& l) {
    auto in = [&](int64_t off, int64_t width) { return off >= 0 && off + width <= l.size; };
    return l.size >= 0x40 && l.size <= 0x400 && l.entity == 0 && in(l.workshop, 4) && in(l.recipe, 4) &&
           in(l.position, 12) && in(l.start, 8) && in(l.sel_category, 32) && in(l.sel_index, 29) &&
           l.sel_index == l.sel_category + 4 && in(l.crafts, 4) && in(l.water_max, 2) && in(l.state, 1) &&
           in(l.missing, 1) && in(l.water, 2) && in(l.use_all, 1) && in(l.energy, 1) && in(l.base, 16) &&
           l.workshop != l.recipe && l.state != l.missing && l.water != l.water_max;
}

template <class T>
T rd(const uint8_t* p, int64_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

struct Row {
    uint32_t entity = 0, workshop = 0, recipe = 0, crafts = 0;
    float position[3] = {};
    int64_t start = 0;
    KilnStationChoice choices[4] = {};
    uint16_t water = 0, water_max = 0;
    uint8_t state = 0, missing = 0, use_all = 0, energy = 0;
    KilnBaseId base{};
};

inline Row decode(const uint8_t* row, const RowLayout& l) {
    Row r;
    r.entity = rd<uint32_t>(row, l.entity);
    r.workshop = rd<uint32_t>(row, l.workshop);
    r.recipe = rd<uint32_t>(row, l.recipe);
    r.crafts = rd<uint32_t>(row, l.crafts);
    std::memcpy(r.position, row + l.position, 12);
    r.start = rd<int64_t>(row, l.start);
    for (int i = 0; i < 4; ++i) {
        r.choices[i].category = rd<uint32_t>(row, l.sel_category + 8 * i);
        r.choices[i].index = row[l.sel_index + 8 * i];
    }
    r.water = rd<uint16_t>(row, l.water);
    r.water_max = rd<uint16_t>(row, l.water_max);
    r.state = row[l.state];
    r.missing = row[l.missing];
    r.use_all = row[l.use_all];
    r.energy = row[l.energy];
    std::memcpy(r.base.guid, row + l.base, 16);
    return r;
}

// One craft of a recipe as factory_station and the HUD scale it: double(ns) * 1e-9 -> float, times the factor,
// -> double * 1e9 -> int64 (cvttsd2si: a value outside int64 becomes kMin). A factor that is not > 0 or not finite: 0
// (unknown: the caller reports no time).
inline int64_t scaled_ns(int64_t duration_ns, float factor) {
    if (!(factor > 0.0f) || !(factor < std::numeric_limits<float>::infinity()) || duration_ns <= 0) return 0;
    const float sec = static_cast<float>(static_cast<double>(duration_ns) * 1e-9);
    const double ns = static_cast<double>(sec * factor) * 1e9;
    if (!(ns < 9.2233720368547758e18) || !(ns >= -9.2233720368547758e18)) return kMin;
    return static_cast<int64_t>(ns);
}

// The game HUD's remaining time (client 26430625 0xb2d6e0) for one row: `craft` is one scaled craft, `crafts` the
// row's count of crafts still loaded, `start` the cycle's start, `now` the clock. kMax / kMin are the game's "infinite"
// times and stay so; every other sum and difference wraps as the game's does. The result counts the cycle in progress
// and every loaded craft after it: craft * (crafts + 1) - (now - start).
inline int64_t game_remaining(int64_t craft, uint32_t crafts, int64_t start, int64_t now) {
    int64_t total;  // craft * crafts, kept at an extreme
    if (crafts == 0) total = 0;
    else if (craft == kMax || craft == kMin) total = craft;
    else total = static_cast<int64_t>(static_cast<uint64_t>(craft) * static_cast<uint64_t>(crafts));
    int64_t elapsed;
    if (now == kMax) elapsed = start != kMax ? kMax : 0;
    else if (now == kMin) elapsed = start != kMin ? kMin : 0;
    else if (start == kMax) elapsed = kMin;
    else if (start == kMin) elapsed = kMax;
    else elapsed = static_cast<int64_t>(static_cast<uint64_t>(now) - static_cast<uint64_t>(start));
    if (total == kMax) total = craft != kMin ? kMax : 0;
    else if (total == kMin) total = craft != kMax ? kMin : 0;
    else if (craft == kMax) return elapsed != kMax ? kMax : 0;
    else if (craft == kMin) return elapsed != kMin ? kMin : 0;
    else total = static_cast<int64_t>(static_cast<uint64_t>(total) + static_cast<uint64_t>(craft));
    if (total == kMax) return elapsed != kMax ? kMax : 0;
    if (total == kMin) return elapsed != kMin ? kMin : 0;
    if (elapsed == kMax) return kMin;
    if (elapsed == kMin) return kMax;
    return static_cast<int64_t>(static_cast<uint64_t>(total) - static_cast<uint64_t>(elapsed));
}

// What a recipe adds to a view: one craft (unscaled), its outputs and inputs (items only; category inputs are inputs).
struct RecipeFacts {
    bool found = false;
    int64_t duration_ns = 0;
    std::vector<uint32_t> outputs, inputs;
};

// A stack of the station's inventory, as the client world's copy has it.
struct Stack {
    uint32_t item = 0, count = 0;
};

// The finished stacks: items that are an output of the selected recipe and no input of it, summed per item, in the
// order the recipe lists its outputs; at most KILN_STATIONS_CLIENT_MAX_OUTPUTS.
inline uint32_t finished(const RecipeFacts& r, const std::vector<Stack>& inventory, KilnStationStack* out) {
    uint32_t n = 0;
    for (uint32_t item : r.outputs) {
        if (!item || std::find(r.inputs.begin(), r.inputs.end(), item) != r.inputs.end()) continue;
        bool seen = false;
        for (uint32_t k = 0; k < n; ++k) seen |= out[k].item == item;
        if (seen || n == KILN_STATIONS_CLIENT_MAX_OUTPUTS) continue;
        uint64_t sum = 0;
        for (const Stack& s : inventory)
            if (s.item == item) sum += s.count;
        if (sum) out[n++] = {item, static_cast<uint32_t>(std::min<uint64_t>(sum, UINT32_MAX))};
    }
    return n;
}

// The view one row gives. `recipe` from the client's registry (found=false: unknown), `factor` the client's
// FactorySpeed, `inventory` the station's stacks when `have_inventory`.
inline KilnStationClientView view_of(const Row& row, const RecipeFacts& recipe, float factor, int64_t now, uint64_t step,
                                     bool have_inventory, const std::vector<Stack>& inventory) {
    KilnStationClientView v{};
    v.size = sizeof v;
    v.entity = row.entity;
    v.workshop = row.workshop;
    v.selected_recipe = row.recipe;
    v.base = row.base;
    std::memcpy(v.position, row.position, sizeof v.position);
    v.state = row.state;
    v.needs = row.missing;
    v.use_all_options = row.use_all;
    std::memcpy(v.selected_choices, row.choices, sizeof v.selected_choices);
    v.crafts_loaded = row.crafts;
    v.water = row.water;
    v.water_max = row.water_max;
    v.recipe_start_ns = row.start;
    v.now_ns = now;
    v.step = step;
    if (row.energy) v.flags |= KILN_STATION_CLIENT_ENERGY;
    if (row.recipe && recipe.found) {
        const int64_t craft = scaled_ns(recipe.duration_ns, factor);
        if (craft > 0) {
            v.flags |= KILN_STATION_CLIENT_RECIPE;
            v.craft_ns = craft;
            if (row.state == KILN_STATION_STATE_RUNNING) {
                const int64_t all = game_remaining(craft, row.crafts, row.start, now);
                const int64_t one = game_remaining(craft, 0, row.start, now);
                if (all != kMax && all != kMin && one != kMax && one != kMin) {
                    v.flags |= KILN_STATION_CLIENT_TIME;
                    v.remaining_ns = std::max<int64_t>(0, all);
                    v.craft_remaining_ns = std::max<int64_t>(0, one);
                    const double done = 1.0 - static_cast<double>(v.craft_remaining_ns) / static_cast<double>(craft);
                    v.progress = static_cast<float>(std::clamp(done, 0.0, 1.0));
                }
            }
        }
    }
    if (have_inventory && row.recipe && recipe.found) {
        v.flags |= KILN_STATION_CLIENT_OUTPUTS;
        v.output_count = finished(recipe, inventory, v.outputs);
    }
    return v;
}

// The bounded caller buffer of stations(): count, truncation, count-only.
inline KilnResult copy_out(const std::vector<KilnStationClientView>& rows, KilnStationClientView* out, uint32_t capacity,
                           uint32_t* out_count) {
    if (!out_count || (capacity && !out)) return KILN_ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < capacity; ++i)
        if (out[i].size != sizeof(KilnStationClientView)) return KILN_ERR_INVALID_ARGUMENT;
    *out_count = static_cast<uint32_t>(rows.size());
    const uint32_t n = std::min<uint32_t>(capacity, static_cast<uint32_t>(rows.size()));
    for (uint32_t i = 0; i < n; ++i) out[i] = rows[i];
    return n < rows.size() ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}

// A demand window: a read at `now_ms` keeps the copying on for KILN_STATIONS_CLIENT_DEMAND_MS.
inline bool demanded(uint64_t last_read_ms, uint64_t now_ms) {
    return last_read_ms != 0 && now_ms >= last_read_ms && now_ms - last_read_ms <= KILN_STATIONS_CLIENT_DEMAND_MS;
}

}  // namespace kitems::client_view
