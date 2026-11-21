// SPDX-License-Identifier: MIT
// kiln.render's outlines without the game (outline_core.h): a fake renderer with instances (group byte, generation),
// several mods on one entity, the game's own highlight, a despawn, an unload, a world change, the colour groups, the
// palette and the player's kiln.json switch. Build: tests/run.sh.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "../outline_core.h"

namespace {
int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass;
    else {
        ++g_fail;
        std::printf("FAIL: %s\n", what);
    }
}

// The game's renderer: handle = generation << 24 | index; an instance's group byte; SetGroups writes when it differs.
struct FakeRenderer : kout::RendererIo {
    struct Inst {
        uint8_t gen = 1;
        uint8_t group = 0;
        bool alive = true;
    };
    std::map<uint32_t, Inst> inst;  // index -> instance
    uint64_t writes = 0, calls = 0;
    uint32_t make(uint32_t index) {
        Inst& i = inst[index];
        i.alive = true;
        i.group = 0;
        return uint32_t(i.gen) << 24 | index;
    }
    void destroy(uint32_t handle) {
        Inst& i = inst[handle & 0xFFFFFF];
        i.alive = false;
        ++i.gen;
    }
    Inst* find(uint32_t handle) {
        if (!handle) return nullptr;
        auto it = inst.find(handle & 0xFFFFFF);
        if (it == inst.end() || !it->second.alive || it->second.gen != (handle >> 24)) return nullptr;
        return &it->second;
    }
    int group(uint32_t model) override {
        Inst* i = find(model);
        return i ? i->group : -1;
    }
    void set(const uint32_t* models, const uint8_t* groups, size_t n) override {
        ++calls;
        for (size_t k = 0; k < n; ++k)
            if (Inst* i = find(models[k]); i && i->group != groups[k]) i->group = groups[k], ++writes;
    }
    int at(uint32_t handle) { return group(handle); }
};

KilnRenderOutline opts(uint32_t rgba, int32_t prio = 0, uint32_t flags = 0, uint32_t ms = 0) {
    KilnRenderOutline o{};
    o.size = sizeof o;
    o.rgba = rgba;
    o.priority = prio;
    o.flags = flags;
    o.duration_ms = ms;
    return o;
}

KilnRenderOutlineInfo info(kout::Book& b, KilnHandle h) {
    KilnRenderOutlineInfo i{};
    i.size = sizeof i;
    if (b.info(h, &i) != KILN_OK) i.state = 0xFFFF;
    return i;
}

// One client step: the plan on the main thread, the stage's apply, the results back.
struct World {
    kout::Book book;
    FakeRenderer r;
    kout::Applied applied;
    std::unordered_map<KilnEntity, uint32_t> models;
    kout::Book::Slot slots[2];
    kout::Setting setting = kout::kGameFirst;
    uintptr_t renderer = 0x1000;
    kout::Plan last;
    void step() {
        last = book.plan(models, setting, slots);
        std::vector<uint8_t> res;
        kout::apply(r, renderer, last, applied, res, nullptr, true);
        book.results(last.generation, res);
    }
};
}  // namespace

