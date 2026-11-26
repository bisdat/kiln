// SPDX-License-Identifier: MIT
// kiln.npcspeech's state and the two things it does inside the game's calls, without Windows or the game: what one
// bubble draw shows (draw) and the mute/restore of a dialogue record's greeting voice-overs (voice). npcspeech.cpp wraps
// it in the hooks; tests/npcspeech_test.cpp drives it on fake structs.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "services/kiln_npcspeech.h"

namespace npcspeech {

// Offsets from sigdb (client.d/npcspeech.json). entry_size 0 = the record layout is unknown: voice() does nothing.
struct Layout {
    int64_t entry_record = 8;   // NpcBubble's entry: +0 entity id, +8 the dialogue record
    int64_t bubble_text = 0;    // BubbleDraw's params: +0 loca tag
    int64_t bubble_show = 4;    //                      +4 show (fades in / out)
    int64_t bubble_reset = 5;   //                      +5 hide at once
    int64_t prio_list = 0, prio_count = 0, plain_list = 0, plain_count = 0;  // the record's two greeting lists (ptr, u64 count)
    int64_t entry_size = 0, entry_vo = 0, vo_slots = 0;                     // a line entry: its inline voice-over GUIDs
    int64_t entry_line = 0;                                                  // a line entry: its line id (u32 at +0, PickLines)
    bool voice_sane() const {
        return prio_list > 0 && prio_count > 0 && plain_list > 0 && plain_count > 0 && entry_size >= 0x100 && entry_size <= 0x1000 &&
               entry_vo > 0 && vo_slots > 0 && vo_slots <= 64 && entry_vo + vo_slots * 16 <= entry_size && entry_line >= 0 &&
               entry_line + 4 <= entry_vo && prio_list < 0x20000 &&
               plain_list < 0x20000 && prio_count < 0x20000 && plain_count < 0x20000;
    }
};

struct Counts {
    uint32_t overrides = 0, says = 0, texts = 0;
    uint64_t bubbles = 0, changed = 0, muted = 0;
};

constexpr uint64_t kMaxEntries = 256;  // a list longer than this is not a dialogue record: leave it

class Core {
public:
    static bool valid_set(uint32_t npc, uint32_t mode, const uint32_t* tags, uint32_t n) {
        if (!npc || mode > KILN_SPEECH_REPLACE) return false;
        if (mode != KILN_SPEECH_REPLACE) return true;
        if (!tags || n == 0 || n > KILN_SPEECH_MAX_LINES) return false;
        for (uint32_t i = 0; i < n; ++i)
            if (!tags[i]) return false;
        return true;
    }

    // Any thread.
    void set(uint32_t npc, uint32_t mode, const uint32_t* tags, uint32_t n) {
        std::lock_guard lock(mu_);
        auto& s = npcs_[npc];
        s.mode = mode;
        s.tags.assign(tags && mode == KILN_SPEECH_REPLACE ? tags : nullptr, tags && mode == KILN_SPEECH_REPLACE ? tags + n : nullptr);
        s.seen_game = 0;
        prune(npc, s);
        publish();
    }

    void say(uint32_t npc, uint32_t tag, uint32_t ms, uint64_t now) {
        std::lock_guard lock(mu_);
        auto& s = npcs_[npc];
        s.say_tag = tag && ms ? tag : 0;
        s.say_until = tag && ms ? now + ms : 0;
        prune(npc, s);
        publish();
    }

    void clear_all() {
        std::lock_guard lock(mu_);
        for (auto it = npcs_.begin(); it != npcs_.end();) {
            it->second.mode = KILN_SPEECH_GAME;
            it->second.tags.clear();
            it->second.say_tag = 0;
            it->second.say_until = 0;
            // keep the NPC while voice-overs are muted: its next bubble call puts them back
            it = it->second.muted.empty() ? npcs_.erase(it) : std::next(it);
        }
        publish();
    }

    void count_text(uint32_t) { texts_.fetch_add(1, std::memory_order_relaxed); }

