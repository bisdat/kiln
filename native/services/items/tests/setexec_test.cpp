// SPDX-License-Identifier: MIT
// kiln.items v6 host test: the executor of equipment sets and its terminal state machine (setexec.h; chapter 10 §4.4)
// against a fake game that stages a transaction as the lab saw the game do it: inventory moves in place before the
// commit, the equipment selection written at the commit, a rollback restoring the stacks. Every transition: done, no
// change, stale, a refusal at the first, a middle and the last step, a dry run, the commit's guards, a commit that
// rolls back by itself, a mismatch after the commit, a rollback that does not restore, and a fault injected at every
// engine call of a run (the structured-exception path), with the rule checked each time: rollback at most once, never
// after the commit, free once, no engine call after a fault. What the real game does is the lab test
// (mods/tests/equipment-sets-test).
#include <cstdio>
#include <cstring>
#include <vector>

#include "../setexec.h"

using namespace kitems;
using namespace kitems::sets;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

constexpr uint32_t G1 = 2001, G2 = 2002, EQA = 2003, EQB = 2005, EQC = 2006;
constexpr uint8_t kRing0 = 10, kRing1 = 11, kHead = 5, kArrow = 14;
constexpr uint32_t RingA = 0x79442800, RingB = 0x4623bcb0, RingC = 0x11111111, Hat = 0x22222222, Arrows = 77, Bolts = 88;

uint64_t slot(uint32_t inv, uint32_t i) { return KILN_SLOT_ID(inv, i); }
// each equipment index has a fixed equipment page slot (the lab: Ring0 is 2550:0, Ring1 2550:1)
uint64_t page_slot(uint32_t x) { return x < 8 ? slot(EQA, x) : x < 16 ? slot(EQB, x - 8) : slot(EQC, x - 16); }

struct FakeGame final : SetEngine {
    SetModel live;         // what the accessors read now
    SetModel before_tx;    // the state at tx_open (a rollback restores its stacks)
    uint64_t pending[kSelectionSlots] = {};
    bool pending_set[kSelectionSlots] = {};
    bool open = false;
    // injections
    int fail_step = -1;          // the step whose equip / unequip the game refuses
    uint8_t fail_error = 0x11;
    long fault_call = -1;        // the engine call (0-based) that faults
    bool guards_roll_back = false, commit_rolls_back = false, commit_mismatch = false, rollback_leaks = false, locate_loses = false;
    // the record
    long calls = 0, calls_after_fault = 0;
    bool faulted = false;
    int tx_opens = 0, commits = 0, rollbacks = 0, frees = 0, steps = 0;
    bool rollback_after_commit = false;

    bool enter() {
        if (faulted) ++calls_after_fault;
        if (calls++ == fault_call) return faulted = true, false;
        return true;
    }
    void sync_items() {  // ItemState.containedInSlotId follows the stacks (the fake keeps them consistent)
        for (ItemRec& r : live.items)
            if (const Stack* s = live.stack_of(r.pide)) r.contained = s->slot;
    }
    Stack& at(uint64_t s) { return const_cast<Stack&>(*live.stack_at(s)); }

