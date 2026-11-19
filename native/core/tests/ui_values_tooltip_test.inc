// SPDX-License-Identifier: MIT
// S7 (included by ui_values_test.cpp): native tooltip lines in the game's item detail view (ui.tooltip.lines), the stat
// numbers' scale (ui.tooltip.values) and the tooltip probe, against a fake UI bundle and a scripted ItemDetail that
// reaches H2 (between the stats and the description) and H1 (the scrolled content's end).

struct alignas(16) TooltipMem {
    uint8_t app[0x100];
    uint8_t bundle[0x11000];
    uintptr_t ui_app;
    uint8_t record[0x200];
    uint8_t frame[0x2000];  // ItemDetail's frame (rbp-relative reads of the probe)
    uint32_t mode;
};
TooltipMem* g_tm = new TooltipMem;

struct Drawn {
    std::string text;
    TextDraw d;
};
std::vector<Drawn> g_drawn;
bool g_draw_fault = false;
bool g_key = false;
std::set<std::string> g_open_features;
std::vector<std::string> g_order;  // "H2:<text>" / "H1:<text>" in draw order

bool fake_draw(const TextDraw* d, uintptr_t* node) {
    if (g_draw_fault) return false;
    g_drawn.push_back({std::string(d->text, d->text_len), *d});
    *node = 0x7000 + g_drawn.size();
    return true;
}

constexpr uint64_t kPal[] = {0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull,
                             0x4444444444444444ull, 0x5555555555555555ull, 0x6666666666666666ull, 0x7777777777777777ull};

void prime_tooltip(bool with_h2 = true) {
    prime_lines();
    std::memset(g_tm, 0, sizeof *g_tm);
    g_tm->ui_app = addr(g_tm->app);
    put(g_tm->app, 0xc8, addr(g_tm->bundle));
    for (int i = 0; i < 7; ++i) put(g_tm->bundle, 0xd5ec + 8 * i, kPal[i]);  // other, common..legendary, unique
    put(g_tm->bundle, 0xd648, uint64_t{0xAAAAAAAAAAAAAAAAull});
    put(g_tm->bundle, 0xd658, uint64_t{0xBBBBBBBBBBBBBBBBull});
    put(g_tm->bundle, 0x10b9c, 0.75f);
    put(g_tm->record, 0xbc, uint32_t{0});
    g_tm->mode = 0;
    put(g_tm->record, 0x170, addr(&g_tm->mode));
    put(g_tm->record, 0x2d, uint8_t{3});
    put(g_tm->record, 0x2e, uint8_t{1});
    fake::symbols["ui/UiApp"] = addr(&g_tm->ui_app);
    fake::symbols["ui/DetailContentEnd"] = 0x140c11fa0ull;
    if (with_h2) fake::symbols["ui/DetailAfterStats"] = 0x140c1193eull;
    fake::symbols["ui/DetailStatsBlockCall"] = 0x140c11924ull;
    fake::symbols["ui/StatValueDraw"] = 0x140c2ddd0ull;
    fake::symbols["ui/DetailStatsModel"] = 0x140c110d5ull;
    fake::symbols["ui/DetailHeaderModel"] = 0x140c10f30ull;
    fake::symbols["ui/NativeTextLine"] = 0x140b1da10ull;
    fake::symbols["ui/NativeTextDraw"] = 0x140b25160ull;
    fake::symbols["ui/UiNodeAlign"] = 0x1411ced70ull;
    fake::symbols["ui/UiNodeSize"] = 0x1411cf360ull;
    fake::symbols["ui/UiNodeRect"] = 0x1411b67d0ull;
    for (const auto& [k, v] : std::map<std::string, int64_t>{
             {"ui/ui.config", 0xc8},         {"ui/tt.descText", 0x570},     {"ui/tt.descStyle", 0x10c6c},
             {"ui/tt.descNode", 0x578},      {"ui/tt.descShown", 0x5ad},    {"ui/tt.descAlign", 0x5a8},
             {"ui/tt.descWrap", 0x5ae},      {"ui/tt.colorText", 0x3a0},    {"ui/tt.colorBits", 0x474},
             {"ui/tt.colorFlags", 0x430},    {"ui/tt.colorValue", 0x450},   {"ui/tt.colorFlags2", 0x432},
             {"ui/tt.smallSize", 0x10b9c},   {"ui/tt.smallFlags", 0x2e0},   {"ui/tt.smallField", 0x2cc},
             {"ui/tt.palette", 0xd5ec},      {"ui/tt.palUnique", 0x30},     {"ui/tt.palLegendary", 0x28},
             {"ui/tt.palEpic", 0x20},        {"ui/tt.palRare", 0x18},       {"ui/tt.palUncommon", 0x10},
             {"ui/tt.palCommon", 0x8},       {"ui/tt.buffPositive", 0xd648}, {"ui/tt.buffNegative", 0xd658},
             {"ui/detail.contentScope", 0x40}, {"ui/detail.scrollScope", 0x1048}, {"ui/detail.mode", 0x170},
             {"ui/detail.compact", 0xbc},    {"ui/detail.available", 0x2d}, {"ui/detail.unlocked", 0x2e}})
        fake::constants[k] = v;
    g_drawn.clear();
    g_order.clear();
    g_draw_fault = false;
    g_key = false;
    g_open_features = {"ui.tooltip.lines", "ui.tooltip.values"};
    Env& e = env();
    e.draw_text = &fake_draw;
    e.node_rect = [](uintptr_t, uintptr_t node, float out[4]) {
        out[0] = 1, out[1] = 2, out[2] = 300, out[3] = static_cast<float>(node & 0xff);
        return true;
    };
    e.key_down = [](uint32_t vk) { return g_key && vk == 0x72; };
    e.thread_id = []() -> uint32_t { return 4242; };
    e.feature_open = [](const char* n) { return g_open_features.count(n) != 0; };
    e.call_scale = [](KilnUiScaleFn fn, const KilnUiProviderContext* c, const KilnUiItemView* v, float* s, void* user,
                      KilnResult* rc) {
        try {
            *rc = fn(c, v, s, user);
            return true;
        } catch (...) {
            return false;
        }
    };
}

