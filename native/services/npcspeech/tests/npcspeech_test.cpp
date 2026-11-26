// SPDX-License-Identifier: MIT
// kiln.npcspeech core on fake game structs: the bubble params (text +0, show +4, reset +5) and a dialogue record with
// the game's two greeting lists (entries of 0x294 bytes, 32 inline voice-over GUIDs at +0x8c), as on client 26430625.
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "npcspeech_core.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c, msg)                                   \
    do {                                                \
        if (c) {                                        \
            ++g_pass;                                   \
        } else {                                        \
            ++g_fail;                                   \
            std::printf("FAIL %s (line %d)\n", msg, __LINE__); \
        }                                               \
    } while (0)

namespace {
npcspeech::Layout layout() {
    npcspeech::Layout l;
    l.prio_list = 0x90;
    l.prio_count = 0x98;
    l.plain_list = 0x5338;
    l.plain_count = 0x5340;
    l.entry_size = 0x294;
    l.entry_vo = 0x8c;
    l.vo_slots = 32;
    return l;
}

struct Params {
    uint8_t b[0x20]{};
    uint32_t text() const { uint32_t t; std::memcpy(&t, b, 4); return t; }
    void set(uint32_t t, uint8_t show) { std::memcpy(b, &t, 4); b[4] = show; b[5] = 0; }
};

struct Record {
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x14b80);
    std::vector<uint8_t> prio, plain;
    Record(int nprio, int nplain) : prio(0x294 * (nprio ? nprio : 1)), plain(0x294 * (nplain ? nplain : 1)) {
        uint8_t* p = prio.data();
        uint8_t* q = plain.data();
        uint64_t a = nprio, b = nplain;
        std::memcpy(&mem[0x90], &p, 8);
        std::memcpy(&mem[0x98], &a, 8);
        std::memcpy(&mem[0x5338], &q, 8);
        std::memcpy(&mem[0x5340], &b, 8);
        for (int i = 0; i < nprio; ++i) std::memset(&prio[i * 0x294 + 0x8c], 0x10 + i, 16);
        for (int i = 0; i < nplain; ++i) std::memset(&plain[i * 0x294 + 0x8c], 0x40 + i, 16);
    }
    uint8_t vo_byte(bool pl, int i) const { return (pl ? plain : prio)[i * 0x294 + 0x8c]; }
};
}  // namespace

