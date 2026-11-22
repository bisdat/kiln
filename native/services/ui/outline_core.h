// SPDX-License-Identifier: MIT
// kiln.render's outlines (kiln_render.h v2, 0.2.0-dev.30.3, issue #111): the logic without the game, host-tested by
// tests/outline_test.cpp. outline.cpp is the glue (sigdb, the palette hooks, the stage on dynamic_render_model, the
// client world's entities). RE: docs/re/client-render.md "Highlight groups (outlines)".
//
//  Book    the requests (any thread, one mutex): add, remove, clear, info, the ends (despawn, world, owner, expiry) and,
//          once per client step on the main thread, the plan: per entity its winning request (priority, then newest),
//          and Kiln's two groups (4, 5) given to the (colour, pulse) pairs the winners use.
//  apply   the stage's step (the world's thread, inside dynamic_render_model, which holds the RenderScene): sets each
//          planned model's group through the renderer, takes only an instance whose group is 0 or Kiln's (or one of the
//          game's own 1..3 when the player put mods first; another tool's group never), and gives the instances Kiln no
//          longer wants back to 0 when nobody changed them.
//  ModelCache  which entities' model handles the client Pre reads again this step (kiln.world's verified reads are
//          not free): new ones, stale ones, and each one once per kModelRefreshSteps steps, within a budget per step.
//  palette the colour the renderer gets for one of Kiln's groups this frame (the render thread).
// No windows.h, no game memory.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nlohmann/json.hpp"
#include "services/kiln_render.h"

