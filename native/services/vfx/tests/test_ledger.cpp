// SPDX-License-Identifier: MIT
// kiln.vfx's resource budget and result-bearing definitions (ledger.cpp, package 17a S2) on a host compiler: the budget
// from what the start built and what the update thread read of the game's table, the answers of define_vfx_result for
// each timing, the receipts, and the pending files. No game, no Windows: the ledger never calls the game. tests/run.sh.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../ledger.h"
#include "nlohmann/json.hpp"
#include "services/kiln_capacity_util.hpp"

namespace fs = std::filesystem;
using namespace kvx;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    (ok ? g_pass : g_fail)++;
}

static ledger::Id guid_n(uint32_t n) {
    ledger::Id g{};
    g[0] = 0x4b;
    std::memcpy(g.data() + 4, &n, 4);
    g[15] = 0x17;
    return g;
}

static KilnCapacityInfo budget() {
    KilnCapacityInfo b{};
    b.size = sizeof b;
    ledger::budget(&b);
    return b;
}

struct Answer {
    KilnResult rc;
    KilnDefinitionReply r;
    std::string report;
    std::string reason() const {
        const auto j = nlohmann::json::parse(report, nullptr, false);
        return j.is_object() && j.contains("reason") && j["reason"].is_string() ? j["reason"].get<std::string>() : std::string();
    }
};

static std::string block_json(int seed) { return "{\"particles\":[{\"burst\":" + std::to_string(seed) + ",\"lifetime\":0.5}]}"; }

static ledger::Request request(const std::string& mod, const std::string& name, uint32_t n, uint64_t definition, uint32_t timing,
                               int seed = 1) {
    ledger::Request q;
    q.mod = mod;
    q.name = name;
    q.guid = guid_n(n);
    q.definition = definition;
    q.block = block_json(seed);
    q.canonical = nlohmann::json::parse(q.block).dump();
    q.timing = timing;
    return q;
}

static Answer define(const ledger::Request& q) {
    Answer a{};
    a.r.size = sizeof a.r;
    a.rc = ledger::define(q, &a.r, &a.report);
    return a;
}

static Answer receipt(const ledger::Id& id) {
    Answer a{};
    a.r.size = sizeof a.r;
    a.rc = ledger::receipt(id.data(), &a.r, &a.report);
    return a;
}

static ledger::Id id_of(const KilnDefinitionReply& r) {
    ledger::Id id{};
    std::memcpy(id.data(), r.registration_id, 16);
    return id;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static std::string stored_registration(const fs::path& p) {
    const auto j = nlohmann::json::parse(read_file(p), nullptr, false);
    return j.is_object() && j.contains("registration") && j["registration"].is_string() ? j["registration"].get<std::string>() : std::string();
}

static size_t files_in(const fs::path& dir) {
    std::error_code ec;
    size_t n = 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file()) ++n;
    return n;
}

// A start that built `n` VFX of `mod` (definitions 100 + i), against the client's numbers: the game's code says 2,048,
// its archive has 1,575.
static ledger::Boot boot_of(const std::string& mod, uint32_t n, uint32_t admission = 2048, uint32_t game = 1575) {
    ledger::Boot b;
    b.admission = admission;
    b.game_vfx = game;
    b.game_counted = true;
    for (uint32_t i = 0; i < n; ++i) b.built.push_back({guid_n(i), mod, "fx" + std::to_string(i), 100 + i});
    return b;
}

static ledger::Seen seen_of(const ledger::Boot& b, uint32_t limit, uint64_t extra = 0, uint8_t verdict = ledger::kOwn, uint32_t state = 2) {
    ledger::Seen s;
    s.limit = limit;
    s.init_state = state;
    uint64_t members = 0;
    for (const auto& v : b.built) {
        s.kiln.push_back({v.guid, verdict});
        members += verdict != ledger::kMissing;
    }
    s.count = b.game_vfx + members + extra;
    return s;
}

