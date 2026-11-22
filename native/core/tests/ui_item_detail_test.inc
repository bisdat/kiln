// SPDX-License-Identifier: MIT
// #120: completed, bounded snapshots only; exact instance/variant identity and session isolation.
using CaptureTextFn = void* (*)(void*, const text::GameText*, const void*, uint8_t);
void* capture_text_next(void* out, const text::GameText*, const void*, uint8_t) { return out; }
void capture_emit(const char* str) {
    text::GameText t{str, std::strlen(str), 0};
    for (const auto& raw : fake::raws)
        if (raw.label == "ui.item_detail/NativeTextLine") reinterpret_cast<CaptureTextFn>(raw.fn)(nullptr, &t, nullptr, 1);
}
void capture_stat_next(const void*, float) { capture_emit("Damage"); capture_emit("42.5"); }
void capture_nested_detail(void*, uint8_t) { capture_emit("Nested name"); }
void capture_nested_stat(const void*, float) {
    g_detail_hook(g_tm->record, 0);
    capture_emit("Outer damage");
}
void capture_game_detail(void*, uint8_t) {
    capture_emit("Weapon name");
    KilnRegisters r{};
    for (const auto& mid : fake::mids) if (mid.label == "ui.tooltip/DetailStatsBlockCall") mid.fn(&r, nullptr);
    for (const auto& raw : fake::raws) if (raw.label == "ui.tooltip/StatValueDraw")
        reinterpret_cast<void (*)(const void*, float)>(raw.fn)(nullptr, 42.5f);
    for (const auto& mid : fake::mids) if (mid.label == "ui.tooltip/DetailStatsBlockEnd") mid.fn(&r, nullptr);
    capture_emit("Description / Ü");
    auto* current = current_detail();
    current->suppress_capture = true; capture_emit("Kiln injected line"); current->suppress_capture = false;
}