// ItemDetail as the client runs it: H2 (if hooked), then H1; values: the stats block opens before H2.
std::vector<float> g_stat_values;  // what the game's StatValueDraw got
void game_stat_value(const void*, float v) { g_stat_values.push_back(v); }
float g_stat_in = 0;
bool g_stats_draw = false;
void tooltip_game_detail(void*, uint8_t) {
    KilnRegisters r{};
    r.rbp = addr(g_tm->frame);
    r.r15 = 0x1234;  // an ItemInfo
    if (g_stats_draw) {
        for (const auto& m : fake::mids)
            if (m.label == "ui.tooltip/DetailStatsBlockCall") m.fn(&r, nullptr);
        for (const auto& w : fake::raws)
            if (w.label == "ui.tooltip/StatValueDraw") reinterpret_cast<void (*)(const void*, float)>(w.fn)(nullptr, g_stat_in);
    }
    for (const auto& m : fake::mids)
        if (m.target == 0x140c1193eull) {
            const size_t before = g_drawn.size();
            m.fn(&r, nullptr);
            for (size_t i = before; i < g_drawn.size(); ++i) g_order.push_back("H2:" + g_drawn[i].text);
        }
    if (g_stats_draw)  // a number drawn after the stats block (the block has other callers): never scaled
        for (const auto& w : fake::raws)
            if (w.label == "ui.tooltip/StatValueDraw") reinterpret_cast<void (*)(const void*, float)>(w.fn)(nullptr, g_stat_in);
    if (KilnMidFn fn = mid_at("ui.tooltip/DetailContentEnd")) {
        const size_t before = g_drawn.size();
        fn(&r, nullptr);
        for (size_t i = before; i < g_drawn.size(); ++i) g_order.push_back("H1:" + g_drawn[i].text);
    }
}