static fs::path fresh_root(const fs::path& base) {
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    ledger::reset();
    ledger::set_pending_root(base / "pending");
    ledger::set_available(true, true);
    return base / "pending";
}

static void test_budget(const fs::path& base) {
    std::printf("-- the budget\n");
    fresh_root(base);
    auto b0 = budget();
    check(b0.readiness == KILN_CAPACITY_NOT_READY && !(b0.flags & KILN_CAPACITY_READY) && b0.capacity == 0 && b0.revision >= 1,
          "before the start's build: NOT_READY, no counts");
    const ledger::Boot boot = boot_of("fxmod", 12);
    ledger::boot(boot);
    auto b1 = budget();
    check(b1.readiness == KILN_CAPACITY_NOT_READY && b1.capacity == 0 && b1.kiln_used == 0,
          "built, the game's table not read yet: NOT_READY with zero counts (no count assumed)");
    check((b1.flags & KILN_CAPACITY_PROCESS) && b1.world_kind == KILN_CAPACITY_NO_WORLD_KIND && b1.generation == 0, "process scope");
    ledger::observe(seen_of(boot, 2048, 0, ledger::kOwn, 1));
    check(budget().readiness == KILN_CAPACITY_NOT_READY && !ledger::observed(), "the system's state is 1 (waiting): still not read");
    ledger::observe(seen_of(boot, 2048));
    const auto b2 = budget();
    check(ledger::observed() && b2.readiness == KILN_CAPACITY_VALID && (b2.flags & KILN_CAPACITY_READY), "state 2: ready");
    check(b2.capacity == 2048 && b2.game_used == 1575 && b2.kiln_used == 12 && b2.reserved == 0,
          "budget 2048 / 1575 / 12 / 0: " + std::to_string(b2.capacity) + " / " + std::to_string(b2.game_used) + " / " +
              std::to_string(b2.kiln_used) + " / " + std::to_string(b2.reserved));
    check(!(b2.flags & KILN_CAPACITY_EXPANDED), "not expanded: the game's own table");
    check(b2.revision > b1.revision, "the revision rose with the reading");
    ledger::observe(seen_of(boot, 2048));
    check(budget().revision == b2.revision, "the same reading again: the same revision");
    // the capacity is the game's: a table read with another limit says that one
    ledger::observe(seen_of(boot, 4096));
    check(budget().capacity == 4096, "the capacity is the limit read from the game's table, not a constant");
    ledger::observe(seen_of(boot, 2048));
    // another loader's resources
    ledger::observe(seen_of(boot, 2048, 3));
    const auto b3 = budget();
    check(b3.readiness == KILN_CAPACITY_VALID && b3.reserved == 3 && b3.kiln_used == 12, "three resources of another loader: reserved 3");
    // a mod that is no longer loaded: its VFX stay in the game's table, reserved, not Kiln's in use
    check(ledger::release("fxmod") == 12, "release: 12 VFX of the mod are kept for it");
    const auto b4 = budget();
    check(b4.kiln_used == 0 && b4.reserved == 15 && b4.game_used + b4.kiln_used + b4.reserved <= b4.capacity,
          "a retired mod's VFX move to reserved: kiln 0, reserved 15");
    ledger::activate("fxmod");
    const auto b5 = budget();
    check(b5.kiln_used == 12 && b5.reserved == 3, "loaded again: back to Kiln's (never counted twice, never for ever)");
    // what does not add up is never a budget
    ledger::Seen low = seen_of(boot, 2048);
    low.count = 1575 + 5;  // fewer resources than the game's and Kiln's
    ledger::observe(low);
    const auto b6 = budget();
    check(b6.readiness == KILN_CAPACITY_ACCOUNTING_MISMATCH && b6.capacity == 0 && b6.kiln_used == 0 && !(b6.flags & KILN_CAPACITY_READY),
          "a table with fewer entries than the game's and Kiln's: ACCOUNTING_MISMATCH, zero counts (not negative)");
    ledger::Seen over = seen_of(boot, 2048);
    over.count = 2049;
    ledger::observe(over);
    check(budget().readiness == KILN_CAPACITY_ACCOUNTING_MISMATCH, "more entries than the limit: ACCOUNTING_MISMATCH");
    // Kiln's VFX with other bytes or bytes not compared: in the table, not Kiln's in use
    ledger::observe(seen_of(boot, 2048, 0, ledger::kOther));
    const auto b7 = budget();
    check(b7.readiness == KILN_CAPACITY_VALID && b7.kiln_used == 0 && b7.reserved == 12, "other bytes under Kiln's GUIDs: reserved, not Kiln's");
    ledger::observe(seen_of(boot, 2048, 0, ledger::kMissing));
    const auto b8 = budget();
    check(b8.readiness == KILN_CAPACITY_VALID && b8.kiln_used == 0 && b8.reserved == 0, "not in the game's table: no slot counted");
    // the game's own count not read from its archive: another loader cannot be told from the game
    ledger::Boot unknown = boot;
    unknown.game_counted = false;
    ledger::boot(unknown);
    ledger::observe(seen_of(unknown, 2048));
    const auto b9 = budget();
    check(b9.readiness == KILN_CAPACITY_UNKNOWN_EXTERNAL && (b9.flags & KILN_CAPACITY_EXTERNAL_UNKNOWN) && b9.capacity == 0,
          "the game's count not counted: UNKNOWN_EXTERNAL, nothing promised");
    ledger::set_available(false, false);
    KilnCapacityInfo off{};
    off.size = sizeof off;
    check(ledger::budget(&off) == KILN_ERR_UNAVAILABLE, "vfx.budget off: KILN_ERR_UNAVAILABLE");
}