namespace kout {

inline constexpr uint8_t kGroups[2] = {4, 5};  // keen::HighlightColorIndex Group1, Group2: transparent and unused in the game
inline constexpr uint32_t kMaxPerOwner = 1024;
inline constexpr size_t kEndedKeep = 4096;
inline constexpr uint32_t kPulseMs = 1200;
inline constexpr uint32_t kModelRefreshSteps = 16;  // ModelCache: every handle is read again once per this many steps
inline constexpr uint32_t kModelReadsPerStep = 64;  // ModelCache: kiln.world reads at most per client step

// The game's own highlight groups: the building cursor's InvalidState 1, ValidState 2, MutedState 3. The player's
// "mods-first" lets Kiln cover these and nothing else: another tool's group (Greeneye's Quick Deposit uses 6, Find and
// The Worst Map Mod use 7) is never taken, whatever the setting.
inline bool game_group(int g) { return g >= 1 && g <= 3; }

enum Setting : uint8_t { kGameFirst = 0, kModsFirst = 1, kOff = 2 };

// kiln.json "ui": {"outlines": "game-first" | "mods-first" | "off"}; anything else, or nothing: game-first.
inline Setting parse_setting(std::string_view kiln_json) {
    try {
        const auto doc = nlohmann::json::parse(kiln_json.begin(), kiln_json.end(), nullptr, false, true);
        if (!doc.is_object()) return kGameFirst;
        const auto ui = doc.find("ui");
        if (ui == doc.end() || !ui->is_object()) return kGameFirst;
        const auto v = ui->find("outlines");
        if (v == ui->end() || !v->is_string()) return kGameFirst;
        const std::string s = v->get<std::string>();
        return s == "mods-first" ? kModsFirst : s == "off" ? kOff : kGameFirst;
    } catch (...) {
        return kGameFirst;
    }
}

// The game's keen::Color is 0xAABBGGRR (red in the low byte: the palette's invalid colour 0xFF0000FF is red).
inline uint32_t to_game_color(uint32_t rgba) {
    const uint32_t r = rgba >> 24, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF, a = rgba & 0xFF;
    return a << 24 | b << 16 | g << 8 | r;
}

// One of Kiln's groups as the palette hook reads it: bit 63 on, bit 32 pulse, low 32 bits the game colour.
inline constexpr uint64_t kSlotOn = uint64_t{1} << 63, kSlotPulse = uint64_t{1} << 32;
inline uint64_t slot_word(uint32_t rgba, bool pulse) {
    return kSlotOn | (pulse ? kSlotPulse : 0) | to_game_color(rgba);
}
// The colour the renderer gets for a group this frame: 'original' (the game's palette entry) when Kiln does not use the
// group; else Kiln's colour, its red, green and blue breathing between 40 % and 100 % with a pulse.
inline uint32_t palette_value(uint64_t word, uint32_t original, uint64_t now_ms) {
    if (!(word & kSlotOn)) return original;
    const uint32_t c = static_cast<uint32_t>(word);
    if (!(word & kSlotPulse)) return c;
    const float phase = static_cast<float>(now_ms % kPulseMs) / static_cast<float>(kPulseMs);
    const float gain = 0.4f + 0.6f * (0.5f - 0.5f * std::cos(phase * 6.2831853f));
    auto ch = [&](int shift) {
        return static_cast<uint32_t>(std::lround(static_cast<float>((c >> shift) & 0xFF) * gain)) << shift;
    };
    return (c & 0xFF000000u) | ch(16) | ch(8) | ch(0);
}

struct Request {
    KilnHandle handle = 0;
    KilnOwner owner = 0;
    KilnEntity entity = 0;
    uint32_t rgba = 0;
    int32_t priority = 0;
    uint32_t flags = 0;
    uint64_t seq = 0;
    uint64_t expire_ms = 0;  // 0: never
    uint32_t state = KILN_RENDER_OUTLINE_WAITING;
    uint32_t reason = KILN_RENDER_OUTLINE_PENDING;
    uint32_t group = 0;
    uint32_t merged = 0;
    bool pulse() const { return (flags & KILN_RENDER_OUTLINE_PULSE) != 0; }
};

// What the stage applies: one model per outlined entity and the group it should show.
struct Target {
    KilnEntity entity = 0;
    uint32_t model = 0;  // keen::ecs::ModelComponent.modelHandle; 0 = none known
    uint8_t group = 0;
};
struct Plan {
    uint64_t generation = 0;
    uint64_t session = 0;  // the client world's session (kiln.world game_world_session) the models belong to
    Setting setting = kGameFirst;
    std::vector<Target> targets;
    std::unordered_map<KilnEntity, size_t> index;  // entity -> its position in targets (one target per entity)
};

// The plan's targets among one slice's entities (ids: the slice's first id, the next one 'stride' bytes further), in
// 'which'. A hash lookup per entity: a slice of thousands with hundreds of outlines stays far inside the stage's budget.
inline void slice_targets(const Plan& plan, const void* ids, uint32_t count, uint32_t stride, std::vector<size_t>& which) {
    which.clear();
    if (!ids || plan.index.empty()) return;
    for (uint32_t r = 0; r < count; ++r) {
        uint32_t id = 0;
        std::memcpy(&id, static_cast<const uint8_t*>(ids) + size_t(r) * stride, sizeof id);
        if (const auto it = plan.index.find(id); it != plan.index.end()) which.push_back(it->second);
    }
}

// What the stage found for plan.targets[i].
enum Result : uint8_t { kNone = 0, kShown = 1, kGame = 2, kNoModel = 3 };

// The renderer as apply() sees it (the game's through outline.cpp, a fake in the host test).
struct RendererIo {
    virtual ~RendererIo() = default;
    virtual int group(uint32_t model) = 0;  // the instance's group, -1 when the handle names no instance (stale, 0)
    virtual void set(const uint32_t* models, const uint8_t* groups, size_t n) = 0;
};

// The stage's memory (the world's thread only): the instances that carry a group Kiln set.
struct Applied {
    uintptr_t renderer = 0;
    uint64_t session = 0;
    std::unordered_map<uint32_t, uint8_t> groups;  // model handle -> the group Kiln set
};

// Applies targets[i] for each i in 'which' (all when null); results is resized to plan.targets.size() and filled for
// those. With 'restore', instances Kiln set before and the plan no longer wants go back to 0 when they still show Kiln's
// group. Returns the models written through the setter.
inline uint64_t apply(RendererIo& io, uintptr_t renderer, const Plan& plan, Applied& applied, std::vector<uint8_t>& results,
                      const std::vector<size_t>* which, bool restore) {
    if (applied.renderer != renderer || applied.session != plan.session) {
        // another world (its renderer may sit at the old address): what Kiln set in the old one is gone with it
        applied.groups.clear();
        applied.renderer = renderer;
        applied.session = plan.session;
    }
    results.resize(plan.targets.size(), kNone);
    std::vector<uint32_t> models;
    std::vector<uint8_t> groups;
    const bool mods_first = plan.setting == kModsFirst;
    auto one = [&](size_t i) {
        const Target& t = plan.targets[i];
        if (!t.model || !t.group) {
            results[i] = kNoModel;
            return;
        }
        const int g = io.group(t.model);
        if (g < 0) {
            results[i] = kNoModel;
            applied.groups.erase(t.model);
            return;
        }
        const auto it = applied.groups.find(t.model);
        const bool ours = it != applied.groups.end() && it->second == g;
        if (g == t.group) {
            applied.groups[t.model] = t.group;
            results[i] = kShown;
        } else if (g == 0 || ours || (mods_first && game_group(g))) {
            models.push_back(t.model);
            groups.push_back(t.group);
            applied.groups[t.model] = t.group;
            results[i] = kShown;
        } else {  // the game's own highlight (the building cursor), or another tool's group under any setting: it goes first
            applied.groups.erase(t.model);
            results[i] = kGame;
        }
    };
    if (which) {
        for (size_t i : *which)
            if (i < plan.targets.size()) one(i);
    } else {
        for (size_t i = 0; i < plan.targets.size(); ++i) one(i);
    }
    if (restore) {
        std::unordered_set<uint32_t> wanted;
        for (const Target& t : plan.targets)
            if (t.model && t.group) wanted.insert(t.model);
        for (auto it = applied.groups.begin(); it != applied.groups.end();) {
            if (wanted.count(it->first)) {
                ++it;
                continue;
            }
            if (io.group(it->first) == it->second) {  // still Kiln's: back to no highlight (Kiln only ever took 0)
                models.push_back(it->first);
                groups.push_back(0);
            }
            it = applied.groups.erase(it);
        }
    }
    if (!models.empty()) io.set(models.data(), groups.data(), models.size());
    return models.size();
}

// The requests. Every member takes the lock; plan() and the ends run on the main thread, the rest anywhere.
class Book {
public:
    struct Slot {
        uint32_t rgba = 0;
        bool pulse = false;
        bool used = false;
    };

