// SPDX-License-Identifier: MIT
// kiln.stations.client (kiln_stations_client.h, since 0.2.0-dev.31.13), the pure part (client_stations_logic.h) on the
// host, no game: the ABI layout the managed side mirrors, a ClientFactoryInfo row of client 26430625 decoded with the
// layout sigdb captures (docs/re/stations.md section 14), the game HUD's remaining-time arithmetic (0xb2d6e0, every
// branch of its saturation), the scaled craft, the view a row gives (time only while Running, outputs only with the
// station's inventory), the bounded caller buffer and the demand window.
//   g++ -std=c++20 -I native/core/abi native/services/items/tests/client_stations_test.cpp && ./a.out
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "../client_stations_logic.h"

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_failed;                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                     \
    } while (0)

using namespace kitems::client_view;

template <class T>
static void put(uint8_t* p, int64_t off, T v) {
    std::memcpy(p + off, &v, sizeof v);
}

// A row as the game's decoder (client 0x3c0920) leaves it: a forge with 3 crafts left, running since t = 100 s.
static std::vector<uint8_t> forge_row() {
    std::vector<uint8_t> b(0xc8, 0xEE);  // bytes the decoder does not write stay whatever they were
    put<uint32_t>(b.data(), 0x00, 4242);
    put<uint32_t>(b.data(), 0x04, 0x56d94f8u);  // workshop
    put<uint32_t>(b.data(), 0x08, 0x1234abcdu); // the selected recipe
    const float pos[3] = {1024.25f, 51.5f, 2048.0f};
    std::memcpy(b.data() + 0x0c, pos, 12);
    put<int64_t>(b.data(), 0x28, 100'000'000'000LL);
    for (int i = 0; i < 4; ++i) {
        put<uint32_t>(b.data(), 0x30 + 8 * i, i == 0 ? 0x77u : 0u);
        b[0x34 + 8 * i] = static_cast<uint8_t>(i == 0 ? 2 : 0);
    }
    put<uint32_t>(b.data(), 0xa0, 3);
    put<uint16_t>(b.data(), 0xa4, 640);  // water max: 10 charges
    b[0xa6] = KILN_STATION_STATE_RUNNING;
    b[0xa7] = 0;
    put<uint16_t>(b.data(), 0xa8, 128);
    b[0xaa] = 1;
    b[0xab] = 0;
    for (int i = 0; i < 16; ++i) b[0xb0 + i] = static_cast<uint8_t>(0xA0 + i);
    return b;
}

int main() {
    // ---- the ABI the managed side and the ABI ledger pin ----
    CHECK(sizeof(KilnStationClientView) == 232);
    CHECK(offsetof(KilnStationClientView, entity) == 4 && offsetof(KilnStationClientView, base) == 16);
    CHECK(offsetof(KilnStationClientView, position) == 32 && offsetof(KilnStationClientView, state) == 44);
    CHECK(offsetof(KilnStationClientView, selected_choices) == 48 && offsetof(KilnStationClientView, crafts_loaded) == 80);
    CHECK(offsetof(KilnStationClientView, flags) == 92 && offsetof(KilnStationClientView, recipe_start_ns) == 96);
    CHECK(offsetof(KilnStationClientView, craft_ns) == 112 && offsetof(KilnStationClientView, remaining_ns) == 128);
    CHECK(offsetof(KilnStationClientView, progress) == 136 && offsetof(KilnStationClientView, output_count) == 140);
    CHECK(offsetof(KilnStationClientView, outputs) == 144 && offsetof(KilnStationClientView, step) == 208);
    CHECK(sizeof(KilnStationsClientStatus) == 192 && offsetof(KilnStationsClientStatus, step) == 16 &&
          offsetof(KilnStationsClientStatus, age_ms) == 24 && offsetof(KilnStationsClientStatus, detail) == 32);
    CHECK(sizeof(KilnStationsClientApi) == 32 && offsetof(KilnStationsClientApi, status) == 8 &&
          offsetof(KilnStationsClientApi, stations) == 16 && offsetof(KilnStationsClientApi, station) == 24);
    CHECK(KILN_STATION_CLIENT_RECIPE == 1 && KILN_STATION_CLIENT_TIME == 2 && KILN_STATION_CLIENT_OUTPUTS == 4 &&
          KILN_STATION_CLIENT_ENERGY == 8);

    // ---- the layout: 26430625's is valid; one that does not fit is refused ----
    RowLayout l;
    CHECK(layout_ok(l));
    RowLayout bad = l;
    bad.base = 0xc0;  // 16 bytes past the row's end
    CHECK(!layout_ok(bad));
    bad = l;
    bad.sel_index = 0x38;  // the choices' index byte must follow its category
    CHECK(!layout_ok(bad));
    bad = l;
    bad.entity = 4;  // the decoder writes the entity first; FindRow binary-searches it at +0
    CHECK(!layout_ok(bad));

    // ---- one row decoded ----
    const auto bytes = forge_row();
    const Row r = decode(bytes.data(), l);
    CHECK(r.entity == 4242 && r.workshop == 0x56d94f8u && r.recipe == 0x1234abcdu && r.crafts == 3);
    CHECK(r.position[0] == 1024.25f && r.position[1] == 51.5f && r.position[2] == 2048.0f);
    CHECK(r.start == 100'000'000'000LL);
    CHECK(r.choices[0].category == 0x77u && r.choices[0].index == 2 && r.choices[1].category == 0);
    CHECK(r.water == 128 && r.water_max == 640 && r.state == KILN_STATION_STATE_RUNNING && r.missing == 0);
    CHECK(r.use_all == 1 && r.energy == 0 && r.base.guid[0] == 0xA0 && r.base.guid[15] == 0xAF);

    // ---- the scaled craft (factory_station's float path) ----
    CHECK(scaled_ns(60'000'000'000LL, 1.0f) == 60'000'000'000LL);
    CHECK(scaled_ns(60'000'000'000LL, 0.5f) == 30'000'000'000LL);
    CHECK(scaled_ns(60'000'000'000LL, 0.0f) == 0);   // no factor: unknown
    CHECK(scaled_ns(60'000'000'000LL, -1.0f) == 0);
    CHECK(scaled_ns(60'000'000'000LL, std::nanf("")) == 0);
    CHECK(scaled_ns(0, 1.0f) == 0);
    // the float step loses what a float cannot hold, as the game does: 1.000000001 s scales to 1 s
    CHECK(scaled_ns(1'000'000'001LL, 1.0f) == 1'000'000'000LL);

    // ---- the HUD's remaining time: craft * (crafts + 1) - (now - start) ----
    const int64_t s = 1'000'000'000LL;
    CHECK(game_remaining(60 * s, 3, 100 * s, 130 * s) == 60 * s * 4 - 30 * s);  // 3:30 left
    CHECK(game_remaining(60 * s, 0, 100 * s, 130 * s) == 30 * s);               // the cycle in progress alone
    CHECK(game_remaining(60 * s, 0, 100 * s, 200 * s) == -40 * s);              // overdue: the caller floors it
    // the game's extremes are "infinite" and stay so
    CHECK(game_remaining(kMax, 2, 100 * s, 130 * s) == kMax);
    CHECK(game_remaining(kMin, 2, 100 * s, 130 * s) == kMin);
    CHECK(game_remaining(kMax, 0, 100 * s, 130 * s) == kMax);
    CHECK(game_remaining(60 * s, 1, kMax, 130 * s) == kMax);  // a start at +inf: elapsed -inf, remaining +inf
    CHECK(game_remaining(60 * s, 1, kMin, 130 * s) == kMin);
    CHECK(game_remaining(60 * s, 1, 100 * s, kMax) == kMin);  // now +inf: elapsed +inf
    CHECK(game_remaining(60 * s, 1, kMax, kMax) == 120 * s);  // inf - inf = 0 elapsed, as the game's cmov
    CHECK(game_remaining(kMax, 1, 100 * s, kMax) == 0);       // total +inf and elapsed +inf: 0
    // wrapping products as the game's imul
    CHECK(game_remaining(kMax / 2, 4, 0, 0) == static_cast<int64_t>(static_cast<uint64_t>(kMax / 2) * 5u));

    // ---- the view one row gives ----
    RecipeFacts forge;
    forge.found = true;
    forge.duration_ns = 60 * s;
    forge.outputs = {501, 502};
    forge.inputs = {502, 601};  // 502 is also an input: never a finished output
    std::vector<Stack> inv = {{501, 2}, {601, 5}, {501, 3}, {502, 9}, {999, 1}};
    KilnStationClientView v = view_of(r, forge, 1.0f, 130 * s, 77, true, inv);
    CHECK(v.size == sizeof v && v.entity == 4242 && v.workshop == 0x56d94f8u && v.selected_recipe == 0x1234abcdu);
    CHECK(v.state == KILN_STATION_STATE_RUNNING && v.crafts_loaded == 3 && v.water == 128 && v.water_max == 640);
    CHECK(v.use_all_options == 1 && v.selected_choices[0].category == 0x77u && v.step == 77);
    CHECK(v.flags == (KILN_STATION_CLIENT_RECIPE | KILN_STATION_CLIENT_TIME | KILN_STATION_CLIENT_OUTPUTS));
    CHECK(v.craft_ns == 60 * s && v.craft_remaining_ns == 30 * s && v.remaining_ns == 210 * s);
    CHECK(std::fabs(v.progress - 0.5f) < 1e-6f);
    CHECK(v.output_count == 1 && v.outputs[0].item == 501 && v.outputs[0].count == 5);
    CHECK(v.now_ns == 130 * s && v.recipe_start_ns == 100 * s);

    // overdue: floored at zero, progress full
    v = view_of(r, forge, 1.0f, 400 * s, 78, true, inv);
    CHECK((v.flags & KILN_STATION_CLIENT_TIME) && v.craft_remaining_ns == 0 && v.remaining_ns == 0 && v.progress == 1.0f);
    // the client's factory speed halves the craft
    v = view_of(r, forge, 0.5f, 130 * s, 79, false, {});
    CHECK(v.craft_ns == 30 * s && v.craft_remaining_ns == 0 && v.remaining_ns == 90 * s);
    CHECK(!(v.flags & KILN_STATION_CLIENT_OUTPUTS) && v.output_count == 0);

    // only while Running, as the game's HUD: a stopped station has a recipe but no time
    Row stopped = r;
    stopped.state = KILN_STATION_STATE_STOPPED;
    v = view_of(stopped, forge, 1.0f, 130 * s, 80, true, inv);
    CHECK((v.flags & KILN_STATION_CLIENT_RECIPE) && !(v.flags & KILN_STATION_CLIENT_TIME));
    CHECK(v.craft_remaining_ns == 0 && v.remaining_ns == 0 && v.progress == 0.0f && v.output_count == 1);

    // a recipe the client's registry does not have: no time, no outputs (nothing says which items are outputs)
    RecipeFacts unknown;
    v = view_of(r, unknown, 1.0f, 130 * s, 81, true, inv);
    CHECK(v.flags == 0 && v.craft_ns == 0 && v.output_count == 0);
    // no recipe selected
    Row idle = r;
    idle.recipe = 0;
    idle.state = KILN_STATION_STATE_NO_RECIPE;
    v = view_of(idle, forge, 1.0f, 130 * s, 82, true, inv);
    CHECK(v.flags == 0 && v.selected_recipe == 0);
    // no factory speed (no BalancingRegistry): no time
    v = view_of(r, forge, 0.0f, 130 * s, 83, true, inv);
    CHECK(!(v.flags & (KILN_STATION_CLIENT_RECIPE | KILN_STATION_CLIENT_TIME)) && (v.flags & KILN_STATION_CLIENT_OUTPUTS));
    // a power-hungry station says so
    Row grinder = r;
    grinder.energy = 1;
    CHECK(view_of(grinder, forge, 1.0f, 130 * s, 84, false, {}).flags & KILN_STATION_CLIENT_ENERGY);

    // at most 8 finished stacks
    RecipeFacts many;
    many.found = true;
    many.duration_ns = s;
    std::vector<Stack> lots;
    for (uint32_t i = 1; i <= 12; ++i) many.outputs.push_back(i), lots.push_back({i, i});
    KilnStationStack outs[KILN_STATIONS_CLIENT_MAX_OUTPUTS] = {};
    CHECK(finished(many, lots, outs) == KILN_STATIONS_CLIENT_MAX_OUTPUTS && outs[7].item == 8);

    // ---- the caller's buffer ----
    std::vector<KilnStationClientView> rows(3);
    for (uint32_t i = 0; i < 3; ++i) rows[i] = view_of(r, forge, 1.0f, 130 * s, i, false, {}), rows[i].entity = 10 + i;
    KilnStationClientView buf[4] = {};
    for (auto& b : buf) b.size = sizeof b;
    uint32_t n = 99;
    CHECK(copy_out(rows, buf, 4, &n) == KILN_OK && n == 3 && buf[2].entity == 12);
    CHECK(copy_out(rows, buf, 2, &n) == KILN_ERR_BUFFER_TOO_SMALL && n == 3 && buf[1].entity == 11);
    CHECK(copy_out(rows, nullptr, 0, &n) == KILN_ERR_BUFFER_TOO_SMALL && n == 3);
    CHECK(copy_out({}, nullptr, 0, &n) == KILN_OK && n == 0);
    CHECK(copy_out(rows, nullptr, 2, &n) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(copy_out(rows, buf, 2, nullptr) == KILN_ERR_INVALID_ARGUMENT);
    buf[1].size = 8;  // an older or foreign row size: refused, not misread
    CHECK(copy_out(rows, buf, 2, &n) == KILN_ERR_INVALID_ARGUMENT);

    // ---- the demand window ----
    CHECK(!demanded(0, 1000));
    CHECK(demanded(1000, 1000) && demanded(1000, 1000 + KILN_STATIONS_CLIENT_DEMAND_MS));
    CHECK(!demanded(1000, 1001 + KILN_STATIONS_CLIENT_DEMAND_MS));
    CHECK(!demanded(2000, 1000));  // a clock that went back: no demand

    std::printf("client stations: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