static void test_define(const fs::path& base) {
    std::printf("-- definitions\n");
    const fs::path root = fresh_root(base);
    const ledger::Boot boot = boot_of("fxmod", 2);
    ledger::boot(boot);
    // a new GUID, registry closed: RequireRegistered refuses and writes nothing
    auto a = define(request("fxmod", "spark", 50, 7, KILN_DEFINE_REQUIRE_REGISTERED));
    check(a.rc == KILN_OK && !a.r.ok && a.r.disposition == KILN_DEFINITION_REFUSED && a.r.reason_code == KILN_DEFINE_REGISTRY_CLOSED &&
              a.r.slots_consumed == 0 && a.r.kind == KILN_DEFINITION_VFX,
          "a late new GUID under RequireRegistered: RegistryClosed, 0 slots: " + a.reason());
    check(!a.reason().empty(), "a refusal has a reason text");
    check(files_in(base) == 0, "and no pending file");
    // the same under AllowNextStart: stored for the next start
    const auto q = request("fxmod", "spark", 50, 7, KILN_DEFINE_ALLOW_NEXT_START);
    a = define(q);
    check(a.rc == KILN_OK && a.r.ok && a.r.disposition == KILN_DEFINITION_ACCEPTED_NEXT_START && a.r.slots_consumed == 0 &&
              a.r.reason_code == KILN_DEFINE_NONE && a.reason().empty(),
          "a late new GUID under AllowNextStart: AcceptedNextStart, 0 slots");
    const ledger::Id id = id_of(a.r);
    check(std::memcmp(a.r.definition_guid, q.guid.data(), 16) == 0, "the reply names the definition's GUID");
    check(id == ledger::registration_id("fxmod", q.guid, q.canonical), "the registration id: UUIDv5 of mod, kind, GUID and SHA-256");
    {
        uint8_t want[16];
        kiln::capacity::registration_id("fxmod", KILN_DEFINITION_VFX, q.guid.data(), kiln::capacity::sha256(q.canonical).data(), want);
        check(std::memcmp(want, id.data(), 16) == 0, "the same id kiln_capacity_util.hpp derives");
    }
    const fs::path file = root / "fxmod" / "spark.json";
    check(fs::exists(file) && !fs::exists(fs::path(file) += ".tmp"), "the pending file is there, no temporary file left");
    const auto doc = nlohmann::ordered_json::parse(read_file(file), nullptr, false);
    check(doc.is_object() && doc.value("format", 0) == 1 && stored_registration(file) == ledger::uuid_text(id) && doc.contains("vfx") &&
              doc["vfx"].is_object() && doc["vfx"].contains("spark") && doc["vfx"]["spark"].dump() == nlohmann::ordered_json::parse(q.block).dump(),
          "the file is a blocks file of the one block, in its own key order, with the registration id");
    const uint64_t rev = a.r.snapshot_revision;
    auto again = define(q);
    check(again.r.ok && again.r.disposition == KILN_DEFINITION_ACCEPTED_NEXT_START && id_of(again.r) == id && again.r.snapshot_revision == rev,
          "the same definition again: the same answer, nothing new (no new revision)");
    auto rc = receipt(id);
    check(rc.rc == KILN_OK && rc.r.ok && rc.r.disposition == KILN_DEFINITION_ACCEPTED_NEXT_START, "its receipt: AcceptedNextStart");
    // RequireRegistered for the accepted definition: refused for this call, the receipt keeps what it is
    auto req = define(request("fxmod", "spark", 50, 7, KILN_DEFINE_REQUIRE_REGISTERED));
    check(!req.r.ok && req.r.reason_code == KILN_DEFINE_REGISTRY_CLOSED && id_of(req.r) == id,
          "RequireRegistered for a definition stored for the next start: refused RegistryClosed");
    check(receipt(id).r.disposition == KILN_DEFINITION_ACCEPTED_NEXT_START, "and its receipt still says AcceptedNextStart");
    // a changed content of the same block: the earlier receipt is superseded, the file holds the new one
    const auto q2 = request("fxmod", "spark", 50, 8, KILN_DEFINE_ALLOW_NEXT_START, 2);
    a = define(q2);
    check(a.r.ok && a.r.disposition == KILN_DEFINITION_ACCEPTED_NEXT_START && id_of(a.r) != id, "a changed definition: a new receipt");
    rc = receipt(id);
    check(!rc.r.ok && rc.r.reason_code == KILN_DEFINE_SUPERSEDED, "the earlier receipt: Superseded: " + rc.reason());
    check(stored_registration(file) == ledger::uuid_text(id_of(a.r)) && !stored_registration(file).empty(), "the file holds the later one");
    // the two-argument define_json storing another content of the block supersedes too
    const ledger::Id later = id_of(a.r);
    ledger::kept("fxmod", "spark", 99);
    check(receipt(later).r.reason_code == KILN_DEFINE_SUPERSEDED, "kept() with another content: Superseded");
    // built at this start, the table not read: NotReady (RequireRegistered), AcceptedNextStart (AllowNextStart), nothing written
    const size_t before = files_in(base);
    auto nr = define(request("fxmod", "fx0", 0, 100, KILN_DEFINE_REQUIRE_REGISTERED, 3));
    check(!nr.r.ok && nr.r.reason_code == KILN_DEFINE_NOT_READY && nr.r.slots_consumed == 0,
          "built at this start, table not read: RequireRegistered is NotReady: " + nr.reason());
    auto nn = define(request("fxmod", "fx0", 0, 100, KILN_DEFINE_ALLOW_NEXT_START, 3));
    check(nn.r.ok && nn.r.disposition == KILN_DEFINITION_ACCEPTED_NEXT_START && files_in(base) == before,
          "and AllowNextStart: AcceptedNextStart without a new file (its source builds it at the next start)");
    // the table read: registered, slot 1, both timings; its receipt follows
    ledger::observe(seen_of(boot, 2048));
    auto reg = define(request("fxmod", "fx0", 0, 100, KILN_DEFINE_REQUIRE_REGISTERED, 3));
    check(reg.r.ok && reg.r.disposition == KILN_DEFINITION_REGISTERED && reg.r.slots_consumed == 1 && reg.r.reason_code == 0,
          "read, its bytes the served ones, state 2: Registered, 1 slot");
    check(reg.r.snapshot_revision == budget().revision, "its snapshot revision is the budget's");
    check(receipt(id_of(nn.r)).r.disposition == KILN_DEFINITION_REGISTERED, "the earlier AllowNextStart receipt of it: Registered now");
    check(define(request("fxmod", "fx0", 0, 100, KILN_DEFINE_REQUIRE_REGISTERED, 3)).r.slots_consumed == 1,
          "again: 1 slot, not 2 (an answer, not an increment)");
    check(budget().kiln_used == 2, "the budget counts it once");
    // a changed content of a built block under RequireRegistered: registry closed
    auto chg = define(request("fxmod", "fx0", 0, 555, KILN_DEFINE_REQUIRE_REGISTERED, 4));
    check(!chg.r.ok && chg.r.reason_code == KILN_DEFINE_REGISTRY_CLOSED, "a built block with a changed content: RegistryClosed");
    // a revoked owner
    auto q3 = request("fxmod", "late", 60, 9, KILN_DEFINE_ALLOW_NEXT_START);
    q3.owner_live = false;
    const size_t files = files_in(base);
    auto oi = define(q3);
    check(!oi.r.ok && oi.r.reason_code == KILN_DEFINE_OWNER_INACTIVE && files_in(base) == files, "a revoked owner: OwnerInactive, nothing stored");
    // a GUID another mod's built block has
    auto col = define(request("othermod", "fx1", 1, 101, KILN_DEFINE_ALLOW_NEXT_START));
    check(!col.r.ok && col.r.reason_code == KILN_DEFINE_ID_COLLISION, "another mod's GUID: IdCollision: " + col.reason());
    // retired after registration: retained, its slot still taken
    ledger::release("fxmod");
    auto ret = receipt(id_of(reg.r));
    check(!ret.r.ok && ret.r.disposition == KILN_DEFINITION_RETAINED && ret.r.slots_consumed == 1 && ret.r.reason_code == KILN_DEFINE_OWNER_INACTIVE,
          "the mod unloaded: Retained, 1 slot, OwnerInactive");
    auto back = define(request("fxmod", "fx0", 0, 100, KILN_DEFINE_REQUIRE_REGISTERED, 3));
    check(back.r.ok && back.r.disposition == KILN_DEFINITION_REGISTERED, "loaded again and defined again: Registered");
    auto gone = request("fxmod", "fx0", 0, 100, KILN_DEFINE_ALLOW_NEXT_START, 3);
    gone.owner_live = false;
    auto revoked = define(gone);
    check(!revoked.r.ok && revoked.r.reason_code == KILN_DEFINE_OWNER_INACTIVE && revoked.r.disposition == KILN_DEFINITION_RETAINED &&
              revoked.r.slots_consumed == 1,
          "a revoked owner's call for a registered VFX: OwnerInactive, Retained with its slot (never reported free)");
    // what the game's table says otherwise
    ledger::Seen s = seen_of(boot, 2048);
    s.kiln[1].second = ledger::kMissing;
    s.count -= 1;
    ledger::observe(s);
    auto miss = define(request("fxmod", "fx1", 1, 101, KILN_DEFINE_REQUIRE_REGISTERED, 5));
    check(!miss.r.ok && miss.r.reason_code == KILN_DEFINE_GAME_REFUSED && miss.r.slots_consumed == 0,
          "not in the game's table: GameRefused: " + miss.reason());
    s = seen_of(boot, 2048);
    s.kiln[1].second = ledger::kOther;
    ledger::observe(s);
    auto other = define(request("fxmod", "fx1", 1, 101, KILN_DEFINE_ALLOW_NEXT_START, 5));
    check(!other.r.ok && other.r.reason_code == KILN_DEFINE_GAME_REFUSED, "other bytes under its GUID: GameRefused, also with AllowNextStart");
    s.kiln[1].second = ledger::kUnchecked;
    ledger::observe(s);
    auto unchecked = define(request("fxmod", "fx1", 1, 101, KILN_DEFINE_REQUIRE_REGISTERED, 5));
    check(!unchecked.r.ok && unchecked.r.reason_code == KILN_DEFINE_NOT_READY, "bytes not compared: never Registered (NotReady)");
    // receipts
    auto nf = receipt(guid_n(999));
    check(nf.rc == KILN_OK && !nf.r.ok && nf.r.reason_code == KILN_DEFINE_NOT_FOUND && !nf.reason().empty(), "an unknown id: NotFound");
    Answer zero{};
    zero.r.size = sizeof zero.r;
    const uint8_t z[16] = {};
    check(ledger::receipt(z, &zero.r, &zero.report) == KILN_ERR_INVALID_ARGUMENT, "the empty id: INVALID_ARGUMENT");
    // bad input never reaches a file
    auto bad = request("fxmod", "../evil", 70, 1, KILN_DEFINE_ALLOW_NEXT_START);
    Answer bn{};
    bn.r.size = sizeof bn.r;
    check(ledger::define(bad, &bn.r, &bn.report) == KILN_ERR_INVALID_ARGUMENT && !fs::exists(root / "evil.json"), "a name with a path: INVALID_ARGUMENT");
    auto badmod = request("..", "x", 71, 1, KILN_DEFINE_ALLOW_NEXT_START);
    check(ledger::define(badmod, &bn.r, &bn.report) == KILN_ERR_INVALID_ARGUMENT, "a mod id \"..\": INVALID_ARGUMENT");
    auto badtiming = request("fxmod", "x", 72, 1, 7);
    check(ledger::define(badtiming, &bn.r, &bn.report) == KILN_ERR_INVALID_ARGUMENT, "timing 7: INVALID_ARGUMENT");
    KilnDefinitionReply small{};
    small.size = 8;
    std::string rep;
    check(ledger::define(request("fxmod", "x", 73, 1, 0), &small, &rep) == KILN_ERR_INVALID_ARGUMENT, "a reply struct too small: INVALID_ARGUMENT");
    ledger::set_available(true, false);
    Answer un{};
    un.r.size = sizeof un.r;
    check(ledger::define(request("fxmod", "x", 74, 1, 1), &un.r, &un.report) == KILN_ERR_UNAVAILABLE &&
              ledger::receipt(id.data(), &un.r, &un.report) == KILN_ERR_UNAVAILABLE,
          "blocks.vfx_registration off: both members KILN_ERR_UNAVAILABLE");
}