    KilnResult add(KilnOwner owner, KilnEntity entity, const KilnRenderOutline& o, uint64_t now_ms, KilnHandle handle) {
        std::lock_guard lk(mu_);
        uint32_t mine = 0;
        for (const Request& r : live_)
            if (r.owner == owner) ++mine;
        if (mine >= kMaxPerOwner) return KILN_ERR_BUFFER_TOO_SMALL;
        Request r;
        r.handle = handle;
        r.owner = owner;
        r.entity = entity;
        r.rgba = o.rgba;
        r.priority = o.priority;
        r.flags = o.flags;
        r.seq = ++seq_;
        r.expire_ms = o.duration_ms ? now_ms + o.duration_ms : 0;
        r.reason = setting_ == kOff ? KILN_RENDER_OUTLINE_SWITCHED_OFF : KILN_RENDER_OUTLINE_PENDING;
        live_.push_back(r);
        return KILN_OK;
    }

    KilnResult info(KilnHandle h, KilnRenderOutlineInfo* out) const {
        std::lock_guard lk(mu_);
        const Request* r = find_any(h);
        if (!r) return KILN_ERR_NOT_FOUND;
        out->state = r->state;
        out->reason = r->reason;
        out->entity = r->entity;
        out->group = r->group;
        out->merged = r->merged;
        // 0.2.0-dev.31: a through-walls request that has not ended carries its entity's latest position
        if ((r->flags & KILN_RENDER_OUTLINE_THROUGH_WALLS) && r->state != KILN_RENDER_OUTLINE_ENDED)
            if (const auto it = positions_.find(r->entity); it != positions_.end()) {
                out->position_ok = 1;
                out->position_step = it->second.step;
                for (int k = 0; k < 3; ++k) out->position[k] = it->second.p[k];
            }
        return KILN_OK;
    }

