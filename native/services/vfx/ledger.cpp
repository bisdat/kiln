// SPDX-License-Identifier: MIT
// kiln.vfx's resource budget and its result-bearing definitions (ledger.h; package 17a S2, chapter 17.1 §5, I1, I5).
//
// The budget is the game's VFX table as the FxController update last read it (observe): its count limit is the capacity,
// the game's own VfxResources (counted in its archive at the start) are game_used, Kiln's VFX of loaded mods that hold the
// bytes Kiln served are kiln_used, and everything else in the table is reserved (Kiln's VFX of a mod that is no longer
// loaded, which stay for the process, and other loaders' resources). Before that reading, or when the system's state is
// not 2 (ready), the budget is NOT_READY with no counts: nothing is assumed.
//
// A receipt is what one definition (a mod's block of one GUID with one content) became. Its state is worked out again
// from the ledger whenever something changes (settle), so a reader only copies it. The game lists its VFX once, before
// any mod loads: a definition registers in this process only when this start built that block with that content and the
// table holds it with Kiln's bytes; anything else is refused now, or (AllowNextStart) written to the pending folder for the
// next start through kiln::capacity::write_atomic.
#include "ledger.h"
#include "pending.h"
#include "pending_cleanup.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "nlohmann/json.hpp"
#include "services/kiln_capacity_util.hpp"

