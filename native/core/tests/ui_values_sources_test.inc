// SPDX-License-Identifier: MIT
// Included by ui_values_test.cpp after ui_values_sockets_test.inc: ui.gemforge.sources (gemforge_sources), a server's
// offered chest gems in the GEMS grid. The grid link, the selected-row resolve and the two price tests run against a
// fake game array, a fake state block and a fake frame; "game memory" is this process's.
namespace src_test {
using Grid = void (*)(void*, void*, void*, void*, float, uint8_t);
struct Seen {
    int calls = 0;
    uint64_t data = 0, count = 0;
    void* header = nullptr;
    std::vector<uint8_t> rows;
} seen;
void fake_grid(void*, void* header, void*, void*, float, uint8_t) {
    ++seen.calls;
    seen.header = header;
    std::memcpy(&seen.data, header, 8);
    std::memcpy(&seen.count, static_cast<uint8_t*>(header) + 8, 8);
    seen.rows.assign(reinterpret_cast<const uint8_t*>(seen.data), reinterpret_cast<const uint8_t*>(seen.data) + seen.count * 0x48);
}
}  // namespace src_test

struct SourcesFixture {
    // the three sites' measured bytes, in "code" the fake symbols point at
    std::vector<uint8_t> code = std::vector<uint8_t>(0x200, 0xCC);
    uintptr_t resolve = 0, slot = 0, upgrade = 0;
    std::vector<uint8_t> game = std::vector<uint8_t>(0x48 * 2);  // the game's GEMS array: two of the player's own rows
    uint64_t header[4]{};
    uint8_t state[0x100]{};
    uint8_t frame[0x80]{};  // rbp points at frame + 0x40: the selected view at rbp-0x20, its item at rbp-0x18
    KilnOwner owner = 0;
    explicit SourcesFixture(bool row = true) {
        fake::reset();
        if (row) fake::rows.insert("ui.gemforge.sources");
        const uint8_t tail[] = {0xF2, 0x0F, 0x10, 0x42, 0x40, 0xF2, 0x0F, 0x11, 0x45, 0x20};
        std::memcpy(code.data() + 0x40 - sizeof tail, tail, sizeof tail);
        const uint8_t r[] = {0x48, 0x8B, 0x05};
        std::memcpy(code.data() + 0x40, r, sizeof r);
        const uint8_t s[] = {0x41, 0x3B, 0xF7, 0x0F, 0x93, 0x44, 0x24, 0x51};
        std::memcpy(code.data() + 0x100, s, sizeof s);
        const uint8_t u[] = {0x45, 0x3B, 0x66, 0x08, 0x73, 0x09, 0x41, 0x3B, 0xF7, 0x72, 0x04};
        std::memcpy(code.data() + 0x180, u, sizeof u);
        resolve = addr(code.data()) + 0x40;
        slot = addr(code.data()) + 0x100;
        upgrade = addr(code.data()) + 0x180;
        fake::symbols["ui/GemForgeGrid"] = 0x140caefc0ull;
        fake::symbols["ui/GemForgeMenu"] = resolve - 0x416;
        fake::symbols["ui/GemForgeActions"] = slot - 0x44c;
        fake::symbols["ui/GemForgeSelectResolve"] = resolve;
        fake::symbols["ui/GemForgeAffordSlot"] = slot;
        fake::symbols["ui/GemForgeAffordUpgrade"] = upgrade;
        fake::constants["ui/view.stride"] = 0x48;
        fake::constants["ui/gemforge.selected"] = 0x10;
        env().gemforge_abi = +[] { return true; };
        // the client's catalog: 110..113 are gems (WeaponGem 15), 200 a sword (Weapons 2), anything else unknown
        env().item_category = +[](uint32_t item, uint8_t* cat) {
            if (item >= 110 && item <= 113) return *cat = 15, 1;
            if (item == 200) return *cat = 2, 1;
            return 0;
        };
        fake::client.session = 3;
        fake::client.local_player = 77;
        fake::ticks = 10000;
        owner = fake::owner("kiln.ui.values");
        fill_view_bytes(game.data(), 0x5000, 2, 110, 1, 901, 0, 0);
        fill_view_bytes(game.data() + 0x48, 0x5000, 3, 111, 1, 902, 0, 0);
        header[0] = addr(game.data());
        header[1] = 2;
        header[2] = 2;
    }
    static KilnUiGemForgeSource row(uint32_t page, uint8_t slot, uint32_t item, uint32_t pide, uint32_t level, uint8_t group = 0) {
        KilnUiGemForgeSource r{};
        r.size = sizeof r;
        r.page = page;
        r.slot = slot;
        r.item = item;
        r.pide = pide;
        r.level = level;
        r.rarity = 2;
        r.group = group;
        r.token = 0xabc0 + pide;
        r.expires_ms = 20000;
        return r;
    }
    KilnResult set(std::initializer_list<KilnUiGemForgeSource> rows, std::initializer_list<KilnUiGemForgePayer> payers = {}) {
        std::vector<KilnUiGemForgeSource> r(rows);
        std::vector<KilnUiGemForgePayer> p(payers);
        return T()->gemforge_sources(owner, r.data(), static_cast<uint32_t>(r.size()), p.data(), static_cast<uint32_t>(p.size()));
    }
    const fake::Raw* raw() const {
        for (const auto& r : fake::raws)
            if (r.label == "ui.values/GemForgeSources") return &r;
        return nullptr;
    }
    KilnMidFn mid(const char* label) const {
        for (const auto& m : fake::mids)
            if (m.label == label) return m.fn;
        return nullptr;
    }
    // the grid as the game calls it (GEMS role unless weapons)
    void draw(uint8_t weapons = 0) {
        src_test::seen = {};
        const auto* r = raw();
        if (!r) return src_test::fake_grid(nullptr, header, nullptr, nullptr, 0, weapons);
        *r->original = reinterpret_cast<void*>(&src_test::fake_grid);
        reinterpret_cast<src_test::Grid>(r->fn)(nullptr, header, nullptr, nullptr, 0.5f, weapons);
    }
    // the selected-row resolve with `key` at state+0x10 and the game's found item (0: not found)
    KilnRegisters resolve_with(uint32_t page, uint8_t slot_index, uint32_t found_item) {
        std::memset(state, 0, sizeof state);
        std::memset(frame, 0xEE, sizeof frame);
        put(state, 0x10, page);
        state[0x14] = slot_index;
        put(frame, 0x40 - 0x18, found_item);
        KilnRegisters regs{};
        regs.rdi = addr(state);
        regs.rbp = addr(frame) + 0x40;
        regs.rcx = found_item;
        if (auto fn = mid("ui.values/GemForgeSelectResolve")) fn(&regs, nullptr);
        return regs;
    }
    uint32_t price(const char* label, uint32_t page, uint8_t slot_index, uint32_t material, uint32_t have) {
        std::memset(state, 0, sizeof state);
        put(state, 0x10, page);
        state[0x14] = slot_index;
        KilnRegisters regs{};
        regs.r13 = addr(state);
        regs.r10 = material;
        regs.rsi = have;
        if (auto fn = mid(label)) fn(&regs, nullptr);
        return static_cast<uint32_t>(regs.rsi);
    }
};