    bool read(SetModel* m, SnapStatus* st) override {
        if (!enter()) return false;
        *m = live;
        *st = SnapStatus::Ok;
        return true;
    }
    bool tx_open(bool) override {
        if (!enter()) return false;
        ++tx_opens;
        open = true;
        before_tx = live;
        std::memset(pending_set, 0, sizeof pending_set);
        return true;
    }
    bool equip(uint64_t source, uint8_t game_slot, uint8_t* error) override {
        if (!enter()) return false;
        const int x = selection_index(game_slot);
        if (steps++ == fail_step) return *error = fail_error, true;
        *error = 0;
        const uint64_t target = page_slot(static_cast<uint32_t>(x));
        Stack moving = at(source), old = at(target);
        Stack& a = at(source);
        Stack& b = at(target);
        a.item = old.item, a.count = old.count, a.pide = old.pide;
        b.item = moving.item, b.count = moving.count, b.pide = moving.pide;
        pending[x] = target;
        pending_set[x] = true;
        sync_items();
        return true;
    }
    bool unequip(uint64_t, uint8_t game_slot, uint8_t* error) override {
        if (!enter()) return false;
        const int x = selection_index(game_slot);
        if (steps++ == fail_step) return *error = fail_error, true;
        *error = 0;
        const uint64_t from = pending_set[x] ? pending[x] : live.equipment[x];
        for (uint32_t e : {G1, G2})
            for (uint32_t k = 0; k < 8; ++k) {
                Stack& g = at(slot(e, k));
                if (!is_empty(g)) continue;
                Stack& s = at(from);
                g.item = s.item, g.count = s.count, g.pide = s.pide;
                s.item = s.count = s.pide = 0;
                pending[x] = 0;
                pending_set[x] = true;
                sync_items();
                return true;
            }
        *error = 0x11;
        return true;
    }
    bool locate(uint32_t pide, uint64_t* s) override {
        if (!enter()) return false;
        const Stack* st = live.stack_of(pide);
        *s = st && !locate_loses ? st->slot : 0;
        return true;
    }
    bool guards(bool* would) override {
        if (!enter()) return false;
        *would = guards_roll_back || bit_set;
        return true;
    }
    bool bit_set = false;
    bool force_guard_bit() override {
        if (!enter()) return false;
        bit_set = true;
        commit_rolls_back = true;  // as the game's commit does with the bit set
        return true;
    }
    void restore() {
        const SetModel keep = before_tx;
        live.leaves = keep.leaves;
        if (rollback_leaks) at(slot(G2, 7)).item = 5, at(slot(G2, 7)).count = 1;  // something outside the rollback
        sync_items();
        open = false;
    }
    bool commit() override {
        if (!enter()) return false;
        ++commits;
        if (commit_rolls_back) return restore(), true;
        for (uint32_t x = 0; x < kSelectionSlots; ++x)
            if (pending_set[x]) live.equipment[x] = pending[x];
        if (commit_mismatch) live.equipment[selection_index(kRing0)] = 0;  // a slot the set left out changes
        open = false;
        return true;
    }
    bool rollback() override {
        if (!enter()) return false;
        ++rollbacks;
        if (commits) rollback_after_commit = true;
        restore();
        return true;
    }
    bool free_arrays() override {
        if (!enter()) return false;
        ++frees;
        return true;
    }
};

// A player: ring A worn in Ring0, ring B and a hat and ring C in the backpack, arrows worn, bolts in the backpack.
FakeGame world() {
    FakeGame g;
    SetModel& m = g.live;
    m.player = 1;
    NodeRec root{};
    root.entity = 1;
    const uint32_t links[5] = {G1, G2, EQA, EQB, EQC};
    const uint8_t cats[5] = {4, 4, 2, 2, 2};
    for (int k = 0; k < 5; ++k) root.link_entity[k] = links[k], root.link_cat[k] = cats[k];
    root.link_count = 5;
    root.initialized = 1;
    m.nodes.push_back(root);
    for (uint32_t e : {G1, G2, EQA, EQB, EQC}) {
        LeafRec l{};
        l.entity = e;
        for (uint32_t k = 0; k < 8; ++k) l.s[k].slot = slot(e, k);
        m.leaves.push_back(l);
    }
    m.actionbar[0] = 3001;
    auto put = [&](uint64_t s, uint32_t item, uint32_t count, uint32_t pide) {
        Stack& st = g.at(s);
        st.item = item, st.count = count, st.pide = pide;
        if (pide) {
            ItemRec r{};
            r.pide = pide, r.item = item, r.stamp = pide - 4000, r.has_state = 1, r.contained = s, r.has_owner = 1, r.owner = 1;
            m.items.push_back(r);
        }
    };
    put(page_slot(selection_index(kRing0)), RingA, 1, 5001);
    m.equipment[selection_index(kRing0)] = page_slot(selection_index(kRing0));
    put(slot(G1, 2), RingB, 1, 5002);
    put(slot(G1, 3), Hat, 1, 5003);
    put(slot(G2, 0), RingC, 1, 5004);
    put(page_slot(selection_index(kArrow)), Arrows, 40, 0);
    m.equipment[selection_index(kArrow)] = page_slot(selection_index(kArrow));
    put(slot(G1, 5), Bolts, 30, 0);
    std::sort(m.items.begin(), m.items.end(), [](const ItemRec& a, const ItemRec& b) { return a.pide < b.pide; });
    return g;
}

SnapPrefix prefix() {
    SnapPrefix p;
    p.world_epoch = 7;
    p.player = 1;
    p.identity = KilnEventEntityKey{1, KILN_EVENT_KEY_PLAYER_SESSION, 0, 9, 4, 20};
    return p;
}

