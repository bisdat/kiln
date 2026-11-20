// SPDX-License-Identifier: MIT
// kiln.world.budget (program_budget.cpp, package 17b S6) over a fake session, a fake directory and a fake slot ledger:
// the reply's layout, NoWorld and NotReady clearing both budgets while the configured limits stay, a ready pair with the
// game's, Kiln's and another loader's entries told apart, one revision for both that rises only on a change, and the
// prediction kind refused.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <vector>

#include "../program_budget.cpp"

namespace kw {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 1;
Layout L;
Features F;
void logf(int, const char*, ...) {}
KilnSessionInfo g_session{};
std::vector<uintptr_t> g_table;  // the directory's descriptors, by index
uint32_t g_game = 0;
bool g_directory = false;
std::vector<uintptr_t> g_client_table;  // a game client's client world (0.2.0-dev.29: Kiln's client-table programs)
uint32_t g_client_game = 0;
bool g_client_directory = false;
ProgramLedger g_ledger;
KilnResult KILN_CALL game_programs_status(KilnGameProgramsStatus* out) {
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    out->directory[KILN_WORLD_SERVER] = g_directory;
    out->programs[KILN_WORLD_SERVER] = static_cast<uint32_t>(g_table.size());
    out->game_programs[KILN_WORLD_SERVER] = g_game;
    out->directory[KILN_WORLD_CLIENT] = g_client_directory;
    out->programs[KILN_WORLD_CLIENT] = static_cast<uint32_t>(g_client_table.size());
    out->game_programs[KILN_WORLD_CLIENT] = g_client_game;
    return KILN_OK;
}
bool game_program_at(uint32_t kind, int32_t index, ProgramRef* out) {
    const auto& t = kind == KILN_WORLD_CLIENT ? g_client_table : g_table;
    const bool dir = kind == KILN_WORLD_CLIENT ? g_client_directory : kind == KILN_WORLD_SERVER && g_directory;
    if (!dir || index < 0 || static_cast<size_t>(index) >= t.size()) return false;
    out->index = index;
    out->desc = t[static_cast<size_t>(index)];
    return true;
}
ProgramLedger program_ledger() { return g_ledger; }
uint32_t program_quota() { return 64; }
KilnResult KILN_CALL program_reservation(KilnStr, KilnStr, char*, uint32_t, uint32_t*) { return KILN_ERR_NOT_FOUND; }
}  // namespace kw

