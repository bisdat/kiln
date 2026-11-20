// SPDX-License-Identifier: MIT
// Compile the actual portable game_index_keys body from hooks.cpp, with its game-memory seams stubbed.
// An any-thread catalog open before readiness must never try to initialize the game's content reader.
#include "assets.h"
#include <cstdlib>
#include <iostream>

namespace ka {
static bool g_content_on = true;
static std::atomic<const void*> g_game_map{nullptr};
static int game_calls = 0;
bool game_blob_find(const HashK&, GameBlob*) { ++game_calls; return false; }
struct MapView {
    const uint8_t *buckets = nullptr, *keys = nullptr, *values = nullptr;
    uint32_t nb = 0, nk = 0;
};
static bool rd(uintptr_t, void*, size_t) { std::abort(); }
static bool view_map(const void*, MapView*) { std::abort(); }
static bool verify_game_map(const MapView&, std::string*) { std::abort(); }
#include "catalog_index.inc"
}

int main() {
    std::vector<ka::HashK> keys{{1, 2, 3, 4}};
    uint64_t total = 99;
    std::string why;
    auto result = ka::game_index_keys(&keys, 1000000, &total, &why);
    if (result != ka::GameIndex::not_ready || !keys.empty() || total != 0 || ka::game_calls != 0) {
        std::cerr << "FAIL early catalog query must return NotReady without calling the game\n";
        return 1;
    }
    std::cout << "PASS early catalog query returns NotReady without calling the game\n";
}
