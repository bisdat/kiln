// SPDX-License-Identifier: MIT
// kiln.items v9 host test (0.2.0-dev.30.3, GitHub issue #112): renaming a container without the game. The text check
// (what the game's rename message and text field take) and the rename steps of rename_logic.h against a fake game whose
// string table behaves like the server's set_string (docs/re/items.md section 14): ids are the FNV-1a 64 of the text,
// records are shared and counted, the old id is released first, an empty text returns id 0, the same text returns at
// once, a full table refuses with error 0x27 (after releasing the old id), and a text past the record's 256 bytes
// crashes the server (the fake records it; no request may ever get there).
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "../rename_logic.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

using namespace kitems::rename;

int32_t text_status(const std::string& s) { return check_text(s.data(), static_cast<uint32_t>(s.size()), nullptr, 0); }

// ---- the fake game ----
struct Ent {
    bool exists = true;
    int kind = 1;  // 0 no inventory, 1 container, 2 player, 3 NPC
    bool has = false;
    uint64_t id = 0;
};
struct Rec {
    std::string text;
    uint32_t refs = 0;
};
struct Game {
    std::map<uint32_t, Ent> ents;
    std::map<uint64_t, Rec> table;
    uint32_t limit = 1024;
    bool exists_known = true;
    // what the game's calls did
    int set_string_calls = 0, writes = 0, adds = 0;
    int tracked_at = -1;             // set_string calls made when the tracked write ran
    bool store_after_track = false;  // the last store came after exactly one set_string following the tracked write
    bool crashed = false;
    // fault injection
    bool fault_set_string = false, fault_write = false, wrong_id = false;
    uint8_t force_error = 0;

    std::string name(uint32_t e) const {  // what container_name would read: the component's id, then the table
        auto it = ents.find(e);
        if (it == ents.end() || !it->second.has || it->second.id == 0) return "";
        auto r = table.find(it->second.id);
        return r == table.end() ? "<dangling>" : r->second.text;
    }
};

Game* G(void* c) { return static_cast<Game*>(c); }
int f_exists(void* c, KilnEntity e) {
    if (!G(c)->exists_known) return -1;
    auto it = G(c)->ents.find(e);
    return it != G(c)->ents.end() && it->second.exists ? 1 : 0;
}
int f_container(void* c, KilnEntity e) {
    auto it = G(c)->ents.find(e);
    return it != G(c)->ents.end() && it->second.exists && it->second.kind == 1 ? 1 : 0;
}
bool f_name_id(void* c, KilnEntity e, bool* has, uint64_t* id) {
    auto& en = G(c)->ents[e];
    *has = en.has;
    *id = en.id;
    return true;
}
bool f_table(void* c, uint64_t id, bool* holds, uint32_t* used, uint32_t* limit) {
    *holds = G(c)->table.count(id) != 0;
    *used = static_cast<uint32_t>(G(c)->table.size());
    *limit = G(c)->limit;
    return true;
}
// set_string as the server's 0x915b80 does it
bool f_set_string(void* c, uint64_t old_id, const char* text, uint32_t len, uint8_t* error, uint64_t* id) {
    Game& g = *G(c);
    ++g.set_string_calls;
    if (g.fault_set_string) return false;
    *error = 0;
    const uint64_t want = text_id(text, len);
    if (want == old_id) {  // early out: the same text
        *id = old_id;
        return true;
    }
    if (auto r = g.table.find(old_id); r != g.table.end() && --r->second.refs == 0) g.table.erase(r);
    if (len == 0) {
        *id = 0;
        return true;
    }
    if (len > 256) {  // the record's growth callback is null: the server stops
        g.crashed = true;
        *id = 0;
        return true;
    }
    if (g.force_error) {
        *error = g.force_error;
        *id = 0;
        return true;
    }
    if (auto r = g.table.find(want); r != g.table.end()) {
        ++r->second.refs;
    } else {
        if (g.table.size() >= g.limit) {
            *error = 0x27;
            *id = 0;
            return true;
        }
        g.table[want] = Rec{std::string(text, len), 1};
    }
    *id = g.wrong_id ? want ^ 1 : want;
    return true;
}
// the handler's tracked write: marks the component written and hands out where its id is, before set_string
bool f_track(void* c, KilnEntity e, void** slot) {
    Game& g = *G(c);
    if (g.fault_write) return false;
    auto& en = g.ents[e];
    if (!en.has) return false;
    g.tracked_at = g.set_string_calls;
    *slot = &en.id;
    return true;
}
bool f_store(void* c, void* slot, uint64_t id) {
    Game& g = *G(c);
    *static_cast<uint64_t*>(slot) = id;
    ++g.writes;
    g.store_after_track = g.tracked_at >= 0 && g.set_string_calls == g.tracked_at + 1;
    return true;
}
bool f_add_id(void* c, KilnEntity e, uint64_t id) {
    Game& g = *G(c);
    auto& en = g.ents[e];
    en.has = true;
    en.id = id;
    ++g.adds;
    return true;
}

