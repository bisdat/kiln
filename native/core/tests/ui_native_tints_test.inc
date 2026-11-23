// SPDX-License-Identifier: MIT
// Native descriptor copy, ordered tint rules and owner lifetime; no game executable runs.
static uint32_t tint_seen, tint_units, tint_player, tint_screen;
static void game_tint_options(void* out, void* descriptor, void*, void*) {
    std::memcpy(static_cast<uint8_t*>(out) + 0x30, static_cast<uint8_t*>(descriptor) + 0x48, 8);
}
static KilnResult KILN_CALL tint_rule(const KilnUiItemView* v, uint32_t* rgba, void* user) {
    if (v->size < sizeof *v) return KILN_ERR_INVALID_ARGUMENT;  // same guard as the managed bridge
    tint_seen = v->item; tint_units = v->units_per_charge; tint_player = v->local_player; tint_screen = v->screen.size;
    *rgba = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
    return KILN_OK;
}
static void native_tints_tests() {
    fake::reset(); prime_bars();
    fake::rows.insert("ui.slots.tint");
    fake::symbols["ui/SlotAppearance"] = 0x140d1a430;
    set_installer(kListTints, &install_tints);
    env().call_tint = [](KilnUiTintFn fn, const KilnUiItemView* v, uint32_t* rgba, void* u, KilnResult* rc) {
        try { *rc = fn(v, rgba, u); return true; } catch (...) { return false; }
    };
    const auto* t = tints_table();
    auto owner = fake::owner("mod:tint");
    auto r = reg_record();
    CHECK(t->subscribe(owner, 0, &tint_rule, reinterpret_cast<void*>(0x12345678), &r) == KILN_OK && r.ok,
          "native tint: registration installs its own capability and hook");
    g_tint_next = &game_tint_options;
    uint8_t view[0x48]{}; fill_view_bytes(view, 0x5000, 2, 0xAA, 4, 900, 0, 0);
    uint8_t descriptor[0x90]{}; put(descriptor, 0, addr(view));
    put(descriptor, 0x48, uint32_t{0xff112233}); put(descriptor, 0x4c, 0.75f);
    uint8_t before[0x90]; std::memcpy(before, descriptor, sizeof before);
    uint8_t output[0x98]{};
    tint_seen = 0; hk_tint_options(output, descriptor, nullptr, nullptr);
    uint32_t packed; std::memcpy(&packed, output + 0x30, 4);
    CHECK(packed == 0x78563412 && tint_seen == 0xAA,
          "native tint: canonical RGBA becomes game ABGR on the native background, with copied item identity");
    CHECK(tint_units == 64 && tint_player == 77 && tint_screen == sizeof(KilnUiScreen),
          "native tint: charge divisor, player and screen metadata follow the normal item-view contract");
    CHECK(std::memcmp(before, descriptor, sizeof before) == 0 && *reinterpret_cast<float*>(output + 0x34) == 0.75f,
          "native tint: no borrowed game memory changes and the game's colour strength survives");
    auto other = fake::owner("mod:later"); auto later = reg_record();
    t->subscribe(other, 0, &tint_rule, reinterpret_cast<void*>(0xAABBCCDD), &later);
    hk_tint_options(output, descriptor, nullptr, nullptr); std::memcpy(&packed, output + 0x30, 4);
    CHECK(packed == 0xDDCCBBAA, "native tint: later rules win deterministically");
    fake::revoke(other); owner_retired(other);
    hk_tint_options(output, descriptor, nullptr, nullptr); std::memcpy(&packed, output + 0x30, 4);
    CHECK(packed == 0x78563412, "native tint: a retiring owner's tint stops before its callback can run");
    t->unsubscribe(r.handle);
    hk_tint_options(output, descriptor, nullptr, nullptr); std::memcpy(&packed, output + 0x30, 4);
    CHECK(packed == 0xff112233, "native tint: removing all rules restores vanilla without a write-back");
    r = reg_record();
    CHECK(t->subscribe(0, 0, &tint_rule, nullptr, &r) == KILN_ERR_INVALID_ARGUMENT && !r.ok,
          "native tint: owner zero is refused");
}