KilnSetOpV1 op(uint8_t game_slot, uint8_t kind = 0, uint32_t pide = 0, uint32_t item = 0, uint64_t source = 0, uint32_t count = 0) {
    KilnSetOpV1 o{};
    o.size = sizeof o;
    o.equipment_slot = game_slot;
    o.kind = kind;
    if (kind == 1) o.item_entity = pide, o.item_id = item, o.expected_count = 1, o.instance_stamp = pide - 4000;
    if (kind == 2) o.item_id = item, o.source_slot = source, o.expected_count = count;
    return o;
}

SetJob job(const FakeGame& g, std::vector<KilnSetOpV1> e, uint32_t flags = 0) {
    SetJob j;
    j.ticket = 9;
    j.player = 1;
    j.flags = flags;
    j.count = static_cast<uint32_t>(e.size());
    for (size_t i = 0; i < e.size(); ++i) j.entries[i] = e[i];
    j.prefix = prefix();
    encode(j.prefix, g.live, &j.baseline);
    return j;
}

std::vector<uint8_t> bytes(const FakeGame& g) {
    std::vector<uint8_t> b;
    encode(prefix(), g.live, &b);
    return b;
}

// The rules every run keeps.
bool rules(const FakeGame& g) {
    return g.rollbacks <= 1 && !g.rollback_after_commit && g.commits <= 1 && g.frees <= 1 && g.tx_opens <= 1 && g.calls_after_fault == 0;
}

std::vector<KilnSetOpV1> three() { return {op(kHead, 1, 5003, Hat), op(kRing1, 1, 5004, RingC), op(kArrow, 2, 0, Bolts, slot(G1, 5), 30)}; }
}  // namespace