bool arm_tooltip() {
    bool ok = false;
    for (const auto& r : fake::raws) {
        if (r.label == "ui.values/ItemDetail") {
            g_detail_hook = reinterpret_cast<DetailHookFn>(r.fn);
            *r.original = reinterpret_cast<void*>(&tooltip_game_detail);
            ok = true;
        }
        if (r.label == "ui.tooltip/StatValueDraw") *r.original = reinterpret_cast<void*>(&game_stat_value);
    }
    return ok;
}

void show(uint32_t item = 0xAA) {
    fill_view_bytes(g_tm->record, 0x5000, 2, item, 1, 900, 0, 0);
    put(g_tm->record, 0x2d, uint8_t{3});  // PerkContainerNew's copy: 3 available, 1 unlocked
    put(g_tm->record, 0x2e, uint8_t{1});
    g_detail_hook(g_tm->record, 0);
}
// The probe key: pressed while one detail shows, released while the next shows; then a clean slate to look at.
void press(int times = 1) {
    for (int i = 0; i < times; ++i) {
        g_key = true;
        show();
        g_key = false;
        show();
    }
    g_drawn.clear();
    g_order.clear();
}
size_t mids_labelled(const char* part) {
    size_t n = 0;
    for (const auto& m : fake::mids) n += m.label.find(part) != std::string::npos;
    return n;
}

void tooltip_probe_installs() {
    prime_tooltip();
    CHECK(probe_key_of("") == 0x72 && probe_key_of("F3") == 0x72 && probe_key_of("f12") == 0x7b &&
              probe_key_of("F24") == 0x87 && probe_key_of("y") == 'Y' && probe_key_of("F25") == 0 &&
              probe_key_of("Insert") == 0,
          "probe: the key is F1..F24 or a letter (default F3)");
    CHECK(!probe_start(fake::owner("kiln.ui.values"), "Insert") && !tooltip_probe_on() && fake::raws.empty(),
          "probe: an unknown key leaves the probe off and hooks nothing");
    prime_tooltip();
    const bool on = probe_start(fake::owner("kiln.ui.values"), "");
    CHECK(on && tooltip_probe_on() && tooltip_lines_on() && tooltip_values_on() && arm_tooltip(),
          "probe: on, with native lines and values (features open)");
    CHECK(mid_at("ui.tooltip/DetailContentEnd") && mids_labelled("ui.tooltip/DetailAfterStats") == 1 &&
              mids_labelled("ui.tooltip/DetailStatsBlockCall") == 1 && mids_labelled("ui.tooltip/DetailStatsBlockEnd") == 1 &&
              mids_labelled("probe.DetailStatsModel") == 1 && mids_labelled("probe.DetailHeaderModel") == 1,
          "probe: H1, H2, the stats window's two ends, H3 and H5 are hooked once each (%zu mids)", fake::mids.size());
    CHECK(fake::logged("tooltip-probe: on (key F3") == 1, "probe: one kiln.log line says it is on and how");
    show();
    CHECK(g_drawn.empty() && fake::logged("tooltip-probe: DETAIL mode=0 compact=0 item=170 pide=900 lvl=3 rar=2 perks=3/1") == 1 &&
              fake::logged("tooltip-probe: SITE H1 tid=4242 r15=info") == 1 && fake::logged("tooltip-probe: SITE H2") == 1,
          "probe: phase off draws nothing; DETAIL and SITE lines name the record and the thread");
    show();
    CHECK(fake::logged("tooltip-probe: DETAIL") == 1 && fake::logged("tooltip-probe: SITE H1") == 1,
          "probe: an unchanged detail is not logged again");
    // held or off features: no native install, the probe still logs
    prime_tooltip();
    g_open_features.clear();
    CHECK(probe_start(fake::owner("kiln.ui.values"), "F3") && !tooltip_lines_on() && !tooltip_values_on() &&
              !mid_at("ui.tooltip/DetailContentEnd") && fake::logged("no native tooltip lines: the capability is off or held") == 1,
          "probe: a held ui.tooltip.lines installs no native site and says why");
}