    // ---- positions for through-walls requests (0.2.0-dev.31) ----
    // The entities of live requests with KILN_RENDER_OUTLINE_THROUGH_WALLS (sorted, once each).
    std::vector<KilnEntity> wall_entities() const {
        std::lock_guard lk(mu_);
        std::vector<KilnEntity> v;
        for (const Request& r : live_)
            if (r.flags & KILN_RENDER_OUTLINE_THROUGH_WALLS) v.push_back(r.entity);
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }
    // The entity's position this client step (ok false: not readable now, forgotten). A finite position in the game's
    // range only (x and z in [-1000, 12000], y in [-1000, 5000] metres, as kiln.markers checks): anything else is not ok.
    void set_position(KilnEntity e, bool ok, const double p[3], uint32_t step) {
        std::lock_guard lk(mu_);
        auto in = [](double v, double lo, double hi) { return v == v && v >= lo && v <= hi; };
        if (!ok || !in(p[0], -1000, 12000) || !in(p[1], -1000, 5000) || !in(p[2], -1000, 12000)) {
            positions_.erase(e);
            return;
        }
        positions_[e] = Pos{{p[0], p[1], p[2]}, step};
    }
    // Forgets the positions of entities no through-walls request names any more.
    void keep_positions(const std::vector<KilnEntity>& wall) {
        std::lock_guard lk(mu_);
        std::erase_if(positions_, [&](const auto& kv) { return !std::binary_search(wall.begin(), wall.end(), kv.first); });
    }

    KilnResult remove(KilnHandle h) {
        std::lock_guard lk(mu_);
        for (size_t i = 0; i < live_.size(); ++i)
            if (live_[i].handle == h) {
                end_at(i, KILN_RENDER_OUTLINE_REMOVED);
                return KILN_OK;
            }
        return find_any(h) ? KILN_OK : KILN_ERR_NOT_FOUND;  // ended already: nothing more to do
    }

    // Ends the owner's live requests (REMOVED); its requests that had ended before are forgotten.
    void clear(KilnOwner owner) {
        std::lock_guard lk(mu_);
        std::erase_if(ended_, [&](const Request& r) { return r.owner == owner; });
        end_if([&](const Request& r) { return r.owner == owner; }, KILN_RENDER_OUTLINE_REMOVED);
    }
    void end_owner(KilnOwner owner, uint32_t reason) {
        std::lock_guard lk(mu_);
        end_if([&](const Request& r) { return r.owner == owner; }, reason);
    }
    void end_entity(KilnEntity e, uint32_t reason) {
        std::lock_guard lk(mu_);
        end_if([&](const Request& r) { return r.entity == e; }, reason);
    }
    void end_all(uint32_t reason) {
        std::lock_guard lk(mu_);
        end_if([](const Request&) { return true; }, reason);
    }
    void expire(uint64_t now_ms) {
        std::lock_guard lk(mu_);
        end_if([&](const Request& r) { return r.expire_ms && now_ms >= r.expire_ms; }, KILN_RENDER_OUTLINE_EXPIRED);
    }

    std::vector<KilnEntity> entities() const {
        std::lock_guard lk(mu_);
        std::vector<KilnEntity> v;
        for (const Request& r : live_) v.push_back(r.entity);
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }
    std::vector<KilnOwner> owners() const {
        std::lock_guard lk(mu_);
        std::vector<KilnOwner> v;
        for (const Request& r : live_) v.push_back(r.owner);
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }
    size_t live() const {
        std::lock_guard lk(mu_);
        return live_.size();
    }
    Setting setting() const {
        std::lock_guard lk(mu_);
        return setting_;
    }

