// SPDX-License-Identifier: MIT
// Run the actual menu_scroll.cpp callbacks with an in-memory disk and a nesting-checking game stand-in.
// The model in menu_scroll_test.cpp cannot exercise failed OS operations or callback interleavings.
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <vector>
#include "services/kiln_hooks.h"
#include "../menu_scroll.cpp"

namespace kui {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 7;
void logf(int, const char*, ...) {}
uint32_t game_menu_mode() { return menu_rows::show_auto; }
size_t game_pause_mods_rows() { return 0; }
}

using namespace kui;
static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static KilnResult KILN_CALL build_info(KilnBuildInfo* b) {
    std::strcpy(b->exe_sha256, "build");
    b->kiln_version = {"version", 7};
    return KILN_OK;
}
static const void* KILN_CALL service(KilnStr, uint32_t) { return nullptr; }
static KilnApi api = [] { KilnApi a{}; a.build_info = build_info; a.service = service; return a; }();
static bool marker() { return fake_disk::files.count(g_trying) != 0; }
static void reset() {
    g_api = &api;
    g_guard = {};
    g_clear_marker = false;
    g_clear_retry = 0;
    g_frames_done = 0;
    g_trying = L"state/menu-scroll.trying";
    fake_disk::files.clear();
    fake_disk::deletes = fake_disk::writes = fake_disk::fail_deletes = 0;
    fake_disk::fail_writes = false;
    g_on = true;
}
static void end_frame() { scroll_after_frame(g_frames_done + 1); }
static void world_stop() {
    KilnWorldEvent e{};
    e.size = sizeof e;
    e.event = KILN_WORLD_STOPPING;
    // Join on the UI thread: a lock held across a game call would deadlock this sequence.
    std::thread t([&] { on_world(&e, nullptr); });
    t.join();
}
static void probe_frame() {
    CHECK(arm(menu_scroll::Try::probe));
    {
        std::lock_guard lk(g_guard_mu);
        if (menu_scroll::guard_step(g_guard, menu_scroll::Try::probe)) g_clear_marker = true;
    }
    end_frame();
}

// Fake game calls check the nesting used by the production probe and scroll-view paths.
static std::vector<void*> frames;
struct Node { float rect[4] = {0, 0, 0, 0}; };
static Node column_node, inner_node, view_node, top_node, end_node;
static bool fail_size = false, stop_in_size = false;
static void* vstack(void* builder, float) {
    frames.push_back(builder);
    Node* node = builder == g_inner ? &inner_node : builder == g_probe_top ? &top_node : &end_node;
    std::memcpy(static_cast<uint8_t*>(builder) + 8, &node, sizeof node);
    return node;
}
static void frame_end(void* builder) {
    CHECK(!frames.empty() && frames.back() == builder);
    if (!frames.empty()) frames.pop_back();
}
static void size_node(void* node, uint64_t) {
    if (node == &top_node && stop_in_size) world_stop();
    if (node == &top_node && fail_size) throw 1;
}
static void begin(void* layout, uint8_t, void*, void*, void*) {
    frames.push_back(layout);
    Node* node = &view_node;
    std::memcpy(static_cast<uint8_t*>(layout) + 8, &node, sizeof node);
}
static void end(void* layout) { frame_end(layout); }
static float speed(void*) { return 1; }
static void set_speed(void*, float) {}
static void* init(void* p) { return p; }
static void align_node(void*, const uint16_t*) {}
static const float* rect(void* node) { return static_cast<Node*>(node)->rect; }
static float offset = 0;
static void* state(void*, uint64_t, uint32_t, void*, uint32_t) { return &offset; }
static bool state_new(void*) { return false; }
alignas(16) static uint8_t column[0x80], app[0x80], config[0x80];
static void* app_ptr = app;
static KilnRegisters registers{};
static void setup_game(bool wrapped) {
    reset();
    F = {};
    F.vstack = vstack; F.frame_end = frame_end; F.size = size_node; F.rect = rect;
    F.begin = begin; F.end = end; F.speed = speed; F.set_speed = set_speed;
    F.params_init = F.bar_init = init; F.align = align_node;
    F.state = state; F.state_new = state_new;
    F.ui_app = reinterpret_cast<uintptr_t>(&app_ptr);
    void* config_ptr = config;
    std::memcpy(app, &config_ptr, sizeof config_ptr);
    F.column_node = 8;
    Node* node = &column_node;
    std::memcpy(column + 8, &node, sizeof node);
    registers.rbp = reinterpret_cast<uintptr_t>(column);
    column_node.rect[3] = 1020;
    inner_node.rect[3] = 1080;
    view_node.rect[3] = 933;
    g_pair = {}; g_nest = {}; g_decider = {};
    g_decider.fit.known = true; g_decider.fit.wrap = wrapped;
    g_broken = g_in_call = false;
    g_entries_last = 1;
    g_scrolled_frames = 0;
    fail_size = stop_in_size = false;
    frames = {column};
}