KilnUiGemForgePayer payer(uint8_t group, uint32_t item, uint32_t count, bool player_first = false) {
    KilnUiGemForgePayer p{};
    p.size = sizeof p;
    p.group = group;
    p.item = item;
    p.count = count;
    p.player_first = player_first;
    return p;
}

void sources_tests() {
    static_assert(offsetof(KilnUiValuesApi, gemforge_sources) == 112 && offsetof(KilnUiValuesApi, gemforge_sources_state) == 120);
    static_assert(offsetof(KilnUiValuesApi, tooltip_lines_native) == 128);  // the next slot starts where this one ended
    static_assert(sizeof(KilnUiGemForgeSource) == 48 && offsetof(KilnUiGemForgeSource, token) == 32);
    static_assert(sizeof(KilnUiGemForgePayer) == 16 && sizeof(KilnUiGemForgeSourcesState) == 200);
    {
        SourcesFixture f;
        CHECK(T()->header.size == sizeof(KilnUiValuesApi) && T()->gemforge_sources && T()->gemforge_sources_state,
              "sources: the table's size covers gemforge_sources and its state");
        CHECK(f.set({}) == KILN_OK && fake::raws.empty() && fake::mids.empty(), "sources: no rows installs nothing (a vanilla forge)");
        f.draw();
        CHECK(src_test::seen.count == 2 && src_test::seen.header == f.header, "sources: without the hooks the grid gets the game's array");
        CHECK(T()->gemforge_sources(0, nullptr, 0, nullptr, 0) == KILN_ERR_INVALID_ARGUMENT, "sources: owner 0 is refused");
        KilnUiGemForgeSource bad = SourcesFixture::row(0x9001, 8, 110, 1201, 3);
        CHECK(T()->gemforge_sources(f.owner, &bad, 1, nullptr, 0) == KILN_ERR_INVALID_ARGUMENT && fake::raws.empty(),
              "sources: slot 8 is refused before any hook");
        bad = SourcesFixture::row(0, 1, 110, 1201, 3);
        CHECK(T()->gemforge_sources(f.owner, &bad, 1, nullptr, 0) == KILN_ERR_INVALID_ARGUMENT, "sources: page 0 is refused");
        bad = SourcesFixture::row(0x9001, 1, 110, 0, 3);
        CHECK(T()->gemforge_sources(f.owner, &bad, 1, nullptr, 0) == KILN_ERR_INVALID_ARGUMENT, "sources: a row without its item entity is refused");
        KilnUiGemForgePayer bp = payer(0, 0x2b0cf1d5, 9);
        KilnUiGemForgeSource ok = SourcesFixture::row(0x9001, 1, 110, 1201, 3, 1);
        CHECK(T()->gemforge_sources(f.owner, &ok, 1, &bp, 1) == KILN_ERR_INVALID_ARGUMENT, "sources: a payer row of group 0 is refused");
    }
    {
        // review 2026-11-17: a row whose item the client's catalog does not know as a gem is dropped (never drawn)
        SourcesFixture f;
        CHECK(f.set({SourcesFixture::row(0x9001, 1, 112, 1201, 3), SourcesFixture::row(0x9001, 2, 999, 1202, 3),
                     SourcesFixture::row(0x9001, 3, 200, 1203, 3)}) == KILN_OK,
              "sources: rows with an unknown item and a non-gem item are taken as a call");
        KilnUiGemForgeSourcesState st{};
        st.size = sizeof st;
        CHECK(T()->gemforge_sources_state(&st) == KILN_OK && st.rows == 1, "sources: only the row of a known gem is kept");
        f.draw();
        CHECK(src_test::seen.count == 3, "sources: the grid gets the game's two rows and the one known gem");
        env().item_category = +[](uint32_t, uint8_t*) { return -1; };  // no catalog on this client
        CHECK(f.set({SourcesFixture::row(0x9001, 1, 112, 1201, 3)}) == KILN_OK, "sources: without a catalog the call is taken");
        st = {};
        st.size = sizeof st;
        CHECK(T()->gemforge_sources_state(&st) == KILN_OK && st.rows == 0, "sources: without a catalog no row is kept (fail closed)");
    }
    {
        SourcesFixture f(false);
        CHECK(f.set({SourcesFixture::row(0x9001, 1, 110, 1201, 3)}) == KILN_ERR_UNAVAILABLE && fake::raws.empty() && fake::mids.empty(),
              "sources: the row off: refused, nothing hooked");
    }
    {
        SourcesFixture f;
        f.code[0x100] = 0xE9;  // another hook (or another build) on the Insert price test
        CHECK(f.set({SourcesFixture::row(0x9001, 1, 110, 1201, 3)}) == KILN_ERR_UNAVAILABLE && fake::raws.empty() && fake::mids.empty() &&
                  !fake::rows_off.empty() && fake::rows_off.back() == "ui.gemforge.sources",
              "sources: a site whose bytes are not the measured instruction: off, nothing hooked, the row reported off");
        f.code[0x100] = 0x41;
        CHECK(f.set({SourcesFixture::row(0x9001, 1, 110, 1201, 3)}) == KILN_ERR_UNAVAILABLE && fake::raws.empty(),
              "sources: off stays off for the process");
        f.draw();
        CHECK(src_test::seen.header == f.header, "sources: off: the grid gets the game's array");
    }
    {
        SourcesFixture f;
        f.code[0x40 - 2] = 0x00;  // the view copy right before the resolve site is not the measured one
        CHECK(f.set({SourcesFixture::row(0x9001, 1, 110, 1201, 3)}) == KILN_ERR_UNAVAILABLE && fake::mids.empty(),
              "sources: the resolve's view copy differs: off");
    }
    {
        SourcesFixture f;
        env().gemforge_abi = +[] { return false; };  // a foreign detour on the grid's entry
        CHECK(f.set({SourcesFixture::row(0x9001, 1, 110, 1201, 3)}) == KILN_ERR_UNAVAILABLE && fake::raws.empty(),
              "sources: a grid entry that is not the measured function (and not Kiln's own link): off");
    }
    {
        SourcesFixture f;
        // a server offered two chest gems (one with a payer group) and one the player's own list holds already
        CHECK(f.set({SourcesFixture::row(0x9001, 1, 112, 1201, 3, 1), SourcesFixture::row(0x9001, 4, 113, 1202, 7),
                     SourcesFixture::row(0x5000, 3, 111, 902, 1)}) == KILN_OK,
              "sources: the first rows install the hooks");
        CHECK(f.raw() && f.raw()->target == 0x140caefc0ull && f.raw()->priority < 0 && fake::mids.size() == 3,
              "sources: one grid link after the snapshot's, three mids");
        CHECK(fake::mids[0].target == f.resolve && fake::mids[1].target == f.slot && fake::mids[2].target == f.upgrade,
              "sources: the mids sit on the resolve and the two price tests");
        f.draw();
        auto key = [](const std::vector<uint8_t>& rows, size_t i) {
            uint32_t page = 0;
            std::memcpy(&page, rows.data() + i * 0x48, 4);
            return std::pair<uint32_t, uint8_t>{page, rows[i * 0x48 + 4]};
        };
        CHECK(src_test::seen.calls == 1 && src_test::seen.count == 4 && src_test::seen.header != f.header,
              "sources: the grid draws the game's two rows and the two chest rows (the duplicate key is not added)");
        using Key = std::pair<uint32_t, uint8_t>;
        const bool order = src_test::seen.count == 4 && key(src_test::seen.rows, 0) == Key(0x5000, 2) &&
                           key(src_test::seen.rows, 1) == Key(0x5000, 3) && key(src_test::seen.rows, 2) == Key(0x9001, 1) &&
                           key(src_test::seen.rows, 3) == Key(0x9001, 4);
        CHECK(order, "sources: the player's rows first, in the game's order, then the chest rows");
        uint32_t item = 0, count = 0, pide = 0, level = 0;
        std::memcpy(&item, src_test::seen.rows.data() + 2 * 0x48 + 8, 4);
        std::memcpy(&count, src_test::seen.rows.data() + 2 * 0x48 + 0x10, 4);
        std::memcpy(&pide, src_test::seen.rows.data() + 2 * 0x48 + 0x14, 4);
        std::memcpy(&level, src_test::seen.rows.data() + 2 * 0x48 + 0x24, 4);
        CHECK(item == 112 && count == 1 && pide == 1201 && level == 3 && src_test::seen.rows[2 * 0x48 + 0x2c] == 2,
              "sources: a chest row is the view the game would hold: item, count 1, item entity, level, rarity");
        uint64_t game_count = 0;
        std::memcpy(&game_count, reinterpret_cast<const uint8_t*>(f.header) + 8, 8);
        CHECK(game_count == 2 && f.header[0] == addr(f.game.data()), "sources: the game's own array and header are untouched");
        f.draw(1);
        CHECK(src_test::seen.header == f.header, "sources: the WEAPONS grid is never touched");

        // the selected-row resolve
        KilnRegisters r = f.resolve_with(0x9001, 4, 0);
        uint32_t got = 0, got_pide = 0;
        std::memcpy(&got, f.frame + 0x40 - 0x18, 4);
        std::memcpy(&got_pide, f.frame + 0x40 - 0x20 + 0x14, 4);
        CHECK(got == 113 && got_pide == 1202 && r.rcx == 113, "sources: a chest key the model does not hold resolves to its view; rcx is its item");
        r = f.resolve_with(0x5000, 2, 110);
        std::memcpy(&got, f.frame + 0x40 - 0x18, 4);
        CHECK(got == 110 && r.rcx == 110 && f.frame[0x20] == 0xEE, "sources: a key the player's model resolved is left alone");
        r = f.resolve_with(0x9002, 1, 0);
        std::memcpy(&got, f.frame + 0x40 - 0x18, 4);
        CHECK(got == 0 && r.rcx == 0, "sources: an unknown key stays unresolved (as the game)");

        // the price tests: group 1 pays from containers that hold 40 of the material; the player's 3 do not count
        CHECK(f.price("ui.values/GemForgeAffordUpgrade", 0x9001, 1, 0x2b0cf1d5, 3) == 3,
              "sources: before the server said what the payers hold, the price test is the game's");
        KilnUiGemForgeSourcesState st{};
        st.size = sizeof st;
        CHECK(T()->gemforge_sources_state(&st) == KILN_OK && st.installed && st.rows == 3 && st.wanted_count == 1 &&
                  st.wanted[0] == 0x2b0cf1d5, "sources: the material the price test asked about is wanted");
        CHECK(f.set({SourcesFixture::row(0x9001, 1, 112, 1201, 3, 1), SourcesFixture::row(0x9001, 4, 113, 1202, 7)},
                    {payer(1, 0x2b0cf1d5, 40)}) == KILN_OK, "sources: the server's payer counts arrive");
        CHECK(f.price("ui.values/GemForgeAffordUpgrade", 0x9001, 1, 0x2b0cf1d5, 3) == 40 &&
                  f.price("ui.values/GemForgeAffordSlot", 0x9001, 1, 0x2b0cf1d5, 0) == 40,
              "sources: a chest row of a payer group: have = what the group holds (both price tests)");
        CHECK(f.price("ui.values/GemForgeAffordSlot", 0x9001, 4, 0x2b0cf1d5, 3) == 3, "sources: a row the player pays for: the game's test");
        CHECK(f.price("ui.values/GemForgeAffordSlot", 0x5000, 2, 0x2b0cf1d5, 3) == 3, "sources: the player's own gem: the game's test");
        CHECK(f.set({SourcesFixture::row(0x9001, 1, 112, 1201, 3, 1)}, {payer(1, 0x2b0cf1d5, 40, true)}) == KILN_OK &&
                  f.price("ui.values/GemForgeAffordUpgrade", 0x9001, 1, 0x2b0cf1d5, 3) == 43,
              "sources: player first: the player's own count is added");
        st = {};
        st.size = sizeof st;
        T()->gemforge_sources_state(&st);
        CHECK(st.wanted_count == 0 && st.injected >= 1 && st.resolved >= 1 && st.priced >= 1, "sources: the state counts what the hooks did");

        // lifetime: expiry, another client world, the owner
        fake::ticks = 30000;
        f.draw();
        CHECK(src_test::seen.header == f.header, "sources: expired rows are gone: the game's array again");
        r = f.resolve_with(0x9001, 1, 0);
        CHECK(r.rcx == 0, "sources: an expired row does not resolve");
        fake::ticks = 10000;
        CHECK(f.set({SourcesFixture::row(0x9001, 1, 112, 1201, 3)}) == KILN_OK, "sources: new rows");
        fake::client.session = 4;
        f.draw();
        CHECK(src_test::seen.header == f.header, "sources: another client world drops the rows");
        fake::client.session = 3;
        CHECK(f.set({SourcesFixture::row(0x9001, 1, 112, 1201, 3)}) == KILN_OK, "sources: rows of this world again");
        fake::revoke(f.owner);
        f.draw();
        CHECK(src_test::seen.header == f.header, "sources: a retired owner's rows go with it");
    }
}
