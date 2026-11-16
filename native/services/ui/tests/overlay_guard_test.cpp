#include "../overlay_guard.h"
#include "../overlay_mode.h"
#include "../overlay_wrappers.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
using namespace kui::overlay_guard;
int main() {
    unsigned checks = 0;
    auto check = [&](bool b) { ++checks; assert(b); };
    check(!begin({}, "none").off);
    for (unsigned s = 0; s <= unsigned(Stage::swapchain); ++s) {
        auto first = begin({}, "sl");
        first.record.stage = Stage(s);
        auto off = begin(first.record, "sl");
        check(off.off && off.record.failures == 1);
        check(begin(off.record, "sl").off); // killed guarded start must not earn a retry
        auto retry = begin(finish(off.record), "sl");
        check(!retry.off);
        retry.record.stage = Stage(s);
        auto locked = begin(retry.record, "sl");
        check(locked.off && locked.record.phase == Phase::locked);
        check(begin(finish(locked.record), "sl").off);
        check(!begin(locked.record, "none").off);
        check(!begin({}, "sl").off); // deleting record re-enables
        check(!begin(finish(first.record), "sl").off); // successful first drawn frame
    }
    // A start that never drew (quit from the main menu without opening Kiln's menu or a mod's window) is not a death at
    // the overlay's start once idle() counted it fine: the owner's dev.29 test, 2026-11-15.
    {
        auto start = begin({}, "m");
        start.record.stage = Stage::swapchain;
        check(!begin(start.record, "m").off == false); // without idle(): the old false alarm (guarded next start)
        auto fine = idle(start.record);
        check(fine.phase == Phase::complete && fine.failures == 0);
        check(!begin(fine, "m").off);
        // the first draw arms it again: a death before that frame is presented guards the next start
        auto armed = rearm(fine);
        check(armed.phase == Phase::trying && armed.stage == Stage::swapchain);
        auto died = begin(armed, "m");
        check(died.off && died.record.phase == Phase::guarded && died.record.failed_stage == Stage::swapchain);
        // the frame presented after the re-arm completes it as before
        auto drawn = armed; drawn.stage = Stage::frame;
        check(!begin(finish(drawn), "m").off);
        // a second death at the first draw after a retry still locks
        auto retry = begin(finish(died.record), "m").record;
        check(retry.phase == Phase::trying);
        retry.stage = Stage::swapchain;
        check(begin(rearm(idle(retry)), "m").record.phase == Phase::locked);
    }
    // the owner's record after the false alarm (retry, one failure at the swapchain): a start that idles, or exits
    // normally (idle() too), is fine
    {
        Record owner{Phase::retry, Stage::swapchain, Stage::swapchain, 1, "mb"};
        auto next = begin(owner, "mb");
        check(!next.off && next.record.phase == Phase::trying);
        next.record.stage = Stage::swapchain;
        auto fine = idle(next.record);
        check(fine.phase == Phase::complete && fine.failures == 1); // the history stays until a frame is drawn
        auto drawn = rearm(fine); drawn.stage = Stage::frame;
        check(finish(drawn).failures == 0);                         // a drawn frame clears it
        auto again = begin(fine, "mb").record;
        again.stage = Stage::swapchain;
        check(!begin(idle(again), "mb").off); // a second quiet start does not lock
        check(begin(again, "mb").off);        // a real death while trying still guards
    }
    // idle() and rearm() change nothing outside their phases
    for (Phase p : {Phase::complete, Phase::guarded, Phase::retry, Phase::locked}) {
        Record x{p, Stage::swapchain, Stage::device, 1, "m"};
        check(idle(x).phase == p && idle(x).failures == 1);
    }
    for (Phase p : {Phase::trying, Phase::guarded, Phase::retry, Phase::locked}) {
        Record x{p, Stage::device, Stage::device, 1, "m"};
        check(rearm(x).phase == p && rearm(x).stage == Stage::device);
    }
    Record r;
    check(!parse("unreadable", r));
    check(!begin(r, "sl").off);
    auto a = begin({}, "sl").record;
    check(parse(encode(a), r) && r.phase == Phase::trying && r.modules == "sl");
    check(!parse("version=99\n", r));
    check(!begin(finish(a), "sl").off);
    // Exercise the runtime detector on disk, not a hand-written replacement for its identity.
    unsigned failures = 0;
    auto expect = [&](bool b, const char* name) {
        ++checks;
        if (!b) { ++failures; std::printf("FAIL: %s\n", name); }
    };
    const auto root = std::filesystem::temp_directory_path() /
        ("kiln-overlay-guard-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "Streamline");
    auto write = [&](const std::filesystem::path& path, const char* text) { std::ofstream(path, std::ios::binary) << text; };
    const auto bare = detect_wrappers(root);
    expect(bare.identity.starts_with("w2-") && !bare.streamline, "the wrapper identity is versioned");
    write(root / "nvngx_dlss.dll", "the game's DLSS");
    expect(detect_wrappers(root).identity == bare.identity, "the game's DLSS is not a wrapper");
    write(root / "Streamline/nvngx_dlss.dll", "a leftover DLSS file");
    write(root / "Streamline/sl.dlss_g.dll", "a leftover Streamline helper");
    expect(detect_wrappers(root).identity == bare.identity, "leftover helpers do not keep Streamline detected");
    LoadedWrappers system;
    for (unsigned i : {1u, 2u, 4u, 5u}) system[i] = root / "Windows/System32" / wrapper_names[i];
    expect(detect_wrappers(root, system).identity == bare.identity, "loaded DLSS and Windows system DLLs do not count");
    expect(wrapper_path(root / "WRAPPER.dll") == wrapper_path(root / "wrapper.dll"), "path spelling ignores ASCII case");
    write(root / "powrprof.dll", "wrapper one");
    const auto installed = detect_wrappers(root);
    const auto timestamp = std::filesystem::last_write_time(root / "powrprof.dll");
    write(root / "powrprof.dll", "wrapper two"); // same size, same timestamp, different contents
    std::filesystem::last_write_time(root / "powrprof.dll", timestamp);
    const auto replaced = detect_wrappers(root);
    Record locked{Phase::locked, Stage::device, Stage::device, 2, installed.identity};
    expect(!begin(locked, replaced.identity).off, "replacing a wrapper permits a new attempt");
    expect(begin(locked, installed.identity).off, "the same wrapper remains locked");
    expect(detect_wrappers(root).identity == replaced.identity, "an unchanged wrapper has a stable identity");
    // Old Kiln's presence-only record can match even after a wrapper that loaded late was removed.
    for (const char* old : {"m", "mb", "mbf", "mabf"}) {
        Record legacy{Phase::locked, Stage::device, Stage::device, 2, old};
        expect(!begin(legacy, replaced.identity).off, "presence-only legacy history gets one new attempt");
    }
    std::filesystem::remove(root / "powrprof.dll");
    expect(detect_wrappers(root).identity == bare.identity, "removing the last wrapper ignores leftover DLSS");
    for (const char* old : {"m", "mb", "ma", "mab", "mbf", "mabf"}) {
        for (Phase phase : {Phase::trying, Phase::guarded, Phase::retry, Phase::locked}) {
            Record legacy;
            const auto text = "1 " + std::to_string(unsigned(phase)) + " 4 4 2 " + old + "\n";
            expect(parse(text, legacy), "dev.29 records remain readable");
            const auto next = begin(legacy, bare.identity);
            expect(!next.off && next.record.failures == 0 && next.record.stage == Stage::begun,
                   "an old record heals with no wrapper, including an interrupted or locked start");
        }
    }
    Record missing_set;
    expect(!parse("1 4 4 4 2\n", missing_set) && !begin(missing_set, bare.identity).off,
           "a partial old record without a wrapper set permits startup");

    const auto sl_file = root / "Streamline/sl.interposer.dll";
    write(sl_file, "Streamline one");
    const auto sl_disk = detect_wrappers(root);
    expect(sl_disk.streamline && sl_disk.identity != bare.identity, "adding Streamline changes the identity");
    LoadedWrappers loaded;
    loaded[0] = root / "Streamline/../Streamline/sl.interposer.dll";
    const auto sl_loaded = detect_wrappers(root, loaded);
    expect(sl_loaded.identity == sl_disk.identity && sl_loaded.streamline, "loading an installed wrapper does not grant a retry");
    loaded[0] = root / "Streamline/SL.INTERPOSER.DLL";
    expect(detect_wrappers(root, loaded).identity == sl_disk.identity, "loaded path case does not grant a retry");
    std::filesystem::create_symlink(sl_file, root / "interposer-link.dll");
    loaded[0] = root / "interposer-link.dll";
    expect(detect_wrappers(root, loaded).identity == sl_disk.identity, "an alias of the installed file is counted once");
    write(root / "sl.interposer.dll", "a second interposer");
    const auto both = detect_wrappers(root, loaded);
    expect(both.identity != sl_disk.identity, "two copies of one wrapper are tracked separately");
    std::filesystem::remove(root / "sl.interposer.dll");
    expect(detect_wrappers(root, loaded).identity == sl_disk.identity, "removing one copy changes the identity");
    std::filesystem::create_directory(root / "external");
    std::filesystem::rename(sl_file, root / "external/sl.interposer.dll");
    loaded[0] = root / "external/sl.interposer.dll";
    const auto external = detect_wrappers(root, loaded);
    expect(external.streamline && external.identity != sl_disk.identity, "a loaded external wrapper uses its full path");
    expect(detect_wrappers(root).identity == bare.identity, "unknown subfolder and file names are not scanned");
    write(root / "external/ReShade64.dll", "external ReShade");
    loaded = {};
    loaded[3] = root / "external/ReShade64.dll";
    const auto reshade = detect_wrappers(root, loaded);
    expect(!reshade.streamline && reshade.identity != bare.identity, "a loaded ReShade copy is still a wrapper");
    // Adding, removing or replacing wrappers resets every phase. It earns only one
    // attempt: after persistence, another death with the same set guards and then locks.
    for (const auto& before : {bare.identity, installed.identity, sl_disk.identity, both.identity}) {
        for (const auto& after : {bare.identity, replaced.identity, sl_disk.identity, external.identity}) {
            if (before == after) continue;
            for (Phase phase : {Phase::trying, Phase::guarded, Phase::retry, Phase::locked}) {
                Record previous{phase, Stage::device, Stage::device, 2, before};
                auto next = begin(previous, after);
                expect(!next.off && next.record.phase == Phase::trying && next.record.failures == 0,
                       "a wrapper change resets every interrupted phase");
                next.record.stage = Stage::device;
                Record persisted;
                expect(parse(encode(next.record), persisted) && persisted.modules == after, "new identities round trip");
                const auto died = begin(persisted, after);
                expect(died.off && died.record.failures == 1, "the same set guards a fresh crash");
                auto retry = begin(finish(died.record), after).record;
                retry.stage = Stage::device;
                const auto died_twice = begin(retry, after);
                expect(died_twice.off && died_twice.record.phase == Phase::locked, "the same set locks after a second crash");
            }
        }
    }
    Record old_locked{Phase::locked, Stage::swapchain, Stage::swapchain, 2, "mb"};
    const auto healed = begin(old_locked, bare.identity);
    expect(kui::choose_overlay_mode(false, healed.off, false, -1) == kui::OverlayMode::on,
           "automatic drawing resumes after removal without deleting the old guard");
    expect(kui::choose_overlay_mode(false, false, sl_disk.streamline, -1) == kui::OverlayMode::drawing_off,
           "a fresh attempt does not bypass automatic Streamline protection");
    std::filesystem::remove_all(root);
    std::printf("overlay_guard: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