int main() {
    // ---- Done ----
    {
        FakeGame g = world();
        const SetJob j = job(g, {op(kRing0, 1, 5002, RingB)});
        const SetOutcome o = execute(g, j, PlanOptions{}, nullptr, nullptr);
        check(o.status == SetStatus::Done && (o.flags & kResultCommitted) && !(o.flags & kResultMayHaveMutated) && !o.engine_fault,
              "one swap: Done, committed");
        check(o.changes.size() == 1 && o.changes[0].game_slot == kRing0 && o.changes[0].before.pide == 5001 && o.changes[0].after.pide == 5002 &&
                  o.changes[0].after.stamp == 1002,
              "the change is the actual delta (Ring0: A -> B, with stamps)");
        check(g.commits == 1 && g.rollbacks == 0 && g.frees == 1 && rules(g), "commit once, no rollback, free once");
        FakeGame h = world();
        const SetOutcome o3 = execute(h, job(h, three()), PlanOptions{}, nullptr, nullptr);
        check(o3.status == SetStatus::Done && o3.changes.size() == 3 && o3.steps == 3 && rules(h), "three slots in one transaction: Done, three deltas");
        FakeGame c = world();
        const SetOutcome oc = execute(c, job(c, {op(kRing0, 0), op(kRing1, 1, 5001, RingA)}), PlanOptions{}, nullptr, nullptr);
        check(oc.status == SetStatus::Done && oc.changes.size() == 2 && rules(c), "an unequip and an equip of what came off: Done");
        FakeGame y = world();
        y.at(page_slot(selection_index(kRing1))) = Stack{page_slot(selection_index(kRing1)), RingB, 1, 5006};
        y.live.equipment[selection_index(kRing1)] = page_slot(selection_index(kRing1));
        ItemRec r{};
        r.pide = 5006, r.item = RingB, r.stamp = 1006, r.has_state = 1, r.contained = page_slot(selection_index(kRing1)), r.has_owner = 1, r.owner = 1;
        y.live.items.push_back(r);
        PlanOptions cycles;
        cycles.cycles = true;
        const SetJob swap = job(y, {op(kRing0, 1, 5006, RingB), op(kRing1, 1, 5001, RingA)});
        check(execute(y, swap, PlanOptions{}, nullptr, nullptr).status == SetStatus::Refused && y.tx_opens == 0, "a cycle, refused by default: no transaction");
        const SetOutcome os = execute(y, swap, cycles, nullptr, nullptr);
        check(os.status == SetStatus::Done && os.steps == 3 && os.changes.size() == 2 && rules(y), "a cycle when allowed: three steps, two deltas, Done");
    }

    // ---- no transaction ----
    {
        FakeGame g = world();
        SetOutcome o = execute(g, job(g, {op(kRing0, 1, 5001, RingA), op(kRing1, 0)}), PlanOptions{}, nullptr, nullptr);
        check(o.status == SetStatus::NoChange && g.tx_opens == 0 && o.changes.empty(), "every slot holds already: NoChange, no transaction");
        const SetJob j = job(g, {op(kRing0, 1, 5002, RingB)});
        g.at(slot(G1, 7)).item = 9, g.at(slot(G1, 7)).count = 1;  // something moved after the snapshot
        o = execute(g, j, PlanOptions{}, nullptr, nullptr);
        check(o.status == SetStatus::StaleSnapshot && g.tx_opens == 0 && o.refusal == KILN_SET_REFUSAL_STALE, "the inventory changed: StaleSnapshot, no transaction");
    }

    // ---- refusals: the first, a middle and the last step ----
    for (int k = 0; k < 3; ++k) {
        FakeGame g = world();
        const SetJob j = job(g, three());
        const std::vector<uint8_t> was = bytes(g);
        g.fail_step = k;
        const SetOutcome o = execute(g, j, PlanOptions{}, nullptr, nullptr);
        char what[160];
        std::snprintf(what, sizeof what, "step %d refused: Refused, rollback once, no commit, free once, the inventory as it was", k);
        check(o.status == SetStatus::Refused && o.failed_operation == static_cast<uint32_t>(k) && o.game_error == 0x11 && o.changes.empty() &&
                  g.rollbacks == 1 && g.commits == 0 && g.frees == 1 && bytes(g) == was && rules(g),
              what);
    }
    {
        FakeGame g = world();
        g.locate_loses = true;
        const SetOutcome o = execute(g, job(g, {op(kRing0, 1, 5002, RingB)}), PlanOptions{}, nullptr, nullptr);
        check(o.status == SetStatus::Refused && o.refusal == KILN_SET_REFUSAL_NOT_HERE && g.rollbacks == 1 && g.commits == 0 && rules(g),
              "an item not where the plan put it: Refused before its equip, rolled back");
    }

    // ---- dry run ----
    {
        FakeGame g = world();
        const std::vector<uint8_t> was = bytes(g);
        const SetOutcome o = execute(g, job(g, three(), kRequestDryRun), PlanOptions{}, nullptr, nullptr);
        check(o.status == SetStatus::Simulated && (o.flags & kResultDryRun) && !(o.flags & kResultCommitted) && o.changes.empty() && o.steps == 3,
              "a dry run: Simulated, DryRun, not committed, no changes");
        check(g.rollbacks == 1 && g.commits == 0 && g.frees == 1 && bytes(g) == was && rules(g), "a dry run rolls back once, never commits, frees once");
        FakeGame r = world();
        r.fail_step = 1;
        const SetOutcome orf = execute(r, job(r, three(), kRequestDryRun), PlanOptions{}, nullptr, nullptr);
        check(orf.status == SetStatus::Refused && (orf.flags & kResultDryRun) && r.rollbacks == 1 && rules(r), "a refused dry run keeps its DryRun flag");
    }

    // ---- the commit's guards, a commit that rolls back by itself, a mismatch ----
    {
        FakeGame g = world();
        g.guards_roll_back = true;
        const SetOutcome o = execute(g, job(g, three()), PlanOptions{}, nullptr, nullptr);
        check(o.status == SetStatus::Refused && g.commits == 0 && g.rollbacks == 1 && g.frees == 1 && rules(g),
              "the commit would roll back by itself: refused before the commit, rollback once");
        FakeGame c = world();
        const std::vector<uint8_t> was = bytes(c);
        c.commit_rolls_back = true;
        const SetOutcome oc = execute(c, job(c, three()), PlanOptions{}, nullptr, nullptr);
        check(oc.status == SetStatus::CommitRolledBack && !(oc.flags & kResultCommitted) && oc.changes.empty() && c.commits == 1 && c.rollbacks == 0 &&
                  c.frees == 1 && bytes(c) == was && rules(c),
              "a commit that rolled back by itself: CommitRolledBack, no second rollback, free once");
        FakeGame m = world();
        m.commit_mismatch = true;
        const SetOutcome om = execute(m, job(m, three()), PlanOptions{}, nullptr, nullptr);
        check(om.status == SetStatus::InvariantFailure && (om.flags & kResultMayHaveMutated) && (om.flags & kResultCommitted) && om.engine_fault &&
                  !om.changes.empty() && m.rollbacks == 0 && rules(m),
              "after the commit neither as asked nor as before: InvariantFailure, MayHaveMutated, committed, the deltas, no rollback");
        FakeGame l = world();
        l.fail_step = 2;
        l.rollback_leaks = true;
        const SetOutcome ol = execute(l, job(l, three()), PlanOptions{}, nullptr, nullptr);
        check(ol.status == SetStatus::InvariantFailure && (ol.flags & kResultMayHaveMutated) && !(ol.flags & kResultCommitted) && ol.engine_fault &&
                  l.rollbacks == 1 && l.commits == 0 && rules(l),
              "a rollback that does not restore: InvariantFailure, MayHaveMutated, not committed");
    }

    // ---- a fault at every engine call of a run (the structured-exception path) ----
    {
        FakeGame probe = world();
        execute(probe, job(probe, three()), PlanOptions{}, nullptr, nullptr);
        const long total = probe.calls;
        bool all = true;
        for (long n = 0; n < total; ++n) {
            FakeGame g = world();
            g.fault_call = n;
            const SetOutcome o = execute(g, job(g, three()), PlanOptions{}, nullptr, nullptr);
            const bool ok = o.engine_fault && (o.status == SetStatus::InvariantFailure || (n == 0 && o.status == SetStatus::Unavailable)) &&
                            g.calls_after_fault == 0 && rules(g) && (n == 0 || (o.flags & kResultMayHaveMutated));
            if (!ok) std::printf("     fault at call %ld of %ld: status %u flags %u after %ld\n", n, total, static_cast<unsigned>(o.status), o.flags, g.calls_after_fault);
            all = all && ok;
        }
        char what[160];
        std::snprintf(what, sizeof what, "a fault at each of the %ld engine calls: InvariantFailure (or Unavailable before the transaction), off, no call after it", total);
        check(all && total >= 8, what);
        FakeGame d = world();
        execute(d, job(d, three(), kRequestDryRun), PlanOptions{}, nullptr, nullptr);
        const long dry_total = d.calls;
        bool dry_all = true;
        for (long n = 1; n < dry_total; ++n) {
            FakeGame g = world();
            g.fault_call = n;
            const SetOutcome o = execute(g, job(g, three(), kRequestDryRun), PlanOptions{}, nullptr, nullptr);
            dry_all = dry_all && o.engine_fault && o.status == SetStatus::InvariantFailure && (o.flags & kResultDryRun) && g.calls_after_fault == 0 && rules(g);
        }
        check(dry_all, "a fault at each engine call of a dry run: InvariantFailure, DryRun kept, no call after it");
    }

    // ---- the lab's faults (a real run injects them; here the fake answers as the game would) ----
    {
        FakeGame g = world();
        SetJob j = job(g, three());
        j.lab.fail_step = 1;
        const std::vector<uint8_t> was = bytes(g);
        const SetOutcome o = execute(g, j, PlanOptions{}, nullptr, nullptr);
        check(o.status == SetStatus::Refused && o.failed_operation == 1 && g.rollbacks == 1 && g.commits == 0 && bytes(g) == was && rules(g),
              "lab: a refusal at step 1 (an empty source): Refused at 1, rolled back");
        FakeGame b = world();
        SetJob jb = job(b, three());
        jb.lab.guard_bit = true;
        const SetOutcome ob = execute(b, jb, PlanOptions{}, nullptr, nullptr);
        check(ob.status == SetStatus::Refused && b.commits == 0 && b.rollbacks == 1 && rules(b), "lab: the commit's bit set: refused before the commit");
        FakeGame c = world();
        SetJob jc = job(c, three());
        jc.lab.commit_anyway = true;
        const SetOutcome oc = execute(c, jc, PlanOptions{}, nullptr, nullptr);
        check(oc.status == SetStatus::CommitRolledBack && c.commits == 1 && c.rollbacks == 0 && rules(c), "lab: the bit set and committed anyway: CommitRolledBack");
        FakeGame m = world();
        SetJob jm = job(m, three());
        jm.lab.fake_mismatch = true;
        const SetOutcome om = execute(m, jm, PlanOptions{}, nullptr, nullptr);
        check(om.status == SetStatus::InvariantFailure && om.engine_fault && (om.flags & kResultCommitted) && !om.changes.empty() && m.rollbacks == 0 && rules(m),
              "lab: a faked mismatch after the commit: InvariantFailure with the real deltas, no rollback");
    }

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