void tooltip_probe_lines() {
    prime_tooltip();
    probe_start(fake::owner("kiln.ui.values"), "F3");
    arm_tooltip();
    press();  // -> short
    show();
    CHECK(fake::logged("tooltip-probe: PHASE short") == 1 && g_drawn.size() == 1 &&
              g_drawn[0].text == "Kiln probe: one short line" && g_order == (std::vector<std::string>{"H1:Kiln probe: one short line"}),
          "lines: the key steps to the short phase; its line is drawn once, at the content's end (H1)");
    const TextDraw& d = g_drawn[0].d;
    CHECK(d.line_fn == 0x140b1da10ull && d.draw_fn == 0x140b25160ull && d.align_fn == 0x1411ced70ull &&
              d.size_fn == 0x1411cf360ull && d.style == addr(g_tm->bundle) + 0x10c6c && d.align == 0x101,
          "recipe: the description's text, draw, align and size functions, its style in the bundle, alignment 0x101 with an ItemInfo");
    CHECK(d.node == 8 && d.shown == 0x3d && d.align_field == 0x38 && d.wrap == 0x3e && d.flags == 0x90 && d.flags2 == 0x92 &&
              d.color == 0xb0 && d.bits == 0xd4 && d.size_field == 0x7c && !d.colored && !d.small,
          "recipe: text object fields from the captures (node +8, shown +0x3d, align +0x38, wrap +0x3e, flags +0x90, colour +0xb0, size +0x7c)");
    CHECK(d.text_fnv == text::fnv64(d.text, d.text_len), "recipe: the game's string view carries the FNV-1a of the bytes");
    press();  // -> 16 lines
    show();
    CHECK(g_drawn.size() == 16 && g_drawn.back().text == "Kiln probe line 16 of 16 (the last)", "lines: 16 lines in one occurrence");
    press(2);  // long english, long german
    show();
    CHECK(g_drawn.size() == 1 && g_drawn[0].text.find("ß") != std::string::npos, "lines: the long German line keeps its UTF-8");
    press();  // colours
    show();
    std::map<std::string, TextDraw> by;
    for (const auto& x : g_drawn) by[x.text.substr(0, 8)] = x.d;
    CHECK(g_drawn.size() == 13 && !by["Default "].colored && by["Positive"].colored && by["Positive"].color_value == kPal[2] &&
              !by["Negative"].colored && by["Common"].color_value == kPal[1] && by["Uncommon"].color_value == kPal[2] &&
              by["Rare"].color_value == kPal[3] && by["Epic"].color_value == kPal[4] && by["Legendar"].color_value == kPal[5] &&
              by["Unique"].color_value == kPal[6] && by["Muted (t"].color_value == kPal[0],
          "colours: the header's palette entries (Positive = Uncommon, Negative = the default colour until Q6, Muted = the no-rarity entry)");
    CHECK(by["Small te"].small && by["Small te"].small_size == 0.75f && !by["Small te"].colored &&
              by["Buff pos"].color_value == 0xAAAAAAAAAAAAAAAAull && by["Buff neg"].color_value == 0xBBBBBBBBBBBBBBBBull,
          "colours: small text takes the bundle's small size; the probe's two buff tints are raw bundle entries");
    press();  // after-stats
    show();
    CHECK(g_order == (std::vector<std::string>{"H2:Kiln probe: between the stats and the description (AfterStats)",
                                                "H1:Kiln probe: after the last vanilla line (End)"}),
          "places: an AfterStats line at H2, an End line at H1");
    CHECK(fake::logged("tooltip-probe: DRAW n=2 of 2 phase=after-stats") == 1, "probe: a DRAW line per change");
    // 30 occurrences later: one RECT line with the list, the scroll area and the drawn lines
    put(g_tm->frame, 0x40 + 8, uintptr_t{0x9001});
    put(g_tm->frame, 0x1048 + 8, uintptr_t{0x9002});
    for (int i = 0; i < 40; ++i) show();
    CHECK(fake::logged("tooltip-probe: RECT list=1.0,2.0 300.0x1.0 scroll=1.0,2.0 300.0x2.0 line[0]=") == 1,
          "probe: one RECT line per item and phase once the layout settled");
}