int main() {
    // A sharing violation at the end of a successful trial must not survive an orderly exit.
    reset();
    for (uint32_t i = 1; i < menu_scroll::kTrialFrames; ++i) probe_frame();
    fake_disk::fail_deletes = 1;
    probe_frame();
    CHECK(marker());
    scroll_exit();
    CHECK(!marker());

    // Native Windows need not run on_unload: idle UI frames must retry a failed deletion too.
    reset();
    probe_frame();
    fake_disk::fail_deletes = 1;
    for (uint32_t i = 0; i < menu_scroll::kIdleFrames; ++i) end_frame();
    CHECK(marker());
    for (uint32_t i = 0; i < menu_scroll::kIdleFrames; ++i) end_frame();
    CHECK(!marker());

    // A world destroyed during the first guarded frame is orderly, but only once that frame finishes.
    reset();
    CHECK(arm(menu_scroll::Try::probe));
    world_stop();
    CHECK(marker()); // a crash before the frame completes must still leave evidence
    end_frame();
    CHECK(!marker()); // the native-Windows quit path may have no further frames or exit callback

    // An unsuccessful deletion remains pending, with bounded retries rather than disk I/O every frame.
    reset();
    probe_frame();
    fake_disk::fail_deletes = 100;
    for (int i = 0; i < 80; ++i) end_frame();
    CHECK(marker() && fake_disk::deletes == 10);
    fake_disk::fail_deletes = 0;
    world_stop();
    CHECK(!marker());

    // Reopening cancels an old pending deletion before entering untried game calls.
    reset();
    probe_frame();
    fake_disk::fail_deletes = 1;
    for (uint32_t i = 0; i < menu_scroll::kIdleFrames; ++i) end_frame();
    CHECK(marker());
    CHECK(arm(menu_scroll::Try::wrap));
    end_frame();
    CHECK(marker());

    // A marker queued for deletion by one builder must not erase a new trial in the same UI frame.
    reset();
    for (uint32_t i = 1; i < menu_scroll::kTrialFrames; ++i) probe_frame();
    CHECK(arm(menu_scroll::Try::probe));
    CHECK(menu_scroll::guard_step(g_guard, menu_scroll::Try::probe));
    g_clear_marker = true;
    CHECK(arm(menu_scroll::Try::wrap));
    end_frame();
    CHECK(marker());

    // A normal completed trial makes no more file calls in subsequent menu frames.
    reset();
    for (uint32_t i = 0; i < menu_scroll::kTrialFrames; ++i) probe_frame();
    const int writes = fake_disk::writes, deletes = fake_disk::deletes;
    for (int i = 0; i < 200; ++i) probe_frame();
    CHECK(!marker() && fake_disk::writes == writes && fake_disk::deletes == deletes);

    // A probe nested inside a loader's wrapper closes itself without closing the wrapper.
    for (bool wrapped : {false, true}) {
        setup_game(wrapped);
        on_open(registers);
        int wrapper;
        frames.push_back(&wrapper);
        scroll_entry_next();
        scroll_entry_drawn(false);
        CHECK(frames.back() == &wrapper);
        frame_end(&wrapper);
        on_close(registers);
        CHECK(frames.size() == 1 && frames.back() == column);
    }
    // A caught size exception still ends its probe; turning off mid-frame still ends the scroll view.
    setup_game(true);
    on_open(registers);
    fail_size = true;
    scroll_entry_next();
    CHECK(!scroll_on());
    on_close(registers);
    CHECK(frames.size() == 1 && frames.back() == column);

    // The guard can fail before the first probe after the wrap's own trial has already passed.
    setup_game(true);
    g_guard.trial[1].done = true;
    on_open(registers);
    fake_disk::fail_writes = true;
    scroll_entry_next();
    CHECK(!scroll_on());
    on_close(registers);
    CHECK(frames.size() == 1 && frames.back() == column);

    // World-stop callbacks may run from another thread while a game call is in progress.
    setup_game(true);
    on_open(registers);
    stop_in_size = true;
    scroll_entry_next();
    CHECK(marker());
    scroll_entry_drawn(false);
    on_close(registers);
    CHECK(marker());
    end_frame();
    CHECK(!marker() && frames.size() == 1);

    std::printf("menu_scroll_host_test: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