void item_detail_tests() {
    prime_tooltip();
    item_detail_reset_for_tests();
    fake::rows.insert("ui.item_detail");
    fake::client.session = 7;
    auto owner = fake::owner("mod:item-detail");
    KilnUiItemDetailQuery q{sizeof q, 900, 0xAA, 4, 2};
    KilnUiItemDetailSnapshot s{}; s.size = sizeof s;
    std::vector<KilnUiItemDetailLine> lines(KILN_UI_ITEM_DETAIL_MAX_LINES);
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok,
          "item detail: first read installs capture and reports no cached panel");
    CHECK(arm_tooltip(), "item detail: shares the existing ItemDetail hook");
    DetailCopy c; c.ok = true;
    fill_view_bytes(c.raw, 0x5000, 2, 0xAA, 1, 900, 0, 0);
    put(c.raw, 0x24, uint32_t{4}); put(c.raw, 0x2c, uint8_t{2});
    item_detail_begin(c);
    item_detail_text(c, "Damage", 6, true, 42.5f);
    item_detail_text(c, "Long description / Ü", std::strlen("Long description / Ü"), false, 0);
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok,
          "item detail: in-progress occurrence is not visible");
    item_detail_end(c);
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), 1) == KILN_ERR_BUFFER_TOO_SMALL && s.count == 2,
          "item detail: small destination reports required count without partial rows");
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && s.ok && s.count == 2 &&
              std::string(lines[0].text, lines[0].text_bytes) == "Damage" && lines[0].value == 42.5f &&
              (lines[0].flags & KILN_UI_ITEM_DETAIL_VALUE),
          "item detail: copies owned UTF-8 and the computed value");
    q.level = 5;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok,
          "item detail: upgraded variant cannot use old numbers");
    q.level = 4; q.entity = 901;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok,
          "item detail: another entity cannot borrow same-type instance stats");
    q.entity = 0;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && s.ok,
          "item detail: item+level+rarity lookup is supported");
    fake::client.session = 8;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok,
          "item detail: cache is invalidated on a world change");
    item_detail_begin(c);
    std::string huge(4097, 'x'); item_detail_text(c, huge.data(), huge.size(), false, 0); item_detail_end(c);
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok,
          "item detail: budget overflow refuses whole panel, never truncates");
    for (auto& raw : fake::raws) {
        if (raw.label == "ui.item_detail/NativeTextLine") *raw.original = reinterpret_cast<void*>(&capture_text_next);
        if (raw.label == "ui.tooltip/StatValueDraw") *raw.original = reinterpret_cast<void*>(&capture_stat_next);
        if (raw.label == "ui.values/ItemDetail") *raw.original = reinterpret_cast<void*>(&capture_game_detail);
    }
    std::memcpy(g_tm->record, c.raw, sizeof c.raw);
    g_detail_hook(g_tm->record, 0);
    q.entity = 900;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && s.ok && s.count == 4 &&
          std::string(lines[0].text, lines[0].text_bytes) == "Weapon name" && lines[1].value == 42.5f &&
          lines[1].flags == KILN_UI_ITEM_DETAIL_VALUE && !lines[2].flags &&
          std::string(lines[3].text, lines[3].text_bytes) == "Description / Ü",
          "item detail hooks: TLS-scoped text in draw order, computed stat metadata once, injected text excluded");
    capture_emit("not in any detail");
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && s.count == 4,
          "item detail hooks: text outside ItemDetail cannot contaminate the cache");
    CHECK(std::count_if(fake::raws.begin(), fake::raws.end(), [](const auto& raw) {
              return raw.label == "ui.tooltip/StatValueDraw"; }) == 1,
          "item detail hooks: only one shared stat detour");
    CHECK(item_detail_copy(0, &q, &s, lines.data(), lines.size()) == KILN_ERR_INVALID_ARGUMENT,
          "item detail: rejects owner zero");
    q.size = 4;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_ERR_INVALID_ARGUMENT,
          "item detail: rejects short query"); q.size = sizeof q;
    for (uint32_t id = 1000; id < 1065; ++id) {
        put(c.raw, 0x14, id); item_detail_begin(c); item_detail_text(c, "row", 3, false, 0); item_detail_end(c);
    }
    q.entity = 1000;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok,
          "item detail: evicts the oldest instance beyond 64 captures");
    q.entity = 1064;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && s.ok,
          "item detail: keeps the most recent instance within its bound");
    fake::client.local_player = 0; // kiln.client retains its session counter on the title screen
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok && s.reserved == 1,
          "item detail: leaving the world refuses stale captures even if the session counter remains");
    fake::client.local_player = 42;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok,
          "item detail: observing no local player clears the retained cache");
    q.entity = 900;
    for (auto& raw : fake::raws) if (raw.label == "ui.values/ItemDetail")
        *raw.original = reinterpret_cast<void*>(&capture_nested_detail);
    DetailCopy* outer = current_detail();
    item_detail_begin(c);
    t_detail = &c;
    item_detail_stat(c, 77, &capture_nested_stat, nullptr);
    t_detail = outer;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && s.ok &&
              lines[0].flags == 0,
          "item detail: a nested panel cannot inherit its caller's stat metadata");
    item_detail_end(c);
    q.entity = 1064;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && s.ok &&
              lines[0].flags == KILN_UI_ITEM_DETAIL_VALUE && lines[0].value == 77,
          "item detail: nested capture preserves the outer stat metadata");
    item_detail_begin(c);
    for (unsigned i = 0; i < 65; ++i) item_detail_text(c, "row", 3, false, 0);
    item_detail_end(c);
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok && s.reserved == 2,
          "item detail: a rejected replacement is a bounded refusal, not stale data or an unexplained miss");
    item_detail_begin(c);
    std::string row4096(4096, 'x');
    for (unsigned i = 0; i < 8; ++i) item_detail_text(c, row4096.data(), row4096.size(), false, 0);
    item_detail_end(c);
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && s.ok && s.count == 8,
          "item detail: accepts exact 4096-byte rows and 32768-byte panel bound");
    item_detail_begin(c);
    for (unsigned i = 0; i < 8; ++i) item_detail_text(c, row4096.data(), row4096.size(), false, 0);
    item_detail_text(c, "x", 1, false, 0); item_detail_end(c);
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok && s.reserved == 2,
          "item detail: total UTF-8 budget refuses the whole replacement");
    item_detail_begin(c);
    item_detail_text(c, "\xff", 1, false, 0); item_detail_end(c);
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok && s.reserved == 2,
          "item detail: invalid UTF-8 refuses the whole replacement");
    fake::have_client = false;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok && s.reserved == 1,
          "item detail: unavailable client state explains the offline miss");
    fake::have_client = true;
    fake::client_side = false;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_ERR_UNAVAILABLE,
          "item detail: a server refuses the client-only service");
    fake::client_side = true;
    fake::revoke(owner);
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_ERR_REVOKED,
          "item detail: a retired owner cannot read cached panels");
    owner = fake::owner("mod:item-detail-new");
    std::atomic<bool> coherent{true};
    std::thread reader([&] {
        KilnUiItemDetailSnapshot copy{}; copy.size = sizeof copy;
        std::vector<KilnUiItemDetailLine> rows(64);
        for (unsigned i = 0; i < 100; ++i) {
            if (item_detail_copy(owner, &q, &copy, rows.data(), rows.size()) != KILN_OK ||
                (copy.ok && (copy.count != 1 || rows[0].text_bytes != 4 ||
                    std::memcmp(rows[0].text, "data", 4) != 0 || rows[0].value != 42))) coherent = false;
        }
    });
    for (unsigned i = 0; i < 100; ++i) {
        item_detail_begin(c); item_detail_text(c, "data", 4, true, 42); item_detail_end(c);
    }
    reader.join();
    CHECK(coherent.load(), "item detail: worker reads see complete immutable copies during UI-thread publication");
    item_detail_begin(c); item_detail_text(c, "data", 4, true, 42);
    fake::client.local_player = 0;
    item_detail_end(c); // world exit was observed inside a capture, with no intervening mod read
    fake::client.local_player = 42;
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_OK && !s.ok,
          "item detail: world exit observed at publication clears older captures too");
    fake::rows.erase("ui.item_detail");
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_ERR_UNAVAILABLE,
          "item detail: capability off refuses capture");
    item_detail_begin(c);
    CHECK(!c.capture, "item detail: disabling capture stops new panel allocations");
    fake::rows.insert("ui.item_detail");
    item_detail_reset_for_tests();
    fake::symbols.erase("ui/NativeTextLine");
    CHECK(item_detail_copy(owner, &q, &s, lines.data(), lines.size()) == KILN_ERR_UNAVAILABLE &&
              std::find(fake::rows_off.begin(), fake::rows_off.end(), "ui.item_detail") != fake::rows_off.end(),
          "item detail: missing capture symbol refuses and reports the capability off");
}