    // Something to do in a bubble call (an override, a Say, or voice-overs to put back).
    bool active() const { return any_.load(std::memory_order_acquire); }

    // BubbleDraw inside npc's bubble call: what the bubble shows. now: ms.
    void draw(uint32_t npc, uint8_t* params, const Layout& l, uint64_t now) {
        std::lock_guard lock(mu_);
        auto it = npcs_.find(npc);
        if (it == npcs_.end()) return;
        auto& s = it->second;
        if (s.say_tag && now >= s.say_until) {
            s.say_tag = 0;
            s.say_until = 0;
            prune_later_ = true;
        }
        if (!s.say_tag && s.mode == KILN_SPEECH_GAME) {
            if (s.muted.empty()) {  // nothing left for this NPC (no voice-overs to put back: voice() may never run)
                npcs_.erase(it);
                publish();
            }
            return;
        }
        bubbles_++;
        uint32_t text;
        std::memcpy(&text, params + l.bubble_text, 4);
        if (s.say_tag) {
            std::memcpy(params + l.bubble_text, &s.say_tag, 4);
            params[l.bubble_show] = 1;
            params[l.bubble_reset] = 0;
            changed_++;
            return;
        }
        if (s.mode == KILN_SPEECH_SUPPRESS) {
            if (params[l.bubble_show] || text) changed_++;
            params[l.bubble_show] = 0;
            return;
        }
        // Replace: a game pick (nonzero) becomes one of the mod's lines; a new game pick, a new line of the mod's
        if (!text || s.tags.empty()) return;
        if (text != s.seen_game && !is_ours(s, text)) {
            s.seen_game = text;
            s.pick = s.tags.size() > 1 ? static_cast<uint32_t>(next_rand() % s.tags.size()) : 0;
        }
        const uint32_t ours = s.tags[s.pick < s.tags.size() ? s.pick : 0];
        if (text != ours) {
            std::memcpy(params + l.bubble_text, &ours, 4);
            changed_++;
        }
    }

    // NpcBubble with the NPC's record: mutes the greeting voice-overs while Kiln speaks for it, puts them back after.
    // The record is the one the game passes now: its record map keeps records inline and copies all of them to new
    // storage when it grows (0x459590 on 26430625), Kiln's zeros with them, so the saved voice-overs follow the NPC, not
    // an address. Each is put back only where the slot is still empty and the entry still holds the same line.
    // Faults are the caller's (SEH): this only reads and writes inside the record's own lists.
    void voice(uint32_t npc, uint8_t* record, const Layout& l) {
        if (!l.entry_size || !record) return;
        std::lock_guard lock(mu_);
        auto it = npcs_.find(npc);
        if (it == npcs_.end()) return;
        auto& s = it->second;
        const bool speaking = s.mode != KILN_SPEECH_GAME || s.say_tag;
        if (speaking) {
            mute_list(s, record, l, l.prio_list, l.prio_count, 0);
            mute_list(s, record, l, l.plain_list, l.plain_count, 1);
        } else {  // nothing left for this NPC: put its voice-overs back and forget it
            if (!s.muted.empty()) restore(s, record, l);
            npcs_.erase(it);
            publish();
        }
        if (prune_later_) {
            prune_later_ = false;
            publish();
        }
    }

    Counts counts() const {
        std::lock_guard lock(mu_);
        Counts c;
        tally(c);
        c.texts = texts_.load(std::memory_order_relaxed);
        c.bubbles = bubbles_;
        c.changed = changed_;
        c.muted = muted_;
        return c;
    }

    // tests
    size_t tracked() const {
        std::lock_guard lock(mu_);
        return npcs_.size();
    }

private:
    struct Muted {
        uint8_t list;     // 0 priority, 1 plain
        uint32_t index;   // entry in that list
        uint32_t slot;    // voice-over slot in the entry
        uint32_t line;    // the entry's line id when it was muted
        uint8_t guid[16];
    };
    struct Npc {
        uint32_t mode = KILN_SPEECH_GAME;
        std::vector<uint32_t> tags;
        uint32_t say_tag = 0;
        uint64_t say_until = 0;
        uint32_t seen_game = 0, pick = 0;
        std::vector<Muted> muted;
    };

