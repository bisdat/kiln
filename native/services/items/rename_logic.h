// SPDX-License-Identifier: MIT
// kiln.items v9 (since 0.2.0-dev.30.3, GitHub issue #112): renaming a container, the rules without game memory. The
// text check (what the game's rename message and text field take) and the steps of the game's own rename handler
// (docs/re/items.md section 14), run against `RenameOps`: rename.cpp binds them to the game, tests/rename_test.cpp to a
// fake game that behaves like the server's string table.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "services/kiln_items.h"

namespace kitems {
namespace rename {

// The game's custom string ids: FNV-1a 64 of the UTF-8 text (set_string's own hash).
inline uint64_t text_id(const char* text, uint32_t len) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < len; ++i) h = (h ^ static_cast<uint8_t>(text[i])) * 0x100000001b3ull;
    return h;
}

// KILN_ITEMS_RENAME_DONE when the text is one the game's own Rename field lets a player confirm, otherwise TOO_LONG or
// BAD_TEXT (`why` says which character). The field (client 26430625, docs/re/items.md section 14): at most
// KILN_ITEMS_NAME_MAX_CHARS code points (FbUiBundle.maxInputLengthForLabelledEntities, 48 in the game's data) in a buffer
// of KILN_ITEMS_NAME_MAX_BYTES bytes of UTF-8; typing and pasting drop every code point below U+0020 except TAB, and
// U+007F; a paste that is not UTF-8 is refused whole. Nothing else is filtered (any script, emoji, U+0080-U+009F) and
// nothing is trimmed. Kiln refuses where the field would cut or drop, so a mod never stores what a player could not type;
// it also refuses text that is not well-formed UTF-8 (no overlong form, no surrogate, nothing above U+10FFFF), which
// the field cannot produce whole. An empty text is taken: it clears the name (the server's rename does that; the Rename
// field itself never sends an empty name).
inline int32_t check_text(const char* text, uint32_t len, char* why, size_t why_cap) {
    auto say = [&](const char* fmt, uint32_t a, uint32_t b) {
        if (why && why_cap) std::snprintf(why, why_cap, fmt, a, b);
    };
    if (why && why_cap) why[0] = 0;
    if (len > KILN_ITEMS_NAME_MAX_BYTES) {
        say("the name is %u bytes of UTF-8; the game's Rename field holds at most %u", len, KILN_ITEMS_NAME_MAX_BYTES);
        return KILN_ITEMS_RENAME_TOO_LONG;
    }
    if (len && !text) {
        say("no text for %u bytes", len, 0);
        return KILN_ITEMS_RENAME_BAD_TEXT;
    }
    const auto* s = reinterpret_cast<const uint8_t*>(text);
    uint32_t chars = 0;
    for (uint32_t i = 0; i < len;) {
        const uint8_t c = s[i];
        uint32_t cp, n;
        if (c < 0x80) { cp = c; n = 1; }
        else if (c >= 0xC2 && c <= 0xDF) { cp = c & 0x1F; n = 2; }
        else if (c >= 0xE0 && c <= 0xEF) { cp = c & 0x0F; n = 3; }
        else if (c >= 0xF0 && c <= 0xF4) { cp = c & 0x07; n = 4; }
        else { say("byte %u (0x%02x) does not start a UTF-8 character", i, c); return KILN_ITEMS_RENAME_BAD_TEXT; }
        if (i + n > len) { say("the UTF-8 character at byte %u is cut off", i, 0); return KILN_ITEMS_RENAME_BAD_TEXT; }
        for (uint32_t k = 1; k < n; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) { say("byte %u (0x%02x) is not a UTF-8 continuation", i + k, s[i + k]); return KILN_ITEMS_RENAME_BAD_TEXT; }
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
            say("the UTF-8 character at byte %u is not valid (U+%04X)", i, cp);
            return KILN_ITEMS_RENAME_BAD_TEXT;
        }
        if ((cp < 0x20 && cp != 0x09) || cp == 0x7F) {
            say("the name holds a control character (U+%04X at byte %u); the game's Rename field drops it", cp, i);
            return KILN_ITEMS_RENAME_BAD_TEXT;
        }
        i += n;
        ++chars;
    }
    if (chars > KILN_ITEMS_NAME_MAX_CHARS) {
        say("the name is %u characters; the game's Rename field takes at most %u", chars, KILN_ITEMS_NAME_MAX_CHARS);
        return KILN_ITEMS_RENAME_TOO_LONG;
    }
    return KILN_ITEMS_RENAME_DONE;
}

