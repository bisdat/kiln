// SPDX-License-Identifier: MIT
// Callback compiled before dev.30.5 indexes with the released 112-byte stride.
uint32_t legacy_second_item = 0;
void KILN_CALL legacy_slots_callback(const KilnUiSlotsFrame*, const KilnUiVisibleSlot* slots, uint32_t count, void*) {
    if (count >= 2) std::memcpy(&legacy_second_item, reinterpret_cast<const uint8_t*>(slots) + 112 + 20, 4);
}
uint32_t expanded_level = 0;
void KILN_CALL expanded_slots_callback(const KilnUiSlotsFrame*, const KilnUiVisibleSlotV2* slots, uint32_t count, void*) {
    if (count >= 2 && slots[1].item == 0xBB) expanded_level = slots[1].level;
}
void slot_subscription_compat_tests() {
    prime_slots();
    auto owner = fake::owner("mod:legacy-slots"); auto reg = reg_record();
    CHECK(T()->subscribe_slots(owner, &legacy_slots_callback, nullptr, &reg) == KILN_OK && reg.ok,
          "legacy slots: subscription accepted");
    CHECK(arm_slots(), "legacy slots: fixture armed");
    FakeSlot first, second;
    g_frame_script = {desc(&first, 0x5000, 0, 0xAA, 900), desc(&second, 0x6000, 0, 0xBB, 950)};
    auto expanded = reg_record();
    CHECK(slots_table()->subscribe(owner, 121, &expanded_slots_callback, nullptr, &expanded) == KILN_ERR_INVALID_ARGUMENT,
          "expanded slots: unknown record size is refused");
    CHECK(slots_table()->subscribe(owner, 120, &expanded_slots_callback, nullptr, &expanded) == KILN_OK && expanded.ok,
          "expanded slots: 120-byte subscription negotiated");
    put(g_frame_script[1]->view_bytes, 0x24, uint32_t{7});
    ui_frame(1001);
    CHECK(expanded_level == 7, "expanded slots: correct second row and level at negotiated stride");
    CHECK(legacy_second_item == 0xBB, "legacy slots: second item at released 112-byte callback stride (got %x)", legacy_second_item);
}
