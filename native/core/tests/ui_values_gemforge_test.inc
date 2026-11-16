// SPDX-License-Identifier: MIT
// Included by ui_values_test.cpp: the same guarded-memory, owner and hook fakes.
using GemGrid = void (*)(void*, void*, void*, void*, float, uint8_t);
struct GemHeader { uintptr_t data; uint64_t count, capacity; uintptr_t grow; };
struct GemFixture;
GemFixture* gem_fixture = nullptr;
struct GemFixture {
    std::vector<uint8_t> play = std::vector<uint8_t>(0x3100), models = std::vector<uint8_t>(0x8f70 * 2);
    std::vector<uint8_t> rows = std::vector<uint8_t>(0x48 * 2), sockets = std::vector<uint8_t>(0x48);
    uint8_t selector[0x60]{};
    GemHeader header{};
    GemGrid grid = nullptr;
    KilnMidFn end = nullptr;
    FrameHookFn frame = nullptr;
    KilnOwner owner = 0;
    bool draw = true, change = false, combined = false, change_weapons = false, read_during = false;
    bool change_after_grids = false, omit_end = false;
    KilnUiGemForgeFrame during{};
    KilnResult during_result = KILN_ERR_INTERNAL;
    uint8_t role = 0;
    int original_calls = 0;
    GemFixture() {
        prime_slots();
        gem_fixture = this;
        fake::rows.insert("ui.gemforge.inspect");
        set_installer(kListGemForge, &kiln::uivalues::install_gemforge);
        fake::symbols["ui/GemForgeGrid"] = 0x140caefc0ull;
        for (const char* n : {"ui/GemForgeMenu", "ui/GemForgeLists", "ui/GemForgeCandidates", "ui/GemForgeSourceRet",
             "ui/GemForgeSlotRet", "ui/GemForgeScreenCall", "ui/GemForgeListsCall", "ui/GemForgeGemsCall",
             "ui/GemForgeWeaponsCall", "ui/GemForgeSwitch", "ui/GemForgeLowerBoundary",
             "ui/GemForgeEnd", "ui/PlayStateUi"}) fake::symbols[n] = 0x140000001ull;
        const std::map<std::string, int64_t> fields = {
            {"ui/state.ui_state", 0xc}, {"ui/play.hud_screen", 0x1cd8}, {"ui/play.localEntity", 0x18},
            {"ui/gemforge.mode", 0}, {"ui/gemforge.selected", 0x10}, {"ui/gemforge.weapon", 0x18},
            {"ui/gemforge.sockets", 0x6b40}, {"ui/gemforge.socketCount", 0x6b48},
            {"ui/gemforge.arrayData", 0}, {"ui/gemforge.arrayCount", 8}, {"ui/gemforge.arrayCapacity", 16}};
        for (auto& [n, v] : fields) fake::constants[n] = v;
        put(g_mem.state, 0xc, uint8_t{7});
        put(g_mem.state, 0x38, addr(play.data()));
        put(play.data(), 0x18, uint32_t{77});
        put(play.data(), 0x1cd8, uint8_t{45});
        put(play.data(), 0x3060, addr(models.data()));
        put(play.data(), 0x3068, uint64_t{2});
        put(models.data(), 0, uint32_t{99}); // other owners never supply socket views
        put(models.data(), 0x8f70, uint32_t{77});
        put(models.data(), 0x8f70 + 0x6b40, addr(sockets.data()));
        put(models.data(), 0x8f70 + 0x6b48, uint64_t{1});
        fill_view_bytes(rows.data(), 0x5000, 2, 220, 1, 700, 0, 0);
        fill_view_bytes(rows.data() + 0x48, 0x5001, 3, 110, 5, 901, 0, 0);
        fill_view_bytes(sockets.data(), 700, 0, 111, 1, 800, 0, 0);
        put(selector, 0x10, uint32_t{0x5000}); selector[0x14] = 2;
        std::memcpy(selector + 0x18, rows.data(), 0x48);
        header = {addr(rows.data()), 2, 2, 0};
        fake::screen.hud_screen = 45; fake::screen.available = 1;
        env().gemforge_abi = +[] { return true; };
        env().item_category = +[](uint32_t item, uint8_t* cat) {
            *cat = item == 220 ? 2 : 15; return item == 220 || item == 110 || item == 111;
        };
        env().call_gemforge = +[](KilnUiGemForgeFn fn, const KilnUiGemForgeFrame* f,
            const KilnUiGemForgeCandidate* r, uint32_t n, void* user) {
            try { fn(f, r, n, user); return true; } catch (...) { return false; }
        };
        owner = fake::owner("mod:gemforge");
    }
    void arm() {
        for (const auto& r : fake::raws) {
            if (r.label == "ui.values/GemForgeGrid") {
                grid = reinterpret_cast<GemGrid>(r.fn);
                *r.original = reinterpret_cast<void*>(+[](void*, void*, void*, void*, float, uint8_t weapons) {
                    ++gem_fixture->original_calls;
                    if (gem_fixture->change) ++gem_fixture->selector[0];
                    if (weapons && gem_fixture->change_weapons) {
                        put(gem_fixture->selector, 0x10, uint32_t{0x5001});
                        gem_fixture->selector[0x14] = 3;
                    }
                });
            }
            if (r.label == "ui.values/GemForgeFrame") {
                frame = reinterpret_cast<FrameHookFn>(r.fn);
                *r.original = reinterpret_cast<void*>(+[](void*, void*) {
                    auto& f = *gem_fixture;
                    if (f.draw) f.grid(f.selector, &f.header, nullptr, nullptr, 12.5f, f.role);
                    if (f.draw && f.combined) f.grid(f.selector, &f.header, nullptr, nullptr, 12.5f, 1);
                    if (f.change_after_grids) ++f.selector[0];
                    if (f.end && !f.omit_end) {
                        KilnRegisters regs{}; regs.rdi = addr(f.selector);
                        f.end(&regs, nullptr);
                    }
                    ++fake::screen.frame;
                    if (f.read_during) f.during_result = f.read(f.during);
                });
            }
        }
        for (const auto& r : fake::mids)
            if (r.label == "ui.values/GemForgeEnd") end = r.fn;
    }
    KilnResult read(KilnUiGemForgeFrame& f, KilnUiGemForgeCandidate* r = nullptr, uint32_t cap = 0,
                    uint64_t session = 0, uint64_t frame_id = 0) {
        f = {}; f.size = sizeof f;
        uint32_t required = 0;
        if (cap) r[0].size = sizeof(*r);
        return T()->copy_gemforge_candidates(owner, session, frame_id, &f, r, cap, &required);
    }
};
void gemforge_tests() {
    static_assert(offsetof(KilnUiValuesApi, copy_gemforge_candidates) == 88);
    static_assert(sizeof(KilnUiGemForgeCandidate) == 376);
    static_assert(sizeof(KilnUiGemForgeFrame) == 200);
    {
        GemFixture f; KilnUiGemForgeFrame m{}; KilnUiGemForgeCandidate rows[2]{};
        CHECK(f.read(m) == KILN_OK && !m.ok, "gemforge: first read installs and returns no frame");
        f.arm(); CHECK(f.grid && f.frame, "gemforge: installs grid and frame wrappers");
        if (!f.grid || !f.frame) return;
        f.frame(nullptr, nullptr);
        CHECK(f.read(m) == KILN_ERR_BUFFER_TOO_SMALL && m.count == 2, "gemforge: bounded sizing query");
        const auto first = m;
        f.frame(nullptr, nullptr);
        CHECK(f.read(m, rows, 2, first.session, first.frame) == KILN_OK && m.ok && m.frame == first.frame,
              "gemforge: retained frame retry keeps metadata and records together");
        CHECK(rows[0].source_row.item == 220 && rows[0].displayed_gem.item == 111 && rows[0].has_weapon_carrier &&
              rows[0].weapon_carrier.client_entity == 700 && rows[0].displayed_gem.inventory_page == 700 &&
              rows[0].displayed_gem.slot_index == 0 && rows[0].displayed_gem.units_per_charge == 1 && rows[1].displayed_gem.item == 110 && !rows[1].has_weapon_carrier,
              "gemforge: carrier, row and actual gem source stay distinct");
        CHECK(m.has_selected_row && m.selected_page == 0x5000 && m.selected_index == 2 && m.has_selected_weapon &&
              m.selected_weapon.item == 220 && m.generation && m.session == 5 && f.original_calls == 2 && !fake::writes,
              "gemforge: captures selection and calls original once without writes");
        f.frame(nullptr, nullptr);
        CHECK(f.read(m, rows, 2, first.session, first.frame) == KILN_OK && !m.ok,
              "gemforge: a third frame ages out a pinned copy");
        f.read(m, rows, 2); const auto generation = m.generation;
        f.change = true; f.frame(nullptr, nullptr);
        CHECK(f.read(m, rows, 2) == KILN_OK && !m.ok, "gemforge: selection or mode changes during draw invalidate");
        f.change = false; f.frame(nullptr, nullptr);
        CHECK(f.read(m, rows, 2) == KILN_OK && m.ok && m.generation > generation,
              "gemforge: next coherent frame has a new generation");
        f.draw = false; f.frame(nullptr, nullptr);
        CHECK(f.read(m, rows, 2) == KILN_OK && !m.ok, "gemforge: absent draw clears latest immediately");
        f.draw = true; f.role = 1; f.frame(nullptr, nullptr);
        CHECK(f.read(m, rows, 2) == KILN_OK && !m.ok, "gemforge: weapons role never supplies GEMS");
        f.role = 0; f.frame(nullptr, nullptr); fake::screen.hud_screen = 0;
        CHECK(f.read(m, rows, 2) == KILN_OK && !m.ok, "gemforge: closed screen invalidates even before another draw");
        fake::screen.hud_screen = 45; f.frame(nullptr, nullptr); ++fake::client.session;
        CHECK(f.read(m, rows, 2) == KILN_OK && !m.ok, "gemforge: world change invalidates retained frames");
    }
    for (int bad = 0; bad < 7; ++bad) {
        GemFixture f; KilnUiGemForgeFrame m{}; f.read(m); f.arm();
        if (bad == 0) f.header.count = 4097;
        if (bad == 1) f.header.capacity = 1;
        if (bad == 2) f.header.data = UINTPTR_MAX - 10;
        if (bad == 3) fake::unreadable.insert(addr(f.rows.data()));
        if (bad == 4) put(f.models.data(), 0x8f70 + 0x6b48, uint64_t{4097});
        if (bad == 5) put(f.sockets.data(), 0, uint32_t{701});
        if (bad == 6) f.selector[0] = 5;
        f.frame(nullptr, nullptr);
        CHECK(f.read(m) == KILN_ERR_UNAVAILABLE && fake::rows_off == std::vector<std::string>{"ui.gemforge.inspect"} &&
              f.original_calls == 1 && !fake::writes, "gemforge: invariant %d disables only this feature", bad);
    }
    for (int gate = 0; gate < 5; ++gate) {
        GemFixture f; KilnUiGemForgeFrame m{};
        if (gate == 0) fake::layout_supported = false;
        if (gate == 1) fake::symbols.erase("ui/GemForgeSourceRet");
        if (gate == 2) fake::constants["ui/gemforge.weapon"] = 0x20;
        if (gate == 3) env().gemforge_abi = +[] { return false; };
        if (gate == 4) fake::rows.erase("ui.screens");
        CHECK(f.read(m) == KILN_ERR_UNAVAILABLE && fake::raws.empty(),
              "gemforge: unsupported layout/signature/ABI/dependency %d installs nothing", gate);
    }
    {
        GemFixture f; KilnUiGemForgeFrame m{}; KilnUiGemForgeCandidate rows[2]{};
        f.read(m); f.arm(); f.frame(nullptr, nullptr); f.read(m, rows, 2);
        const auto old = m;
        fake::rows.erase("ui.gemforge.inspect");
        CHECK(f.read(m, rows, 2) == KILN_ERR_UNAVAILABLE, "gemforge: capability loss refuses reads");
        fake::rows.insert("ui.gemforge.inspect");
        CHECK(f.read(m, rows, 2, old.session, old.frame) == KILN_OK && !m.ok,
              "gemforge: capability restoration cannot resurrect a pre-loss copy");
        f.frame(nullptr, nullptr); f.read(m, rows, 2);
        CHECK(m.ok && m.generation > old.generation, "gemforge: resumed draw advances the generation");
        fake::ticks += 1001;
        CHECK(f.read(m, rows, 2) == KILN_OK && !m.ok, "gemforge: native reads enforce the age limit");
    }
    {
        GemFixture f; KilnUiGemForgeFrame m{}; KilnUiGemForgeCandidate rows[2]{};
        f.read(m); f.arm(); f.frame(nullptr, nullptr); f.read(m, rows, 2);
        const auto old = m;
        fake::ticks += 1001; f.frame(nullptr, nullptr);
        CHECK(f.read(m, rows, 2, old.session, old.frame) == KILN_OK && !m.ok,
              "gemforge: a pinned previous frame cannot exceed the age limit");
        CHECK(f.read(m, rows, 2) == KILN_OK && m.ok,
              "gemforge: an expired retry leaves the fresh latest frame readable");
    }
    for (uint32_t n : {0u, 4096u}) {
        GemFixture f; KilnUiGemForgeFrame m{}; f.read(m); f.arm();
        f.rows.resize(static_cast<size_t>(n) * 0x48);
        for (uint32_t i = 0; i < n; ++i) fill_view_bytes(f.rows.data() + i * 0x48, 0x5000, 1, 110, 1, 0, 0, 0);
        f.header = {addr(f.rows.data()), n, n, 0};
        f.frame(nullptr, nullptr);
        std::vector<KilnUiGemForgeCandidate> rows(n + 1);
        rows.back().index = 0xdead;
        CHECK(f.read(m, rows.data(), n) == KILN_OK && m.ok && m.count == n && rows.back().index == 0xdead,
              "gemforge: copies all %u rows, including empty and maximum lists, within output bounds", n);
        if (n) CHECK(rows[n - 1].index == n - 1, "gemforge: duplicate rows preserve source order");
    }
    {
        GemFixture f; KilnUiGemForgeFrame m{}; KilnUiGemForgeCandidate rows[2]{};
        f.read(m); f.arm(); f.frame(nullptr, nullptr); f.read(m, rows, 2);
        const auto old = m;
        // kiln.ui and slot subscribers run inside the frame chain, after screen.frame advances.
        f.read_during = true; f.frame(nullptr, nullptr);
        CHECK(f.during_result == KILN_OK && !f.during.ok && f.during.reason == KUI_STALE_FRAME,
              "gemforge: a read inside the publishing frame is unavailable");
        CHECK(f.read(m, rows, 2) == KILN_OK && m.ok && m.generation == old.generation,
              "gemforge: an in-progress read does not change a coherent generation");
        CHECK(f.read(m, rows, 2, old.session, old.frame) == KILN_OK && m.ok && m.frame == old.frame,
              "gemforge: an in-progress read preserves the previous sizing retry");
    }
    {
        GemFixture f; KilnUiGemForgeFrame m{}; KilnUiGemForgeCandidate rows[2]{};
        f.read(m); f.arm(); f.combined = true;
        f.frame(nullptr, nullptr); f.read(m, rows, 2);
        const auto old = m;
        f.change_weapons = true; f.frame(nullptr, nullptr);
        CHECK(f.original_calls == 4 && f.read(m, rows, 2) == KILN_OK && !m.ok,
              "gemforge: selection in the later WEAPONS grid invalidates the GEMS capture");
        CHECK(f.read(m, rows, 2, old.session, old.frame) == KILN_OK && !m.ok,
              "gemforge: a later weapon selection also invalidates pinned GEMS retries");
        f.change_weapons = false; f.frame(nullptr, nullptr);
        CHECK(f.read(m, rows, 2) == KILN_OK && m.ok && m.selected_page == 0x5001 && m.selected_index == 3 &&
              m.generation > old.generation,
              "gemforge: the next combined frame publishes the changed selection");
    }
    {
        GemFixture f; KilnUiGemForgeFrame m{}; KilnUiGemForgeCandidate rows[2]{};
        f.read(m); f.arm(); f.frame(nullptr, nullptr); f.read(m, rows, 2);
        const auto old = m;
        f.change_after_grids = true; f.frame(nullptr, nullptr);
        CHECK(f.read(m, rows, 2) == KILN_OK && !m.ok,
              "gemforge: a menu action after both grids invalidates the capture");
        CHECK(f.read(m, rows, 2, old.session, old.frame) == KILN_OK && !m.ok,
              "gemforge: a menu action also invalidates pinned retries");
        f.change_after_grids = false; f.frame(nullptr, nullptr);
        CHECK(f.read(m, rows, 2) == KILN_OK && m.ok && m.mode == 1,
              "gemforge: the next menu frame has the updated mode");
        f.omit_end = true; f.frame(nullptr, nullptr);
        CHECK(f.read(m, rows, 2) == KILN_OK && !m.ok,
              "gemforge: a capture without a completed menu check is refused");
    }
    {
        kiln::uivalues::GemReader rd;
        CHECK(!rd.charge(kiln::uivalues::kGemBytes + 1), "gemforge: byte budget refuses excess work");
        CHECK(!kiln::uivalues::GemReader::range(UINTPTR_MAX - 1, 1, 72) &&
              !kiln::uivalues::GemReader::range(1, UINT64_MAX, 72), "gemforge: address and multiplication overflow refused");
    }
    {
        GemFixture f;
        struct Seen { int calls = 0; uint32_t ok = 0, count = 0; } seen;
        auto reg = reg_record();
        const auto callback = +[](const KilnUiGemForgeFrame* m, const KilnUiGemForgeCandidate*, uint32_t count, void* user) {
            auto& s = *static_cast<Seen*>(user); ++s.calls; s.ok = m->ok; s.count = count;
        };
        CHECK(T()->subscribe_gemforge_candidates(f.owner, callback, &seen, &reg) == KILN_OK && reg.ok,
              "gemforge: subscription installs through the owner registry");
        f.arm(); f.frame(nullptr, nullptr);
        CHECK(seen.calls == 1 && seen.ok && seen.count == 2, "gemforge: subscribers receive the completed copy");
        f.draw = false; f.frame(nullptr, nullptr);
        CHECK(seen.calls == 2 && !seen.ok && !seen.count, "gemforge: subscribers receive absent-frame invalidation");
        fake::revoke(f.owner); f.frame(nullptr, nullptr);
        CHECK(seen.calls == 2 && !any(kListGemForge), "gemforge: owner retirement removes native callbacks");
        KilnUiGemForgeFrame m{};
        CHECK(f.read(m) == KILN_ERR_REVOKED, "gemforge: a retired owner cannot read retained copies");
    }
}
