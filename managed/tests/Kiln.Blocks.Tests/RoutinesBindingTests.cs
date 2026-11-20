// SPDX-License-Identifier: MIT
// Routines (Npc/Routines.cs) over a table this test serves in place of kiln.routines: what goes to the service, what a
// mod gets back, which events are posted, and what throws. The service's own rules (the queue, owners, revisions) are
// the native tests' and the kit's fake's; here the answers are scripted, so each is one the service can give
// (kiln_routines.h).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Xunit;

namespace Kiln.BlockTests;

public sealed unsafe class RoutinesBindingTests : IDisposable
{
    private const string ModId = "routine-test";
    private const ulong ModOwner = 31;

    // ---- the table this test serves ------------------------------------------------------------------------------

    private sealed class Backend_ : IKilnBackend
    {
        public bool Tick = true, Feature = true, Table = true;
        public string Version => "test";
        public bool IsTickThread => Tick;
        public bool HasFeature(string name) => Feature && name == "routines.decisions";
        public nint Service(string name, uint minVersion) => Table && name == "kiln.routines" ? (nint)Served : 0;
        public void Log(ulong owner, LogLevel level, string text) => Logged.Add(text);
        public readonly List<string> Logged = [];
    }

    private sealed record Call(string Function, ulong Owner, uint NpcId, string? Plan, nint Done, nint User);

    private static readonly RoutineNative.Table* Served = MakeTable();
    private static readonly List<Call> Calls = [];
    private static int _answer;
    private static ulong _tickets;
    private static string? _snapshot;
    private static nint _eventFn, _eventUser;
    private static readonly List<ulong> Revoked = [];

    private static RoutineNative.Table* MakeTable()
    {
        var t = (RoutineNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(RoutineNative.Table));  // process lifetime
        t->Header = new ServiceHeader { Size = (uint)sizeof(RoutineNative.Table), Version = 1 };
        t->Assign = &Assign;
        t->Remove = &Remove;
        t->Resume = &Resume;
        t->Inspect = &Inspect;
        t->RevokeOwner = &RevokeOwner;
        t->SubscribeEvents = &Subscribe;
        t->UnsubscribeEvents = &Unsubscribe;
        t->Restore = &Restore;
        t->SetRevisionFloor = &SetFloor;
        return t;
    }