void tooltip_no_after_stats_site() {
    prime_tooltip(false);
    probe_start(fake::owner("kiln.ui.values"), "F3");
    arm_tooltip();
    press(6);  // -> after-stats
    show();
    CHECK(g_order == (std::vector<std::string>{"H1:Kiln probe: between the stats and the description (AfterStats)",
                                                "H1:Kiln probe: after the last vanilla line (End)"}),
          "places: without H2 an AfterStats line goes last, in its order");
}

void tooltip_fault_turns_off() {
    prime_tooltip();
    probe_start(fake::owner("kiln.ui.values"), "F3");
    arm_tooltip();
    press(2);  // 16 lines
    g_draw_fault = true;
    show(0xAB);
    CHECK(!tooltip_lines_on() && g_drawn.empty() && fake::rows_off.size() == 1 && fake::rows_off[0] == "ui.tooltip.lines" &&
              fake::logged("native tooltip lines are off for this session: drawing a line into the game's item detail view faulted") == 1,
          "fault: the first faulting draw turns the native lines off for the session and reports the row");
    g_draw_fault = false;
    show(0xAC);
    CHECK(g_drawn.empty(), "fault: nothing is drawn natively afterwards (the details window keeps its lines)");
}

void tooltip_values_scale() {
    prime_tooltip();
    probe_start(fake::owner("kiln.ui.values"), "F3");
    arm_tooltip();
    g_stats_draw = true;
    g_stat_in = 21.5f;
    g_stat_values.clear();
    show();
    CHECK(g_stat_values == (std::vector<float>{21.5f, 21.5f}), "values: scale 1 passes the game's numbers through");
    press(7);  // -> values-x2
    g_stat_values.clear();
    show();
    CHECK(g_stat_values == (std::vector<float>{43.0f, 21.5f}) && fake::logged("tooltip-probe: VALUE 21.5 -> 43") == 1,
          "values: inside ItemDetail's stats block the number is doubled, after the block (another caller) it is not");
    g_stats_draw = false;
}

// ---- the API: providers registered for the game's tooltip, and value providers ----
struct StyledScript {
    std::vector<std::pair<std::string, uint32_t>> lines;
    std::vector<KilnResult> appends;
};
KilnResult KILN_CALL styled_provider(const KilnUiProviderContext* c, const KilnUiItemView*, KilnUiLineWriter* w, void* user) {
    auto* s = static_cast<StyledScript*>(user);
    s->appends.clear();
    for (const auto& [text, flags] : s->lines) {
        KilnUiTextId id = 0;
        c->text->intern(c, KilnStr{text.data(), text.size()}, &id);
        s->appends.push_back(w->append_line(w, id, flags));
    }
    return KILN_OK;
}
KilnHandle add_styled(KilnOwner o, StyledScript* s, uint32_t flags = KUI_LINES_IN_GAME_TOOLTIP) {
    auto r = reg_record();
    return T()->tooltip_lines_native(o, 0, flags, &styled_provider, s, &r) == KILN_OK && r.ok ? r.handle : 0;
}
void prime_api(bool lines_row = true, bool values_row = true) {
    prime_tooltip();
    if (lines_row) fake::rows.insert("ui.tooltip.lines");
    if (values_row) fake::rows.insert("ui.tooltip.values");
    set_installer(kListValues, &kiln::uivalues::tt::install_values_list);
}