int main() {
    const uint32_t gold = 0xFFC440FFu, green = 0x46FF6EFFu, blue = 0x3070FFFFu;
    // ---- one mod, one chest: shown, then removed and given back ----
    {
        World w;
        const uint32_t chest = w.r.make(7);
        w.models[1001] = chest;
        check(w.book.add(11, 1001, opts(gold), 0, 1) == KILN_OK, "add");
        check(info(w.book, 1).state == KILN_RENDER_OUTLINE_WAITING && info(w.book, 1).reason == KILN_RENDER_OUTLINE_PENDING,
              "a new request waits for the next step");
        w.step();
        check(w.r.at(chest) == 4, "the chest's instance shows Kiln's first group");
        check(info(w.book, 1).state == KILN_RENDER_OUTLINE_SHOWN && info(w.book, 1).group == 4, "state shown, group 4");
        check(w.slots[0].used && w.slots[0].rgba == gold && !w.slots[1].used, "group 4 is gold, group 5 unused");
        const uint64_t writes = w.r.writes;
        w.step();
        check(w.r.writes == writes, "a shown outline is not written again every step");
        check(w.book.remove(1) == KILN_OK && info(w.book, 1).state == KILN_RENDER_OUTLINE_ENDED &&
                  info(w.book, 1).reason == KILN_RENDER_OUTLINE_REMOVED, "remove ends it (REMOVED)");
        w.step();
        check(w.r.at(chest) == 0, "the instance is given back to no highlight");
        check(w.applied.groups.empty(), "the stage forgets it");
        check(w.book.remove(1) == KILN_OK, "removing an ended request again is fine");
        check(w.book.remove(999) == KILN_ERR_NOT_FOUND, "an unknown handle: NOT_FOUND");
    }
    // ---- several mods on one entity: priority, then newest; the loser takes over when the winner goes ----
    {
        World w;
        const uint32_t chest = w.r.make(3);
        w.models[50] = chest;
        w.book.add(1, 50, opts(gold, 0), 0, 10);
        w.book.add(2, 50, opts(green, 5), 0, 11);
        w.book.add(3, 50, opts(blue, 5), 0, 12);
        w.step();
        check(info(w.book, 12).state == KILN_RENDER_OUTLINE_SHOWN, "highest priority, newest wins (blue)");
        check(info(w.book, 11).reason == KILN_RENDER_OUTLINE_OUTSHONE && info(w.book, 10).reason == KILN_RENDER_OUTLINE_OUTSHONE,
              "the others wait, outshone");
        check(w.slots[0].rgba == blue && !w.slots[1].used, "only the winner's colour takes a group");
        w.book.clear(3);
        w.step();
        check(info(w.book, 11).state == KILN_RENDER_OUTLINE_SHOWN, "mod 3 unloads: mod 2 (green) shows");
        check(w.r.at(chest) == 4 || w.r.at(chest) == 5, "the chest keeps a Kiln group");
        const int g = w.r.at(chest);
        check((g == 4 ? w.slots[0] : w.slots[1]).rgba == green, "its group is green now");
        check(info(w.book, 12).state == KILN_RENDER_OUTLINE_ENDED && info(w.book, 12).reason == KILN_RENDER_OUTLINE_REMOVED,
              "clear ends the owner's requests");
        w.book.clear(3);
        check(info(w.book, 12).state == 0xFFFF, "a second clear forgets the owner's ended requests");
    }
    // ---- the game's own highlight goes first; Kiln takes over when it is back to 0 ----
    {
        World w;
        const uint32_t chest = w.r.make(9);
        w.models[77] = chest;
        w.book.add(1, 77, opts(gold), 0, 1);
        w.r.inst[9].group = 2;  // the building cursor hovers it (ValidState)
        w.step();
        check(w.r.at(chest) == 2 && info(w.book, 1).state == KILN_RENDER_OUTLINE_GAME, "the game's ValidState stays, state GAME");
        w.r.inst[9].group = 0;  // the hover ended
        w.step();
        check(w.r.at(chest) == 4 && info(w.book, 1).state == KILN_RENDER_OUTLINE_SHOWN, "then Kiln's group shows");
        w.r.inst[9].group = 1;  // the cursor again (InvalidState) while Kiln's group is on
        w.step();
        check(w.r.at(chest) == 1 && info(w.book, 1).state == KILN_RENDER_OUTLINE_GAME, "a game group over Kiln's wins again");
        w.book.remove(1);
        w.step();
        check(w.r.at(chest) == 1, "ending the outline does not take the game's group away");
        // another tool's group (Greeneye's Find uses 7)
        w.r.inst[9].group = 7;
        w.book.add(1, 77, opts(gold), 0, 2);
        w.step();
        check(w.r.at(chest) == 7 && info(w.book, 2).state == KILN_RENDER_OUTLINE_GAME, "another tool's group is left alone");
    }
    // ---- mods first (kiln.json ui.outlines) ----
    {
        World w;
        w.setting = kout::kModsFirst;
        const uint32_t chest = w.r.make(4);
        w.models[5] = chest;
        w.r.inst[4].group = 2;
        w.book.add(1, 5, opts(gold), 0, 1);
        w.step();
        check(w.r.at(chest) == 4 && info(w.book, 1).state == KILN_RENDER_OUTLINE_SHOWN, "mods-first: Kiln's group over the game's");
        // a partial apply (the stage's later slices) re-applies only the listed targets
        w.r.inst[4].group = 2;
        std::vector<uint8_t> res;
        std::vector<size_t> which{0};
        kout::apply(w.r, w.renderer, w.last, w.applied, res, &which, false);
        check(w.r.at(chest) == 4 && res[0] == kout::kShown, "mods-first: a later slice takes it back");
    }
    // ---- off ----
    {
        World w;
        w.setting = kout::kOff;
        const uint32_t chest = w.r.make(4);
        w.models[5] = chest;
        w.book.add(1, 5, opts(gold), 0, 1);
        w.step();
        check(w.r.at(chest) == 0 && info(w.book, 1).reason == KILN_RENDER_OUTLINE_SWITCHED_OFF, "off: nothing set, SWITCHED_OFF");
        check(!w.slots[0].used && !w.slots[1].used, "off: no group coloured");
        w.setting = kout::kGameFirst;
        w.step();
        check(w.r.at(chest) == 4, "back on: shown");
        w.setting = kout::kOff;
        w.step();
        check(w.r.at(chest) == 0, "switched off while shown: given back");
    }
    // ---- no model, a model made later, a model remade (new handle) ----
    {
        World w;
        w.book.add(1, 42, opts(gold), 0, 1);
        w.step();
        check(info(w.book, 1).state == KILN_RENDER_OUTLINE_WAITING && info(w.book, 1).reason == KILN_RENDER_OUTLINE_NO_MODEL,
              "no model known: NO_MODEL");
        const uint32_t m1 = w.r.make(20);
        w.models[42] = m1;
        w.step();
        check(w.r.at(m1) == 4, "the model appears: shown");
        w.r.destroy(m1);  // hidden or streamed out: the instance is destroyed, the handle is stale
        w.step();
        check(info(w.book, 1).reason == KILN_RENDER_OUTLINE_NO_MODEL, "a stale handle: NO_MODEL, nothing written");
        const uint32_t m2 = w.r.make(20);  // the game makes it again (same index, next generation)
        check(m2 != m1, "a new generation");
        w.models[42] = m2;
        w.step();
        check(w.r.at(m2) == 4 && info(w.book, 1).state == KILN_RENDER_OUTLINE_SHOWN, "the new model shows it");
    }
    // ---- despawn, world change, owner revoked, expiry ----
    {
        World w;
        const uint32_t a = w.r.make(1), b = w.r.make(2), c = w.r.make(3);
        w.models[100] = a, w.models[200] = b, w.models[300] = c;
        w.book.add(1, 100, opts(gold), 0, 1);
        w.book.add(2, 200, opts(gold), 0, 2);
        w.book.add(3, 300, opts(gold, 0, 0, 500), 1000, 3);
        w.step();
        check(w.r.at(a) == 4 && w.r.at(b) == 4 && w.r.at(c) == 4, "three outlines shown");
        w.book.end_entity(100, KILN_RENDER_OUTLINE_DESPAWNED);
        w.r.destroy(a);
        w.models.erase(100);
        w.step();
        check(info(w.book, 1).reason == KILN_RENDER_OUTLINE_DESPAWNED && w.applied.groups.size() == 2, "despawn ends it");
        w.book.end_owner(2, KILN_RENDER_OUTLINE_OWNER);
        w.step();
        check(info(w.book, 2).reason == KILN_RENDER_OUTLINE_OWNER && w.r.at(b) == 0, "owner revoked: ended, given back");
        w.book.expire(1499);
        check(info(w.book, 3).state != KILN_RENDER_OUTLINE_ENDED, "not expired at 499 ms");
        w.book.expire(1500);
        check(info(w.book, 3).reason == KILN_RENDER_OUTLINE_EXPIRED, "expired at 500 ms");
        w.step();
        check(w.r.at(c) == 0, "an expired outline is given back");
        // world change: the stage meets another renderer; what Kiln set in the old one is forgotten, not written
        w.book.add(4, 300, opts(gold), 0, 4);
        w.step();
        check(w.r.at(c) == 4, "shown again");
        w.book.end_all(KILN_RENDER_OUTLINE_WORLD);
        check(info(w.book, 4).reason == KILN_RENDER_OUTLINE_WORLD, "the client world ended: WORLD");
        FakeRenderer other;
        kout::Plan p = w.book.plan(w.models, w.setting, w.slots);
        std::vector<uint8_t> res;
        kout::apply(other, 0x2000, p, w.applied, res, nullptr, true);
        check(w.applied.groups.empty() && other.calls == 0, "a new renderer: nothing restored into it");
        // the next world's renderer at the old address: the session tells them apart
        w.book.add(5, 300, opts(gold), 0, 5);
        const uint32_t c2 = w.r.make(3);
        w.models[300] = c2;
        p = w.book.plan(w.models, w.setting, w.slots, 1);
        kout::apply(w.r, w.renderer, p, w.applied, res, nullptr, true);
        check(w.r.at(c2) == 4 && w.applied.session == 1, "session 1 shown");
        w.book.end_all(KILN_RENDER_OUTLINE_WORLD);
        w.r.inst[3].group = 4;  // the new world's instance at the same handle shows group 4 (say, another outline of it)
        p = w.book.plan(w.models, w.setting, w.slots, 2);
        const uint64_t before = w.r.writes;
        kout::apply(w.r, w.renderer, p, w.applied, res, nullptr, true);
        check(w.r.writes == before && w.applied.groups.empty(), "session 2 at the same renderer address: nothing of session 1 restored");
    }
    // ---- colours: two groups, a third pair shares the nearest; pairs keep their group ----
    {
        World w;
        for (uint32_t i = 1; i <= 4; ++i) w.models[i] = w.r.make(i);
        w.book.add(1, 1, opts(gold, 3), 0, 1);
        w.book.add(1, 2, opts(green, 2), 0, 2);
        w.step();
        const int g_gold = w.r.at(w.models[1]), g_green = w.r.at(w.models[2]);
        check(g_gold != g_green && (g_gold == 4 || g_gold == 5) && (g_green == 4 || g_green == 5), "two colours, two groups");
        w.book.add(1, 3, opts(0xFFD040FFu, 1), 0, 3);  // nearly gold, lowest priority
        w.step();
        check(w.r.at(w.models[3]) == g_gold && info(w.book, 3).merged == 1, "a third colour shares the nearest group (merged)");
        check(w.r.at(w.models[1]) == g_gold && w.r.at(w.models[2]) == g_green, "the first two keep their groups");
        w.book.add(1, 4, opts(green, 0, KILN_RENDER_OUTLINE_PULSE), 0, 4);  // green but pulsing: another pair
        w.step();
        check(info(w.book, 4).merged == 1 && info(w.book, 1).merged == 0, "a pulsing green is its own pair (merged here)");
        w.book.remove(1);  // gold goes: the near-gold pair and the pulsing green compete for gold's group
        w.step();
        check(info(w.book, 2).merged == 0 && w.r.at(w.models[2]) == g_green, "green keeps its group");
        check(info(w.book, 3).merged == 0 && w.r.at(w.models[3]) == g_gold, "near-gold (priority 1) takes gold's free group");
        check(info(w.book, 4).merged == 1, "the pulsing green (priority 0) still shares");
    }
    // ---- palette ----
    {
        check(kout::to_game_color(0xFF000080u) == 0x800000FFu, "RGBA red -> the game's AABBGGRR");
        const uint64_t on = kout::slot_word(0x40C4FFFFu, false);
        check(kout::palette_value(0, 0x11223344u, 0) == 0x11223344u, "an unused group passes the game's colour through");
        check(kout::palette_value(on, 0, 123) == kout::to_game_color(0x40C4FFFFu), "a used group gets Kiln's colour");
        const uint64_t pulse = kout::slot_word(0xC8C8C8FFu, true);
        uint32_t lo = 255, hi = 0;
        for (uint64_t t = 0; t < kout::kPulseMs; t += 10) {
            const uint32_t v = kout::palette_value(pulse, 0, t);
            lo = std::min(lo, v & 0xFF), hi = std::max(hi, v & 0xFF);
            if ((v >> 24) != 0xFF) check(false, "a pulse keeps alpha");
        }
        check(lo >= 79 && lo <= 81 && hi >= 199 && hi <= 200, "a pulse breathes from 40 % to 100 %");
    }
    // ---- the player's switch ----
    {
        check(kout::parse_setting("") == kout::kGameFirst, "no kiln.json: game first");
        check(kout::parse_setting(R"({"ui": {"outlines": "mods-first"}})") == kout::kModsFirst, "mods-first");
        check(kout::parse_setting(R"({ // comment
            "ui": {"outlines": "off"}})") == kout::kOff, "off, with a comment");
        check(kout::parse_setting(R"({"ui": {"outlines": 3}})") == kout::kGameFirst, "a wrong type: game first");
        check(kout::parse_setting("{not json") == kout::kGameFirst, "broken json: game first");
    }
    // ---- budget, kept ended requests ----
    {
        kout::Book b;
        for (uint32_t i = 0; i < kout::kMaxPerOwner; ++i) b.add(9, 1 + i, opts(gold), 0, 1 + i);
        check(b.add(9, 5000, opts(gold), 0, 5000) == KILN_ERR_BUFFER_TOO_SMALL, "1024 live requests per owner");
        check(b.add(8, 5000, opts(gold), 0, 5001) == KILN_OK, "another owner is not limited by it");
        b.end_owner(9, KILN_RENDER_OUTLINE_OWNER);
        check(b.add(9, 5000, opts(gold), 0, 5002) == KILN_OK, "ended requests do not count");
        for (uint32_t i = 0; i < kout::kEndedKeep + 10; ++i) {
            b.add(7, 1, opts(gold), 0, 10000 + i);
            b.remove(10000 + i);
        }
        check(info(b, 1).state == 0xFFFF && info(b, 10000 + kout::kEndedKeep + 9).state == KILN_RENDER_OUTLINE_ENDED,
              "only the newest 4096 ended requests are kept");
        const auto c = b.counts();
        check(c.requests == 2 && c.entities == 1, "counts");
    }
    std::printf("outline_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