static void test_capacity(const fs::path& base) {
    std::printf("-- the 474th\n");
    const fs::path root = fresh_root(base);
    ledger::boot(boot_of("big", 0));
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < 473; ++i)
        accepted += define(request("big", "n" + std::to_string(i), 1000 + i, i, KILN_DEFINE_ALLOW_NEXT_START)).r.disposition ==
                    KILN_DEFINITION_ACCEPTED_NEXT_START;
    check(accepted == 473, "473 new VFX fit (2,048 - 1,575): " + std::to_string(accepted));
    const size_t files = files_in(base);
    auto over = define(request("big", "n473", 1473, 473, KILN_DEFINE_ALLOW_NEXT_START));
    check(!over.r.ok && over.r.reason_code == KILN_DEFINE_CAPACITY_EXCEEDED && over.r.slots_consumed == 0 && files_in(base) == files,
          "the 474th: CapacityExceeded, nothing stored: " + over.reason());
    auto over2 = define(request("big", "n474", 1474, 474, KILN_DEFINE_REQUIRE_REGISTERED));
    check(!over2.r.ok && over2.r.reason_code == KILN_DEFINE_CAPACITY_EXCEEDED, "and under RequireRegistered: CapacityExceeded (the concrete refusal)");
    // a changed content of an accepted block takes no new slot
    auto chg = define(request("big", "n0", 1000, 9999, KILN_DEFINE_ALLOW_NEXT_START, 9));
    check(chg.r.ok && chg.r.disposition == KILN_DEFINITION_ACCEPTED_NEXT_START, "a changed content of a stored block: accepted (no new slot)");
    auto replacement = define(request("big", "n0", 9000, 10000, KILN_DEFINE_ALLOW_NEXT_START, 10));
    check(replacement.r.ok && files_in(base) == files && receipt(id_of(chg.r)).r.reason_code == KILN_DEFINE_SUPERSEDED,
          "replacing the GUID in one pending file at full capacity reuses that file's reservation");
    // the capacity follows the game's table once read
    ledger::Boot b = boot_of("big", 0);
    ledger::Seen s;
    s.limit = 1600;
    s.count = 1575;
    s.init_state = 2;
    ledger::observe(s);
    auto small = define(request("big", "late", 5000, 1, KILN_DEFINE_ALLOW_NEXT_START));
    check(!small.r.ok && small.r.reason_code == KILN_DEFINE_CAPACITY_EXCEEDED, "with the table read, its limit is what admits");
    (void)b;
    (void)root;
}