void tooltip_api_lines() {
    static_assert(offsetof(KilnUiValuesApi, tooltip_values) == 136 && offsetof(KilnUiValuesApi, tooltip_native_state) == 144 &&
                  sizeof(KilnUiValuesApi) == 152);
    static_assert(offsetof(KilnUiLineWriter, append) == 280 && offsetof(KilnUiLineWriter, append_line) == 288 &&
                  sizeof(KilnUiLineWriter) == 296 && sizeof(KilnUiTooltipNativeState) == 152);
    prime_api();
    CHECK(T()->header.size == sizeof(KilnUiValuesApi) && T()->tooltip_lines_native && T()->tooltip_values &&
              T()->tooltip_native_state,
          "table: the three tooltip slots are appended and covered by header.size");
    const KilnOwner o = fake::owner("mod:tooltip-api");
    StyledScript s;
    s.lines = {{"+12 % fire", KUI_COLOR_POSITIVE}, {"Set: 2 of 4", uint32_t{KUI_COLOR_EPIC} | uint32_t{KUI_LINE_SMALL}},
               {"Affix: sharp", KUI_LINE_AFTER_STATS}};
    LinesScript plain;
    plain.lines = {"only in the window"};
    CHECK(add_styled(o, &s) != 0 && add_lines(o, 0, &plain) != 0 && tooltip_lines_on() && arm_tooltip(),
          "api: a provider for the game's tooltip installs the native sites (row available)");
    show();
    CHECK(g_order == (std::vector<std::string>{"H2:Affix: sharp", "H1:+12 % fire", "H1:Set: 2 of 4"}),
          "api: the provider's lines go into the game's view at their places; a tooltip_lines provider's never");
    CHECK(g_drawn.size() == 3 && g_drawn[1].d.colored && g_drawn[1].d.color_value == kPal[2] && g_drawn[2].d.small &&
              g_drawn[2].d.color_value == kPal[4],
          "api: the colours and the small size the flags name");
    const auto d = read_details(o, 1);
    CHECK(lines_of(d) == (std::vector<std::string>{"+12 % fire", "Set: 2 of 4", "Affix: sharp", "only in the window"}),
          "api: every line is in the details window's snapshot too");
    KilnUiTooltipNativeState ns{};
    ns.size = sizeof ns;
    CHECK(T()->tooltip_native_state(&ns) == KILN_OK && ns.lines_on == 1 && ns.after_stats == 1 && ns.drawn_lines == 3 &&
              ns.probe == 0,
          "state: lines on, AfterStats place, lines drawn counted");
    // invalid flags: a fault of that provider, nothing of its lines anywhere
    StyledScript bad;
    bad.lines = {{"x", 0x200}};
    StyledScript bad_color;
    bad_color.lines = {{"y", 10}};
    add_styled(o, &bad);
    add_styled(o, &bad_color);
    g_drawn.clear();
    show(0xAB);
    CHECK(bad.appends == (std::vector<KilnResult>{KILN_ERR_INVALID_ARGUMENT}) &&
              bad_color.appends == (std::vector<KilnResult>{KILN_ERR_INVALID_ARGUMENT}) && g_drawn.size() == 3 &&
              fake::logged("wrote an invalid line") == 2,
          "api: an unknown flag or colour is an invalid line (a fault), the other providers' lines stay");
    auto r = reg_record();
    CHECK(T()->tooltip_lines_native(o, 0, 2, &lines_provider, &plain, &r) == KILN_ERR_INVALID_ARGUMENT,
          "api: an unknown registration flag is an argument error");
}

KilnResult KILN_CALL unit_scale(const KilnUiProviderContext*, const KilnUiItemView*, float*, void*) { return KILN_OK; }

