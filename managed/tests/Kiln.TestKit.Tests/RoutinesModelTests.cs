// SPDX-License-Identifier: MIT
// The model under the kit's fake kiln.routines (Fakes/Routines/RoutinesModel.cs): the service's rules with no game.
// One owner per villager, changes committed at Pre and completed once at Post, the queue's limit, revisions, the two
// event queues, which window a minute takes, and the keep-awake prefix's marks. The golden wire text and hash are the
// same constants Kiln.Blocks.Tests (RoutineTests) asserts Kiln.Blocks produces.
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed class RoutinesModelTests
{
    private const string GoldenHash = "02dcb72ca219f3a72202cede3638112dda4aa5a71948b89543c43f667c4d3ebb";
    private const string GoldenWire =
        """{"format":1,"key":"routine-test:day","planVersion":2,"contentHash":"02dcb72ca219f3a72202cede3638112dda4aa5a71948b89543c43f667c4d3ebb","windows":[{"startMinute":360,"endMinute":720,"allDay":false,"priority":0,"task":{"kind":"inspect"}},{"startMinute":1320,"endMinute":360,"allDay":false,"priority":5,"task":{"kind":"sleep"}},{"startMinute":0,"endMinute":0,"allDay":true,"priority":-1,"task":{"kind":"sit"}}],"fallback":{"kind":"roam"}}""";

    private const string A = "routine-test", B = "other-mod";

    /// <summary>A wire document made the way the service's canonical form says, for plans the tests write themselves.</summary>
    private static string Wire(string key, FakeRoutineKind fallback = FakeRoutineKind.Vanilla, uint version = 1, params FakeRoutineWindow[] windows)
    {
        string[] names = ["vanilla", "idle", "roam", "sleep", "sit", "inspect"];
        var list = string.Join(",", windows.Select(w =>
            "{\"startMinute\":" + w.Start + ",\"endMinute\":" + w.End + ",\"allDay\":" + (w.AllDay ? "true" : "false") + ",\"priority\":" + w.Priority + ",\"task\":{\"kind\":\"" + names[(int)w.Task] + "\"}}"));
        return "{\"format\":1,\"key\":\"" + key + "\",\"planVersion\":" + version + ",\"contentHash\":\"" + RoutinesModel.HashOf(version, windows, fallback)
               + "\",\"windows\":[" + list + "],\"fallback\":{\"kind\":\"" + names[(int)fallback] + "\"}}";
    }

    private static FakeRoutineWindow AllDay(FakeRoutineKind task) => new(0, 0, true, 0, task);

    private static ulong Assign(RoutinesModel m, string owner, uint npc, string wire)
    {
        Assert.Equal(FakeRoutineCall.Ok, m.Assign(owner, npc, wire, out var ticket, out var problem));
        Assert.Null(problem);
        return ticket;
    }

    private static void Step(RoutinesModel m)
    {
        m.Pre();
        m.Post();
    }

    // ---- the wire document ------------------------------------------------------------------------------------------

    [Fact]
    public void The_model_reads_the_wire_document_Kiln_Blocks_writes()
    {
        Assert.True(RoutinesModel.TryParse(GoldenWire, out var plan, out var problem), problem);
        Assert.Equal(("routine-test:day", 2u, GoldenHash, FakeRoutineKind.Roam), (plan!.Key, plan.Version, plan.ContentHash, plan.Fallback));
        Assert.Equal([new(360, 720, false, 0, FakeRoutineKind.Inspect), new(1320, 360, false, 5, FakeRoutineKind.Sleep), new(0, 0, true, -1, FakeRoutineKind.Sit)], plan.Windows);
        Assert.Equal(GoldenHash, RoutinesModel.HashOf(plan.Version, plan.Windows, plan.Fallback));
    }

    [Theory]
    [InlineData("\"priority\":5", "\"priority\":6", "contentHash")]
    [InlineData("\"format\":1", "\"format\":2", "format")]
    [InlineData("{\"kind\":\"inspect\"}", "{\"kind\":\"inspect\",\"duration\":5}", "not a window")]
    [InlineData("{\"kind\":\"inspect\"}", "{\"kind\":\"goTo\"}", "not a window")]
    [InlineData("\"fallback\":{\"kind\":\"roam\"}", "\"fallback\":{\"kind\":\"sleep\"}", "fallback")]
    [InlineData("\"format\":1,", "\"format\":1,\"owner\":\"x\",", "owner")]
    [InlineData("routine-test:day", "day", "key")]
    public void A_wire_document_that_is_wrong_is_a_bad_argument_and_makes_no_ticket(string old, string with, string says)
    {
        var m = new RoutinesModel();
        Assert.Contains(old, GoldenWire);
        Assert.Equal(FakeRoutineCall.InvalidArgument, m.Assign(A, 7, GoldenWire.Replace(old, with), out var ticket, out var problem));
        Assert.Equal(0ul, ticket);
        Assert.Contains(says, problem);
        Step(m);
        Assert.Empty(m.Completions);
        Assert.Empty(m.AssignmentEvents);
        Assert.Null(m.Inspect(7));
    }

    [Fact]
    public void A_plan_of_another_mods_key_is_a_bad_argument()
    {
        var m = new RoutinesModel();
        Assert.Equal(FakeRoutineCall.InvalidArgument, m.Assign(B, 7, GoldenWire, out var ticket, out var problem));
        Assert.Equal(0ul, ticket);
        Assert.Contains("not a plan of other-mod", problem);
        // a key that only starts like the owner's id is not the owner's
        Assert.Equal(FakeRoutineCall.InvalidArgument, m.Assign("routine", 7, GoldenWire, out _, out _));
    }

    // ---- one completion per change: the assignment, not the first task -------------------------------------------------

    [Fact]
    public void An_assignment_completes_once_on_the_Post_of_the_step_that_committed_it()
    {
        var m = new RoutinesModel();
        var ticket = Assign(m, A, 7, GoldenWire);
        Assert.Null(m.Inspect(7));                 // queued, not committed
        Assert.Empty(m.Completions);
        m.Pre();
        Assert.NotNull(m.Inspect(7));
        Assert.Empty(m.Completions);               // committed, completes on Post
        Assert.Empty(m.AssignmentEvents);
        m.Post();
        var done = Assert.Single(m.Completions);
        Assert.Equal(new FakeRoutineCompletion(ticket, true, null, 7, 1), done);
        var e = Assert.Single(m.AssignmentEvents);
        Assert.Null(e.Before);
        Assert.Equal(("Assigned", false, FakeAssignmentState.Assigned, 1ul, A, "routine-test:day", 2u), (e.Reason, e.Teardown, e.After.Assignment, e.After.Revision, e.After.Owner, e.After.Key, e.After.PlanVersion));
        Assert.Equal((2u, GoldenHash), m.Plans["routine-test:day"]);
        Step(m);
        Step(m);
        Assert.Single(m.Completions);              // never a second time
    }

    // the reference contract test_assignment_ack_not_task_completion (out/blocks-b/evidence/test_selected_contracts.py)
    [Fact]
    public void A_later_task_refusal_reaches_only_the_task_events()
    {
        var m = new RoutinesModel();
        Assign(m, A, 7, Wire("routine-test:day", FakeRoutineKind.Idle, 1, AllDay(FakeRoutineKind.Sit)));
        Step(m);
        Assert.Single(m.Completions);
        // another mod's assign on the same villager: refused with the owner's name, no revision, no event
        var other = Assign(m, B, 7, Wire("other-mod:day"));
        Step(m);
        Assert.Equal(new FakeRoutineCompletion(other, false, "OwnedBy:routine-test", 7, 1), m.Completions[1]);
        Assert.Equal(1ul, m.Revision);
        Assert.Single(m.AssignmentEvents);
        // the task runs and the game fails it (no seat): a task event, no completion
        Assert.Equal(FakeRoutineKind.Sit, m.Decide(7));
        m.Finish(7, ok: false, "NoTarget");
        Step(m);
        Assert.Equal(2, m.Completions.Count);
        Assert.Equal([(FakeTaskState.None, FakeTaskState.Chosen, null), (FakeTaskState.Chosen, FakeTaskState.Started, null), (FakeTaskState.Started, FakeTaskState.Refused, "NoTarget")],
            m.TaskEvents.Select(t => (t.From, t.To, t.Reason)));
        Assert.All(m.TaskEvents, t => Assert.Equal((7u, "routine-test:day", 1u, 1ul, 1ul, FakeRoutineKind.Sit), (t.NpcId, t.Key, t.PlanVersion, t.Revision, t.Invocation, t.Task!.Value)));
        Assert.Equal("NoTarget", m.Inspect(7)!.LastReason);
        // a manual command suspends; only the owner resumes
        m.ManualSuspend(7, "GoTo");
        var foreign = m.Resume(B, 7, out var foreignTicket);
        var own = m.Resume(A, 7, out var ownTicket);
        Assert.Equal((FakeRoutineCall.Ok, FakeRoutineCall.Ok), (foreign, own));
        m.Pre();
        Assert.Equal(FakeAssignmentState.Assigned, m.Inspect(7)!.Assignment);
        m.Post();
        Assert.Equal(new FakeRoutineCompletion(foreignTicket, false, "OwnedBy:routine-test", 7, 2), m.Completions[2]);
        Assert.Equal(new FakeRoutineCompletion(ownTicket, true, null, 7, 3), m.Completions[3]);
        Assert.Equal(["Assigned", "ManualSuspended:GoTo", "Resumed"], m.AssignmentEvents.Select(e => e.Reason));
    }

    [Fact]
    public void A_change_that_is_already_in_effect_is_ok_with_no_revision_and_no_event()
    {
        var m = new RoutinesModel();
        Assert.Equal(FakeRoutineCall.Ok, m.Remove(A, 7, out var nothing));        // nothing assigned
        Step(m);
        Assert.Equal(new FakeRoutineCompletion(nothing, true, null, 7, 0), Assert.Single(m.Completions));
        Assign(m, A, 7, GoldenWire);
        Step(m);
        Assert.Equal(FakeRoutineCall.Ok, m.Resume(A, 7, out var running));        // not suspended
        Step(m);
        Assert.Equal(new FakeRoutineCompletion(running, true, null, 7, 1), m.Completions[2]);
        Assert.Equal(1ul, m.Revision);
        Assert.Single(m.AssignmentEvents);
        // a repeated manual command while suspended writes nothing
        m.ManualSuspend(7, "Follow");
        m.ManualSuspend(7, "Follow");
        Step(m);
        Assert.Equal(2ul, m.Revision);
        Assert.Equal(2, m.AssignmentEvents.Count);
        // a resume of a villager with no routine is a refusal
        Assert.Equal(FakeRoutineCall.Ok, m.Resume(A, 99, out var none));
        Step(m);
        Assert.Equal(new FakeRoutineCompletion(none, false, "NotAssigned", 99, 0), m.Completions[^1]);
    }

    [Fact]
    public void Only_the_owner_removes_and_the_same_owner_replaces()
    {
        var m = new RoutinesModel();
        Assign(m, A, 7, GoldenWire);
        Step(m);
        Assert.Equal(FakeRoutineCall.Ok, m.Remove(B, 7, out var losing));
        Step(m);
        Assert.Equal(new FakeRoutineCompletion(losing, false, "OwnedBy:routine-test", 7, 1), m.Completions[^1]);
        Assert.NotNull(m.Inspect(7));
        // the owner replaces its plan: the next revision, Before and After
        m.ManualSuspend(7, "Stay");
        var replace = Assign(m, A, 7, Wire("routine-test:night", FakeRoutineKind.Idle));
        Step(m);
        Assert.Equal(new FakeRoutineCompletion(replace, true, null, 7, 3), m.Completions[^1]);
        var e = m.AssignmentEvents[^1];
        Assert.Equal(("Replaced", "routine-test:day", "routine-test:night"), (e.Reason, e.Before!.Key, e.After.Key));
        Assert.Equal(FakeAssignmentState.ManualSuspended, m.Inspect(7)!.Assignment);   // a new plan does not resume by itself
        // the owner removes: gone, the next revision, and the villager is free for another mod
        Assert.Equal(FakeRoutineCall.Ok, m.Remove(A, 7, out var remove));
        var next = Assign(m, B, 7, Wire("other-mod:day"));                         // queued behind the removal in the same step
        Step(m);
        Assert.Equal(new FakeRoutineCompletion(remove, true, null, 7, 4), m.Completions[^2]);
        Assert.Equal(new FakeRoutineCompletion(next, true, null, 7, 5), m.Completions[^1]);
        Assert.Equal(FakeAssignmentState.Removed, m.AssignmentEvents[^2].After.Assignment);
        Assert.Equal(B, m.Inspect(7)!.Owner);
    }

    [Fact]
    public void An_earlier_success_stays_a_success_when_a_later_change_of_the_same_step_removes_it()
    {
        var m = new RoutinesModel();
        var assign = Assign(m, A, 7, GoldenWire);
        Assert.Equal(FakeRoutineCall.Ok, m.Remove(A, 7, out var remove));
        Step(m);
        Assert.Equal([new FakeRoutineCompletion(assign, true, null, 7, 1), new FakeRoutineCompletion(remove, true, null, 7, 2)], m.Completions);
        Assert.Null(m.Inspect(7));
        Assert.Equal(["Assigned", "Removed"], m.AssignmentEvents.Select(e => e.Reason));
    }

    // ---- the queue's limit -----------------------------------------------------------------------------------------------

    [Fact]
    public void The_257th_queued_change_is_refused_before_anything_changes()
    {
        var m = new RoutinesModel();
        for (uint i = 1; i <= RoutinesModel.MaxQueued; i++) Assign(m, A, i, GoldenWire);
        Assert.Equal(256, m.Queued);
        Assert.Equal(FakeRoutineCall.QueueFull, m.Assign(A, 1000, GoldenWire, out var ticket, out var problem));
        Assert.Equal((0ul, "QueueFull"), (ticket, problem));
        Assert.Equal(FakeRoutineCall.QueueFull, m.Remove(A, 1, out ticket));
        Assert.Equal(0ul, ticket);
        Step(m);
        Assert.Equal(256, m.Completions.Count);                 // every admitted change completed; the refused ones never do
        Assert.All(m.Completions, c => Assert.True(c.Ok));
        Assert.Equal(256, m.AssignmentEvents.Count);            // no event was dropped
        Assert.Null(m.Inspect(1000));
        Assert.Equal(Enumerable.Range(1, 256).Select(i => (ulong)i), m.Completions.Select(c => c.Revision));
        // the queue is free again after the step
        Assign(m, A, 1000, GoldenWire);
        Step(m);
        Assert.NotNull(m.Inspect(1000));
    }

    // ---- world stop and owner revoke ---------------------------------------------------------------------------------------

    [Fact]
    public void A_world_stop_completes_queued_changes_once_and_keeps_the_assignments_as_pending()
    {
        var m = new RoutinesModel();
        Assign(m, A, 7, Wire("routine-test:day", FakeRoutineKind.Idle, 1, AllDay(FakeRoutineKind.Roam)));
        Step(m);
        m.Decide(7);
        var queued = Assign(m, A, 8, GoldenWire);
        m.StopWorld();
        Assert.Equal(new FakeRoutineCompletion(queued, false, "WorldStopped", 8, 0), m.Completions[^1]);
        var e = m.AssignmentEvents[^1];
        Assert.Equal((true, "WorldStopped", FakeAssignmentState.Assigned, FakeAssignmentState.PendingSpawn, 1ul), (e.Teardown, e.Reason, e.Before!.Assignment, e.After.Assignment, e.After.Revision));
        var t = m.TaskEvents[^1];
        Assert.Equal((true, FakeTaskState.Started, FakeTaskState.Suspended, "WorldStopped"), (t.Teardown, t.From, t.To, t.Reason));
        Assert.Null(m.Inspect(8));
        // after the stop: no ticket, and nothing more completes
        Assert.Equal(FakeRoutineCall.WorldStopped, m.Assign(A, 9, GoldenWire, out var late, out _));
        Assert.Equal(0ul, late);
        var count = m.Completions.Count;
        m.StopWorld();
        Step(m);
        Assert.Equal(count, m.Completions.Count);
        Assert.Null(m.Decide(7));                               // the game's own chooser runs
    }

    [Fact]
    public void An_owners_revoke_completes_its_queued_changes_and_removes_its_assignments_at_the_next_Pre()
    {
        var m = new RoutinesModel();
        Assign(m, A, 7, GoldenWire);
        Assign(m, B, 8, Wire("other-mod:day"));
        Step(m);
        var queued = Assign(m, A, 9, GoldenWire);
        var others = Assign(m, B, 10, Wire("other-mod:day"));
        m.RevokeOwner(A);
        Assert.Equal(2, m.Completions.Count);                   // nothing until the next Pre
        Assert.NotNull(m.Inspect(7));
        Step(m);
        Assert.Null(m.Inspect(7));
        Assert.Null(m.Inspect(9));
        Assert.Equal(new FakeRoutineCompletion(queued, false, "Revoked", 9, 0), m.Completions[^2]);
        Assert.Equal(new FakeRoutineCompletion(others, true, null, 10, 4), m.Completions[^1]);
        var removed = m.AssignmentEvents.Single(e => e.Reason == "OwnerUnloaded");
        Assert.Equal((7u, FakeAssignmentState.Removed, 3ul), (removed.After.NpcId, removed.After.Assignment, removed.After.Revision));
        Assert.NotNull(m.Inspect(8));
        // the villager is free: nothing of the old owner's plan comes back
        Assign(m, B, 7, Wire("other-mod:day"));
        Step(m);
        Assert.Equal(B, m.Inspect(7)!.Owner);
    }

    [Fact]
    public void A_revoke_takes_only_the_old_owners_changes_when_the_mod_comes_back_with_a_new_owner()
    {
        var m = new RoutinesModel();
        Assert.Equal(FakeRoutineCall.Ok, m.Assign("owner 1", 7, GoldenWire, out _, out _, mod: A));
        Step(m);
        m.RevokeOwner("owner 1");                               // the mod reloads: its old owner goes
        Assert.Equal(FakeRoutineCall.Ok, m.Assign("owner 2", 8, GoldenWire, out var again, out _, mod: A));
        Step(m);
        Assert.Null(m.Inspect(7));                              // the old owner's assignment went
        Assert.Equal(new FakeRoutineCompletion(again, true, null, 8, 3), m.Completions[^1]);
        Assert.Equal(A, m.Inspect(8)!.Owner);                   // a snapshot names the mod, not the owner
        // another owner of the villager is refused with the mod's name
        Assert.Equal(FakeRoutineCall.Ok, m.Remove("owner 3", 8, out var foreign));
        Step(m);
        Assert.Equal(new FakeRoutineCompletion(foreign, false, "OwnedBy:" + A, 8, 3), m.Completions[^1]);
    }

    [Fact]
    public void Revisions_start_after_what_a_world_saved()
    {
        var m = new RoutinesModel();
        m.StartRevisionsAfter(41);
        Assign(m, A, 7, GoldenWire);
        Step(m);
        Assert.Equal(42ul, m.Inspect(7)!.Revision);
    }

    // ---- the chooser -----------------------------------------------------------------------------------------------------

    [Fact]
    public void The_window_of_the_minute_decides_and_two_villagers_keep_their_own_state()
    {
        var m = new RoutinesModel();
        Assign(m, A, 7, GoldenWire);
        Assign(m, B, 8, Wire("other-mod:night", FakeRoutineKind.Roam, 1, new FakeRoutineWindow(1200, 300, false, 0, FakeRoutineKind.Idle)));
        Step(m);
        m.SetClock(7);
        Assert.Equal(FakeRoutineKind.Inspect, m.Decide(7));
        Assert.Equal(FakeRoutineKind.Roam, m.Decide(8));          // no window at 07:00: the fallback, as a task
        Assert.Equal((0, -1), (m.Inspect(7)!.WindowIndex, m.Inspect(8)!.WindowIndex));
        Assert.Equal((FakeVillagerState.FindTarget, (byte)3), (m.Villager(7).State, m.Villager(7).Decision));
        Assert.Equal((FakeVillagerState.Roam, (byte)1), (m.Villager(8).State, m.Villager(8).Decision));
        m.SetClock(13);
        Assert.Equal(FakeRoutineKind.Sit, m.Decide(7));           // the all-day window below the others
        m.SetClock(23, 59);
        Assert.Equal(FakeRoutineKind.Idle, m.Decide(8));
        m.NowNs += 60_000_000_000;                                // midnight
        Assert.Equal(0, m.Minute);
        Assert.Equal(FakeRoutineKind.Idle, m.Decide(8));          // the window runs over the rollover
        Assert.Equal((FakeVillagerState.Idle, (byte)7), (m.Villager(8).State, m.Villager(8).Decision));
        Assert.Equal(0, m.Villager(7).VanillaChoices + m.Villager(8).VanillaChoices);
        // each decision is its own invocation
        Assert.Equal([1ul, 2ul, 3ul, 4ul, 5ul], Drain(m).Where(t => t.To == FakeTaskState.Chosen).Select(t => t.Invocation));
    }

    private static List<FakeRoutineTaskEvent> Drain(RoutinesModel m)
    {
        var before = m.TaskEvents.Count;
        m.Post();
        return m.TaskEvents.Skip(before).ToList();
    }

    [Theory]
    [InlineData((byte)0)]
    [InlineData((byte)1)]
    [InlineData((byte)4)]
    public void An_active_phase_chooses(byte phase)
    {
        var m = new RoutinesModel();
        Assign(m, A, 7, Wire("routine-test:day", FakeRoutineKind.Idle));
        Step(m);
        Assert.Equal(FakeRoutineKind.Idle, m.Decide(7, phase));
        Assert.Equal(0, m.Villager(7).VanillaChoices);
    }

    [Fact]
    public void Phase_2_and_a_villager_without_a_routine_run_the_games_chooser_once_and_phase_3_chooses_nothing()
    {
        var m = new RoutinesModel();
        Assign(m, A, 7, Wire("routine-test:day", FakeRoutineKind.Idle));
        Step(m);
        Assert.Null(m.Decide(7, phase: 2));
        Assert.Equal(1, m.Villager(7).VanillaChoices);
        Assert.Null(m.Decide(7, phase: 3));
        Assert.Equal(1, m.Villager(7).VanillaChoices);
        Assert.Equal(FakeVillagerState.DecideTask, m.Villager(7).State);
        Assert.Null(m.Decide(8));                                 // unassigned
        Assert.Equal(1, m.Villager(8).VanillaChoices);
        m.ManualSuspend(7, "GoTo");
        Assert.Null(m.Decide(7));                                 // suspended
        Assert.Equal(2, m.Villager(7).VanillaChoices);
        m.Post();
        Assert.Empty(m.TaskEvents);
        // a vanilla window is a task of the plan that runs the game's chooser
        Assign(m, B, 9, Wire("other-mod:day"));
        Step(m);
        Assert.Equal(FakeRoutineKind.Vanilla, m.Decide(9));
        Assert.Equal(1, m.Villager(9).VanillaChoices);
    }

    [Fact]
    public void A_sleep_the_game_does_not_allow_now_is_refused_and_the_fallback_runs()
    {
        var m = new RoutinesModel();
        Assign(m, A, 7, Wire("routine-test:day", FakeRoutineKind.Idle, 1, AllDay(FakeRoutineKind.Sleep)));
        Step(m);
        Assert.Equal(FakeRoutineKind.Idle, m.Decide(7));
        Assert.Equal(1, m.Villager(7).NativeSleepChecks);         // the game's check, once
        m.Post();
        Assert.Equal([(FakeRoutineKind.Sleep, FakeTaskState.Chosen, null), (FakeRoutineKind.Sleep, FakeTaskState.Refused, "NativeSleepNotCurrentlyAllowed"),
                (FakeRoutineKind.Idle, FakeTaskState.Chosen, null), (FakeRoutineKind.Idle, FakeTaskState.Started, null)],
            m.TaskEvents.Select(t => (t.Task!.Value, t.To, t.Reason)));
        Assert.Equal(-1, m.Inspect(7)!.WindowIndex);
        Assert.Single(m.Completions);                              // the assignment's, nothing for the refusal
        // when the game allows it, its own check has taken FindBed: adopted
        m.Villager(7).State = FakeVillagerState.DecideTask;
        m.Villager(7).NativeSleepAllowed = true;
        Assert.Equal(FakeRoutineKind.Sleep, m.Decide(7));
        Assert.Equal((FakeVillagerState.FindBed, (byte)5), (m.Villager(7).State, m.Villager(7).Decision));
        Assert.Equal(2, m.Villager(7).NativeSleepChecks);
    }

    // ---- the keep-awake prefix (the reference contract test_keep_awake_prefix_preserves_only_false_path) ------------------

    private static RoutinesModel Awake(out FakeRoutineVillager v)
    {
        var m = new RoutinesModel();
        Assign(m, A, 7, Wire("routine-test:day", FakeRoutineKind.Vanilla, 1, AllDay(FakeRoutineKind.Inspect)));
        Step(m);
        v = m.Villager(7);
        (v.Decision, v.CommonFlags, v.TaskFailures, v.WakeNs) = (3, 0x10, 42, 6);
        v.NativeSleepAllowed = true;    // the game would put it to bed: the prefix must not ask it
        return m;
    }

    [Fact]
    public void The_prefix_keeps_the_false_paths_writes_and_makes_no_transition_in_all_18_states()
    {
        for (byte state = 0; state < 18; state++)
        {
            var m = Awake(out var v);
            v.State = v.Previous = (FakeVillagerState)state;
            m.NowNs = 7;                                           // just after the wake time
            Assert.False(m.SleepCheck(7));
            Assert.Equal(0, v.NativeSleepChecks);
            Assert.Equal(((FakeVillagerState)state, (FakeVillagerState)state, (byte)3, (byte)42), (v.State, v.Previous, v.Decision, v.TaskFailures));
            var reaches = state is 3 or 4 or 6 or 7;               // Idle, DecideTask, GoToTarget, Roam
            Assert.Equal(reaches ? 0 : 0x10, v.CommonFlags);
            var marks = m.Marks.Select(x => x.Record).ToArray();
            Assert.Equal(state == 6 ? ["dyn", "dyn", "common", "common", "common", "common", "common"]
                : reaches ? ["dyn", "common", "common", "common", "common", "common"]
                : (string[])["dyn"], marks);
            Assert.All(m.Marks, x => Assert.Equal(7u, x.NpcId));
        }
    }

    [Fact]
    public void The_prefix_clears_the_flag_only_after_the_wake_time_and_leaves_a_walk_to_bed_alone()
    {
        // at the wake time, and just before: the flag stays, and the mark that precedes the clearing is not made
        foreach (var now in new long[] { 6, 5 })
        {
            var m = Awake(out var v);
            v.State = FakeVillagerState.Idle;
            m.NowNs = now;
            Assert.False(m.SleepCheck(7));
            Assert.Equal(0x10, v.CommonFlags);
            Assert.Equal(4, m.Marks.Count(x => x.Record == "common"));
        }
        // just after
        {
            var m = Awake(out var v);
            v.State = FakeVillagerState.Idle;
            m.NowNs = 7;
            Assert.False(m.SleepCheck(7));
            Assert.Equal(0, v.CommonFlags);
            Assert.Equal(5, m.Marks.Count(x => x.Record == "common"));
        }
        // without the flag: the two reads of the sleep window only
        {
            var m = Awake(out var v);
            (v.State, v.CommonFlags) = (FakeVillagerState.Roam, 0x01);
            m.NowNs = 7;
            Assert.False(m.SleepCheck(7));
            Assert.Equal(0x01, v.CommonFlags);
            Assert.Equal(["dyn", "common", "common"], m.Marks.Select(x => x.Record));
        }
        // on the way to a target with the decision Sleep: two marks, nothing else
        {
            var m = Awake(out var v);
            (v.State, v.Decision) = (FakeVillagerState.GoToTarget, 5);
            m.NowNs = 7;
            Assert.False(m.SleepCheck(7));
            Assert.Equal(0x10, v.CommonFlags);
            Assert.Equal(["dyn", "dyn"], m.Marks.Select(x => x.Record));
        }
    }

    [Fact]
    public void The_games_own_sleep_check_runs_once_where_the_plan_does_not_keep_the_villager_awake()
    {
        // no routine
        var m = new RoutinesModel();
        m.Villager(7).NativeSleepAllowed = true;
        Assert.True(m.SleepCheck(7));
        Assert.Equal(1, m.Villager(7).NativeSleepChecks);
        Assert.Empty(m.Marks);
        // a window of sleep, and a window of vanilla
        foreach (var kind in new[] { FakeRoutineKind.Sleep, FakeRoutineKind.Vanilla })
        {
            m = new RoutinesModel();
            Assign(m, A, 7, Wire("routine-test:day", FakeRoutineKind.Idle, 1, AllDay(kind)));
            Step(m);
            Assert.False(m.SleepCheck(7));
            Assert.Equal(1, m.Villager(7).NativeSleepChecks);
            Assert.Empty(m.Marks);
        }
        // suspended by a manual command
        m = Awake(out var v);
        v.State = FakeVillagerState.Idle;
        m.ManualSuspend(7, "GoTo");
        Assert.True(m.SleepCheck(7));
        Assert.Equal(1, v.NativeSleepChecks);
        Assert.Empty(m.Marks);
        Assert.Equal(0x10, v.CommonFlags);
    }
}