    private sealed record Restored(ulong Owner, uint NpcId, string Plan, ulong Revision, uint Flags, string Reason);
    private static readonly List<Restored> Restores = [];
    private static readonly List<ulong> Floors = [];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Restore(ulong owner, uint npc, NativeStr plan, ulong revision, uint flags, NativeStr reason,
        delegate* unmanaged[Cdecl]<RoutineNative.Result*, void*, void> done, void* user, ulong* ticket)
    {
        Restores.Add(new Restored(owner, npc, plan.ToString(), revision, flags, reason.ToString()));
        *ticket = ++_tickets;
        return RoutineNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SetFloor(ulong floor)
    {
        Floors.Add(floor);
        return RoutineNative.Ok;
    }

    private static int Queue(string function, ulong owner, uint npc, string? plan, void* done, void* user, ulong* ticket)
    {
        Calls.Add(new Call(function, owner, npc, plan, (nint)done, (nint)user));
        *ticket = 0;
        if (_answer != RoutineNative.Ok) return _answer;
        *ticket = ++_tickets;
        return RoutineNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Assign(ulong owner, uint npc, NativeStr plan, delegate* unmanaged[Cdecl]<RoutineNative.Result*, void*, void> done, void* user,
        ulong* ticket) => Queue("assign", owner, npc, plan.ToString(), done, user, ticket);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Remove(ulong owner, uint npc, delegate* unmanaged[Cdecl]<RoutineNative.Result*, void*, void> done, void* user, ulong* ticket) =>
        Queue("remove", owner, npc, null, done, user, ticket);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Resume(ulong owner, uint npc, delegate* unmanaged[Cdecl]<RoutineNative.Result*, void*, void> done, void* user, ulong* ticket) =>
        Queue("resume", owner, npc, null, done, user, ticket);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Inspect(uint npc, byte* json, nuint cap, nuint* length)
    {
        Calls.Add(new Call("inspect", 0, npc, null, 0, 0));
        *length = 0;
        if (_snapshot is null) return RoutineNative.NotFound;
        var bytes = Encoding.UTF8.GetBytes(_snapshot);
        *length = (nuint)bytes.Length;
        if ((nuint)bytes.Length > cap) return RoutineNative.BufferTooSmall;
        bytes.CopyTo(new Span<byte>(json, bytes.Length));
        return RoutineNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void RevokeOwner(ulong owner) => Revoked.Add(owner);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Subscribe(ulong owner, delegate* unmanaged[Cdecl]<NativeStr, void*, void> fn, void* user, ulong* handle)
    {
        (_eventFn, _eventUser) = ((nint)fn, (nint)user);
        *handle = 1;
        return RoutineNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsubscribe(ulong handle) => RoutineNative.Ok;

    /// <summary>The service completes call <paramref name="index"/> (as on its Post).</summary>
    private static void Complete(int index, bool ok, ulong revision, string reason = "")
    {
        var c = Calls[index];
        var bytes = Encoding.UTF8.GetBytes(reason);
        fixed (byte* p = bytes)
        {
            var r = new RoutineNative.Result
            {
                Size = (uint)sizeof(RoutineNative.Result), Ok = ok ? 1u : 0u, Ticket = 1, Revision = revision, NpcId = c.NpcId,
                Reason = new NativeStr { Ptr = bytes.Length == 0 ? null : p, Len = (nuint)bytes.Length },
            };
            ((delegate* unmanaged[Cdecl]<RoutineNative.Result*, void*, void>)c.Done)(&r, (void*)c.User);
        }
    }

    /// <summary>The service sends an event document.</summary>
    private static void Send(string json)
    {
        Assert.NotEqual(0, _eventFn);
        var bytes = Encoding.UTF8.GetBytes(json);
        fixed (byte* p = bytes)
            ((delegate* unmanaged[Cdecl]<NativeStr, void*, void>)_eventFn)(new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, (void*)_eventUser);
    }

    // ---- setup ------------------------------------------------------------------------------------------------------

    private readonly IKilnBackend _previous = Backend.Current;
    private readonly ModContext? _caller = HostAccess.Current;
    private readonly Backend_ _backend = new();
    private readonly ModContext _mod;
    private readonly List<KilnEvent> _events = [];
    private readonly EventSubscription _onAssignment, _onTask;
    private readonly BlockHandle<RoutineBlock> _day;
    private readonly Villager _villager = new(new EntityId(4242), 7);

    public RoutinesBindingTests()
    {
        RoutineRegistry.Forget(ModId);
        Calls.Clear();
        Revoked.Clear();
        Restores.Clear();
        Floors.Clear();
        (_answer, _snapshot, _eventFn, _eventUser) = (RoutineNative.Ok, null, 0, 0);
        Backend.Current = _backend;
        Capabilities.Reset();
        Routines.ResetForTests();
        _mod = HostAccess.CreateContext(new ModInfo(ModId, ModId, "1", ""), ModOwner, "");
        HostAccess.Current = _mod;
        _day = Define(new ModBlocks(ModId, 1) { ManifestClients = "none" }, "day", RoutineTests.Day);
        _onAssignment = Events.On<RoutineAssignmentChangedEvent>(_events.Add);
        _onTask = Events.On<RoutineTaskChangedEvent>(_events.Add);
    }

    public void Dispose()
    {
        _onAssignment.Dispose();
        _onTask.Dispose();
        Routines.ResetForTests();
        RoutineRegistry.Forget(ModId);
        RoutineRegistry.Forget("routine-other");
        HostAccess.Current = _caller;
        Backend.Current = _previous;
        Capabilities.Reset();
    }

    private static BlockHandle<RoutineBlock> Define(ModBlocks mod, string name, RoutineBlock b)
    {
        var (handle, low, _) = Compiler.Lower(mod, name, b);
        low.Commit();
        mod.Handles[name] = handle;
        return (BlockHandle<RoutineBlock>)handle;
    }

    private const string Snapshot =
        """{"npcId":7,"owner":"routine-test","key":"routine-test:day","planVersion":2,"revision":1,"assignment":"Assigned","windowIndex":0,"invocation":3,"task":"inspect","taskState":"Started","nativeState":5,"lastReason":null,"paused":false}""";

    // ---- the plan's S3 proofs -----------------------------------------------------------------------------------------

    [Fact]
    public void Assign_sends_the_plans_wire_document_and_completes_Ok_when_the_service_says_the_assignment_changed()
    {
        var task = Routines.Assign(_villager, _day);
        var call = Assert.Single(Calls, c => c.Function == "assign");
        Assert.Equal((ModOwner, 7u, RoutineTests.GoldenWire), (call.Owner, call.NpcId, call.Plan));
        Assert.False(task.IsCompleted);                                     // until the service's Post
        // the villager is paused and no window is active: still Ok, the assignment changed (kiln_routines.h)
        Complete(0, ok: true, revision: 1);
        Assert.True(task.IsCompletedSuccessfully);
        Assert.Equal(new RoutineResult(true, null, 7, 1), task.Result);
        Assert.NotEqual(0, _eventFn);                                       // the events are subscribed with the first call
    }

    [Fact]
    public void A_later_task_refusal_reaches_only_the_task_event()
    {
        var task = Routines.Assign(_villager, _day);
        Complete(0, ok: true, revision: 1);
        Send($$"""{"type":"assignment","before":null,"after":{{Snapshot}},"reason":"Assigned","teardown":false}""");
        Send("""{"type":"task","npcId":7,"key":"routine-test:day","planVersion":2,"revision":1,"invocation":3,"windowIndex":1,"task":"sleep","from":"Chosen","to":"Refused","reason":"NativeSleepNotCurrentlyAllowed","teardown":false}""");
        Assert.Equal(new RoutineResult(true, null, 7, 1), task.Result);     // the answer stays the assignment's
        var assigned = Assert.IsType<RoutineAssignmentChangedEvent>(_events[0]);
        Assert.Null(assigned.Before);
        Assert.Equal(("Assigned", false, RoutineAssignmentState.Assigned, "routine-test", 3ul, VillagerState.FindTarget),
            (assigned.Reason, assigned.Teardown, assigned.After.Assignment, assigned.After.Owner, assigned.After.Invocation, assigned.After.NativeState));
        Assert.Equal(new InspectTask(), assigned.After.Task);
        var refused = Assert.IsType<RoutineTaskChangedEvent>(_events[1]);
        Assert.Equal((7u, "routine-test:day", 2u, 1ul, 3ul, RoutineTaskState.Chosen, RoutineTaskState.Refused, "NativeSleepNotCurrentlyAllowed", false),
            (refused.NpcId, refused.Key, refused.PlanVersion, refused.Revision, refused.Invocation, refused.From, refused.To, refused.Reason, refused.Teardown));
        Assert.Equal(new SleepTask(), refused.Task);
        Assert.Equal(2, _events.Count);
    }

    [Fact]
    public void An_already_effective_Remove_or_Resume_is_Ok_with_no_revision_and_no_event()
    {
        var remove = Routines.Remove(_villager);
        var resume = Routines.Resume(_villager);
        Assert.Equal(["remove", "resume"], Calls.Select(c => c.Function));
        Assert.All(Calls, c => Assert.Equal((ModOwner, 7u), (c.Owner, c.NpcId)));
        Complete(0, ok: true, revision: 0);                                   // nothing to remove
        Complete(1, ok: true, revision: 4);                                   // not suspended: the old revision
        Assert.Equal(new RoutineResult(true, null, 7, 0), remove.Result);
        Assert.Equal(new RoutineResult(true, null, 7, 4), resume.Result);
        Assert.Empty(_events);
    }

    // ---- refusals, and what throws ----------------------------------------------------------------------------------

    [Fact]
    public void A_refusal_on_Post_is_Ok_false_with_the_services_reason()
    {
        var other = Routines.Assign(_villager, _day);
        var resume = Routines.Resume(_villager);
        Complete(0, ok: false, revision: 5, "OwnedBy:other-mod");
        Complete(1, ok: false, revision: 0, "NotAssigned");
        Assert.Equal(new RoutineResult(false, "OwnedBy:other-mod", 7, 5), other.Result);
        Assert.Equal(new RoutineResult(false, "NotAssigned", 7, 0), resume.Result);
    }

    [Fact]
    public void A_full_queue_is_QueueFull_at_once_and_no_world_is_WorldStopped()
    {
        _answer = RoutineNative.BufferTooSmall;
        var full = Routines.Assign(_villager, _day);
        Assert.Equal(new RoutineResult(false, "QueueFull", 7, 0), full.Result);
        _answer = RoutineNative.Unavailable;                                  // the row reads on: the service has no world
        var stopped = Routines.Remove(_villager);
        Assert.Equal(new RoutineResult(false, "WorldStopped", 7, 0), stopped.Result);
        _answer = RoutineNative.Ok;
        // nothing waits for the refused calls: a late callback with their ids finds nothing
        var later = Routines.Resume(_villager);
        Complete(2, ok: true, revision: 1);
        Assert.Equal(new RoutineResult(true, null, 7, 1), later.Result);
    }

    [Fact]
    public void A_bad_argument_the_service_finds_throws_ArgumentException_and_a_wrong_thread_answer_InvalidOperationException()
    {
        _answer = RoutineNative.InvalidArgument;
        Assert.Throws<ArgumentException>(() => { _ = Routines.Assign(_villager, _day); });
        _answer = RoutineNative.WrongThread;
        Assert.Throws<InvalidOperationException>(() => { _ = Routines.Remove(_villager); });
        Assert.Equal(2, Calls.Count);
    }

    [Fact]
    public void The_capability_off_or_the_service_missing_throws_KilnUnavailableException_naming_routines_decisions()
    {
        _backend.Feature = false;
        Capabilities.Reset();
        var off = Assert.Throws<KilnUnavailableException>(() => { _ = Routines.Assign(_villager, _day); });
        Assert.Contains("routines.decisions", off.Message, StringComparison.Ordinal);
        Assert.Throws<KilnUnavailableException>(() => { _ = Routines.Remove(_villager); });
        Assert.Throws<KilnUnavailableException>(() => { _ = Routines.Resume(_villager); });
        Assert.Throws<KilnUnavailableException>(() => Routines.Inspect(_villager));
        Assert.Empty(Calls);
        // the row reads on again: decided at the call, not from an earlier read
        _backend.Feature = true;
        Capabilities.Reset();
        _ = Routines.Remove(_villager);
        Assert.Single(Calls);
        // no table: off, with the reason
        _backend.Table = false;
        Routines.ResetForTests();
        var missing = Assert.Throws<KilnUnavailableException>(() => { _ = Routines.Remove(_villager); });
        Assert.Contains("routines.decisions", missing.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void A_foreign_or_undefined_plan_throws_ArgumentException_and_nothing_is_sent()
    {
        HostAccess.Current = null;
        var foreign = Define(new ModBlocks("routine-other", 1) { ManifestClients = "none" }, "day", RoutineTests.Day);
        HostAccess.Current = _mod;
        Assert.Throws<ArgumentException>(() => { _ = Routines.Assign(_villager, foreign); });
        RoutineRegistry.Forget(ModId);                                        // the mod's plan is no longer defined
        var gone = Assert.Throws<ArgumentException>(() => { _ = Routines.Assign(_villager, _day); });
        Assert.Contains("not defined now", gone.Message, StringComparison.Ordinal);
        Assert.Empty(Calls);
    }

    [Fact]
    public void Off_the_tick_thread_or_outside_mod_code_throws_InvalidOperationException()
    {
        _backend.Tick = false;
        Assert.Throws<InvalidOperationException>(() => { _ = Routines.Assign(_villager, _day); });
        Assert.Throws<InvalidOperationException>(() => Routines.Inspect(_villager));
        _backend.Tick = true;
        HostAccess.Current = null;
        Assert.Throws<InvalidOperationException>(() => { _ = Routines.Remove(_villager); });
        Assert.Empty(Calls);
    }

    // ---- Inspect, the mod's unload, the teardown --------------------------------------------------------------------

    [Fact]
    public void Inspect_reads_the_services_snapshot_also_when_it_is_longer_than_the_first_buffer()
    {
        Assert.Null(Routines.Inspect(_villager));
        _snapshot = Snapshot;
        var s = Routines.Inspect(_villager)!;
        Assert.Equal(new RoutineSnapshot(7, "routine-test", "routine-test:day", 2, 1, RoutineAssignmentState.Assigned, 0, 3, new InspectTask(),
            RoutineTaskState.Started, VillagerState.FindTarget, null), s);
        _snapshot = Snapshot.Replace("\"lastReason\":null", "\"lastReason\":\"" + new string('x', 2000) + "\"");
        Assert.Equal(2000, Routines.Inspect(_villager)!.LastReason!.Length);
        Assert.Equal(4, Calls.Count(c => c.Function == "inspect"));          // the long one asked twice
    }

    [Fact]
    public void A_mod_that_unloads_revokes_its_owner_and_its_waiting_answers_are_canceled()
    {
        var task = Routines.Assign(_villager, _day);
        _mod.Retire();
        Assert.Equal([ModOwner], Revoked);
        Assert.True(task.IsCanceled);
        Complete(0, ok: false, revision: 0, "Revoked");                     // the service's late answer finds nothing
        Assert.True(task.IsCanceled);
    }

    [Fact]
    public void A_teardown_event_is_posted_with_Teardown_set_and_an_unreadable_event_is_logged_not_thrown()
    {
        _ = Routines.Remove(_villager);                                       // subscribes
        Send($$"""{"type":"assignment","before":{{Snapshot}},"after":{{Snapshot.Replace("\"Assigned\"", "\"PendingSpawn\"")}},"reason":"WorldStopped","teardown":true}""");
        var e = Assert.IsType<RoutineAssignmentChangedEvent>(Assert.Single(_events));
        Assert.Equal((true, RoutineAssignmentState.Assigned, RoutineAssignmentState.PendingSpawn), (e.Teardown, e.Before!.Assignment, e.After.Assignment));
        Send("""{"type":"something","teardown":false,"reason":null}""");
        Send("""{"type":"task","npcId":7}""");
        Assert.Single(_events);
        Assert.Equal(2, _backend.Logged.Count(l => l.Contains("an event was not posted", StringComparison.Ordinal)));
    }

    // ---- across a restart (S5): the saved book follows the events, and a world start binds it again ---------------

    private RoutineBook UseBook(string world = "world-1")
    {
        var book = new RoutineBook();
        RoutineSaves.TestBook = book;
        RoutineSaves.TestWorld = world;
        RoutineSaves.OwnerLoaded = id => id == ModId;
        RoutineSaves.OwnerOf = id => id == ModId ? ModOwner : 0;
        return book;
    }

    private static string Event(string after, string reason, bool teardown = false) =>
        $$"""{"type":"assignment","before":null,"after":{{after}},"reason":"{{reason}}","teardown":{{(teardown ? "true" : "false")}}}""";

    [Fact]
    public void The_saved_book_follows_every_assignment_event_and_a_teardown_leaves_it_alone()
    {
        var book = UseBook();
        Routines.Assign(_villager, _day);                                       // subscribes the events
        Send(Event(Snapshot, "Assigned"));
        var row = Assert.Single(book.Assignments);
        var plan = RoutineRegistry.Find(_day)!.Plan;
        Assert.Equal((7u, ModId, "routine-test:day", 2u, plan.ContentHash, false, 1ul),
            (row.NpcId, row.Owner, row.Key, row.PlanVersion, row.ContentHash, row.ManualSuspended, row.Revision));
        Send(Event(Snapshot.Replace("\"revision\":1", "\"revision\":2").Replace("\"Assigned\"", "\"ManualSuspended\""), "ManualSuspended"));
        Assert.True(Assert.Single(book.Assignments).ManualSuspended);
        // the world stops: the book is what the next start binds again
        Send(Event(Snapshot.Replace("\"revision\":1", "\"revision\":2").Replace("\"Assigned\"", "\"PendingSpawn\""), "WorldStopped", teardown: true));
        Assert.True(Assert.Single(book.Assignments).ManualSuspended);
        // the owner unloads (a server stop unloads every mod before its last save): the saved row stays for the next start
        Send(Event(Snapshot.Replace("\"revision\":1", "\"revision\":3").Replace("\"Assigned\"", "\"Removed\""), "OwnerUnloaded"));
        Assert.True(Assert.Single(book.Assignments).ManualSuspended);
        Assert.Equal(3ul, book.LastRevision);
        Send(Event(Snapshot.Replace("\"revision\":1", "\"revision\":4").Replace("\"Assigned\"", "\"Removed\""), "Removed"));
        Assert.Empty(book.Assignments);
        Assert.Equal(4ul, book.LastRevision);
    }

    [Fact]
    public void A_world_start_binds_the_saved_rows_again_at_their_revisions_and_says_what_it_removed()
    {
        var book = UseBook();
        var plan = RoutineRegistry.Find(_day)!.Plan;
        book.World = "world-1";
        book.LastRevision = 9;
        book.Assignments.Add(new SavedRoutine { NpcId = 7, Owner = ModId, Key = plan.Key, PlanVersion = plan.Version, ContentHash = plan.ContentHash, ManualSuspended = true, Revision = 5 });
        book.Assignments.Add(new SavedRoutine { NpcId = 8, Owner = ModId, Key = "routine-test:gone", PlanVersion = 1, ContentHash = "x", Revision = 6 });
        book.Assignments.Add(new SavedRoutine { NpcId = 9, Owner = "routine-other", Key = "routine-other:day", PlanVersion = 1, ContentHash = "y", Revision = 7 });
        RoutineSaves.Tick();
        var r = Assert.Single(Restores);
        Assert.Equal(new Restored(ModOwner, 7, RoutineTests.GoldenWire, 5, RoutineNative.RestoreSuspended, ""), r);
        Assert.Equal([11ul], Floors);                                             // two removals took the next two revisions
        Assert.Equal([7u], book.Assignments.Select(a => a.NpcId));
        var removed = _events.OfType<RoutineAssignmentChangedEvent>().ToList();
        Assert.Equal([(8u, "DefinitionMissing"), (9u, "OwnerMissing")], removed.Select(e => (e.After.NpcId, e.Reason)));
        Assert.All(removed, e => Assert.Equal(RoutineAssignmentState.Removed, e.After.Assignment));
        // once per world: the next tick binds nothing again
        RoutineSaves.Tick();
        Assert.Single(Restores);
        // a raised version migrates: a new revision, and the service says Migrated
        Restores.Clear();
        var next = UseBook("world-2");
        next.Assignments.Add(new SavedRoutine { NpcId = 7, Owner = ModId, Key = plan.Key, PlanVersion = plan.Version - 1, ContentHash = "old", Revision = 4 });
        next.LastRevision = 4;
        RoutineSaves.Tick();
        Assert.Equal(new Restored(ModOwner, 7, RoutineTests.GoldenWire, 5, 0, "Migrated"), Assert.Single(Restores));
    }
}
