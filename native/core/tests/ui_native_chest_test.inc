// SPDX-License-Identifier: MIT
static int chest_original_windows, chest_original_footers, chest_draws, chest_presses;
static bool chest_activate, chest_was_deferred;
static std::string chest_caption;
static KilnUiChestPress chest_last{};
static void fake_chest_footer(void*, uint16_t, void*, uint8_t, uint8_t) {
    ++chest_original_footers;
    KilnRegisters regs{};
    on_chest_row_end(&regs, nullptr);
}
static void fake_chest_window(uint8_t) {
    ++chest_original_windows;
    const int before = chest_presses;
    hk_chest_footer(nullptr, 0, nullptr, 1, 0);
    chest_was_deferred &= chest_presses == before;
}
static KilnResult KILN_CALL on_test_chest(const KilnUiChestPress* p, void*) {
    ++chest_presses; chest_last = *p; return KILN_OK;
}
static void native_chest_tests() {
    prime_bars();
    fake::rows.insert("ui.chest.buttons");
    fake::symbols["ui/ChestWindow"] = 0x140b9d0f0;
    fake::symbols["ui/ChestFooter"] = 0x140c3a440;
    fake::symbols["ui/ActionRow"] = 0x140ce8da0;
    fake::symbols["ui/ActionRowEnd"] = 0x140ce8f79;
    fake::symbols["ui/SystemMenuEntry"] = 0x140cb47f0;
    fake::constants["ui/play.target"] = 0x70;
    fake::constants["ui/state.ui_state"] = 0x10;
    fake::constants["ui/play.hud_screen"] = 0x74;
    put(g_mem.state, 0x10, uint8_t{7}); put(g_mem.play, 0x70, uint32_t{4100}); put(g_mem.play, 0x74, uint8_t{22});
    set_installer(kListChest, &install_chest);
    env().draw_chest = [](uintptr_t, const text::GameText* text, bool* pressed) {
        ++chest_draws; chest_caption.assign(text->p, text->n); *pressed = chest_activate; return true;
    };
    env().call_chest = [](KilnUiChestFn fn, const KilnUiChestPress* p, void* u, KilnResult* rc) {
        try { *rc = fn(p, u); return true; } catch (...) { return false; }
    };
    const auto* t = chest_table(); auto owner = fake::owner("mod:chest"); auto r = reg_record();
    CHECK(t->subscribe(owner, KilnStr{"Salvage", 7}, &on_test_chest, nullptr, &r) == KILN_OK && r.ok,
          "native chest: owns a registration in a separate service table");
    g_chest_window_next = &fake_chest_window; g_chest_footer_next = &fake_chest_footer;
    chest_original_windows = chest_original_footers = chest_draws = chest_presses = 0; chest_activate = true; chest_was_deferred = true;
    hk_chest_window(0);
    CHECK(chest_draws == 1 && chest_caption == "Salvage" && chest_presses == 1 && chest_last.size == sizeof chest_last &&
          chest_last.container == 4100 && chest_last.local_player == 77 && chest_last.session == 3,
          "native chest: the native row draws the caption and an activation carries this chest's current identity");
    CHECK(chest_original_windows == 1 && chest_original_footers == 1 && fake::writes == 0 && chest_was_deferred,
          "native chest: each vanilla continuation runs once and no game action or memory is written");
    hk_chest_window(1); hk_chest_window(2); hk_chest_footer(nullptr, 0, nullptr, 1, 0);
    CHECK(chest_draws == 1 && chest_presses == 1, "native chest: commerce, factory and footers outside a chest stay vanilla");
    put(g_mem.play, 0x70, uint32_t{4200}); chest_activate = false; hk_chest_window(0);
    CHECK(chest_draws == 2 && chest_presses == 1, "native chest: drawing alone never fires a button callback");
    chest_activate = true; hk_chest_window(0);
    CHECK(chest_last.container == 4200, "native chest: a later chest never reuses the previous chest's target");
    put(g_mem.play, 0x70, uint32_t{0}); hk_chest_window(0);
    CHECK(chest_draws == 3, "native chest: an unknown target draws no mod action");
    put(g_mem.play, 0x70, uint32_t{4100}); t->unsubscribe(r.handle); hk_chest_window(0);
    CHECK(chest_draws == 3, "native chest: disposal removes the native row entry");
    auto a = reg_record(); t->subscribe(owner, KilnStr{"A", 1}, &on_test_chest, nullptr, &a);
    auto duplicate = reg_record(); t->subscribe(owner, KilnStr{"A", 1}, &on_test_chest, nullptr, &duplicate);
    CHECK(!duplicate.ok && duplicate.reason == KUI_REJECTED, "native chest: duplicate captions cannot alias a native widget identity");
    auto other_owner = fake::owner("mod:duplicate");
    const auto duplicate_before = text::usage("mod:duplicate").charged;
    duplicate = reg_record(); t->subscribe(other_owner, KilnStr{"A", 1}, &on_test_chest, nullptr, &duplicate);
    CHECK(!duplicate.ok && text::usage("mod:duplicate").charged == duplicate_before,
          "native chest: a duplicate refusal admits no permanent text membership for another mod");
    for (char ch : {'B', 'C', 'D'}) {
        auto b = reg_record(); t->subscribe(owner, KilnStr{&ch, 1}, &on_test_chest, nullptr, &b);
        CHECK(b.ok, "native chest: the row accepts another place up to four");
    }
    auto full = reg_record(); t->subscribe(owner, KilnStr{"E", 1}, &on_test_chest, nullptr, &full);
    CHECK(!full.ok && full.reason == KUI_REGISTRATION_BUDGET, "native chest: the fifth active button is refused, never silently hidden");
    fake::revoke(owner); owner_retired(owner); hk_chest_window(0);
    CHECK(chest_draws == 3, "native chest: owner retirement removes every button");

    auto full_owner = fake::owner("mod:full");
    bool filled = true;
    for (uint32_t i = 0; i < kRegistrationsPerMod; ++i) {
        auto reg = reg_record();
        add(full_owner, KUI_SLOT, 0, reinterpret_cast<void*>(&pass), nullptr, &reg);
        filled &= reg.ok != 0;
    }
    const auto text_before = text::usage("mod:full").charged;
    auto refused = reg_record();
    t->subscribe(full_owner, KilnStr{"No admission", 12}, &on_test_chest, nullptr, &refused);
    CHECK(filled && !refused.ok && refused.reason == KUI_REGISTRATION_BUDGET && text::usage("mod:full").charged == text_before,
          "native chest: a full shared registration budget admits no permanent text or rate token");

}