    // The plan for this step: models[entity] = its model handle now (absent or 0: none). Fills slots[0..1] (Kiln's two
    // groups) and the requests' states. Requests whose entity's model is unknown wait (NO_MODEL).
    Plan plan(const std::unordered_map<KilnEntity, uint32_t>& models, Setting setting, Slot slots[2], uint64_t session = 0) {
        std::lock_guard lk(mu_);
        setting_ = setting;
        Plan p;
        p.generation = ++generation_;
        p.session = session;
        p.setting = setting;
        slots[0] = slots[1] = Slot{};
        plan_entities_.clear();
        plan_generation_ = p.generation;
        // winners: per entity the highest priority, then the newest
        std::unordered_map<KilnEntity, size_t> win;
        for (size_t i = 0; i < live_.size(); ++i) {
            auto [it, fresh] = win.try_emplace(live_[i].entity, i);
            if (!fresh && better(live_[i], live_[it->second])) it->second = i;
        }
        for (Request& r : live_) r.group = r.merged = 0;
        if (setting == kOff) {
            for (Request& r : live_) set_state(r, KILN_RENDER_OUTLINE_WAITING, KILN_RENDER_OUTLINE_SWITCHED_OFF);
            last_[0] = last_[1] = Slot{};
            return p;
        }
        // (colour, pulse) pairs of the winners, best first; the two best get Kiln's groups (a pair keeps last step's
        // group while it is among them), the others share the group whose colour is nearest
        struct Pair {
            uint32_t rgba;
            bool pulse;
            size_t best;  // index into live_ of its best winner
            int slot = -1;
            bool merged = false;
        };
        std::vector<Pair> pairs;
        for (const auto& [e, i] : win) {
            const Request& r = live_[i];
            auto f = std::find_if(pairs.begin(), pairs.end(), [&](const Pair& x) { return x.rgba == r.rgba && x.pulse == r.pulse(); });
            if (f == pairs.end()) pairs.push_back({r.rgba, r.pulse(), i});
            else if (better(r, live_[f->best])) f->best = i;
        }
        std::sort(pairs.begin(), pairs.end(), [&](const Pair& a, const Pair& b) { return better(live_[a.best], live_[b.best]); });
        const size_t taken = std::min<size_t>(pairs.size(), 2);
        bool used[2] = {false, false};
        for (size_t k = 0; k < taken; ++k)
            for (int s = 0; s < 2; ++s)
                if (!used[s] && last_[s].used && last_[s].rgba == pairs[k].rgba && last_[s].pulse == pairs[k].pulse) {
                    pairs[k].slot = s;
                    used[s] = true;
                    break;
                }
        for (size_t k = 0; k < taken; ++k)
            if (pairs[k].slot < 0) {
                const int s = used[0] ? 1 : 0;
                pairs[k].slot = s;
                used[s] = true;
            }
        for (size_t k = 0; k < taken; ++k) slots[pairs[k].slot] = Slot{pairs[k].rgba, pairs[k].pulse, true};
        for (size_t k = taken; k < pairs.size(); ++k) {
            uint64_t best_d = UINT64_MAX;
            pairs[k].merged = true;
            for (size_t j = 0; j < taken; ++j) {
                const uint64_t d = distance(pairs[k].rgba, pairs[j].rgba);
                if (d < best_d) best_d = d, pairs[k].slot = pairs[j].slot;
            }
        }
        last_[0] = slots[0];
        last_[1] = slots[1];
        // targets in entity order (deterministic)
        std::vector<std::pair<KilnEntity, size_t>> order(win.begin(), win.end());
        std::sort(order.begin(), order.end());
        for (const auto& [e, i] : order) {
            Request& r = live_[i];
            const auto pf = std::find_if(pairs.begin(), pairs.end(), [&](const Pair& x) { return x.rgba == r.rgba && x.pulse == r.pulse(); });
            r.group = kGroups[pf->slot];
            r.merged = pf->merged ? 1 : 0;
            const auto m = models.find(e);
            const uint32_t model = m == models.end() ? 0 : m->second;
            p.index.emplace(e, p.targets.size());
            p.targets.push_back({e, model, kGroups[pf->slot]});
            plan_entities_.push_back(e);
            if (!model) set_state(r, KILN_RENDER_OUTLINE_WAITING, KILN_RENDER_OUTLINE_NO_MODEL);
            else if (r.state != KILN_RENDER_OUTLINE_SHOWN && r.state != KILN_RENDER_OUTLINE_GAME)
                set_state(r, KILN_RENDER_OUTLINE_WAITING, KILN_RENDER_OUTLINE_PENDING);
        }
        for (size_t i = 0; i < live_.size(); ++i)
            if (win.find(live_[i].entity)->second != i) {
                set_state(live_[i], KILN_RENDER_OUTLINE_WAITING, KILN_RENDER_OUTLINE_OUTSHONE);
                live_[i].group = 0;
            }
        return p;
    }