using namespace kw;
int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* what) {
        ++checks;
        if (!ok) ++failures, std::fprintf(stderr, "FAIL %s\n", what);
    };
    KilnApi api{};
    api.size = sizeof api;
    api.session = [](KilnSessionInfo* out) -> KilnResult {
        *out = g_session;
        return KILN_OK;
    };
    g_api = &api;
    F.programs = true;
    L.programs_max = 896;
    check(sizeof(KilnProgramBudgetReply) == 152 && offsetof(KilnProgramBudgetReply, access) == 24 &&
              offsetof(KilnProgramBudgetReply, kiln_slots) == 56 && offsetof(KilnProgramBudgetReply, engine_slots) == 104 &&
              sizeof(KilnProgramAccessLimits) == 32 && sizeof(KilnWorldBudgetApi) == 24 && offsetof(KilnWorldBudgetApi, program_reservation) == 16,
          "the reply is 152 bytes: access +24, kiln_slots +56, engine_slots +104");
    auto ask = [](uint32_t kind) {
        KilnProgramBudgetReply r;
        std::memset(&r, 0xAB, sizeof r);
        r.size = sizeof r;
        const KilnResult rc = program_budget(kind, &r);
        return std::make_pair(rc, r);
    };
    auto zero = [](const KilnCapacityInfo& i) { return !i.capacity && !i.game_used && !i.kiln_used && !i.reserved && !(i.flags & KILN_CAPACITY_READY); };
    auto limits = [](const KilnProgramBudgetReply& r) {
        return r.per_file_max == 32 && r.access.templates == 8 && r.access.read_write == 42 && r.access.singleton_reads == 64 &&
               r.access.singleton_writes == 64 && r.access.other_templates == 9 && r.access.other_accessors == 92 &&
               r.access.events_in_out == 24 && r.access.iterator_bytes == 4096;
    };

    g_session.size = sizeof g_session;
    auto [rc, r] = ask(KILN_WORLD_SERVER);
    check(rc == KILN_OK && r.readiness == KILN_CAPACITY_NO_WORLD && r.generation == 0 && zero(r.kiln_slots) && zero(r.engine_slots) &&
              r.kiln_slots.readiness == KILN_CAPACITY_NO_WORLD && limits(r),
          "no world: NoWorld, both budgets cleared, the configured limits still given");
    check(ask(KILN_WORLD_PREDICTION).first == KILN_ERR_INVALID_ARGUMENT && ask(7).first == KILN_ERR_INVALID_ARGUMENT,
          "the prediction kind and unknown kinds are bad arguments");
    KilnProgramBudgetReply small{};
    small.size = 100;
    check(program_budget(KILN_WORLD_SERVER, &small) == KILN_ERR_INVALID_ARGUMENT && program_budget(KILN_WORLD_SERVER, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          "a reply that is too small or missing is a bad argument");

    g_session.worlds[KILN_WORLD_SERVER] = 0x9000;
    g_session.sessions[KILN_WORLD_SERVER] = 3;
    std::tie(rc, r) = ask(KILN_WORLD_SERVER);
    check(r.readiness == KILN_CAPACITY_NOT_READY && r.generation == 3 && zero(r.kiln_slots) && zero(r.engine_slots) &&
              r.engine_slots.generation == 3 && (r.engine_slots.flags & KILN_CAPACITY_WORLD) && limits(r),
          "a live world whose table is not read yet: NotReady with its generation, both cleared");

    // 817 of the game's, 13 of Kiln's (of 15 slots taken, 13 in use), 2 of another loader that registered directly
    std::vector<uintptr_t> kiln;
    for (uintptr_t i = 0; i < 15; ++i) kiln.push_back(0x5000 + i * 0x200);
    g_ledger.taken = 15;
    g_ledger.active = 13;
    g_ledger.descs = kiln;
    g_table.clear();
    for (uintptr_t i = 0; i < 817; ++i) g_table.push_back(0x140000000 + i * 0x190);
    for (size_t i = 0; i < 13; ++i) g_table.push_back(kiln[i]);
    g_table.push_back(0x7ff000);
    g_table.push_back(0x7ff400);
    g_game = 817;
    g_directory = true;
    std::tie(rc, r) = ask(KILN_WORLD_SERVER);
    const auto& e = r.engine_slots;
    const auto& k = r.kiln_slots;
    check(rc == KILN_OK && r.readiness == KILN_CAPACITY_VALID && r.generation == 3, "a read table: ready");
    check(e.capacity == 896 && e.game_used == 817 && e.kiln_used == 13 && e.reserved == 2 && (e.flags & KILN_CAPACITY_WORLD) &&
              e.world_kind == 0 && e.generation == 3,
          "the engine: 896, the game's 817, Kiln's 13, another loader's 2 (world scope)");
    check(k.capacity == 64 && k.game_used == 0 && k.kiln_used == 13 && k.reserved == 2 && (k.flags & KILN_CAPACITY_PROCESS) &&
              (k.flags & KILN_CAPACITY_EXPANDED) && k.world_kind == KILN_CAPACITY_NO_WORLD_KIND,
          "Kiln's slots: 64, 13 in use, 2 held by programs that cannot run (process scope, expanded)");
    check(e.revision == k.revision, "one revision for the pair");
    const uint64_t rev = e.revision;
    check(ask(KILN_WORLD_SERVER).second.engine_slots.revision == rev, "the same answer again: the same revision");
    g_table.pop_back();
    const auto r2 = ask(KILN_WORLD_SERVER).second;
    check(r2.engine_slots.revision > rev && r2.engine_slots.reserved == 1, "a change: a higher revision");
    check(ask(KILN_WORLD_CLIENT).second.readiness == KILN_CAPACITY_NO_WORLD, "the client kind on a dedicated server: NoWorld");

    // counts that do not add up are never room
    g_game = 900;
    std::tie(rc, r) = ask(KILN_WORLD_SERVER);
    check(r.readiness == KILN_CAPACITY_ACCOUNTING_MISMATCH && zero(r.engine_slots) && zero(r.kiln_slots), "counts over the table: a mismatch, both cleared");
    g_game = 817;
    // A game client's client world (0.2.0-dev.29): Kiln's entries in its table are the client-side descriptors the ledger
    // lists next to the server ones (a "both" program: one slot, an entry in each table). Its own scope and generation.
    g_ledger.descs.push_back(0x9100);  // a both program's client-side descriptor
    g_ledger.descs.push_back(0x9300);  // a client program's
    std::sort(g_ledger.descs.begin(), g_ledger.descs.end());
    g_session.worlds[KILN_WORLD_CLIENT] = 0xA000;
    g_session.sessions[KILN_WORLD_CLIENT] = 2;
    g_client_table.clear();
    for (uintptr_t i = 0; i < 817; ++i) g_client_table.push_back(0x150000000 + i * 0x190);
    g_client_table.push_back(0x9100);
    g_client_table.push_back(0x9300);
    g_client_game = 817;
    g_client_directory = true;
    std::tie(rc, r) = ask(KILN_WORLD_CLIENT);
    check(rc == KILN_OK && r.readiness == KILN_CAPACITY_VALID && r.generation == 2 && r.engine_slots.world_kind == KILN_WORLD_CLIENT &&
              r.engine_slots.game_used == 817 && r.engine_slots.kiln_used == 2 && r.engine_slots.reserved == 0,
          "the client world: its table's 896, the game's 817, Kiln's 2 client-side entries found by the ledger");
    check(r.kiln_slots.capacity == 64 && r.kiln_slots.kiln_used == 13, "the 64 Kiln slots are the same process-wide ledger");
    g_client_table.push_back(0x7ff800);  // a client entry the ledger does not know: another loader's
    check(ask(KILN_WORLD_CLIENT).second.engine_slots.reserved == 1, "an entry Kiln did not register: another loader's");
    g_session.worlds[KILN_WORLD_CLIENT] = 0;
    check(ask(KILN_WORLD_CLIENT).second.readiness == KILN_CAPACITY_NO_WORLD, "the client world stopped (the menu): NoWorld");
    F.programs = false;
    check(ask(KILN_WORLD_SERVER).second.readiness == KILN_CAPACITY_NOT_READY, "programs off on this build: NotReady");
    std::printf("world program budget: %s %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