static void test_stored(const fs::path& base) {
    std::printf("-- a definition stored at an earlier start\n");
    fresh_root(base);
    ledger::Boot b = boot_of("fxmod", 1);
    const std::string canonical = nlohmann::json::parse(block_json(1)).dump();
    ledger::Stored s1;
    s1.guid = guid_n(0);
    s1.mod = "fxmod";
    s1.name = "fx0";
    s1.definition = 100;
    s1.built = true;
    s1.registration = ledger::registration_id("fxmod", s1.guid, canonical);
    ledger::Stored s2;
    s2.guid = guid_n(77);
    s2.mod = "fxmod";
    s2.name = "toomany";
    s2.registration = ledger::registration_id("fxmod", s2.guid, nlohmann::json::parse(block_json(2)).dump());
    s2.code = KILN_DEFINE_CAPACITY_EXCEEDED;
    s2.why = "fxmod: over the game's limits";
    b.stored = {s1, s2};
    ledger::boot(b);
    auto r1 = receipt(s1.registration);
    check(r1.r.ok && r1.r.disposition == KILN_DEFINITION_ACCEPTED_NEXT_START, "built from its stored file, table not read: still AcceptedNextStart");
    auto r2 = receipt(s2.registration);
    check(!r2.r.ok && r2.r.reason_code == KILN_DEFINE_CAPACITY_EXCEEDED && r2.reason() == s2.why,
          "refused by this start's build: Refused with the build's reason");
    ledger::observe(seen_of(b, 2048));
    r1 = receipt(s1.registration);
    check(r1.r.ok && r1.r.disposition == KILN_DEFINITION_REGISTERED && r1.r.slots_consumed == 1, "the table read: Registered at this start");
    // forget (a prepared build taken back): nothing built
    ledger::forget();
    check(budget().readiness == KILN_CAPACITY_NOT_READY, "after forget: not ready");
}