namespace kvx::ledger {
namespace {

namespace fs = std::filesystem;

struct Receipt {
    Id guid{};
    std::string mod, name;
    uint64_t definition = 0;
    bool accepted = false;  // its definition is there at the next start: stored by a call of this process, or built at this
                            // start from the mod's own files or a file stored at an earlier start
    uint32_t code = 0;      // the refusal of its last call (what it says when nothing else does)
    std::string why;
    // what it says now (settle):
    uint32_t disposition = KILN_DEFINITION_REFUSED, slots = 0, now_code = KILN_DEFINE_NOT_FOUND;
    std::string now_why;
    uint64_t revision = 0;
    uint64_t age = 0;  // insertion order, for forgetting old refusals
};

#ifdef KILN_VFX_LEDGER_TEST
std::function<bool(const fs::path&, std::string_view)> g_writer;
#endif
struct Pending {
    Id id, guid;
    std::string mod, name;
    bool cancelled = false;
};
std::map<uint64_t, Pending> g_pending;
uint64_t g_transaction = 0;
std::mutex g_mu;  // everything below; never held during storage I/O
fs::path g_root;
bool g_budget_on = false, g_registration_on = false;
Boot g_boot;
std::map<Id, size_t> g_by_guid;      // GUID -> index in g_boot.built
std::set<std::string> g_retired;     // mods no longer loaded (release), until a block of theirs compiles again
bool g_seen = false;
Seen g_last;
std::map<Id, uint8_t> g_verdict;
std::map<Id, Receipt> g_receipts;
std::map<std::pair<std::string, std::string>, Id> g_file;  // (mod, name) -> the receipt whose definition its pending file holds
uint64_t g_age = 0;
KilnCapacityInfo g_pub{};  // the published budget
kiln::capacity::Revision g_rev;

const BuiltVfx* built_of(const Id& guid) {
    const auto it = g_by_guid.find(guid);
    return it == g_by_guid.end() ? nullptr : &g_boot.built[it->second];
}

uint8_t verdict_of(const Id& guid) {
    const auto it = g_verdict.find(guid);
    return it == g_verdict.end() ? static_cast<uint8_t>(kUnchecked) : it->second;
}

// Accepted next-start GUIDs and other in-flight transactions compete for the same admission room.
uint64_t waiting_for(const Request& q, uint64_t exclude = 0) {
    std::set<Id> later;
    for (const auto& [id, x] : g_receipts)
        if (x.accepted && x.guid != q.guid && !built_of(x.guid) && (x.mod != q.mod || x.name != q.name)) later.insert(x.guid);
    for (const auto& [token, p] : g_pending)
        if (token != exclude && !p.cancelled && p.guid != q.guid && !built_of(p.guid)) later.insert(p.guid);
    return later.size();
}
bool fits_now(const Request& q, uint64_t exclude) {
    if (built_of(q.guid)) return true;
    const uint64_t cap = g_seen ? g_last.limit : g_boot.admission;
    const uint64_t taken = g_seen ? g_last.count : uint64_t{g_boot.game_vfx} + g_boot.built.size();
    return taken < cap && waiting_for(q,exclude) < cap - taken;
}

// The budget as it is now (revision not set).
KilnCapacityInfo compute() {
    KilnCapacityInfo n{};
    n.size = sizeof n;
    const auto scope = kiln::capacity::Scope::process();
    if (!g_seen) {
        kiln::capacity::not_ready(&n, KILN_CAPACITY_NOT_READY, scope, 0);
        return n;
    }
    if (!g_boot.game_counted) {
        kiln::capacity::not_ready(&n, KILN_CAPACITY_UNKNOWN_EXTERNAL, scope, 0);
        return n;
    }
    uint64_t members = 0, active = 0, retired = 0;
    for (const BuiltVfx& b : g_boot.built) {
        const uint8_t v = verdict_of(b.guid);
        if (v == kUnchecked) {
            kiln::capacity::not_ready(&n, KILN_CAPACITY_ACCOUNTING_MISMATCH, scope, 0);
            return n;
        }
        if (v == kMissing) continue;
        ++members;
        if (v == kOwn) (g_retired.count(b.mod) ? retired : active) += 1;
    }
    const uint64_t game = g_boot.game_vfx;
    if (g_last.count < game + members) {  // fewer entries than the game's and Kiln's: never a negative "other"
        kiln::capacity::not_ready(&n, KILN_CAPACITY_ACCOUNTING_MISMATCH, scope, 0);
        return n;
    }
    const uint64_t foreign = g_last.count - game - members;  // other loaders' resources
    // in the table and not Kiln's in use: a retired mod's, other bytes under a Kiln GUID, bytes not compared; and foreign
    std::set<Id> writing;
    for (const auto& [token, p] : g_pending)
        if (!p.cancelled && !built_of(p.guid)) writing.insert(p.guid);
    const uint64_t reserved = members - active + foreign + writing.size();
    kiln::capacity::publish(&n, g_last.limit, game, active, reserved, scope, 0, false);
    return n;
}

// What the receipt says now.
void settle(Receipt& r) {
    uint32_t disp = KILN_DEFINITION_REFUSED, slots = 0, code = r.code ? r.code : uint32_t{KILN_DEFINE_NOT_FOUND};
    std::string why = r.why;
    const BuiltVfx* b = built_of(r.guid);
    const bool same = b && b->mod == r.mod && b->definition == r.definition;
    if (same && g_seen) {
        switch (verdict_of(r.guid)) {
            case kOwn:
                if (g_retired.count(r.mod)) {
                    disp = KILN_DEFINITION_RETAINED, slots = 1, code = KILN_DEFINE_OWNER_INACTIVE;
                    why = "the mod that defined it is not loaded; its VFX stays in the game's table until the process ends";
                } else {
                    disp = KILN_DEFINITION_REGISTERED, slots = 1, code = KILN_DEFINE_NONE, why.clear();
                }
                break;
            case kOther:
                code = KILN_DEFINE_GAME_REFUSED;
                why = "the game's VFX table holds other data under its GUID (another loader's resource?): it is not this definition";
                break;
            case kMissing:
                code = KILN_DEFINE_GAME_REFUSED;
                why = "the game's VFX table does not have it: it was not registered when the game listed its VFX (kiln.log says why)";
                break;
            default:
                code = KILN_DEFINE_NOT_READY;
                why = "it is in the game's VFX table, but its bytes there could not be compared with what Kiln served";
                break;
        }
    } else if (r.accepted) {
        disp = KILN_DEFINITION_ACCEPTED_NEXT_START, code = KILN_DEFINE_NONE, why.clear();
    } else if (same) {
        code = KILN_DEFINE_NOT_READY;
        why = "built at this start; the game's VFX table is read at the first update of a client world, and then its receipt answers";
    }
    if (code != KILN_DEFINE_NONE && why.empty()) why = code == KILN_DEFINE_NOT_FOUND ? "no definition of this id" : "refused";
    if (disp != r.disposition || slots != r.slots || code != r.now_code || why != r.now_why) {
        r.disposition = disp, r.slots = slots, r.now_code = code, r.now_why = std::move(why);
        r.revision = 0;  // set by publish
    }
}

// Settles every receipt and publishes the budget; the revision rises when anything published changed.
void publish() {
    bool changed = false;
    for (auto& [id, r] : g_receipts) {
        settle(r);
        changed |= r.revision == 0;
    }
    KilnCapacityInfo n = compute();
    KilnCapacityInfo old = g_pub;
    old.revision = 0;
    if (!g_pub.size || std::memcmp(&old, &n, sizeof n) != 0) changed = true;
    n.revision = changed ? g_rev.next() : g_pub.revision;
    g_pub = n;
    for (auto& [id, r] : g_receipts)
        if (r.revision == 0) r.revision = n.revision;
}

void forget_old_refusals() {
    while (g_receipts.size() > kReceiptsMax) {
        auto oldest = g_receipts.end();
        for (auto it = g_receipts.begin(); it != g_receipts.end(); ++it)
            if (it->second.disposition == KILN_DEFINITION_REFUSED &&
                std::none_of(g_pending.begin(), g_pending.end(), [&](const auto& p){return p.second.id == it->first;}) && (oldest == g_receipts.end() || it->second.age < oldest->second.age)) oldest = it;
        if (oldest == g_receipts.end()) return;
        for (auto f = g_file.begin(); f != g_file.end();)
            f = f->second == oldest->first ? g_file.erase(f) : std::next(f);
        g_receipts.erase(oldest);
    }
}

void fill(const Id& id, const Receipt& r, KilnDefinitionReply* out) {
    kiln::capacity::reply_init(out, KILN_DEFINITION_VFX, id.data(), r.guid.data());
    if (r.now_code == KILN_DEFINE_NONE) kiln::capacity::reply_ok(out, r.disposition, r.slots, r.revision);
    else kiln::capacity::reply_refused(out, r.now_code, r.revision, r.disposition, r.slots);
}

// The file of (mod, name) now holds receipt `id`'s definition: a receipt whose definition it held before is superseded.
void file_holds(const std::string& mod, const std::string& name, const Id* id) {
    const auto key = std::make_pair(mod, name);
    const auto it = g_file.find(key);
    if (it != g_file.end() && (!id || it->second != *id)) {
        if (const auto r = g_receipts.find(it->second); r != g_receipts.end() && r->second.accepted) {
            r->second.accepted = false;
            r->second.code = KILN_DEFINE_SUPERSEDED;
            r->second.why = "a later definition of the block " + name + " replaced the one stored for the next start";
        }
        g_file.erase(it);
    }
    if (id) g_file[key] = *id;
}

Receipt& receipt_of(const Id& id) {
    Receipt& r = g_receipts[id];
    if (!r.age) r.age = ++g_age;
    return r;
}

}  // namespace

// ---- the process ----
#ifdef KILN_VFX_LEDGER_TEST
void set_writer_for_tests(std::function<bool(const fs::path&, std::string_view)> writer) {
    std::lock_guard lk(g_mu);
    g_writer = std::move(writer);
}
#endif

void reset() {
    std::lock_guard lk(g_mu);
    g_root.clear();
    g_pending.clear();
    g_budget_on = g_registration_on = false;
    g_boot = {};
    g_by_guid.clear();
    g_retired.clear();
    g_seen = false;
    g_last = {};
    g_verdict.clear();
    g_receipts.clear();
    g_file.clear();
    g_age = 0;
    g_pub = {};
}

void set_pending_root(const std::filesystem::path& root) {
    std::lock_guard lk(g_mu);
    g_root = root;
}

void set_available(bool budget_on, bool registration_on) {
    std::lock_guard lk(g_mu);
    g_budget_on = budget_on;
    g_registration_on = budget_on && registration_on;
}

bool budget_available() {
    std::lock_guard lk(g_mu);
    return g_budget_on;
}

bool registration_available() {
    std::lock_guard lk(g_mu);
    return g_registration_on;
}

void boot(Boot b) {
    std::lock_guard lk(g_mu);
    g_boot = std::move(b);
    g_by_guid.clear();
    for (size_t i = 0; i < g_boot.built.size(); ++i) g_by_guid.emplace(g_boot.built[i].guid, i);
    for (const Stored& s : g_boot.stored) {
        if (s.registration == Id{}) continue;
        Receipt& r = receipt_of(s.registration);
        r.guid = s.guid, r.mod = s.mod, r.name = s.name, r.definition = s.definition;
        // built from its file: it is there at the next start too; refused by this start's build: what refused it
        r.accepted = s.built;
        r.code = s.built ? 0u : (s.code ? s.code : uint32_t{KILN_DEFINE_GAME_REFUSED});
        r.why = s.built ? std::string() : (s.why.empty() ? "this start did not build it (kiln.log says why)" : s.why);
        if (s.built) g_file[{s.mod, s.name}] = s.registration;
    }
    publish();
}

void forget() {
    std::lock_guard lk(g_mu);
    g_boot.built.clear();
    g_boot.stored.clear();
    g_by_guid.clear();
    g_seen = false;
    g_verdict.clear();
    publish();
}

void observe(const Seen& s) {
    std::lock_guard lk(g_mu);
    if (s.init_state != 2) {
        g_seen = false;
        g_last = {};
        g_verdict.clear();
        publish();
        return;
    }
    g_last = s;
    g_seen = true;
    g_verdict.clear();
    for (const auto& [guid, v] : s.kiln) g_verdict[guid] = v;
    publish();
}

bool observed() {
    std::lock_guard lk(g_mu);
    return g_seen;
}

uint32_t release(const std::string& mod) {
    std::lock_guard lk(g_mu);
    g_retired.insert(mod);
    for (auto& [token, p] : g_pending) if (p.mod == mod) p.cancelled = true;
    uint32_t n = 0;
    for (const BuiltVfx& b : g_boot.built) n += b.mod == mod;
    publish();
    return n;
}

void activate(const std::string& mod) {
    std::lock_guard lk(g_mu);
    if (g_retired.erase(mod)) publish();
}

void kept(const std::string& mod, const std::string& name, uint64_t definition) {
    std::lock_guard lk(g_mu);
    const auto it = g_file.find({mod, name});
    if (it == g_file.end()) return;
    if (const auto r = g_receipts.find(it->second); r != g_receipts.end() && r->second.definition == definition) return;
    file_holds(mod, name, nullptr);
    publish();
}

// ---- the C members ----

KilnResult budget(KilnCapacityInfo* out) {
    std::lock_guard lk(g_mu);
    if (!g_budget_on) return KILN_ERR_UNAVAILABLE;
    const uint32_t size = out->size;
    if (!g_pub.size) {
        kiln::capacity::not_ready(out, KILN_CAPACITY_NOT_READY, kiln::capacity::Scope::process(), g_rev.current());
        return KILN_OK;
    }
    *out = g_pub;
    out->size = size;
    return KILN_OK;
}

KilnResult define(const Request& q, KilnDefinitionReply* out, std::string* report) {
    auto bad = [&](const std::string& why) {
        *report = report_json(why, {});
        return KILN_ERR_INVALID_ARGUMENT;
    };
    if (!out || out->size < sizeof(KilnDefinitionReply)) return bad("the answer struct is missing or smaller than KilnDefinitionReply");
    if (q.timing != KILN_DEFINE_REQUIRE_REGISTERED && q.timing != KILN_DEFINE_ALLOW_NEXT_START)
        return bad("timing " + std::to_string(q.timing) + " is not REQUIRE_REGISTERED (0) or ALLOW_NEXT_START (1)");
    if (!valid_mod(q.mod)) return bad("\"" + q.mod + "\" is not a mod id (letters, digits, '-', '_', '.'; at most 64)");
    if (!valid_name(q.name)) return bad("\"" + q.name + "\" is not a block name (letters, digits, '-', '_', '.'; at most 128)");
    std::unique_lock lk(g_mu);
    if (!g_registration_on) {
        *report = report_json("blocks.vfx_registration is off in this process (Capabilities.Why says why)", {});
        return KILN_ERR_UNAVAILABLE;
    }
    const Id id = registration_id(q.mod, q.guid, q.canonical);
    for (const auto& [token, p] : g_pending) {
        if (p.id == id || (p.mod == q.mod && p.name == q.name) || p.guid == q.guid) {
            kiln::capacity::reply_init(out, KILN_DEFINITION_VFX, id.data(), q.guid.data());
            kiln::capacity::reply_refused(out, KILN_DEFINE_NOT_READY, g_pub.revision);
            *report = report_json("a definition of this name or GUID is being stored; retry after it finishes", q.warnings);
            return KILN_OK;
        }
    }
    const bool pending_collision = std::any_of(g_receipts.begin(), g_receipts.end(), [&](const auto& entry) {
        const Receipt& other = entry.second;
        return other.accepted && other.guid == q.guid && (other.mod != q.mod || other.name != q.name);
    });
    // A second name can hash to the existing receipt (name is not part of its identity).
    // Refuse that call without changing the receipt/file of the first name.
    if (pending_collision && g_receipts.count(id)) {
        kiln::capacity::reply_init(out, KILN_DEFINITION_VFX, id.data(), q.guid.data());
        kiln::capacity::reply_refused(out, KILN_DEFINE_ID_COLLISION, g_pub.revision);
        *report = report_json("another block name already stores this GUID for the next start", q.warnings);
        return KILN_OK;
    }
    Receipt& r = receipt_of(id);
    r.guid = q.guid, r.mod = q.mod, r.name = q.name, r.definition = q.definition;
    uint32_t code = 0;
    std::string why;
    auto refuse = [&](uint32_t c, std::string text) { code = c, why = std::move(text); };
    const BuiltVfx* b = built_of(q.guid);
    const bool same = b && b->mod == q.mod && b->definition == q.definition;
    const auto file = g_file.find({q.mod, q.name});
    const bool stored_now = r.accepted && file != g_file.end() && file->second == id;  // its pending file holds this definition
    // writes the definition for the next start (AllowNextStart, when only the moment stands in the way)
    auto store = [&] {
        if (g_root.empty()) {
            refuse(KILN_DEFINE_STORAGE_FAILURE, "kiln.vfx has no data folder to store the definition in");
            return;
        }
        const fs::path path = g_root / fs::path(std::u8string(q.mod.begin(), q.mod.end())) /
                              fs::path(std::u8string(q.name.begin(), q.name.end())).concat(u8".json");
        const std::string bytes = pending_text(id, q.name, q.block);
        if (bytes.size() > kvx_pending::kFileBytes) {
            refuse(KILN_DEFINE_STORAGE_FAILURE, "the pending definition exceeds the replay file bound of 65536 bytes");
            return;
        }
#ifdef KILN_VFX_LEDGER_TEST
        const auto writer = g_writer;
#endif
        const uint64_t token = ++g_transaction;
        g_pending.emplace(token, Pending{id, q.guid, q.mod, q.name});
        bool wrote = false;
        try {
            r.code = KILN_DEFINE_NOT_READY;
            r.why = "the definition is being stored";
            publish();
            lk.unlock();
            std::unique_lock storage(kvx::pending_io_mutex());
#ifdef KILN_VFX_LEDGER_TEST
            wrote = writer ? writer(path, bytes) : kiln::capacity::write_atomic(path, bytes);
#else
            wrote = kiln::capacity::write_atomic(path, bytes);
#endif
            lk.lock();
            const bool cancelled = g_pending.at(token).cancelled;
            const bool capacity_lost = !fits_now(q,token);
            bool cleaned = true;
            if ((cancelled || capacity_lost) && wrote) {
                // Keep the path reserved until cleanup finishes so another writer cannot lose its file.
                lk.unlock();
                cleaned = kvx::discard_pending(path,bytes);
                lk.lock();
                file_holds(q.mod, q.name, nullptr);
            }
            g_pending.erase(token);
            if (cancelled) {
                refuse(KILN_DEFINE_OWNER_INACTIVE, "the mod unloaded while its definition was being stored");
            } else if (!wrote) {
                refuse(KILN_DEFINE_STORAGE_FAILURE, "could not write the pending VFX definition");
            } else if (capacity_lost) {
                refuse(KILN_DEFINE_CAPACITY_EXCEEDED, "the sampled table no longer has room for the definition after storage completed");
            } else {
                file_holds(q.mod, q.name, &id);
                r.accepted = true;
            }
            if (!cleaned) why += "; cleanup of the unacknowledged pending file could not be confirmed";
        } catch (...) {
            if (!lk.owns_lock()) lk.lock();
            g_pending.erase(token);
            refuse(KILN_DEFINE_STORAGE_FAILURE, "storing the pending VFX definition threw an exception");
        }
    };
    if (!q.owner_live) {
        refuse(KILN_DEFINE_OWNER_INACTIVE, "the mod's owner is no longer active");
    } else if (pending_collision) {
        refuse(KILN_DEFINE_ID_COLLISION, "another owner or block name already has an accepted definition of this GUID for the next start");
    } else if (b && b->mod != q.mod) {
        refuse(KILN_DEFINE_ID_COLLISION, "the GUID is the VFX " + b->mod + ":" + b->name + "'s (\"was\" names another mod's block?)");
    } else {
        // a block of the mod compiles: the mod is loaded
        g_retired.erase(q.mod);
        if (same && g_seen) {
            // registered or not, the game's table says (settle)
        } else if (same) {
            // this start built it; the table is not read yet
            if (q.timing == KILN_DEFINE_REQUIRE_REGISTERED)
                refuse(KILN_DEFINE_NOT_READY, "built at this start; the game's VFX table is read at the first update of a client world (ask its "
                                              "receipt then)");
            else if (file == g_file.end() || file->second == id)
                r.accepted = true;  // the file it was built from builds it at the next start too
            else
                store();  // the pending file holds another content of the block now: this one replaces it
        } else if (stored_now) {
            // the same definition stored again: nothing new; still not registered in this process
            if (q.timing == KILN_DEFINE_REQUIRE_REGISTERED)
                refuse(KILN_DEFINE_REGISTRY_CLOSED, "the game lists its VFX once, before mods load: this definition is stored for the next start");
        } else {
            // a content this start did not build: the game lists its VFX once, so only the next start can have it
            uint64_t cap = 0, taken = 0, waiting = 0;
            bool over = false;
            if (!b) {
                // a new GUID takes a slot at the next start: what the table holds now, the new ones accepted in this process, and it
                cap = g_seen ? g_last.limit : g_boot.admission;
                taken = g_seen ? g_last.count : uint64_t{g_boot.game_vfx} + g_boot.built.size();
                waiting = waiting_for(q);
                over = taken >= cap || waiting >= cap - taken;
            }
            if (over)
                refuse(KILN_DEFINE_CAPACITY_EXCEEDED, "the game's VFX table holds " + std::to_string(cap) + " resources; " + std::to_string(taken) +
                                                          " are taken (the game's own " + std::to_string(g_boot.game_vfx) + ") and " +
                                                          std::to_string(waiting) + " new VFX are accepted for the next start: this one does not fit");
            else if (q.timing == KILN_DEFINE_REQUIRE_REGISTERED)
                refuse(KILN_DEFINE_REGISTRY_CLOSED, std::string("the game lists its VFX once, before mods load: ") +
                                                        (b ? "this changed definition" : "a new VFX") +
                                                        " is there from the next start (define it with AllowNextStart)");
            else
                store();
        }
    }
    r.code = code, r.why = code ? why : std::string();
    if (code && code != KILN_DEFINE_NOT_READY && code != KILN_DEFINE_REGISTRY_CLOSED) {
        // a refusal of the content itself: not what the next start has either, unless its pending file still holds it
        if (const auto f = g_file.find({q.mod, q.name}); f == g_file.end() || f->second != id) r.accepted = false;
    }
    publish();
    // the answer to this call: a call that was refused says why, whatever the receipt keeps for later (a definition stored by an
    // earlier call stays stored); a record it still occupies is Retained with its slot, never reported free
    if (code) {
        const bool occupies = r.disposition == KILN_DEFINITION_REGISTERED || r.disposition == KILN_DEFINITION_RETAINED;
        kiln::capacity::reply_init(out, KILN_DEFINITION_VFX, id.data(), r.guid.data());
        kiln::capacity::reply_refused(out, code, r.revision, occupies ? KILN_DEFINITION_RETAINED : KILN_DEFINITION_REFUSED, occupies ? r.slots : 0);
        *report = report_json(why, q.warnings);
    } else {
        fill(id, r, out);
        *report = report_json(r.now_code ? r.now_why : std::string(), q.warnings);
    }
    forget_old_refusals();
    return KILN_OK;
}

KilnResult receipt(const uint8_t registration_id[16], KilnDefinitionReply* out, std::string* report) {
    if (!out || out->size < sizeof(KilnDefinitionReply)) {
        *report = report_json("the answer struct is missing or smaller than KilnDefinitionReply", {});
        return KILN_ERR_INVALID_ARGUMENT;
    }
    Id id{};
    if (registration_id) std::memcpy(id.data(), registration_id, 16);
    if (id == Id{}) {
        *report = report_json("the empty registration id", {});
        return KILN_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lk(g_mu);
    if (!g_registration_on) {
        *report = report_json("blocks.vfx_registration is off in this process (Capabilities.Why says why)", {});
        return KILN_ERR_UNAVAILABLE;
    }
    const auto it = g_receipts.find(id);
    if (it == g_receipts.end()) {
        kiln::capacity::reply_init(out, KILN_DEFINITION_VFX, id.data(), Id{}.data());
        kiln::capacity::reply_refused(out, KILN_DEFINE_NOT_FOUND, g_pub.size ? g_pub.revision : g_rev.current());
        *report = report_json("no VFX definition of this process (or stored for it at an earlier start) has this registration id", {});
        return KILN_OK;
    }
    fill(id, it->second, out);
    *report = report_json(it->second.now_code ? it->second.now_why : std::string(), {});
    return KILN_OK;
}

uint64_t revision() {
    std::lock_guard lk(g_mu);
    return g_pub.size ? g_pub.revision : g_rev.current();
}

// ---- shared ----

std::string report_json(std::string reason, std::vector<std::string> warnings) {
    for (;;) {
        nlohmann::json r = {{"reason", reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(reason)}, {"warnings", nlohmann::json::array()}};
        for (const std::string& w : warnings) r["warnings"].push_back(w.size() > 256 ? w.substr(0, 256) : w);
        std::string out = r.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        if (out.size() < kReportMax) return out;
        if (!warnings.empty()) warnings.pop_back();
        else reason = reason.substr(0, reason.size() / 2);
    }
}

std::string uuid_text(const Id& id) {
    char b[40];
    std::snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", id[0], id[1], id[2], id[3], id[4], id[5], id[6],
                  id[7], id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
    return b;
}

bool uuid_parse(std::string_view text, Id* out) {
    std::string h;
    for (char c : text)
        if (c != '-') h += c;
    if (h.size() != 32) return false;
    for (size_t i = 0; i < 16; ++i) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nibble(h[2 * i]), lo = nibble(h[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        (*out)[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

Id registration_id(const std::string& mod, const Id& guid, std::string_view canonical) {
    Id id{};
    kiln::capacity::registration_id(mod, KILN_DEFINITION_VFX, guid.data(), kiln::capacity::sha256(canonical).data(), id.data());
    return id;
}

std::string pending_text(const Id& registration, const std::string& name, const std::string& block) {
    // the name is a valid_name (no character JSON escapes); the block is the builder's own JSON text
    return "{\"format\":1,\"registration\":\"" + uuid_text(registration) + "\",\"vfx\":{\"" + name + "\":" + block + "}}\n";
}

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

bool valid_mod(std::string_view mod) {
    if (mod.empty() || mod.size() > 64 || mod == "." || mod == "..") return false;
    for (char c : mod)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

}  // namespace kvx::ledger