    // What the stage found for the plan of 'generation' (res[i] for its targets[i]).
    void results(uint64_t generation, const std::vector<uint8_t>& res) {
        std::lock_guard lk(mu_);
        if (generation != plan_generation_) return;  // an older plan's: the next one decides
        std::unordered_map<KilnEntity, uint8_t> by;  // one pass over the requests, not one per target
        by.reserve(plan_entities_.size());
        for (size_t i = 0; i < res.size() && i < plan_entities_.size(); ++i) by.emplace(plan_entities_[i], res[i]);
        for (Request& r : live_) {
            if (r.reason == KILN_RENDER_OUTLINE_OUTSHONE) continue;
            const auto it = by.find(r.entity);
            if (it == by.end()) continue;
            if (it->second == kShown) set_state(r, KILN_RENDER_OUTLINE_SHOWN, KILN_RENDER_OUTLINE_OK);
            else if (it->second == kGame) set_state(r, KILN_RENDER_OUTLINE_GAME, KILN_RENDER_OUTLINE_OK);
            else if (it->second == kNoModel) set_state(r, KILN_RENDER_OUTLINE_WAITING, KILN_RENDER_OUTLINE_NO_MODEL);
        }
    }

    struct Counts {
        uint32_t requests = 0, entities = 0, shown = 0, game = 0, no_model = 0;
    };
    Counts counts() const {
        std::lock_guard lk(mu_);
        Counts c;
        std::unordered_map<KilnEntity, int> state;  // per entity, its winner's: 1 shown, 2 game, 3 no model, 0 other
        for (const Request& r : live_) {
            ++c.requests;
            int s = 0;
            if (r.reason == KILN_RENDER_OUTLINE_OUTSHONE) s = -1;
            else if (r.state == KILN_RENDER_OUTLINE_SHOWN) s = 1;
            else if (r.state == KILN_RENDER_OUTLINE_GAME) s = 2;
            else if (r.reason == KILN_RENDER_OUTLINE_NO_MODEL) s = 3;
            auto [it, fresh] = state.try_emplace(r.entity, s);
            if (!fresh && s >= 0) it->second = s;
        }
        c.entities = static_cast<uint32_t>(state.size());
        for (const auto& [e, s] : state) {
            if (s == 1) ++c.shown;
            else if (s == 2) ++c.game;
            else if (s == 3) ++c.no_model;
        }
        return c;
    }

private:
    static bool better(const Request& a, const Request& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.seq > b.seq;
    }
    static uint64_t distance(uint32_t a, uint32_t b) {
        uint64_t d = 0;
        for (int s = 8; s <= 24; s += 8) {
            const int64_t x = static_cast<int64_t>((a >> s) & 0xFF) - static_cast<int64_t>((b >> s) & 0xFF);
            d += static_cast<uint64_t>(x * x);
        }
        return d;
    }
    static void set_state(Request& r, uint32_t state, uint32_t reason) {
        r.state = state;
        r.reason = reason;
    }
    const Request* find_any(KilnHandle h) const {
        for (const Request& r : live_)
            if (r.handle == h) return &r;
        for (const Request& r : ended_)
            if (r.handle == h) return &r;
        return nullptr;
    }
    void end_at(size_t i, uint32_t reason) {
        Request r = live_[i];
        r.state = KILN_RENDER_OUTLINE_ENDED;
        r.reason = reason;
        r.group = r.merged = 0;
        live_.erase(live_.begin() + static_cast<ptrdiff_t>(i));
        ended_.push_back(r);
        while (ended_.size() > kEndedKeep) ended_.pop_front();
    }
    template <class F>
    void end_if(F&& f, uint32_t reason) {
        for (size_t i = 0; i < live_.size();)
            if (f(live_[i])) end_at(i, reason);
            else ++i;
    }

