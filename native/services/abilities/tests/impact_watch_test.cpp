// SPDX-License-Identifier: MIT
// Host tests of kiln.abilities' impact pool watch (impact_watch.h, since 0.2.0-dev.29.19; inbox 116): the layout check,
// the threshold crossings with their hysteresis, the per-program counts and their top five, the oldest-K pick and the
// lock-free age table (insert, overwrite, clear, full, wrap), and the control file's words.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "impact_watch.h"

using namespace kab::iw;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_fail;
    }
}

constexpr uint64_t MiB = 1024 * 1024;

static void test_layout() {
    Reading r{1 * MiB, 2 * MiB, 16 * MiB, 10, 20};
    check(check_layout(r) == Layout::Ok, "a 16 MiB pool at 1 MiB used is sane");
    check(check_layout(Reading{0, 0, 16 * MiB, 0, 0}) == Layout::Ok, "an empty pool is sane");
    check(check_layout(Reading{16 * MiB, 16 * MiB, 16 * MiB, 1144, 1144}) == Layout::Ok, "a full pool is sane");
    check(check_layout(Reading{0, 0, 32 * MiB, 0, 0}) == Layout::Ok, "another power of two (32 MiB) is sane");
    check(check_layout(Reading{0, 0, 0, 0, 0}) == Layout::BadCapacity, "capacity 0 fails");
    check(check_layout(Reading{0, 0, 16 * MiB + 8, 0, 0}) == Layout::BadCapacity, "a capacity that is no power of two fails");
    check(check_layout(Reading{0, 0, 512 * 1024, 0, 0}) == Layout::BadCapacity, "below 1 MiB fails");
    check(check_layout(Reading{0, 0, 2048 * MiB, 0, 0}) == Layout::BadCapacity, "above 1 GiB fails");
    check(check_layout(Reading{0, 0, 1024 * MiB, 0, 0}) == Layout::Ok, "1 GiB is the largest sane capacity");
    check(check_layout(Reading{0, 0, 1 * MiB, 0, 0}) == Layout::Ok, "1 MiB is the smallest sane capacity");
    check(check_layout(Reading{16 * MiB + 1, 16 * MiB + 1, 16 * MiB, 0, 0}) == Layout::UsedOverCapacity, "used over capacity fails");
    check(check_layout(Reading{2 * MiB, 1 * MiB, 16 * MiB, 0, 0}) == Layout::PeakUnderUsed, "peak under used fails");
    check(check_layout(Reading{1 * MiB, 17 * MiB, 16 * MiB, 0, 0}) == Layout::UsedOverCapacity, "peak over capacity fails");
    check(check_layout(Reading{1 * MiB, 1 * MiB, 16 * MiB, 21, 20}) == Layout::LiveOverPeak, "live over its peak fails");
    check(check_layout(Reading{1 * MiB, 1 * MiB, 16 * MiB, 0, 1u << 24}) == Layout::LiveOverPeak, "an absurd instance peak fails");
    check(check_layout(Reading{1 * MiB, 1 * MiB, 16 * MiB, 0, (1u << 24) - 1}) == Layout::Ok, "an instance peak under 2^24 is sane");
    check(std::strlen(layout_text(Layout::LiveOverPeak)) > 0 && std::strcmp(layout_text(Layout::Ok), "ok") == 0, "layout texts");
    check(used_pct(Reading{4 * MiB, 8 * MiB, 16 * MiB, 0, 0}) == 25.0 && peak_pct(Reading{4 * MiB, 8 * MiB, 16 * MiB, 0, 0}) == 50.0,
          "percentages of capacity");
    check(used_pct(Reading{4 * MiB, 8 * MiB, 0, 0, 0}) == 0.0, "no capacity: 0 %");
}

