// SPDX-License-Identifier: MIT
// An independent copy of the record released with mod API 0.2.13, not today's expanded native type.
struct ReleasedVisibleSlot013 {
    uint32_t size, available, occurrence, inventory_page, slot_index;
    uint32_t item, count, client_entity, server_entity, view_flags;
    KilnUiRect outer_ui, inner_ui;
    uint8_t native_state[16];
    uint32_t holder, holder_flags;
    uint64_t holder_serial;
    uint32_t page_ordinal, reserved2;
};
static_assert(sizeof(ReleasedVisibleSlot013) == 112);
static_assert(offsetof(ReleasedVisibleSlot013, item) == 20);
uint32_t legacy_second_item = 0, legacy_second_size = 0, legacy_second_available = 0;
void KILN_CALL legacy_slots_callback(const KilnUiSlotsFrame*, const KilnUiVisibleSlot* records, uint32_t count, void*) {
    if (count < 2) return;
    ReleasedVisibleSlot013 slots[2];
    std::memcpy(slots, records, sizeof slots);
    legacy_second_item = slots[1].item;
    legacy_second_size = slots[1].size;
    legacy_second_available = slots[1].available;
}
uint32_t expanded_level = 0, expanded_size = 0, expanded_available = 0, expanded_rarity = 0;
void KILN_CALL expanded_slots_callback(const KilnUiSlotsFrame*, const KilnUiVisibleSlotV2* slots, uint32_t count, void*) {
    if (count >= 2 && slots[1].item == 0xBB) {
        expanded_level = slots[1].level;
        expanded_rarity = slots[1].rarity;
        expanded_size = slots[1].size;
        expanded_available = slots[1].available;
    }
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
    put(g_frame_script[1]->view_bytes, 0x2c, uint8_t{0}); // this fixture tests Common, not the shared default Rare
    ui_frame(1001);
    CHECK(expanded_level == 7, "expanded slots: correct second row and level at negotiated stride");
    CHECK(expanded_size == 120 && expanded_rarity == 0 && (expanded_available & KUI_ITEM_VALUES),
          "expanded slots: 120-byte record carries common rarity and the item-values flag");
    CHECK(legacy_second_item == 0xBB, "legacy slots: second item at released 112-byte callback stride (got %x)", legacy_second_item);
    CHECK(legacy_second_size == 112 && !(legacy_second_available & KUI_ITEM_VALUES),
          "legacy slots: the released size and flags do not advertise an unreadable expanded tail");
}
