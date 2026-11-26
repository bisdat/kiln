// SPDX-License-Identifier: MIT
// kiln.skills.client (kiln_skills_client.h, since 0.2.0-dev.31.13), the pure part (client_view_logic.h) on the host, no
// game: the ABI layout the managed side mirrors, one keen::SkillTreeNode and link of the loaded resource, the screen's
// record merged into a level (plain nodes, nodes with levels, shadow nodes, a missing record), the records map's probe,
// skilltree_ui's point triple, the resource header, the buffers and the demand window. docs/re/progression.md section 8.
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "../client_view_logic.h"

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_failed;                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                     \
    } while (0)

using namespace kiln::skills::client;

template <class T>
static void put(uint8_t* p, size_t off, T v) {
    std::memcpy(p + off, &v, sizeof v);
}

// A node as the resource has it: Emberheart-like, 3 levels, cost 2, at (120.5, -40), 2 links in, 1 out.
static std::vector<uint8_t> node(uint32_t id, uint8_t max_level, uint16_t cost) {
    std::vector<uint8_t> b(NodeOffsets::size, 0);
    put<uint32_t>(b.data(), NodeOffsets::id, id);
    put<float>(b.data(), NodeOffsets::pos, 120.5f);
    put<float>(b.data(), NodeOffsets::pos + 4, -40.0f);
    put<int32_t>(b.data(), NodeOffsets::in, 0x200);
    put<uint32_t>(b.data(), NodeOffsets::in + 4, 2);
    put<int32_t>(b.data(), NodeOffsets::out, 0x300);
    put<uint32_t>(b.data(), NodeOffsets::out + 4, 1);
    put<uint16_t>(b.data(), NodeOffsets::cost, cost);
    b[NodeOffsets::type] = 2;  // ActiveSkill
    b[NodeOffsets::cls] = 1;   // Mage
    b[NodeOffsets::max_level] = max_level;
    b[NodeOffsets::level_index] = max_level ? 7 : 0;
    return b;
}