int main() {
    const auto l = layout();
    CHECK(l.voice_sane(), "the game's layout is sane");
    npcspeech::Layout bad = l;
    bad.entry_vo = 0x290;
    CHECK(!bad.voice_sane(), "a voice-over array past the entry is refused");

    const uint32_t tags[] = {0xa1, 0xa2, 0xa3};
    CHECK(npcspeech::Core::valid_set(7, KILN_SPEECH_REPLACE, tags, 3), "replace with tags");
    CHECK(!npcspeech::Core::valid_set(7, KILN_SPEECH_REPLACE, tags, 0), "replace without tags");
    CHECK(!npcspeech::Core::valid_set(0, KILN_SPEECH_SUPPRESS, nullptr, 0), "entity 0");
    CHECK(!npcspeech::Core::valid_set(7, 3, nullptr, 0), "unknown mode");
    const uint32_t zero_tag[] = {0xa1, 0};
    CHECK(!npcspeech::Core::valid_set(7, KILN_SPEECH_REPLACE, zero_tag, 2), "a zero tag");

    {  // nothing set: the game's bubble is untouched
        npcspeech::Core c;
        Params p;
        p.set(2262475715u, 1);
        CHECK(!c.active(), "idle core is inactive");
        c.draw(7, p.b, l, 1000);
        CHECK(p.text() == 2262475715u && p.b[4] == 1, "no override: the game's line");
    }
    {  // Suppress hides, another NPC is untouched, clearing gives the game back
        npcspeech::Core c;
        c.set(7, KILN_SPEECH_SUPPRESS, nullptr, 0);
        CHECK(c.active(), "suppress makes it active");
        Params p, q;
        p.set(2262475715u, 1);
        q.set(3935509066u, 1);
        c.draw(7, p.b, l, 1000);
        c.draw(8, q.b, l, 1000);
        CHECK(p.b[4] == 0, "suppressed NPC's bubble hidden");
        CHECK(q.b[4] == 1 && q.text() == 3935509066u, "other NPC untouched");
        c.set(7, KILN_SPEECH_GAME, nullptr, 0);
        p.set(2262475715u, 1);
        c.draw(7, p.b, l, 1000);
        CHECK(p.b[4] == 1, "cleared: shown again");
        CHECK(c.tracked() == 0 && !c.active(), "cleared NPC forgotten");
    }
    {  // Replace: a game pick becomes one of ours, stable while the game's pick stays; nothing when the game shows nothing
        npcspeech::Core c;
        c.set(7, KILN_SPEECH_REPLACE, tags, 3);
        Params p;
        p.set(2262475715u, 1);
        c.draw(7, p.b, l, 1000);
        const uint32_t first = p.text();
        CHECK(first == 0xa1 || first == 0xa2 || first == 0xa3, "replaced by one of ours");
        for (int i = 0; i < 20; ++i) {
            p.set(2262475715u, 1);
            c.draw(7, p.b, l, 1000 + i);
            if (p.text() != first) break;
        }
        CHECK(p.text() == first, "same game pick, same line of ours");
        p.set(0, 0);
        c.draw(7, p.b, l, 2000);
        CHECK(p.text() == 0 && p.b[4] == 0, "no game pick: nothing shown");
        bool varied = false;
        uint32_t last = first;
        for (int i = 0; i < 40 && !varied; ++i) {
            p.set(i % 2 ? 2262475715u : 3935509066u, 1);
            c.draw(7, p.b, l, 3000 + i);
            varied = p.text() != last;
            last = p.text();
        }
        CHECK(varied, "a new game pick can bring another line");
    }
    {  // Say: over an override, shown even when the game hides the bubble, ends at its time
        npcspeech::Core c;
        c.set(7, KILN_SPEECH_SUPPRESS, nullptr, 0);
        c.say(7, 0xbeef, 5000, 1000);
        Params p;
        p.set(0, 0);
        c.draw(7, p.b, l, 2000);
        CHECK(p.text() == 0xbeef && p.b[4] == 1, "Say shows over Suppress, also when the game hides it");
        p.set(0, 0);
        c.draw(7, p.b, l, 6001);
        CHECK(p.b[4] == 0, "after its time: back to Suppress");
        c.set(7, KILN_SPEECH_GAME, nullptr, 0);
        c.say(9, 0xcafe, 1000, 0);
        c.say(9, 0, 0, 10);
        p.set(5, 1);
        c.draw(9, p.b, l, 20);
        CHECK(p.text() == 5, "Say ended by tag 0");
    }
    {  // voice: mute both greeting lists while speaking, put them back after
        npcspeech::Core c;
        Record r(2, 3);
        c.set(7, KILN_SPEECH_REPLACE, tags, 1);
        c.voice(7, r.mem.data(), l);
        CHECK(r.vo_byte(false, 0) == 0 && r.vo_byte(false, 1) == 0 && r.vo_byte(true, 2) == 0, "muted");
        // the game rebuilds the record (refills a voice-over): muted again, first copy kept
        std::memset(&r.plain[1 * 0x294 + 0x8c], 0x41, 16);
        c.voice(7, r.mem.data(), l);
        CHECK(r.vo_byte(true, 1) == 0, "refilled slot muted again");
        c.set(7, KILN_SPEECH_GAME, nullptr, 0);
        CHECK(c.active(), "still active until the voice-overs are back");
        c.voice(7, r.mem.data(), l);
        CHECK(r.vo_byte(false, 0) == 0x10 && r.vo_byte(false, 1) == 0x11, "priority back");
        CHECK(r.vo_byte(true, 0) == 0x40 && r.vo_byte(true, 1) == 0x41 && r.vo_byte(true, 2) == 0x42, "plain back");
        CHECK(!c.active() && c.tracked() == 0, "forgotten after restore");
    }
    {  // a list that does not look like one is left alone; clear_all keeps the restore
        npcspeech::Core c;
        Record r(1, 1);
        uint64_t huge = 100000;
        std::memcpy(&r.mem[0x5340], &huge, 8);
        c.set(7, KILN_SPEECH_SUPPRESS, nullptr, 0);
        c.voice(7, r.mem.data(), l);
        CHECK(r.vo_byte(true, 0) == 0x40, "an implausible count is not walked");
        CHECK(r.vo_byte(false, 0) == 0, "the other list muted");
        c.clear_all();
        CHECK(c.active(), "clear_all keeps the NPC whose voice-overs are muted");
        c.voice(7, r.mem.data(), l);
        CHECK(r.vo_byte(false, 0) == 0x10 && !c.active(), "restored after clear_all");
        npcspeech::Layout none;
        c.set(8, KILN_SPEECH_SUPPRESS, nullptr, 0);
        Record r2(1, 1);
        c.voice(8, r2.mem.data(), none);
        CHECK(r2.vo_byte(false, 0) == 0x10, "unknown layout: the record is not touched");
    }
    {  // a record the NPC no longer has is not written to
        npcspeech::Core c;
        Record a(1, 1), b(1, 1);
        c.set(7, KILN_SPEECH_SUPPRESS, nullptr, 0);
        c.voice(7, a.mem.data(), l);
        c.set(7, KILN_SPEECH_GAME, nullptr, 0);
        c.voice(7, b.mem.data(), l);
        CHECK(a.vo_byte(false, 0) == 0 && b.vo_byte(false, 0) == 0x10, "restore only into the record that was muted");
    }
    {  // concurrent writers and a drawing thread
        npcspeech::Core c;
        std::thread w([&] {
            for (int i = 0; i < 20000; ++i) {
                c.set(1 + i % 5, i % 3, tags, 3);
                c.say(1 + i % 7, i % 2 ? 0xbeef : 0, 1000, i);
            }
        });
        Params p;
        for (int i = 0; i < 20000; ++i) {
            p.set(42, 1);
            c.draw(1 + i % 5, p.b, l, i);
        }
        w.join();
        c.clear_all();
        CHECK(!c.active(), "clear_all after the race");
    }
    const auto k = npcspeech::Core().counts();
    CHECK(k.overrides == 0 && k.says == 0, "fresh counts");
    std::printf("npcspeech core: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
