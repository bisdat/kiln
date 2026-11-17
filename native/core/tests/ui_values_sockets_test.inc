// SPDX-License-Identifier: MIT
// Included by ui_values_test.cpp after ui_values_gemforge_test.inc: gems.sockets (gemforge_sockets), the Gem Forge's
// widened category masks. The two mid hooks run against a fake stack frame; "game memory" is this process's.
struct SocketsFixture {
    uint8_t stack[0x100]{};
    KilnOwner a = 0, b = 0;
    explicit SocketsFixture(bool row = true) {
        fake::reset();
        if (row) fake::rows.insert("gems.sockets");
        fake::symbols["ui/GemForgeLists"] = 0x140cafac0ull;
        fake::symbols["ui/GemForgeCandidates"] = 0x140d27cb0ull;
        fake::symbols["ui/GemForgeWeaponsMask"] = 0x140cafc89ull;  // right after `mov dword [rsp+0x6c], 4`
        fake::symbols["ui/GemForgeCarrierMask"] = 0x140d27d80ull;  // right after `mov dword [rsp+0x4c], 4`
        fake::constants["ui/gemforge.weaponsMask"] = 0x6c;
        fake::constants["ui/gemforge.carrierMask"] = 0x4c;
        a = fake::owner("mod:rings");
        b = fake::owner("mod:armour");
        frame();
    }
    void frame() {  // what the game stored before the hooked instruction, and a neighbour that must stay
        std::memset(stack, 0xEE, sizeof stack);
        put(stack, 0x6c, uint32_t{4});
        put(stack, 0x4c, uint32_t{4});
    }
    KilnMidFn mid(const char* label) const {
        for (const auto& m : fake::mids)
            if (m.label == label) return m.fn;
        return nullptr;
    }
    // Runs both hooks as the game would reach them; returns {WEAPONS mask, carrier mask}.
    std::pair<uint32_t, uint32_t> run() {
        frame();
        KilnRegisters regs{};
        regs.rsp = addr(stack);
        if (auto fn = mid("ui.values/GemForgeWeaponsMask")) fn(&regs, nullptr);
        if (auto fn = mid("ui.values/GemForgeCarrierMask")) fn(&regs, nullptr);
        uint32_t w = 0, c = 0;
        std::memcpy(&w, stack + 0x6c, 4);
        std::memcpy(&c, stack + 0x4c, 4);
        return {w, c};
    }
    bool neighbours_untouched() const {
        for (size_t i = 0; i < sizeof stack; ++i)
            if ((i < 0x4c || i >= 0x50) && (i < 0x6c || i >= 0x70) && stack[i] != 0xEE) return false;
        return true;
    }
};

