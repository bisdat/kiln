#include "../overlay_guard.h"
#include "../overlay_guard_file.h"
#include "../overlay_mode.h"
#include "../overlay_wrappers.h"
#include "../overlay_optiscaler.h"
#include <sstream>
#include <cassert>
#include <string_view>
#include <vector>
#include <map>
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
        // a guarded start that ended any way at all (no Vulkan hook of Kiln's in it) earns the retry: 0.2.0-dev.29.18
        auto after_guarded = begin(off.record, "sl");
        check(!after_guarded.off && after_guarded.record.phase == Phase::trying && after_guarded.record.failures == 1);
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
    // Inbox 80 (0.2.0-dev.29.18): the game died right after the overlay's first submit failed. The record of that start
    // (kiln/state/overlay-start.guard in the player's report: 32 bytes, trying at "swapchain created") guards the next
    // start; overlay_guard_failed keeps a start that turned the drawing off armed for a while, overlay_guard_drawn keeps
    // it armed through the overlay's first drawn frames.
    {
        // the player's start: swapchain, a draw attempt that failed (no frame presented), the game died
        auto start = begin({}, "w2-x").record;
        start.stage = Stage::swapchain;
        auto next = begin(start, "w2-x");
        check(next.off && next.record.phase == Phase::guarded && next.record.failed_stage == Stage::swapchain);
        // the same, but OptiScaler's DLL replaced in between (0.9.5 <-> a nightly): one fresh attempt, by design
        auto other = begin(start, "w2-y");
        check(!other.off && other.record.wrapper_retry);
        other.record.stage = Stage::swapchain;
        check(begin(other.record, "w2-y").off);  // and the start after a second death is guarded again
        // drawn frames presented, but fewer than the guard waits for: still trying, a death guards the next start
        auto drawn = begin({}, "m").record;
        drawn.stage = Stage::swapchain;  // overlay_guard_drawn changes nothing before kDrawnFrames or its timer
        check(begin(drawn, "m").off);
        // the drawing went off after the start had counted as fine (a failed submit an hour in): armed again, a death in
        // the window guards the next start, a game that went on counts as fine and keeps no new failure
        auto fine = begin({}, "m").record;
        fine.stage = Stage::frame;
        fine = finish(fine);
        auto armed = rearm(fine);
        check(armed.phase == Phase::trying && armed.stage == Stage::swapchain);
        check(begin(armed, "m").off);
        auto went_on = idle(armed);
        check(went_on.phase == Phase::complete && went_on.failures == 0 && !begin(went_on, "m").off);
        // a start that turned the drawing off and went on, after an earlier death at the same stage: the history stays,
        // so a second death there would still lock
        auto retry = begin(finish(next.record), "w2-x").record;
        retry.stage = Stage::swapchain;
        auto survived = idle(retry);
        check(survived.failures == 1 && !begin(survived, "w2-x").off);
        auto again = begin(survived, "w2-x").record;
        again.stage = Stage::swapchain;
        check(begin(again, "w2-x").record.phase == Phase::locked);
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
    // The owner's 0.2.0-dev.29.18 candidate, 2026-11-19 (Proton): the game died while the overlay was starting (stage:
    // hooks installed), the next start ran guarded and quit normally from the main menu after 26 s, and the start after
    // it was guarded again, and again: a guarded start earned the retry only by running a minute, and the exit left a
    // guarded record alone. kiln/state/overlay-start.guard after the second guarded quit: "2 2 1 1 1 0 w2-a8c7f832281a39c5".
    {
        const std::string m = "w2-a8c7f832281a39c5";
        // the start that died: trying at "hooks installed" on the disk
        auto died = begin({}, m).record;
        died.stage = Stage::hooks;
        Record disk;
        check(parse(encode(died), disk) && disk.phase == Phase::trying);
        // the guarded start: drawing off, its record written at once (overlay_guard_start's save)
        auto guarded = begin(disk, m);
        check(guarded.off && guarded.record.phase == Phase::guarded && guarded.record.failed_stage == Stage::hooks &&
              guarded.record.failures == 1);
        check(parse(encode(guarded.record), disk));
        check(encode(disk) == "3 2 1 1 1 0 - " + m + "\n");  // the owner's file in today's format (no Kiln version given)
        // its normal quit (code 0, Kiln's exit steps ran: overlay_guard_exit): the record no longer reads guarded
        const auto quit = at_exit(disk);
        check(quit.phase == Phase::retry && quit.failures == 1 && quit.failed_stage == Stage::hooks);
        check(parse(encode(quit), disk));
        // the start after it tries the overlay again, keeping the first death
        auto next = begin(disk, m);
        check(!next.off && next.record.phase == Phase::trying && next.record.stage == Stage::begun &&
              next.record.failures == 1 && next.record.failed_stage == Stage::hooks);
        // the owner's record as it is on the disk tonight (written by the candidate) heals at the next start too
        Record owner;
        check(parse("2 2 1 1 1 0 " + m + "\n", owner) && owner.phase == Phase::guarded);
        check(!begin(owner, m).off && begin(owner, m).record.phase == Phase::trying);
        // a guarded start whose quit Kiln did not see (a quit on Windows without code 0, the game killed): the record
        // still reads guarded, and the start after tries all the same
        check(!begin(guarded.record, m).off);
        // that try dies at the same stage again: locked, the drawing stays off until the record is deleted
        auto again = next.record;
        again.stage = Stage::hooks;
        const auto locked = begin(again, m);
        check(locked.off && locked.record.phase == Phase::locked && locked.record.failures == 2);
        check(at_exit(locked.record).phase == Phase::locked);  // a normal exit of the locked start keeps it locked
        check(begin(at_exit(locked.record), m).off && begin(begin(locked.record, m).record, m).off);
        // that try draws: the history clears
        auto drawn = next.record;
        drawn.stage = Stage::frame;
        check(finish(drawn).phase == Phase::complete && finish(drawn).failures == 0);
        // that try quits normally before it draws anything (the main menu, nothing of Kiln's open): fine, history kept
        auto quiet = next.record;
        quiet.stage = Stage::swapchain;
        check(at_exit(quiet).phase == Phase::complete && at_exit(quiet).failures == 1 && !begin(at_exit(quiet), m).off);
    }
    // A guarded start that crashes too (chosen behaviour, 0.2.0-dev.29.18): it ran with no Vulkan hook of Kiln's, so its
    // death is not one at the overlay's start. The record it wrote stays guarded, the start after tries the overlay
    // again with the first death kept, and a second death at that stage locks. A death at another stage guards one
    // start again (as before: each death at a new stage costs one start without the overlay).
    {
        auto died = begin({}, "g").record;
        died.stage = Stage::device;
        const auto guarded = begin(died, "g");                 // drawing off, "2 2 ..." written
        check(guarded.off && guarded.record.phase == Phase::guarded);
        const auto after_crash = begin(guarded.record, "g");   // the guarded start crashed: its record is unchanged
        check(!after_crash.off && after_crash.record.phase == Phase::trying && after_crash.record.failures == 1);
        auto same = after_crash.record;
        same.stage = Stage::device;
        check(begin(same, "g").record.phase == Phase::locked);
        auto other = after_crash.record;
        other.stage = Stage::swapchain;
        const auto guarded_again = begin(other, "g");
        check(guarded_again.off && guarded_again.record.phase == Phase::guarded &&
              guarded_again.record.failed_stage == Stage::swapchain && guarded_again.record.failures == 1);
        // a guarded start never writes trying: the guarded record itself is what the next start reads
        check(guarded.record.phase != Phase::trying && guarded_again.record.phase != Phase::trying);
        // KILN_UI_OVERLAY=1 in a guarded start is an attempt of its own (overlay_guard_start sets trying): a death in it
        // guards, or locks at the same stage, as any other attempt
        auto forced = guarded.record;
        forced.phase = Phase::trying;
        forced.stage = Stage::device;
        check(begin(forced, "g").record.phase == Phase::locked);
    }
    // at_exit() changes only trying (idle) and guarded (retry)
    for (Phase p : {Phase::complete, Phase::retry, Phase::locked}) {
        Record x{p, Stage::swapchain, Stage::device, 1, "m"};
        check(at_exit(x).phase == p && at_exit(x).stage == Stage::swapchain && at_exit(x).failures == 1);
    }
    // Inbox 97: the lock 0.2.0-dev.29.17 wrote ("failed twice at this stage", from bugs a later Kiln fixed) stayed after
    // the update, and the overlay stayed off. The record carries the Kiln version since 0.2.0-dev.29.19; a record of
    // another Kiln (an update, a downgrade) or one without the version (every older Kiln's) starts fresh.
    {
        const std::string m = "w2-a8c7f832281a39c5", now = "0.2.0-dev.29.19";
        Record old;
        check(parse("2 4 1 1 2 0 " + m + "\n", old) && old.phase == Phase::locked && old.kiln.empty());
        check(begin(old, m).off);  // without the Kiln's version the lock holds, as it did
        const auto cleared = for_kiln(old, now, m);
        check(cleared.phase == Phase::complete && cleared.failures == 0 && !cleared.wrapper_retry && cleared.kiln == now &&
              cleared.modules == m && cleared.stage == Stage::begun);
        const auto next = begin(cleared, m);
        check(!next.off && next.record.phase == Phase::trying && next.record.kiln == now);
        check(encode(next.record) == "3 1 0 0 0 0 " + now + " " + m + "\n");
        Record disk;
        check(parse(encode(next.record), disk) && disk.kiln == now && disk.modules == m && disk.phase == Phase::trying);
        // a wrapper recovery and a guarded record of an old Kiln are cleared too
        Record wrapped{Phase::guarded, Stage::device, Stage::device, 1, "w2-old", true, "0.2.0-dev.29.17"};
        const auto fresh = for_kiln(wrapped, now, m);
        check(fresh.phase == Phase::complete && fresh.failures == 0 && !fresh.wrapper_retry && fresh.modules == m);
        // the same Kiln: nothing changes, a lock holds until the record is deleted
        Record locked{Phase::locked, Stage::hooks, Stage::hooks, 2, m, false, now};
        const auto same = for_kiln(locked, now, m);
        check(same.phase == Phase::locked && same.failures == 2 && same.kiln == now);
        check(begin(same, m).off && begin(same, m).record.kiln == now);
        check(parse(encode(locked), disk) && begin(for_kiln(disk, now, m), m).off);
        // another Kiln, a downgrade as well: cleared
        check(!begin(for_kiln(locked, "0.2.0-dev.29.18", m), m).off);
        // a wrapper change within one Kiln keeps its version in the record
        check(begin(locked, "w2-other").record.kiln == now && begin(locked, "w2-other").record.wrapper_retry);
        // malformed records of the new format are rejected (a rejected record starts fresh, never locked)
        check(!parse("3 4 1 1 2 0 " + m + "\n", disk));  // no version field
        check(!parse("3 4 1 1 2 0 " + std::string(65, 'v') + " " + m + "\n", disk));
        check(!parse("3 4 1 1 2 2 " + now + " " + m + "\n", disk));
        check(!parse("3 4 1 1 2 0 " + now + " " + m + " extra\n", disk));
        check(!parse("4 4 1 1 2 0 " + now + " " + m + "\n", disk));
        // the version as one bounded token
        check(kiln_token("0.2.0-dev.29.19+20261119.c370680") == "0.2.0-dev.29.19+20261119.c370680");
        check(kiln_token("") == "unknown" && kiln_token("a b\tc\n") == "a_b_c_" && kiln_token(std::string(100, 'x')).size() == 64);
        check(kiln_token("-") == "unknown");  // "-" stands for no version in the record
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
    // A wrapper updater must not grant an unlimited sequence of crashing attempts.
    {
        auto first = begin({}, "w2-churn0").record;
        first.stage = Stage::device;
        auto changed = begin(first, "w2-churn1");
        expect(!changed.off, "one wrapper change grants recovery");
        Record persisted;
        expect(parse(encode(changed.record), persisted) && persisted.wrapper_retry,
               "wrapper recovery survives persistence");
        expect(idle(persisted).wrapper_retry, "an idle start does not renew wrapper recovery");
        auto drawn = finish(persisted);
        expect(!drawn.wrapper_retry && !begin(drawn, "w2-churn2").off,
               "a successfully drawn frame renews wrapper recovery");
        persisted.stage = Stage::device;
        auto guarded = begin(persisted, "w2-churn2");
        expect(guarded.off, "another wrapper change cannot erase the recovery crash");
        auto retry = begin(finish(guarded.record), "w2-churn3").record;
        retry.stage = Stage::device;
        auto locked = begin(retry, "w2-churn4");
        expect(locked.off && locked.record.phase == Phase::locked, "changing wrappers still locks repeated crashes");
        expect(begin(locked.record, "w2-churn5").off, "further changes cannot bypass a used recovery");
        expect(!parse("2 1 0 0 0 2 w2-a\n", persisted), "an invalid recovery flag is rejected");
        expect(!parse("2 1 0 0 0\n", persisted), "a missing recovery flag is rejected");
        expect(parse("1 4 3 3 2 mb\n", persisted) && !persisted.wrapper_retry,
               "old records can use one wrapper recovery");
    }
    {
        const std::string large(1024 * 1024, 'x');
        std::istringstream in(large);
        size_t read = 0;
        const bool ok = wrapper_contents(in, large.size(), [&](const char*, size_t n) { read += n; });
        expect(ok && read <= 128 * 1024, "wrapper content reads are capped at 128 KiB per file");
    }
    for (size_t size : {0u, 1u, 65535u, 65536u, 65537u, 131072u, 131073u, 1048576u}) {
        std::string data(size, 'a');
        if (size) { data.front() = 'b'; data.back() = 'c'; }
        std::istringstream in(data);
        std::string sampled;
        const bool ok = wrapper_contents(in, size, [&](const char* b, size_t n) { sampled.append(b, n); });
        const auto expected = size <= 131072 ? data : data.substr(0, 65536) + data.substr(size - 65536);
        expect(ok && sampled == expected, "bounded reads include the exact prefix and suffix without overlap");
    }
    {
        std::istringstream truncated("short");
        expect(!wrapper_contents(truncated, 100, [](const char*, size_t) {}), "a truncated file is not a complete sample");
        std::istringstream oversized("short");
        size_t read = 0;
        expect(!wrapper_contents(oversized, UINT64_MAX, [&](const char*, size_t n) { read += n; }) && !read,
               "an unknown or unrepresentable size cannot trigger an unbounded read");
    }
    expect(wrapper_path_key(u8"//?/C:/Möds/sl.interposer.dll") == wrapper_path_key(u8"C:/Möds/sl.interposer.dll"),
           "extended drive paths use the same identity");
    expect(wrapper_path_key(u8"\\\\?\\UNC\\host\\Möds\\ReShade64.dll") == wrapper_path_key(u8"//host/Möds/ReShade64.dll"),
           "extended UNC paths use the same identity");
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
    std::filesystem::last_write_time(root / "powrprof.dll", timestamp + std::chrono::seconds(1));
    expect(detect_wrappers(root).identity == replaced.identity, "a write-time-only change does not grant recovery");
    std::filesystem::last_write_time(root / "powrprof.dll", timestamp);
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
    std::filesystem::create_directories(root / u8"Möds");
    const auto unicode_file = root / u8"Möds/ReShade64.dll";
    write(unicode_file, "a wrapper in a non-ASCII folder");
    loaded[3] = unicode_file;
    const auto unicode = detect_wrappers(root, loaded);
    expect(unicode.identity != bare.identity && unicode.identity == detect_wrappers(root, loaded).identity,
           "a non-ASCII loaded path can be read and remains stable");
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
    {  // The record's write (overlay_guard_file.h): a temporary file, a rename over the record, then a flush asked for on
       // another thread; nothing on the caller's thread forces the disk (inbox 50: the present call waited for it).
        struct FakeFs {
            std::vector<std::string> calls;
            std::map<std::wstring, std::string> files;
            bool fail_write = false, fail_replace = false;
            static std::string narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }
            bool write_new(const std::wstring& f, std::string_view text) {
                calls.push_back("write " + narrow(f));
                if (fail_write) return false;
                files[f] = std::string(text);
                return true;
            }
            bool replace(const std::wstring& from, const std::wstring& to) {
                calls.push_back("replace " + narrow(from) + " " + narrow(to));
                if (fail_replace || !files.count(from)) return false;
                files[to] = files[from];
                files.erase(from);
                return true;
            }
            void flush_later(const std::wstring& f) { calls.push_back("flush later " + narrow(f)); }
        };
        Record r{Phase::trying, Stage::swapchain, Stage::begun, 0, "none"};
        FakeFs fs;
        expect(write_record(fs, L"g.guard", encode(r)), "the record is written");
        expect((fs.calls == std::vector<std::string>{"write g.guard.tmp", "replace g.guard.tmp g.guard", "flush later g.guard"}),
               "temporary file, rename over the record, then a flush on another thread, in that order");
        Record back;
        expect(fs.files.size() == 1 && parse(fs.files[L"g.guard"], back) && back.stage == Stage::swapchain,
               "the record reads back after the rename");
        FakeFs failing;
        failing.fail_write = true;
        expect(!write_record(failing, L"g.guard", encode(r)) && failing.calls.size() == 1, "a failed write renames and flushes nothing");
        FakeFs refused;
        refused.fail_replace = true;
        expect(!write_record(refused, L"g.guard", encode(r)) && refused.calls.size() == 2, "a refused rename asks for no flush");
        FakeFs none;
        expect(write_record(none, L"", encode(r)) && none.calls.empty(), "no state folder: nothing written");
    }
    {  // OptiScaler (overlay_optiscaler.h): found by its contents, never by a proxy name alone; its ini decides the mode
        auto contents = [](const std::string& data) {
            std::istringstream in(data);
            return optiscaler_contents(in, data.size());
        };
        auto utf16 = [](std::string_view a) { std::string w; for (char c : a) { w += c; w += '\0'; } return w; };
        expect(contents("MZ...OptiScaler.ini..."), "the ASCII name of its ini marks OptiScaler");
        expect(contents("MZ" + utf16("OPTISCALER.INI")), "the UTF-16 name, any case, marks OptiScaler");
        expect(!contents("MZ ReShade OptiScaler optiscaler.log"), "other texts do not");
        std::string across(65536 - 5, 'x');
        across += "optiscaler.ini";
        across += std::string(70000, 'y');
        expect(contents(across), "a name across two read chunks is seen");
        std::string across_wide(65536 - 9, 'x');
        across_wide += utf16("optiscaler.ini");
        expect(contents(across_wide), "a UTF-16 name across two read chunks is seen");
        {
            std::istringstream in("optiscaler.ini");
            expect(!optiscaler_contents(in, optiscaler_max_bytes + 1), "an oversized file is not read");
        }
        const auto stock = parse_optiscaler_ini("; OptiScaler.ini as it ships\n[FrameGen]\nEnabled=auto\nFGInput=auto\nFGOutput=auto\n"
                                                "[Menu]\n; Enables new Overlay ImGui menus\nOverlayMenu=auto\nShortcutKey=auto\n");
        expect(stock.read && stock.overlay_menu == -1 && !stock.frame_gen(), "the stock ini: menu on by default, no frame generation");
        const auto menu_off = parse_optiscaler_ini("\xEF\xBB\xBF[menu]\r\n  overlaymenu = FALSE \r\n");
        expect(menu_off.overlay_menu == 0 && !menu_off.frame_gen(), "sections, keys and values ignore case and spaces, after a BOM");
        const auto other_section = parse_optiscaler_ini("[Hotfix]\nOverlayMenu=false\n[Menu]\n#OverlayMenu=false\n");
        expect(other_section.overlay_menu == -1, "the key counts only in [Menu], never in a comment");
        const auto fg = parse_optiscaler_ini("[Menu]\nOverlayMenu=false\n[FrameGen]\nFGInput=upscaler\nFGOutput=fsrfg\n");
        expect(fg.overlay_menu == 0 && fg.frame_gen(), "frame generation needs an input and an output");
        expect(!parse_optiscaler_ini("[FrameGen]\nFGInput=upscaler\nFGOutput=nofg\n").frame_gen(), "no output: no frame generation");
        expect(parse_optiscaler_ini("[Menu]\nOverlayMenu=false\nOverlayMenu=true\n").overlay_menu == 1, "the last of two keys wins");

        const auto game = root / "game-optiscaler";
        std::filesystem::create_directories(game);
        auto opti_bytes = "MZ OptiScaler " + utf16("OptiScaler.ini") + " ReShade64.dll";
        expect(!detect_optiscaler(game).found() && !detect_optiscaler(game).ini_only, "an empty game folder: none");
        write(game / "dxgi.dll", "ReShade 6.3");
        write(game / "winmm.dll", "shroudtopia.dll");
        expect(!detect_optiscaler(game).found(), "ReShade's dxgi.dll and a loader's winmm.dll are not OptiScaler");
        const auto before = detect_wrappers(game);
        write(game / "OptiScaler.ini", "[Menu]\nOverlayMenu=auto\n");
        const auto leftover = detect_optiscaler(game);
        expect(!leftover.found() && leftover.ini_only, "its ini alone (an uninstall's leftover) is not OptiScaler");
        expect(kui::choose_overlay_mode(false, false, leftover.found() && !leftover.coexists(), -1) == kui::OverlayMode::on,
               "a leftover ini does not turn the overlay off");
        std::ofstream(game / "version.dll", std::ios::binary) << opti_bytes;
        auto o = detect_optiscaler(game);
        expect(o.found() && o.file.filename() == "version.dll" && o.ini.read && !o.coexists(), "OptiScaler as version.dll, menu on");
        expect(kui::choose_overlay_mode(false, false, o.found() && !o.coexists(), -1) == kui::OverlayMode::drawing_off,
               "OptiScaler with its menu on: drawing steps aside");
        expect(kui::choose_overlay_mode(false, false, o.found() && !o.coexists(), 1) == kui::OverlayMode::on,
               "KILN_UI_OVERLAY=1 still draws next to it");
        const auto with = detect_wrappers(game, {}, {o.file});
        expect(with.identity != before.identity && !with.streamline, "OptiScaler joins the guard's wrapper identity");
        expect(detect_wrappers(game, {}, {o.file, game / "." / "version.dll"}).identity == with.identity,
               "one file found twice is counted once");
        expect(detect_wrappers(game, {}, {game / "dxgi.dll"}).identity == before.identity,
               "OptiScaler as dxgi.dll, a wrapper name already, is counted once");
        write(game / "OptiScaler.ini", "[Menu]\nOverlayMenu=false\n");
        o = detect_optiscaler(game);
        expect(o.coexists(), "OverlayMenu=false and no frame generation: Kiln draws next to it");
        expect(kui::choose_overlay_mode(false, false, o.found() && !o.coexists(), -1) == kui::OverlayMode::on,
               "its menu off: drawing stays on");
        expect(kui::choose_overlay_mode(false, false, o.found() && !o.coexists(), 0) == kui::OverlayMode::drawing_off,
               "KILN_UI_OVERLAY=0 still turns drawing off");
        write(game / "OptiScaler.ini", "[Menu]\nOverlayMenu=false\n[FrameGen]\nFGInput=upscaler\nFGOutput=fsrfg\n");
        expect(!detect_optiscaler(game).coexists(), "frame generation set: drawing steps aside");
        std::filesystem::remove(game / "version.dll");
        std::filesystem::remove(game / "OptiScaler.ini");
        std::ofstream(game / "OptiScaler.asi", std::ios::binary) << opti_bytes;
        o = detect_optiscaler(game);
        expect(o.found() && !o.ini.read && !o.coexists(), "OptiScaler.asi without an ini: found by its name and contents, menu on");
        std::filesystem::remove(game / "OptiScaler.asi");
        // a loaded copy elsewhere (Ultimate ASI Loader's plugins folder), with its ini next to it (looked up by name: the
        // host's folders are case sensitive, Windows and Wine find any spelling of it)
        const auto plugins = game / "plugins";
        std::filesystem::create_directories(plugins);
        std::ofstream(plugins / "d3d12.dll", std::ios::binary) << opti_bytes;
        write(plugins / "OptiScaler.ini", "[menu]\noverlaymenu=false\n");
        expect(!detect_optiscaler(game).found(), "subfolders are not scanned");
        o = detect_optiscaler(game, {plugins / "d3d12.dll", game / "missing.dll"});
        expect(o.found() && o.file == plugins / "d3d12.dll" && o.coexists(), "a loaded copy counts, with the ini next to it");
        // Windows' own copy of a name: no ini next to it, never read
        const auto system32 = root / "Windows/System32";
        std::filesystem::create_directories(system32);
        std::ofstream(system32 / "dxgi.dll", std::ios::binary) << opti_bytes;
        expect(!detect_optiscaler(root / "empty-game", {system32 / "dxgi.dll"}).found(), "a copy without an ini next to it is not read");
    }
    std::filesystem::remove_all(root);
    std::printf("overlay_guard: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