// The game, as the rename handler sees it. Every call is on the tick thread outside the world's jobs; a false return
// (or `faulted`) means the call could not be made (a fault, an unexpected layout): the rename ends UNAVAILABLE.
struct RenameOps {
    void* ctx = nullptr;
    // 1 the entity exists, 0 it does not, -1 cannot tell (then the inventory check decides)
    int (*exists)(void* ctx, KilnEntity e) = nullptr;
    // 1 a container (an initialized inventory, neither a player's nor an NPC's), 0 not, -1 the read faulted
    int (*container)(void* ctx, KilnEntity e) = nullptr;
    // the entity's name component: *has = it carries one, *id = the id in it (0 = no name)
    bool (*name_id)(void* ctx, KilnEntity e, bool* has, uint64_t* id) = nullptr;
    // the string table: does it hold `id`, records in use, how many it can hold
    bool (*table)(void* ctx, uint64_t id, bool* holds, uint32_t* used, uint32_t* limit) = nullptr;
    // the game's set_string(out, strings, old_id, {text, len}): its error byte (0 = ok) and the id it returns
    bool (*set_string)(void* ctx, uint64_t old_id, const char* text, uint32_t len, uint8_t* error, uint64_t* id) = nullptr;
    // the world's tracked write of the name component (replication, the save): marks it written and gives where its id
    // is (*slot), as the handler's first call does; made before set_string, so a failure here leaves the table untouched
    bool (*track)(void* ctx, KilnEntity e, void** slot) = nullptr;
    // the id into the slot `track` gave (after set_string, as the handler does)
    bool (*store)(void* ctx, void* slot, uint64_t id) = nullptr;
    // AddComponent(world, e, CustomPlayerString, {&id, 8}): an object named for the first time
    bool (*add_id)(void* ctx, KilnEntity e, uint64_t id) = nullptr;
};

struct Outcome {
    int32_t status = KILN_ITEMS_RENAME_UNAVAILABLE;
    uint8_t changed = 0, cleared = 0;
    uint32_t game_error = 0;
    char message[160] = "";
};

// The game's rename of `e` to `text` (already checked with check_text), as its handler does it: the tracked write of
// the component first (its id is the old id set_string releases), then set_string, then its id into the component, or a
// new component for an object that never had one. The tracked write comes first so that nothing can fail between
// set_string (which releases the old record) and storing the new id: a failure there would leave the component naming a
// released record and the new record with a reference nobody holds. A name the
// object already has changes nothing (set_string returns early for it, and nothing is written). An empty text on an
// object without a name has nothing to clear. A full string table is refused before set_string, which would otherwise
// release the old name first.
inline Outcome run(const RenameOps& g, KilnEntity e, const char* text, uint32_t len) {
    Outcome o{};
    auto fail = [&](int32_t status, const char* fmt, unsigned long long a = 0, unsigned long long b = 0) {
        o.status = status;
        std::snprintf(o.message, sizeof o.message, fmt, a, b);
        return o;
    };
    const int alive = g.exists ? g.exists(g.ctx, e) : -1;
    if (alive == 0) return fail(KILN_ITEMS_RENAME_GONE, "entity %llu does not exist (any more)", e);
    const int box = g.container(g.ctx, e);
    if (box < 0) return fail(KILN_ITEMS_RENAME_UNAVAILABLE, "reading entity %llu's inventory faulted", e);
    if (box == 0)
        return alive == 1 ? fail(KILN_ITEMS_RENAME_NOT_CONTAINER, "entity %llu is not a container (no inventory of its own, or a player's or an NPC's)", e)
                          : fail(KILN_ITEMS_RENAME_NOT_CONTAINER, "entity %llu is not a container (or no longer exists)", e);
    bool has = false;
    uint64_t old = 0;
    if (!g.name_id(g.ctx, e, &has, &old)) return fail(KILN_ITEMS_RENAME_UNAVAILABLE, "reading entity %llu's name faulted", e);
    if (!has) old = 0;
    const uint64_t want = len ? text_id(text, len) : 0;
    if (want == old) {
        o.status = KILN_ITEMS_RENAME_DONE;
        o.cleared = want == 0;
        std::snprintf(o.message, sizeof o.message, want ? "entity %u already has this name" : "entity %u has no name to clear", e);
        return o;
    }
    if (want) {
        bool holds = false;
        uint32_t used = 0, limit = 0;
        if (!g.table(g.ctx, want, &holds, &used, &limit)) return fail(KILN_ITEMS_RENAME_UNAVAILABLE, "reading the game's string table failed");
        if (!holds && used >= limit)
            return fail(KILN_ITEMS_RENAME_FULL, "the game's string table holds its maximum of %llu different names (%llu in use)", limit, used);
    }
    void* slot = nullptr;
    if (has && (!g.track(g.ctx, e, &slot) || !slot))
        return fail(KILN_ITEMS_RENAME_UNAVAILABLE, "the tracked write of entity %llu's name failed (nothing changed)", e);
    uint8_t err = 0;
    uint64_t id = 0;
    if (!g.set_string(g.ctx, old, text, len, &err, &id)) return fail(KILN_ITEMS_RENAME_UNAVAILABLE, "the game's set_string faulted");
    if (err) {
        o.game_error = err;
        return fail(KILN_ITEMS_RENAME_FULL, "the game refused the name (set_string error 0x%02llx)", err);
    }
    if (id != want) return fail(KILN_ITEMS_RENAME_UNAVAILABLE, "set_string returned id %016llx, not the text's (%016llx)", id, want);
    if (has) {
        if (!g.store(g.ctx, slot, id)) return fail(KILN_ITEMS_RENAME_UNAVAILABLE, "storing entity %llu's new name id faulted", e);
    } else if (!g.add_id(g.ctx, e, id)) {
        return fail(KILN_ITEMS_RENAME_UNAVAILABLE, "adding entity %llu's name component failed", e);
    }
    o.status = KILN_ITEMS_RENAME_DONE;
    o.changed = 1;
    o.cleared = id == 0;
    std::snprintf(o.message, sizeof o.message, id ? "entity %u renamed" : "entity %u's name cleared", e);
    return o;
}

}  // namespace rename
}  // namespace kitems