static void test_thresholds() {
    Thresholds t;
    check(t.step(10) == kNone && t.step(49.99) == kNone, "under 50 %: nothing");
    check(t.step(50) == kWarn50, "50 % warns once");
    check(t.step(60) == kNone && t.step(50) == kNone, "and not again");
    check(t.step(30) == kNone, "a drop before 75 % says nothing");
    check(t.step(55) == kNone, "crossing 50 % again in the same session says nothing");
    check(t.step(75) == kWarn75, "75 % warns once");
    check(t.step(74) == kNone && t.step(80) == kNone, "and not again");
    check(t.step(40) == kNone, "40 % is not under 40 %");
    check(t.step(39.9) == kRecovered, "under 40 % after 75 %: recovered, once");
    check(t.step(10) == kNone && t.step(39) == kNone, "recovered only once");
    check(t.step(90) == kWarn90, "90 % warns once, also after a recovery");
    check(t.step(20) == kNone, "no second recovery line in the session");
    Thresholds j;
    check(j.step(95) == (kWarn50 | kWarn75 | kWarn90), "a jump past every level reports each of them");
    check(highest(kWarn50 | kWarn75 | kWarn90) == 90 && highest(kWarn50) == 50 && highest(kWarn75) == 75 && highest(kNone) == 0,
          "the highest level crossed");
    j.reset();
    check(j.step(51) == kWarn50, "reset: a new session warns again");
    Thresholds k;
    check(k.step(76) == (kWarn50 | kWarn75) && k.step(39) == kRecovered, "recovery after 75 % reached in one jump");
}

static void test_counts() {
    ProgramCounts<8> c;
    for (int i = 0; i < 5; ++i) c.add(0xAAAA);
    for (int i = 0; i < 3; ++i) c.add(0xBBBB);
    c.add(0xCCCC);
    for (int i = 0; i < 3; ++i) c.add(0x1111);
    c.add(0);  // no program: counted like any other id
    Top top[5];
    const size_t n = c.top(top, 5);
    check(n == 5, "five distinct programs");
    check(top[0].id == 0xAAAA && top[0].count == 5, "the most live instances first");
    check(top[1].id == 0x1111 && top[1].count == 3 && top[2].id == 0xBBBB && top[2].count == 3, "ties: the lower id first");
    check(top[3].count == 1 && top[4].count == 1 && top[3].id == 0 && top[4].id == 0xCCCC, "the singles last, by id");
    check(c.total() == 13 && c.distinct() == 5 && c.overflow() == 0, "totals");
    Top two[2];
    check(c.top(two, 2) == 2 && two[0].id == 0xAAAA && two[1].id == 0x1111, "top 2 of 5");
    ProgramCounts<4> small;
    for (uint32_t id = 1; id <= 6; ++id) small.add(id);
    check(small.distinct() == 4 && small.overflow() == 2 && small.total() == 6, "a full table counts the overflow");
    small.add(1);
    Top one[1];
    check(small.top(one, 1) == 1 && one[0].id == 1 && one[0].count == 2, "an id already in a full table still counts");
    small.clear();
    check(small.distinct() == 0 && small.total() == 0 && small.overflow() == 0 && small.top(one, 1) == 0, "clear");
}

static void test_oldest() {
    OldestK<3> o;
    o.offer(10, 500);
    o.offer(11, 100);
    o.offer(12, kUnknownAge);
    o.offer(13, 300);
    o.offer(14, 50);
    check(o.size() == 3, "keeps K");
    check(o.at(0).key == 14 && o.at(1).key == 11 && o.at(2).key == 13, "the oldest (smallest creation time) first");
    OldestK<3> u;
    u.offer(1, kUnknownAge);
    u.offer(2, 7);
    u.offer(3, kUnknownAge);
    u.offer(4, kUnknownAge);
    check(u.size() == 3 && u.at(0).key == 2 && u.at(1).key == 1 && u.at(2).key == 3, "unknown ages after known ones, first seen first");
    u.clear();
    check(u.size() == 0, "clear");
}