static void test_storage(const fs::path& base) {
    std::printf("-- storage\n");
    fresh_root(base);
    ledger::boot(boot_of("fxmod", 0));
    // the pending root is a file: the write fails, the answer says so and nothing is accepted
    std::ofstream(base / "pending") << "not a folder";
    auto a = define(request("fxmod", "spark", 50, 7, KILN_DEFINE_ALLOW_NEXT_START));
    check(a.rc == KILN_OK && !a.r.ok && a.r.reason_code == KILN_DEFINE_STORAGE_FAILURE && !a.reason().empty(), "cannot write: StorageFailure");
    check(receipt(id_of(a.r)).r.reason_code == KILN_DEFINE_STORAGE_FAILURE, "its receipt: StorageFailure, not accepted");
    // a temporary file left by a crash is replaced, not read
    fs::remove(base / "pending");
    fs::create_directories(base / "pending" / "fxmod");
    std::ofstream(base / "pending" / "fxmod" / "spark.json.tmp") << "{half";
    a = define(request("fxmod", "spark", 50, 7, KILN_DEFINE_ALLOW_NEXT_START));
    check(a.r.ok && nlohmann::json::parse(read_file(base / "pending" / "fxmod" / "spark.json"), nullptr, false).is_object() &&
              !fs::exists(base / "pending" / "fxmod" / "spark.json.tmp"),
          "a crash's temporary file: the new file is whole and the temporary one gone");
}

