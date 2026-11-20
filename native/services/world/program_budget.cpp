// SPDX-License-Identifier: MIT
// kiln.world.budget (native/core/abi/services/kiln_world_budget.h, since 0.2.0-dev.29): the room for world programs,
// chapter 17.4 decision I3. A table of its own next to kiln.world, so kiln.world's table does not grow for it. Answered
// from what kiln.world already keeps, never from game memory: its slot ledger (programs.cpp) and its directory of each
// world's program table (game_programs.cpp, read in that world's Pre once the table is final). Nothing runs for it per
// step: a query counts at the moment it is asked.
#include <algorithm>

#include "boot_programs.h"
#include "services/kiln_caps.h"
#include "services/kiln_capacity_util.hpp"
#include "services/kiln_world_budget.h"
#include "wraps.h"

namespace kw {
namespace {

constexpr KilnProgramAccessLimits kAccess = {8, 42, 64, 64, 9, 92, 24, 4096};  // programs.cpp's guards (register_program)
static_assert(sizeof(KilnProgramAccessLimits) == 32 && sizeof(KilnProgramBudgetReply) == 152 &&
                  offsetof(KilnProgramBudgetReply, access) == 24 && offsetof(KilnProgramBudgetReply, kiln_slots) == 56 &&
                  offsetof(KilnProgramBudgetReply, engine_slots) == 104,
              "KilnProgramBudgetReply layout");

std::mutex g_mu;
uint64_t g_seen = 0;           // what the last answer said (a fingerprint): the revision rises when it changes
kiln::capacity::Revision g_rev;

uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }

bool live_world(uint32_t kind, uintptr_t* world, uint32_t* generation) {
    KilnSessionInfo s{};
    s.size = sizeof s;
    if (g_api->size >= offsetof(KilnApi, session) + sizeof(void*) && g_api->session && g_api->session(&s) == KILN_OK) {
        *world = s.worlds[kind];
        *generation = *world ? s.sessions[kind] : 0;
        return true;
    }
    *world = kind == KILN_WORLD_SERVER ? g_api->server_world() : 0;  // a core without sessions: one server world
    *generation = *world ? 1 : 0;
    return true;
}

}  // namespace

KilnResult KILN_CALL program_budget(uint32_t kind, KilnProgramBudgetReply* out) {
    if (!out || out->size < sizeof(KilnProgramBudgetReply)) return KILN_ERR_INVALID_ARGUMENT;
    if (kind != KILN_WORLD_SERVER && kind != KILN_WORLD_CLIENT) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    out->world_kind = kind;
    out->per_file_max = static_cast<uint32_t>(kBootProgramsPerFile);
    out->access = kAccess;
    out->kiln_slots.size = out->engine_slots.size = sizeof(KilnCapacityInfo);

    uintptr_t world = 0;
    uint32_t gen = 0;
    live_world(kind, &world, &gen);
    const auto scope = kiln::capacity::Scope::world(kind, gen);
    const auto process = kiln::capacity::Scope::process();
    uint32_t readiness = KILN_CAPACITY_VALID;
    KilnGameProgramsStatus st{};
    st.size = sizeof st;
    if (!world) readiness = KILN_CAPACITY_NO_WORLD;
    else if (!F.programs || L.programs_max <= 0) readiness = KILN_CAPACITY_NOT_READY;  // no program table on this build
    else if (game_programs_status(&st) != KILN_OK || !st.directory[kind]) readiness = KILN_CAPACITY_NOT_READY;  // not read yet

    const ProgramLedger ledger = program_ledger();
    uint64_t count = 0, game = 0, kiln = 0;
    if (readiness == KILN_CAPACITY_VALID) {
        count = st.programs[kind];
        game = st.game_programs[kind];
        for (uint32_t i = 0; i < count; ++i) {
            ProgramRef r;
            if (game_program_at(kind, static_cast<int32_t>(i), &r) && std::binary_search(ledger.descs.begin(), ledger.descs.end(), r.desc))
                ++kiln;
        }
        // the world must still be the one asked about (a world that stopped or started meanwhile is not answered for)
        uintptr_t again = 0;
        uint32_t gen2 = 0;
        live_world(kind, &again, &gen2);
        if (again != world || gen2 != gen) readiness = KILN_CAPACITY_NOT_READY;
    }

    uint64_t print = 0xcbf29ce484222325ull;
    for (uint64_t v : {uint64_t{kind}, uint64_t{readiness}, uint64_t{gen}, count, game, kiln, uint64_t{ledger.taken}, uint64_t{ledger.active},
                       static_cast<uint64_t>(L.programs_max)})
        print = mix(print, v);
    uint64_t rev;
    {
        std::lock_guard lock(g_mu);
        if (print != g_seen) g_seen = print, rev = g_rev.next();
        else rev = g_rev.current();
    }

    if (readiness == KILN_CAPACITY_VALID) {
        const uint64_t other = count >= game + kiln ? count - game - kiln : UINT64_MAX;  // another loader's direct entries
        const bool engine_ok = kiln::capacity::publish(&out->engine_slots, static_cast<uint64_t>(L.programs_max), game, kiln, other, scope, rev, false);
        const bool kiln_ok = kiln::capacity::publish(&out->kiln_slots, program_quota(), 0, ledger.active, ledger.taken - ledger.active, process,
                                                     rev, true);
        if (!engine_ok || !kiln_ok) readiness = KILN_CAPACITY_ACCOUNTING_MISMATCH;
    }
    if (readiness != KILN_CAPACITY_VALID) {
        // the pair makes no partial promise: both cleared, also when the process-wide ledger alone could be counted
        kiln::capacity::not_ready(&out->engine_slots, readiness, scope, rev, false);
        kiln::capacity::not_ready(&out->kiln_slots, readiness, process, rev, true);
    }
    out->readiness = readiness;
    out->generation = gen;
    return KILN_OK;
}

namespace {
const KilnWorldBudgetApi kBudgetTable = {{sizeof(KilnWorldBudgetApi), KILN_WORLD_BUDGET_VERSION}, &program_budget, &program_reservation};
}

// After kiln.world's own table: publish kiln.world.budget and its row (world.program_budget). The row is there with the
// table and says "available" also before a world: the answer's readiness says when it cannot count yet.
bool program_budget_register() {
    const bool ok =
        g_api->register_service(g_owner, KilnStr{KILN_WORLD_BUDGET_SERVICE, sizeof(KILN_WORLD_BUDGET_SERVICE) - 1}, &kBudgetTable) == KILN_OK;
    if (const auto* caps = static_cast<const KilnCapsApi*>(g_api->service(KilnStr{KILN_CAPS_SERVICE, sizeof(KILN_CAPS_SERVICE) - 1}, 1));
        caps && caps->report) {
        static constexpr char kRow[] = "world.program_budget";
        const std::string text = !ok ? std::string("kiln.world.budget could not be published")
                                 : F.programs ? std::string("kiln.world.budget v1: Kiln's 64 program slots and the world's table")
                                              : std::string("kiln.world.budget v1: programs are off on this build (world.programs), so it answers NotReady");
        caps->report(g_owner, KilnStr{kRow, sizeof kRow - 1}, ok ? KILN_CAP_AVAILABLE : KILN_CAP_UNAVAILABLE,
                     ok ? KILN_CAP_REASON_NONE : KILN_CAP_REASON_FAULTED, KilnStr{text.data(), text.size()});
    }
    KW_I("kiln.world.budget: %s", ok ? "published (world.program_budget)" : "registering the table failed");
    return ok;
}

}  // namespace kw