RenameOps ops(Game* g) {
    RenameOps o{};
    o.ctx = g;
    o.exists = &f_exists;
    o.container = &f_container;
    o.name_id = &f_name_id;
    o.table = &f_table;
    o.set_string = &f_set_string;
    o.track = &f_track;
    o.store = &f_store;
    o.add_id = &f_add_id;
    return o;
}

Outcome rn(Game& g, uint32_t e, const std::string& s) { return run(ops(&g), e, s.data(), static_cast<uint32_t>(s.size())); }

void texts() {
    check(text_id("", 0) == 0xcbf29ce484222325ull && text_id("a", 1) == 0xaf63dc4c8601ec8cull, "text_id is FNV-1a 64 (reference vectors)");
    check(text_status("") == KILN_ITEMS_RENAME_DONE, "an empty name is taken (it clears the name)");
    check(text_status("Ores") == KILN_ITEMS_RENAME_DONE && text_status("Wood & Stone #2 (big)") == KILN_ITEMS_RENAME_DONE,
          "plain text with spaces and punctuation is taken");
    check(text_status("Erz \xC3\xA9\xC3\xA4") == KILN_ITEMS_RENAME_DONE && text_status("\xE6\x9C\xA8\xE6\x9D\x90") == KILN_ITEMS_RENAME_DONE &&
              text_status("\xF0\x9F\x94\xA5 Fire") == KILN_ITEMS_RENAME_DONE,
          "UTF-8 of any script is taken: accents, CJK, a 4-byte character");
    check(text_status(std::string(48, 'a')) == KILN_ITEMS_RENAME_DONE && text_status(std::string(49, 'a')) == KILN_ITEMS_RENAME_TOO_LONG,
          "48 characters, the Rename field's maximum, are taken; 49 are too long");
    auto rep = [](const char* ch, int n) {
        std::string s;
        for (int i = 0; i < n; ++i) s += ch;
        return s;
    };
    check(text_status(rep("\xC3\xA9", 32)) == KILN_ITEMS_RENAME_DONE && text_status(rep("\xC3\xA9", 33)) == KILN_ITEMS_RENAME_TOO_LONG,
          "the field's 64 bytes of UTF-8 also count: 32 two-byte characters fit, 33 do not");
    check(text_status(rep("\xE6\x9C\xA8", 21)) == KILN_ITEMS_RENAME_DONE && text_status(rep("\xE6\x9C\xA8", 22)) == KILN_ITEMS_RENAME_TOO_LONG,
          "21 three-byte characters fit (63 bytes), 22 do not (the field would cut the 22nd)");
    check(text_status(rep("\xF0\x9F\x94\xA5", 16)) == KILN_ITEMS_RENAME_DONE && text_status(rep("\xF0\x9F\x94\xA5", 17)) == KILN_ITEMS_RENAME_TOO_LONG,
          "16 four-byte characters fit, 17 do not; an emoji counts as one character");
    check(text_status(std::string(64, 'a')) == KILN_ITEMS_RENAME_TOO_LONG && text_status(std::string(300, 'a')) == KILN_ITEMS_RENAME_TOO_LONG,
          "64 ASCII characters and 300 bytes are too long");
    char why[160];
    check(check_text(std::string(300, 'a').data(), 300, why, sizeof why) == KILN_ITEMS_RENAME_TOO_LONG && std::strstr(why, "300") != nullptr,
          "the reason names the length");
    check(check_text(std::string(49, 'a').data(), 49, why, sizeof why) == KILN_ITEMS_RENAME_TOO_LONG && std::strstr(why, "49 characters") != nullptr,
          "the reason names the characters");
    check(text_status("a\xFF") == KILN_ITEMS_RENAME_BAD_TEXT && text_status("\xC0\x80") == KILN_ITEMS_RENAME_BAD_TEXT &&
              text_status("\xE0\x80\xAF") == KILN_ITEMS_RENAME_BAD_TEXT && text_status("\xED\xA0\x80") == KILN_ITEMS_RENAME_BAD_TEXT &&
              text_status("\xF4\x90\x80\x80") == KILN_ITEMS_RENAME_BAD_TEXT && text_status("\xE6\x9C") == KILN_ITEMS_RENAME_BAD_TEXT &&
              text_status("\x80") == KILN_ITEMS_RENAME_BAD_TEXT,
          "not UTF-8 is refused: a stray byte, overlong forms, a surrogate, above U+10FFFF, a cut-off character, a lone continuation");
    check(text_status("a\nb") == KILN_ITEMS_RENAME_BAD_TEXT && text_status("a\rb") == KILN_ITEMS_RENAME_BAD_TEXT &&
              text_status(std::string("a\0b", 3)) == KILN_ITEMS_RENAME_BAD_TEXT && text_status("a\x7F") == KILN_ITEMS_RENAME_BAD_TEXT &&
              text_status("a\x1B") == KILN_ITEMS_RENAME_BAD_TEXT,
          "the control characters the field drops are refused: newline, return, NUL, escape, DEL");
    check(text_status("a\tb") == KILN_ITEMS_RENAME_DONE && text_status("\xC2\x85") == KILN_ITEMS_RENAME_DONE &&
              text_status("\xE2\x80\xA8") == KILN_ITEMS_RENAME_DONE && text_status("  spaced  ") == KILN_ITEMS_RENAME_DONE &&
              text_status(" ") == KILN_ITEMS_RENAME_DONE,
          "what the field lets through is taken: TAB, U+0085, the line separator, untrimmed and whitespace-only names");
    check(check_text(nullptr, 4, nullptr, 0) == KILN_ITEMS_RENAME_BAD_TEXT && check_text(nullptr, 0, nullptr, 0) == KILN_ITEMS_RENAME_DONE,
          "no text with a length is refused; no text and no length is the empty name");
}