static void test_review_regressions(const fs::path& base) {
    fresh_root(base);
    auto b = boot_of("fxmod", 1);
    ledger::boot(b);
    ledger::observe(seen_of(b, 2048));
    ledger::observe({});
    check(!ledger::observed() && budget().readiness == KILN_CAPACITY_NOT_READY,
          "a stopped/unreadable registry invalidates the previous ready snapshot");
    ledger::observe(seen_of(b, 2048));
    check(budget().kiln_used == 1, "a second world publishes a fresh snapshot");
    fresh_root(base);
    ledger::boot(boot_of("fxmod", 0));
    auto first = define(request("fxmod", "one", 80, 1, KILN_DEFINE_ALLOW_NEXT_START));
    auto clash = define(request("othermod", "two", 80, 2, KILN_DEFINE_ALLOW_NEXT_START));
    check(first.r.ok && !clash.r.ok && clash.r.reason_code == KILN_DEFINE_ID_COLLISION && files_in(base) == 1,
          "two pending mods cannot both reserve the same GUID");
    auto alias = define(request("fxmod", "alias", 80, 1, KILN_DEFINE_ALLOW_NEXT_START));
    check(!alias.r.ok && alias.r.reason_code == KILN_DEFINE_ID_COLLISION && files_in(base) == 1,
          "a second pending name for the same GUID is refused before boot rejects duplicate IDs");
    check(receipt(id_of(first.r)).r.ok && stored_registration(base / "pending" / "fxmod" / "one.json") == ledger::uuid_text(id_of(first.r)),
          "refusing a second name preserves the original accepted receipt and file");
    ledger::Seen corrupt;
    corrupt.init_state = 2;
    corrupt.limit = 2048;
    corrupt.count = UINT64_MAX;
    ledger::observe(corrupt);
    auto over = define(request("fxmod", "over", 81, 1, KILN_DEFINE_ALLOW_NEXT_START));
    check(!over.r.ok && over.r.reason_code == KILN_DEFINE_CAPACITY_EXCEEDED && files_in(base) == 1,
          "an invalid huge game count cannot wrap admission into acceptance");
}