void sockets_tests() {
    static_assert(offsetof(KilnUiValuesApi, gemforge_sockets) == 104);
    static_assert(sizeof(KilnUiValuesApi) >= 112);  // 0.2.0-dev.29.15 appended gemforge_sources (ui_values_sources_test.inc)
    {
        SocketsFixture f;
        CHECK(T()->header.size == sizeof(KilnUiValuesApi) && T()->gemforge_sockets, "sockets: the table's size covers gemforge_sockets");
        CHECK(T()->gemforge_sockets(0, 2) == KILN_ERR_INVALID_ARGUMENT, "sockets: owner 0 is refused");
        for (uint32_t bad : {1u << 14, 1u << 15, 1u << 16, 0x8000u | 2u, 0x80000000u})
            CHECK(T()->gemforge_sockets(f.a, bad) == KILN_ERR_INVALID_ARGUMENT && fake::mids.empty(),
                  "sockets: mask 0x%x (a bit outside categories 0..13) is refused before hooking", bad);
        CHECK(T()->gemforge_sockets(f.a, 4) == KILN_OK && fake::mids.empty() && sockets_effective() == 0,
              "sockets: Weapons alone is vanilla and installs nothing");
        CHECK(T()->gemforge_sockets(f.a, 0) == KILN_OK && fake::mids.empty(), "sockets: 0 removes without hooking");
        const auto [w, c] = f.run();
        CHECK(w == 4 && c == 4 && !fake::writes, "sockets: without a widening owner the game's masks stay 4");
    }
    {
        SocketsFixture f;
        CHECK(T()->gemforge_sockets(f.a, 2) == KILN_OK && fake::mids.size() == 2, "sockets: the first extra bit installs two mids");
        CHECK(fake::mids.size() == 2 && fake::mids[0].target == 0x140cafc89ull && fake::mids[1].target == 0x140d27d80ull,
              "sockets: the mids sit right after the two mask stores");
        auto [w, c] = f.run();
        CHECK(w == 6 && c == 6 && f.neighbours_untouched(),
              "sockets: Equipment is ORed into both masks (4 -> 6), nothing else on the stack changes");
        CHECK(T()->gemforge_sockets(f.b, 1u << 8) == KILN_OK && fake::mids.size() == 2 && sockets_effective() == 0x102,
              "sockets: a second owner adds its bits without new hooks");
        std::tie(w, c) = f.run();
        CHECK(w == 0x106 && c == 0x106, "sockets: the effective mask is the OR over owners");
        CHECK(T()->gemforge_sockets(f.a, 2 | 4) == KILN_OK && sockets_effective() == 0x102,
              "sockets: an owner's new mask replaces its old one; Weapons is never an extra bit");
        CHECK(T()->gemforge_sockets(f.a, 0) == KILN_OK && sockets_effective() == 0x100, "sockets: 0 removes only this owner");
        fake::revoke(f.b);
        CHECK(sockets_effective() == 0, "sockets: unloading an owner removes its mask");
        CHECK(T()->gemforge_sockets(f.b, 2) == KILN_ERR_REVOKED, "sockets: a retired owner is refused");
        const size_t writes = fake::writes;
        std::tie(w, c) = f.run();
        CHECK(w == 4 && c == 4 && fake::writes == writes && f.neighbours_untouched(),
              "sockets: with every owner back at 0 the installed hooks write nothing (vanilla)");
        CHECK(sockets_seen() == 0x102, "sockets: the snapshot keeps tolerating categories once widened");
    }
    {
        SocketsFixture f;
        T()->gemforge_sockets(f.a, 2);
        // The game already widened (another engine's patch): OR keeps its bits and adds ours.
        f.frame();
        put(f.stack, 0x6c, uint32_t{4 | 0x40});
        KilnRegisters regs{};
        regs.rsp = addr(f.stack);
        f.mid("ui.values/GemForgeWeaponsMask")(&regs, nullptr);
        uint32_t w = 0;
        std::memcpy(&w, f.stack + 0x6c, 4);
        CHECK(w == (4u | 0x40u | 2u), "sockets: the hook ORs, never replaces another patch's bits");
        fake::unreadable.insert(addr(f.stack) + 0x6c);
        put(f.stack, 0x6c, uint32_t{4});
        const size_t writes = fake::writes;
        f.mid("ui.values/GemForgeWeaponsMask")(&regs, nullptr);
        CHECK(fake::writes == writes, "sockets: an unreadable slot is left alone");
    }
    {
        fake::reset();
        fake::client_side = false;
        const KilnOwner o = fake::owner("mod:server");
        CHECK(T()->gemforge_sockets(o, 2) == KILN_ERR_UNAVAILABLE && T()->gemforge_sockets(o, 0) == KILN_ERR_UNAVAILABLE &&
              fake::mids.empty(), "sockets: a server answers unavailable");
    }
    for (int gate = 0; gate < 7; ++gate) {
        SocketsFixture f(gate != 0);
        if (gate == 1) fake::layout_supported = false;
        if (gate == 2) fake::symbols.erase("ui/GemForgeCarrierMask");
        if (gate == 3) fake::constants["ui/gemforge.weaponsMask"] = 0x6d;  // not an aligned dword slot
        if (gate == 4) fake::constants.erase("ui/gemforge.carrierMask");
        if (gate == 5) fake::symbols["ui/GemForgeWeaponsMask"] = 0x140cb0100ull;  // outside ui/GemForgeLists
        if (gate == 6) fake::hook_result = KILN_ERR_UNAVAILABLE;
        CHECK(T()->gemforge_sockets(f.a, 2) == KILN_ERR_UNAVAILABLE && fake::mids.empty() && sockets_effective() == 0,
              "sockets: gate %d (row, layout, symbol, constant, scope, hook) fails closed", gate);
        if (gate) CHECK(fake::rows_off == std::vector<std::string>{"gems.sockets"} &&
                        T()->gemforge_sockets(f.b, 8) == KILN_ERR_UNAVAILABLE && fake::rows_off.size() == 1,
                        "sockets: gate %d reports the row off once and stays off", gate);
        CHECK(T()->gemforge_sockets(f.a, 0) == KILN_OK, "sockets: gate %d still accepts a removal", gate);
    }
    {
        // The GEMS snapshot with a widened carrier: a ring (Equipment, 1) holding a gem is a carrier row, not a broken
        // contract, once gems.sockets widened Equipment; without it the same list disables only the snapshot.
        for (int widened = 0; widened < 2; ++widened) {
            GemFixture g;
            KilnUiGemForgeFrame m{};
            KilnUiGemForgeCandidate rows[2]{};
            fake::rows.insert("gems.sockets");
            fake::symbols["ui/GemForgeLists"] = 0x140cafac0ull;
            fake::symbols["ui/GemForgeCandidates"] = 0x140d27cb0ull;
            fake::symbols["ui/GemForgeWeaponsMask"] = 0x140cafc89ull;
            fake::symbols["ui/GemForgeCarrierMask"] = 0x140d27d80ull;
            fake::constants["ui/gemforge.weaponsMask"] = 0x6c;
            fake::constants["ui/gemforge.carrierMask"] = 0x4c;
            if (widened) CHECK(T()->gemforge_sockets(g.owner, 2) == KILN_OK, "sockets: widen Equipment for the snapshot");
            g.read(m); g.arm(); gem_weapon_category = 1;
            g.frame(nullptr, nullptr);
            const KilnResult rc = g.read(m, rows, 2);
            if (widened)
                CHECK(rc == KILN_OK && m.ok && m.count == 2 && rows[0].has_weapon_carrier && rows[0].weapon_carrier.item == 220 &&
                      rows[0].displayed_gem.item == 111 && m.has_selected_weapon && fake::rows_off.empty(),
                      "sockets: a widened ring carrier and selection keep the Gem Forge snapshot");
            else
                CHECK(rc == KILN_ERR_UNAVAILABLE && fake::rows_off == std::vector<std::string>{"ui.gemforge.inspect"},
                      "sockets: without the widening a ring carrier is still a broken contract");
        }
    }
}