static void test_age_table() {
    AgeTable<16, 4> t;
    check(t.find(key_of(1, 0x01000005)) == nullptr, "empty: not found");
    check(t.insert_id(key_of(1, 0x01000005), 0x01000005, 1000), "insert");
    const auto* e = t.find(key_of(1, 0x01000005));
    check(e && stamp_id(e->stamp.load()) == 0x01000005 && stamp_ms(e->stamp.load()) == 1000, "the full id and the time");
    // the same slot reused with a new generation: one entry, the newer instance
    check(t.insert_id(key_of(1, 0x02000005), 0x02000005, 2000), "reuse");
    e = t.find(key_of(1, 0x02000005));
    check(e && stamp_id(e->stamp.load()) == 0x02000005 && stamp_ms(e->stamp.load()) == 2000 && t.used() == 1,
          "a reused slot overwrites its entry");
    check(key_of(1, 0x01000005) == key_of(1, 0x02000005) && key_of(1, 5) != key_of(2, 5), "the key: system and slot, no generation");
    // full: 16 slots, probe 4 -> keys pile into one probe window
    AgeTable<16, 4> f;
    std::vector<uint64_t> same;
    for (uint32_t id = 1; same.size() < 6 && id < 100000; ++id)
        if (AgeTable<16, 4>::home(key_of(1, id)) == 3) same.push_back(key_of(1, id));
    check(same.size() == 6, "found keys with one home");
    for (int i = 0; i < 4; ++i) check(f.insert(same[i], 10 + i), "fills the probe window");
    check(!f.insert(same[4], 99) && f.full() == 1, "a fifth key with the same home: full, counted");
    check(f.find(same[4]) == nullptr && f.find(same[3]) != nullptr, "the refused key is not there; the others are");
    // clear one system: tombstones that inserts reuse and lookups skip
    f.clear_system(1);
    check(f.find(same[0]) == nullptr && f.used() == 0, "clear_system removes that system's entries");
    check(f.insert(same[4], 5) && f.find(same[4]) && stamp_ms(f.find(same[4])->stamp.load()) == 5, "a tombstone is reused");
    // remove keeps the probe chain: an entry after a removed one is still found
    AgeTable<16, 4> r;
    for (int i = 0; i < 3; ++i) r.insert(same[i], 100 + i);
    r.remove(same[0]);
    check(r.find(same[0]) == nullptr && r.find(same[2]) && stamp_ms(r.find(same[2])->stamp.load()) == 102, "remove keeps the chain");
    check(r.insert(same[2], 7) && r.used() == 2, "re-inserting an existing key after a tombstone does not duplicate it");
    // wrap: a home at the last slot probes on at slot 0
    AgeTable<16, 4> w;
    std::vector<uint64_t> last;
    for (uint32_t id = 1; last.size() < 3 && id < 100000; ++id)
        if (AgeTable<16, 4>::home(key_of(2, id)) == 15) last.push_back(key_of(2, id));
    check(last.size() == 3, "found keys whose home is the last slot");
    for (int i = 0; i < 3; ++i) check(w.insert(last[i], 1 + i), "insert at the end");
    check(w.slot_of(last[1]) == 0 && w.slot_of(last[2]) == 1, "the probe wraps to slot 0");
    check(w.find(last[2]) && stamp_ms(w.find(last[2])->stamp.load()) == 3, "and is found there");
    // other system's entries survive a clear
    w.insert(key_of(1, 77), 9);
    w.clear_system(2);
    check(w.find(key_of(1, 77)) && w.find(last[0]) == nullptr, "clear_system leaves the other system alone");
    // age arithmetic wraps with the 32-bit millisecond clock
    check(age_ms(0xFFFFFF00u, 0x100u) == 0x200u && age_ms(1000, 3500) == 2500, "age across the clock's wrap");
}

static void test_age_table_threads() {
    static AgeTable<4096, 64> t;
    std::vector<std::thread> th;
    for (uint32_t s = 0; s < 4; ++s)
        th.emplace_back([s] {
            for (uint32_t i = 0; i < 500; ++i) t.insert(key_of(1 + (s & 1), s * 1000 + i), s * 100000 + i);
        });
    for (auto& x : th) x.join();
    uint32_t found = 0;
    for (uint32_t s = 0; s < 4; ++s)
        for (uint32_t i = 0; i < 500; ++i)
            if (const auto* e = t.find(key_of(1 + (s & 1), s * 1000 + i)))
                found += stamp_ms(e->stamp.load()) == s * 100000 + i;
    check(found == 2000 && t.full() == 0, "four writers: every entry there once, with its own time");
}

static void test_control() {
    check(parse_control("dump") == Control::Dump && parse_control("dump\r\n") == Control::Dump && parse_control("  DUMP ") == Control::Dump,
          "dump");
    check(parse_control("off") == Control::Off && parse_control("Off\n") == Control::Off, "off");
    check(parse_control("") == Control::None && parse_control("on") == Control::None && parse_control("dumpster") == Control::None &&
              parse_control("offset") == Control::None,
          "anything else: nothing");
}

int main() {
    test_layout();
    test_thresholds();
    test_counts();
    test_oldest();
    test_age_table();
    test_age_table_threads();
    test_control();
    if (g_fail) {
        std::printf("impact_watch_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::puts("impact_watch_test: layout check, thresholds, top programs, oldest pick, age table, control words");
    return 0;
}