    static bool is_ours(const Npc& s, uint32_t t) {
        for (auto x : s.tags)
            if (x == t) return true;
        return false;
    }

    // An NPC with nothing left is dropped unless voice-overs still need putting back.
    void prune(uint32_t npc, Npc& s) {
        if (s.mode == KILN_SPEECH_GAME && !s.say_tag && s.muted.empty()) npcs_.erase(npc);
    }

    void publish() {
        bool any = false;
        for (auto& [k, v] : npcs_)
            if (v.mode != KILN_SPEECH_GAME || v.say_tag || !v.muted.empty()) any = true;
        any_.store(any, std::memory_order_release);
    }

    void tally(Counts& c) const {
        for (auto& [k, v] : npcs_) {
            if (v.mode != KILN_SPEECH_GAME) c.overrides++;
            if (v.say_tag) c.says++;
        }
    }

    void mute_list(Npc& s, uint8_t* record, const Layout& l, int64_t list_off, int64_t count_off, uint8_t which) {
        uint8_t* base;
        uint64_t n;
        std::memcpy(&base, record + list_off, sizeof base);
        std::memcpy(&n, record + count_off, sizeof n);
        if (!base || n > kMaxEntries) return;
        for (uint64_t i = 0; i < n; ++i) {
            uint8_t* entry = base + i * static_cast<uint64_t>(l.entry_size);
            uint8_t* vo = entry + l.entry_vo;
            for (int64_t k = 0; k < l.vo_slots; ++k) {
                uint8_t* g = vo + k * 16;
                static const uint8_t zero[16] = {};
                if (std::memcmp(g, zero, 16) == 0) continue;  // already muted, or a line without voice
                Muted m{which, static_cast<uint32_t>(i), static_cast<uint32_t>(k), 0, {}};
                std::memcpy(&m.line, entry + l.entry_line, 4);
                std::memcpy(m.guid, g, 16);
                // a slot Kiln already holds that is filled again: the game rebuilt the record, its value is the current one
                bool held = false;
                for (auto& x : s.muted)
                    if (x.list == which && x.index == i && x.slot == k) {
                        x = m;
                        held = true;
                    }
                if (!held) s.muted.push_back(m);
                std::memset(g, 0, 16);
                muted_++;
            }
        }
    }

    void restore(Npc& s, uint8_t* record, const Layout& l) {
        for (auto& m : s.muted) {
            const int64_t list_off = m.list ? l.plain_list : l.prio_list, count_off = m.list ? l.plain_count : l.prio_count;
            uint8_t* base;
            uint64_t n;
            std::memcpy(&base, record + list_off, sizeof base);
            std::memcpy(&n, record + count_off, sizeof n);
            if (!base || m.index >= n || n > kMaxEntries) continue;  // the record changed shape: the game refilled it itself
            uint8_t* entry = base + m.index * static_cast<uint64_t>(l.entry_size);
            uint32_t line;
            std::memcpy(&line, entry + l.entry_line, 4);
            if (line != m.line) continue;  // another line sits there now (a rebuilt list): not this voice-over's place
            uint8_t* g = entry + l.entry_vo + m.slot * 16;
            static const uint8_t zero[16] = {};
            if (std::memcmp(g, zero, 16) == 0) std::memcpy(g, m.guid, 16);
        }
    }

    uint32_t next_rand() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return rng_;
    }

    mutable std::mutex mu_;
    std::unordered_map<uint32_t, Npc> npcs_;
    std::atomic<bool> any_{false};
    std::atomic<uint32_t> texts_{0};
    bool prune_later_ = false;
    uint64_t bubbles_ = 0, changed_ = 0, muted_ = 0;
    uint32_t rng_ = 0x9e3779b9u;
};

}  // namespace npcspeech