void renames() {
    {
        Game g;
        g.ents[1026] = Ent{};
        Outcome o = rn(g, 1026, "Ores");
        check(o.status == KILN_ITEMS_RENAME_DONE && o.changed && !o.cleared && g.adds == 1 && g.writes == 0 && g.name(1026) == "Ores" &&
                  g.table.size() == 1 && g.table.begin()->second.refs == 1,
              "a first name: set_string stores it and the component is added with its id (the handler's AddComponent)");
        o = rn(g, 1026, "Wood");
        check(o.status == KILN_ITEMS_RENAME_DONE && o.changed && g.adds == 1 && g.writes == 1 && g.name(1026) == "Wood" && g.table.size() == 1,
              "a second name: the tracked write puts the new id in, and the old record is released (its last reference)");
        check(g.store_after_track, "the handler's order: the tracked write, then set_string, then the id into the component");
        const int calls = g.set_string_calls;
        o = rn(g, 1026, "Wood");
        check(o.status == KILN_ITEMS_RENAME_DONE && !o.changed && g.writes == 1 && g.set_string_calls == calls,
              "the name it already has: done, nothing called or written");
        o = rn(g, 1026, "");
        check(o.status == KILN_ITEMS_RENAME_DONE && o.changed && o.cleared && g.ents[1026].has && g.ents[1026].id == 0 && g.name(1026).empty() &&
                  g.table.empty(),
              "an empty name clears it like the game's empty Rename: id 0 by the tracked write, the record released");
        o = rn(g, 1026, "");
        check(o.status == KILN_ITEMS_RENAME_DONE && !o.changed && o.cleared && g.writes == 2, "clearing a cleared name changes nothing");
    }
    {
        Game g;
        g.ents[7] = Ent{};
        Outcome o = rn(g, 7, "");
        check(o.status == KILN_ITEMS_RENAME_DONE && !o.changed && o.cleared && g.adds == 0 && g.set_string_calls == 0,
              "clearing an object that never had a name adds nothing");
    }
    {
        Game g;
        g.ents[1] = Ent{};
        g.ents[2] = Ent{};
        rn(g, 1, "Ores");
        rn(g, 2, "Ores");
        check(g.table.size() == 1 && g.table.begin()->second.refs == 2, "two chests of one name share the game's record");
        rn(g, 1, "Gems");
        check(g.name(2) == "Ores" && g.name(1) == "Gems" && g.table.size() == 2 && g.table[text_id("Ores", 4)].refs == 1,
              "renaming one keeps the other's name (the old id is released once, as the handler passes it)");
    }
    {
        Game g;
        g.ents[10] = Ent{false};
        g.ents[11] = Ent{true, 2};
        g.ents[12] = Ent{true, 3};
        g.ents[13] = Ent{true, 0};
        check(rn(g, 10, "X").status == KILN_ITEMS_RENAME_GONE && rn(g, 99, "X").status == KILN_ITEMS_RENAME_GONE,
              "an entity that does not exist (any more) is GONE");
        check(rn(g, 11, "X").status == KILN_ITEMS_RENAME_NOT_CONTAINER && rn(g, 12, "X").status == KILN_ITEMS_RENAME_NOT_CONTAINER &&
                  rn(g, 13, "X").status == KILN_ITEMS_RENAME_NOT_CONTAINER,
              "a player, an NPC and an entity without an inventory are NOT_CONTAINER");
        check(g.set_string_calls == 0 && g.adds == 0 && g.writes == 0, "refused renames call nothing of the game");
        g.exists_known = false;
        check(rn(g, 13, "X").status == KILN_ITEMS_RENAME_NOT_CONTAINER && rn(g, 99, "X").status == KILN_ITEMS_RENAME_NOT_CONTAINER,
              "without kiln.world's exists, the inventory check decides (NOT_CONTAINER)");
    }
    {
        Game g;
        g.limit = 2;
        g.ents[1] = Ent{};
        g.ents[2] = Ent{};
        g.ents[3] = Ent{};
        rn(g, 1, "A");
        rn(g, 2, "B");
        Outcome o = rn(g, 2, "C");
        check(o.status == KILN_ITEMS_RENAME_FULL && g.name(2) == "B" && g.table.size() == 2,
              "a full string table is refused before set_string, so the old name stays (the game would release it first)");
        o = rn(g, 3, "A");
        check(o.status == KILN_ITEMS_RENAME_DONE && g.name(3) == "A" && g.table[text_id("A", 1)].refs == 2,
              "a name the full table already holds is still taken (no new record)");
        o = rn(g, 1, "");
        check(o.status == KILN_ITEMS_RENAME_DONE && o.cleared, "clearing works on a full table");
    }
    {
        Game g;
        g.ents[1] = Ent{};
        g.force_error = 0x27;
        Outcome o = rn(g, 1, "A");
        check(o.status == KILN_ITEMS_RENAME_FULL && o.game_error == 0x27 && g.adds == 0, "a game error from set_string is reported with its code, nothing written");
        g.force_error = 0;
        g.fault_set_string = true;
        check(rn(g, 1, "A").status == KILN_ITEMS_RENAME_UNAVAILABLE, "a set_string fault ends UNAVAILABLE");
        g.fault_set_string = false;
        g.wrong_id = true;
        o = rn(g, 1, "A");
        check(o.status == KILN_ITEMS_RENAME_UNAVAILABLE && g.adds == 0, "an id that is not the text's hash ends UNAVAILABLE and writes nothing");
        g.wrong_id = false;
        rn(g, 1, "A");
        g.fault_write = true;
        const int calls = g.set_string_calls;
        const uint32_t refs_a = g.table[text_id("A", 1)].refs;
        check(rn(g, 1, "B").status == KILN_ITEMS_RENAME_UNAVAILABLE && g.set_string_calls == calls && g.name(1) == "A" &&
                  g.table.size() == 1 && g.table[text_id("A", 1)].refs == refs_a && !g.table.count(text_id("B", 1)),
              "a failed tracked write ends UNAVAILABLE before set_string: the old name and its record are untouched");
    }
    {
        Game g;
        g.ents[1] = Ent{};
        std::string full;
        for (int i = 0; i < 16; ++i) full += "\xF0\x9F\x94\xA5";
        check(text_status(full) == KILN_ITEMS_RENAME_DONE, "the longest name by bytes the field takes passes the check");
        Outcome o = rn(g, 1, full);
        check(o.status == KILN_ITEMS_RENAME_DONE && g.name(1) == full && !g.crashed, "it is stored whole; the server's 256-byte record is never passed");
    }
}

}  // namespace

int main() {
    texts();
    renames();
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