int main() {
    // ---- the ABI the managed side and the ABI ledger pin ----
    CHECK(sizeof(KilnSkillsClientNode) == 32 && offsetof(KilnSkillsClientNode, index) == 4 &&
          offsetof(KilnSkillsClientNode, cost) == 6 && offsetof(KilnSkillsClientNode, type) == 8 &&
          offsetof(KilnSkillsClientNode, level) == 11 && offsetof(KilnSkillsClientNode, flags) == 12 &&
          offsetof(KilnSkillsClientNode, x) == 16 && offsetof(KilnSkillsClientNode, incoming) == 24);
    CHECK(sizeof(KilnSkillsClientLink) == 16 && offsetof(KilnSkillsClientLink, to) == 8);
    CHECK(sizeof(KilnSkillsClientState) == 216 && offsetof(KilnSkillsClientState, player) == 8 &&
          offsetof(KilnSkillsClientState, total) == 12 && offsetof(KilnSkillsClientState, available) == 20 &&
          offsetof(KilnSkillsClientState, links) == 32 && offsetof(KilnSkillsClientState, age_ms) == 48 &&
          offsetof(KilnSkillsClientState, detail) == 56);
    CHECK(sizeof(KilnSkillsClientApi) == 32 && offsetof(KilnSkillsClientApi, state) == 8 &&
          offsetof(KilnSkillsClientApi, nodes) == 16 && offsetof(KilnSkillsClientApi, links) == 24);
    CHECK(KILN_SKILLS_CLIENT_UNLOCKED == 1 && KILN_SKILLS_CLIENT_SHADOW == 2 && KILN_SKILLS_CLIENT_RECORD == 4);

    // ---- a node with levels ----
    const auto ember = node(0xcafe0001u, 3, 2);
    Record rec{true, 2, 1};
    KilnSkillsClientNode n = node_of(ember.data(), 250, rec, false);
    CHECK(n.id == 0xcafe0001u && n.index == 250 && n.cost == 2 && n.type == 2 && n.class_alignment == 1);
    CHECK(n.max_level == 3 && n.level == 2 && n.level_index == 7);
    CHECK(n.x == 120.5f && n.y == -40.0f && n.incoming == 2 && n.outgoing == 1);
    CHECK(n.flags == (KILN_SKILLS_CLIENT_RECORD | KILN_SKILLS_CLIENT_UNLOCKED));
    // unlocked with a level byte of 0 (a byte the game left): at least level 1; above the max: the max
    CHECK(node_of(ember.data(), 1, Record{true, 0, 1}, false).level == 1);
    CHECK(node_of(ember.data(), 1, Record{true, 9, 1}, false).level == 3);
    // locked: level 0 whatever the level byte says (skilltree_ui decodes a level slot for every node)
    n = node_of(ember.data(), 1, Record{true, 2, 0}, false);
    CHECK(n.level == 0 && n.flags == KILN_SKILLS_CLIENT_RECORD);

    // ---- a plain node: its level byte is another node's slot (levelIndex 0) and is ignored ----
    const auto plain = node(0xbeef0002u, 0, 1);
    n = node_of(plain.data(), 3, Record{true, 5, 1}, false);
    CHECK(n.level == 1 && (n.flags & KILN_SKILLS_CLIENT_UNLOCKED) && n.max_level == 0);
    n = node_of(plain.data(), 3, Record{true, 5, 0}, false);
    CHECK(n.level == 0 && !(n.flags & KILN_SKILLS_CLIENT_UNLOCKED));

    // ---- a shadow node: Kiln's screen hook wrote level and unlocked from the server's table ----
    n = node_of(ember.data(), 300, Record{true, 3, 1}, true);
    CHECK(n.level == 3 && n.flags == (KILN_SKILLS_CLIENT_RECORD | KILN_SKILLS_CLIENT_UNLOCKED | KILN_SKILLS_CLIENT_SHADOW));
    // no record (the screen's map does not have the id): locked, and it says so
    n = node_of(ember.data(), 300, Record{}, true);
    CHECK(n.level == 0 && n.flags == KILN_SKILLS_CLIENT_SHADOW);

    // ---- a link ----
    uint8_t l[kLinkSize] = {};
    put<uint32_t>(l, 0, 0x11u);
    put<uint32_t>(l, 4, 0xcafe0001u);
    put<uint32_t>(l, 8, 0xbeef0002u);
    l[12] = 1;
    const KilnSkillsClientLink link = link_of(l);
    CHECK(link.id == 0x11u && link.from == 0xcafe0001u && link.to == 0xbeef0002u && link.bidirectional == 1);
    l[12] = 7;  // any nonzero byte is "both ways"
    CHECK(link_of(l).bidirectional == 1);

    // ---- the records map's probe ----
    const uint64_t cap = 8;
    std::vector<bool> bits(cap, false);
    std::vector<uint32_t> keys(cap, 0);
    auto add = [&](uint32_t id) {
        uint64_t s = id & (cap - 1);
        while (bits[s]) s = (s + 1) & (cap - 1);
        bits[s] = true;
        keys[s] = id;
        return s;
    };
    const uint64_t a = add(0x10u), b = add(0x18u), c = add(0x07u);  // 0x10 and 0x18 collide on slot 0
    auto bit = [&](uint64_t s) { return static_cast<bool>(bits[s]); };
    auto key = [&](uint64_t s) { return keys[s]; };
    CHECK(find_slot(0x10u, cap, bit, key) == static_cast<int64_t>(a));
    CHECK(find_slot(0x18u, cap, bit, key) == static_cast<int64_t>(b) && b == 1);
    CHECK(find_slot(0x07u, cap, bit, key) == static_cast<int64_t>(c));
    CHECK(find_slot(0x20u, cap, bit, key) == -1);  // slot 0 chain ends at slot 2: not there
    CHECK(find_slot(0x10u, 0, bit, key) == -1 && find_slot(0x10u, 6, bit, key) == -1);
    std::vector<bool> full(cap, true);
    CHECK(find_slot(0x99u, cap, [&](uint64_t s) { return static_cast<bool>(full[s]); }, key) == -1);  // a full map ends

    // ---- skilltree_ui's points ----
    CHECK(points_ok(30, 12, 18) && points_ok(10, 12, 0) && points_ok(0, 0, 0));
    CHECK(!points_ok(30, 12, 17) && !points_ok(-1, 0, 0) && !points_ok(10, 12, -2) && !points_ok(200000, 0, 200000));

    // ---- the resource header ----
    uint8_t h[16] = {};
    put<int32_t>(h, 0, 0x40);
    put<uint32_t>(h, 4, 263);
    put<int32_t>(h, 8, 0x40 + 263 * 0x8c - 8);
    put<uint32_t>(h, 12, 332);
    Arrays arr;
    CHECK(arrays_of(h, &arr) && arr.nodes == 0x40 && arr.node_count == 263 && arr.links == 0x40 + 263 * 0x8c && arr.link_count == 332);
    put<uint32_t>(h, 4, 0);
    CHECK(!arrays_of(h, &arr));
    put<uint32_t>(h, 4, 5000);
    CHECK(!arrays_of(h, &arr));
    put<uint32_t>(h, 4, 263);
    put<int32_t>(h, 0, 4);  // inside the header
    CHECK(!arrays_of(h, &arr));

    // ---- the buffers ----
    std::vector<KilnSkillsClientNode> rows(3);
    for (uint16_t i = 0; i < 3; ++i) rows[i].index = i, rows[i].id = 100 + i;
    KilnSkillsClientNode out[4] = {};
    uint32_t cnt = 0;
    CHECK(copy_out(rows, out, 4, &cnt) == KILN_OK && cnt == 3 && out[2].id == 102);
    CHECK(copy_out(rows, out, 1, &cnt) == KILN_ERR_BUFFER_TOO_SMALL && cnt == 3 && out[0].id == 100);
    CHECK(copy_out(rows, static_cast<KilnSkillsClientNode*>(nullptr), 0, &cnt) == KILN_ERR_BUFFER_TOO_SMALL && cnt == 3);
    CHECK(copy_out(rows, static_cast<KilnSkillsClientNode*>(nullptr), 1, &cnt) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(copy_out(rows, out, 1, nullptr) == KILN_ERR_INVALID_ARGUMENT);
    std::vector<KilnSkillsClientLink> none;
    KilnSkillsClientLink lo[1] = {};
    CHECK(copy_out(none, lo, 1, &cnt) == KILN_OK && cnt == 0);

    // ---- the demand window ----
    CHECK(!demanded(0, 5) && demanded(100, 100 + KILN_SKILLS_CLIENT_DEMAND_MS) && !demanded(100, 101 + KILN_SKILLS_CLIENT_DEMAND_MS));

    std::printf("skills client view: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