    mutable std::mutex mu_;
    struct Pos {
        double p[3];
        uint32_t step;
    };
    std::unordered_map<KilnEntity, Pos> positions_;  // through-walls requests' entities (0.2.0-dev.31)
    std::vector<Request> live_;
    std::deque<Request> ended_;
    Slot last_[2];
    std::vector<KilnEntity> plan_entities_;
    uint64_t seq_ = 0, generation_ = 0, plan_generation_ = 0;
    Setting setting_ = kGameFirst;
};

// The entities' model handles between steps (main thread only). kiln.world's verified ECS reads resolve the engine's
// owners several times per read (tens of microseconds for entity_validate + ecs_snapshot), so reading every outlined
// entity every step would cost the main thread milliseconds with a few hundred outlines. A handle is read when it is
// due: a new entity, one whose handle the stage found naming no instance (stale: the model was destroyed or remade),
// and every entity once per kModelRefreshSteps steps (spread over the steps by entity id), at most kModelReadsPerStep
// reads a step, stale ones first. Between reads the stage's own check (Instance with the handle's generation) catches a
// destroyed model at once. A component that still names an instance the stage found destroyed (a hidden entity) is
// remembered as dead and planned as no model until it names another one.
class ModelCache {
public:
    // entities: the book's (sorted, unique). read(e) -> the entity's model handle now (0: none), called for those due.
    // Fills 'out' with every known live handle; returns how many were read.
    template <class Read>
    uint32_t update(const std::vector<KilnEntity>& entities, uint64_t step, Read&& read,
                    std::unordered_map<KilnEntity, uint32_t>& out) {
        for (auto it = map_.begin(); it != map_.end();)  // entities without a live request any more
            it = std::binary_search(entities.begin(), entities.end(), it->first) ? std::next(it) : map_.erase(it);
        for (KilnEntity e : entities) {
            auto [it, fresh] = map_.try_emplace(e);
            if (!fresh && it->second.due == kNotDue && (e + step) % kModelRefreshSteps == 0) it->second.due = kDue;
        }
        uint32_t reads = 0;
        for (const uint8_t pass : {kStale, kDue})
            for (KilnEntity e : entities) {
                if (reads >= kModelReadsPerStep) break;
                Entry& en = map_[e];
                if (en.due != pass) continue;
                const uint32_t m = read(e);
                ++reads;
                if (pass == kStale && m && m == en.model) en.dead = m;  // the component still names the destroyed one
                en.model = m;
                en.due = kNotDue;
            }
        for (KilnEntity e : entities) {
            const auto it = map_.find(e);
            if (it != map_.end() && it->second.model && it->second.model != it->second.dead) out[e] = it->second.model;
        }
        return reads;
    }
    // The stage found no instance for 'model', the handle planned for 'e': read it again first thing next step.
    void stale(KilnEntity e, uint32_t model) {
        const auto it = map_.find(e);
        if (it == map_.end() || !model || it->second.model != model || it->second.dead == model) return;
        it->second.due = kStale;
    }
    void clear() { map_.clear(); }
    size_t size() const { return map_.size(); }

private:
    enum : uint8_t { kNotDue = 0, kDue = 1, kStale = 2 };
    struct Entry {
        uint32_t model = 0;
        uint32_t dead = 0;  // a handle the stage found destroyed that the component still named at the next read
        uint8_t due = kDue;
    };
    std::unordered_map<KilnEntity, Entry> map_;
};

}  // namespace kout