void tooltip_api_fallback() {
    prime_api(false, false);
    const KilnOwner o = fake::owner("mod:tooltip-fallback");
    StyledScript s;
    s.lines = {{"in the window", KUI_COLOR_RARE}};
    CHECK(add_styled(o, &s) != 0 && !tooltip_lines_on() && arm_lines() && !mid_at("ui.tooltip/DetailContentEnd") &&
              fake::logged("no native tooltip lines: the capability is off or held") == 1,
          "fallback: without ui.tooltip.lines the registration still works and no native site is hooked");
    fill_view_bytes(g_mem.record, 0x5000, 2, 0xAA, 1, 900, 0, 0);
    g_detail_hook(g_mem.record, 0);
    CHECK(lines_of(read_details(o, 1)) == (std::vector<std::string>{"in the window"}) && g_drawn.empty(),
          "fallback: the lines are in Kiln's details window, nothing is drawn natively");
    KilnUiTooltipNativeState ns{};
    ns.size = sizeof ns;
    T()->tooltip_native_state(&ns);
    CHECK(ns.lines_on == 0 && std::string(ns.why) == "the capability is off or held", "state: off, with the reason");
    auto r = reg_record();
    CHECK(T()->tooltip_values(o, 0, &unit_scale, nullptr, &r) == KILN_ERR_UNAVAILABLE,
          "values: without ui.tooltip.values a value provider is refused (UNAVAILABLE)");
}

struct ScaleScript {
    float set = 1;
    KilnResult rc = KILN_OK;
    float seen = 0;
    int calls = 0;
    uint32_t item = 0;
};
KilnResult KILN_CALL scale_provider(const KilnUiProviderContext*, const KilnUiItemView* v, float* scale, void* user) {
    auto* s = static_cast<ScaleScript*>(user);
    ++s->calls;
    s->seen = *scale;
    s->item = v->item;
    *scale = s->set;
    return s->rc;
}
KilnHandle add_scale(KilnOwner o, uint32_t item, ScaleScript* s) {
    auto r = reg_record();
    return T()->tooltip_values(o, item, &scale_provider, s, &r) == KILN_OK && r.ok ? r.handle : 0;
}

void tooltip_api_values() {
    prime_api();
    const KilnOwner o = fake::owner("mod:tooltip-values");
    ScaleScript a, b, refuse, wild, other;
    a.set = 1.5f;
    b.set = 3.0f;  // sees 1.5, sets 3: the product so far is the input
    refuse.set = 99;
    refuse.rc = KILN_ERR_UNAVAILABLE;
    wild.set = 5000;
    other.set = 10;
    add_scale(o, 0, &a);
    add_scale(o, 0, &refuse);
    add_scale(o, 0, &wild);
    add_scale(o, 0, &b);
    add_scale(o, 0xBB, &other);
    CHECK(tooltip_values_on() && arm_tooltip() && mids_labelled("ui.tooltip/DetailStatsBlockCall") == 1,
          "values: the first provider installs the value sites");
    g_stats_draw = true;
    g_stat_in = 10.0f;
    g_stat_values.clear();
    show();
    CHECK(g_stat_values == (std::vector<float>{30.0f, 10.0f}) && b.seen == 1.5f && a.item == 0xAA && other.calls == 0,
          "values: providers chain in order (each sees the scale so far), a refusal keeps it, another item's is skipped");
    CHECK(fake::logged("returned a scale outside 0..1000") == 1 && refuse.calls == 1,
          "values: a scale out of range is a fault; the refusal is not");
    show();
    CHECK(a.calls == 2, "values: once per detail occurrence");
    g_stats_draw = false;
}

void tooltip_tests() {
    tooltip_api_lines();
    tooltip_api_fallback();
    tooltip_api_values();
    tooltip_probe_installs();
    tooltip_probe_lines();
    tooltip_no_after_stats_site();
    tooltip_fault_turns_off();
    tooltip_values_scale();
}