static void test_report() {
    std::printf("-- reports\n");
    const std::string r = ledger::report_json(std::string(9000, 'x'), std::vector<std::string>(40, std::string(300, 'w')));
    check(r.size() < ledger::kReportMax && nlohmann::json::parse(r, nullptr, false).is_object(), "a report is valid JSON under 4,096 bytes");
    const std::string ok = ledger::report_json("", {});
    check(ok == "{\"reason\":null,\"warnings\":[]}", "no reason: null");
    ledger::Id id{};
    check(ledger::uuid_parse("6f6d2a44-53c1-5c52-9d51-0a3c6e1f2b77", &id) && ledger::uuid_text(id) == "6f6d2a44-53c1-5c52-9d51-0a3c6e1f2b77",
          "uuid text round trip");
    check(!ledger::uuid_parse("not-a-uuid", &id), "bad uuid text refused");
    check(ledger::valid_name("frost_burst.v2-1") && !ledger::valid_name("") && !ledger::valid_name("a/b") && !ledger::valid_name(std::string(129, 'a')),
          "block names");
    check(ledger::valid_mod("my-mod") && !ledger::valid_mod(".") && !ledger::valid_mod("..") && !ledger::valid_mod("a\\b"), "mod ids");
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path base = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "kiln-vfx-ledger-test";
    test_budget(base);
    test_define(base);
    test_capacity(base);
    test_stored(base);
    test_storage(base);
    test_review_regressions(base);
    test_report();
    std::error_code ec;
    fs::remove_all(base, ec);
    std::printf("ledger: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
