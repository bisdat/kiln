// SPDX-License-Identifier: MIT
// The kit's fake kiln.routines (Fakes/Routines/) on its raw table, without Kiln.Blocks: what it answers must stay what the
// native service of this build answers (native/services/routines/tests, kiln_routines.h).
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed unsafe class RoutinesFakeTests : IDisposable
{
    private const string GoldenWire =
        """{"format":1,"key":"routine-test:day","planVersion":2,"contentHash":"02dcb72ca219f3a72202cede3638112dda4aa5a71948b89543c43f667c4d3ebb","windows":[{"startMinute":360,"endMinute":720,"allDay":false,"priority":0,"task":{"kind":"inspect"}},{"startMinute":1320,"endMinute":360,"allDay":false,"priority":5,"task":{"kind":"sleep"}},{"startMinute":0,"endMinute":0,"allDay":true,"priority":-1,"task":{"kind":"sit"}}],"fallback":{"kind":"roam"}}""";

    // kiln_routines.h as this test sees it (Kiln.Api's NativeStr is not visible here); sizes checked against the kit's mirror
    [StructLayout(LayoutKind.Sequential)]
    private struct Str
    {
        public byte* Ptr;
        public nuint Len;
        public override string ToString() => Ptr == null ? "" : Encoding.UTF8.GetString(Ptr, (int)Len);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Result
    {
        public uint Size, Ok;
        public ulong Ticket, Revision;
        public uint NpcId, ReasonCode;
        public Str Reason;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Table
    {
        public uint Size, Version;
        public delegate* unmanaged[Cdecl]<ulong, uint, Str, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, ulong*, int> Assign;
        public delegate* unmanaged[Cdecl]<ulong, uint, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, ulong*, int> Remove;
        public delegate* unmanaged[Cdecl]<ulong, uint, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, ulong*, int> Resume;
        public delegate* unmanaged[Cdecl]<uint, byte*, nuint, nuint*, int> Inspect;
        public delegate* unmanaged[Cdecl]<ulong, void> RevokeOwner;
        public delegate* unmanaged[Cdecl]<ulong, delegate* unmanaged[Cdecl]<Str, void*, void>, void*, ulong*, int> SubscribeEvents;
        public delegate* unmanaged[Cdecl]<ulong, int> UnsubscribeEvents;
    }

    private static Table* T => (Table*)KitBackend.Instance.Service(RoutinesAbi.Service, 1);

    private sealed record Done(ulong Ticket, bool Ok, ulong Revision, uint NpcId, uint Code, string Reason, nint User);

    private static readonly ConcurrentQueue<Done> Completed = new();
    private static readonly ConcurrentQueue<string> Events = new();

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnDone(Result* r, void* user) =>
        Completed.Enqueue(new Done(r->Ticket, r->Ok != 0, r->Revision, r->NpcId, r->ReasonCode, r->Reason.ToString(), (nint)user));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnEvent(Str json, void* user)
    {
        var text = json.ToString();
        if (text.Contains("\"routine-test:", StringComparison.Ordinal) || text.Contains("\"other-mod:", StringComparison.Ordinal)) Events.Enqueue(text);
    }

    // out values and the plan in native memory: a lambda run on the tick thread may not take a local's address
    private readonly byte* _mem = (byte*)NativeMemory.AllocZeroed(64 * 1024);
    private ulong* Ticket => (ulong*)_mem;
    private ulong* Handle => (ulong*)(_mem + 8);
    private nuint* Length => (nuint*)(_mem + 16);
    private byte* Json => _mem + 1024;
    private byte* Plan => _mem + 16 * 1024;

    public RoutinesFakeTests()
    {
        Completed.Clear();
        Events.Clear();
    }

    public void Dispose() => NativeMemory.Free(_mem);

    private Str Text(string text)
    {
        var bytes = Encoding.UTF8.GetBytes(text);
        bytes.CopyTo(new Span<byte>(Plan, bytes.Length));
        return new Str { Ptr = Plan, Len = (nuint)bytes.Length };
    }

    private int Assign(KilnHarness kiln, ulong owner, uint npc, string wire, nint user = 1)
    {
        var plan = Text(wire);
        return kiln.Run(() => T->Assign(owner, npc, plan, &OnDone, (void*)user, Ticket));
    }

    private string? Inspect(KilnHarness kiln, uint npc)
    {
        var rc = kiln.Run(() => T->Inspect(npc, Json, 4096, Length));
        return rc == FakeTables.NotFound ? null : rc == FakeTables.Ok ? Encoding.UTF8.GetString(Json, (int)*Length) : throw new InvalidOperationException($"inspect answered {rc}");
    }

    private static JsonElement Parse(string json) => JsonDocument.Parse(json).RootElement.Clone();

    private void Subscribe(KilnHarness kiln) => Assert.Equal(FakeTables.Ok, kiln.Run(() => T->SubscribeEvents(4242, &OnEvent, null, Handle)));

    private void Unsubscribe(KilnHarness kiln)
    {
        var handle = *Handle;
        kiln.Run(() => T->UnsubscribeEvents(handle));
    }

    [Fact]
    public void The_table_is_the_headers_v1()
    {
        Assert.Equal((48, 64), (sizeof(Result), sizeof(Table)));
        Assert.Equal((48, 64), (RoutinesAbi.ResultSize, RoutinesAbi.TableSize));
        Assert.True(T != null);
        Assert.Equal((uint)sizeof(Table), ((uint*)T)[0]);  // header: size, version
        Assert.Equal(1u, ((uint*)T)[1]);
        Assert.Contains(Coverage.All, r => r.Capabilities.Contains(RoutinesAbi.Decisions) && r.Kind == CoverageKind.RecordingFake);
    }

    [Fact]
    public void An_assignment_completes_once_in_the_step_that_commits_it_with_its_event()
    {
        using var kiln = new KilnHarness();
        kiln.Boot();
        kiln.Step();
        Subscribe(kiln);
        try
        {
            const ulong owner = 77;
            Assert.Equal(FakeTables.WrongThread, T->Inspect(7, Json, 4096, Length));        // not the tick thread
            var plan = Text(GoldenWire);
            Assert.Equal(FakeTables.WrongThread, T->Assign(owner, 7, plan, &OnDone, null, Ticket));
            // a plan that is not a sound document makes no ticket
            Assert.Equal(FakeTables.InvalidArgument, Assign(kiln, owner, 7, GoldenWire.Replace("\"priority\":5", "\"priority\":6")));
            Assert.Equal(0ul, *Ticket);
            Assert.Equal(FakeTables.InvalidArgument, Assign(kiln, owner, 0, GoldenWire));  // no villager
            Assert.Equal(FakeTables.Ok, Assign(kiln, owner, 7, GoldenWire, user: 11));
            var ticket = *Ticket;
            Assert.NotEqual(0ul, ticket);
            Assert.Equal(1, kiln.Routines.Queued);
            Assert.Null(Inspect(kiln, 7));                                               // queued, not committed
            Assert.Empty(Completed);
            kiln.Step();
            var done = Assert.Single(Completed);
            Assert.Equal(new Done(ticket, true, 1, 7, RoutinesAbi.CodeOk, "", 11), done);
            var e = Parse(Assert.Single(Events));
            Assert.Equal(("assignment", JsonValueKind.Null, "Assigned", false), (e.GetProperty("type").GetString(), e.GetProperty("before").ValueKind,
                e.GetProperty("reason").GetString(), e.GetProperty("teardown").GetBoolean()));
            var after = e.GetProperty("after");
            Assert.Equal(("routine-test", "routine-test:day", 2u, 1ul, "Assigned", -1), (after.GetProperty("owner").GetString(), after.GetProperty("key").GetString(),
                after.GetProperty("planVersion").GetUInt32(), after.GetProperty("revision").GetUInt64(), after.GetProperty("assignment").GetString(),
                after.GetProperty("windowIndex").GetInt32()));
            var snap = Parse(Inspect(kiln, 7)!);
            Assert.Equal(after.ToString(), snap.ToString());
            // the inspect buffer: too small says how long, and writes nothing
            Assert.Equal(FakeTables.BufferTooSmall, kiln.Run(() => T->Inspect(7, Json, 4, Length)));
            Assert.Equal((nuint)Encoding.UTF8.GetByteCount(snap.GetRawText()), *Length);
            kiln.Step(3);
            Assert.Single(Completed);                                                     // never a second time

            // the owner is bound to its mod: another mod's plan from it, and its mod from another owner, are bad arguments
            Assert.Equal(FakeTables.InvalidArgument, Assign(kiln, owner, 8, GoldenWire.Replace("routine-test:day", "other-mod:day")));
            Assert.Equal(FakeTables.InvalidArgument, Assign(kiln, 78, 8, GoldenWire));
            // another owner of the villager: completes OwnedBy with the mod's name, no revision, no event
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Remove(78, 7, &OnDone, (void*)12, Ticket)));
            kiln.Step();
            Assert.Equal(new Done(*Ticket, false, 1, 7, RoutinesAbi.CodeOwnedBy, "OwnedBy:routine-test", 12), Completed.Last());
            Assert.Single(Events);
            // resume of a villager with no routine
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Resume(owner, 99, &OnDone, (void*)13, Ticket)));
            kiln.Step();
            Assert.Equal(new Done(*Ticket, false, 0, 99, RoutinesAbi.CodeNotAssigned, "NotAssigned", 13), Completed.Last());

            // a decision the test plays: task events in the service's shape with the next step
            kiln.Routines.SetClock(7);
            Assert.Equal("inspect", kiln.Routines.Decide(7));
            Assert.Null(kiln.Routines.Decide(8));                                          // no routine: the game chooses
            kiln.Step();
            var tasks = Events.Skip(1).Select(Parse).ToList();
            Assert.Equal([("None", "Chosen"), ("Chosen", "Started")], tasks.Select(t => (t.GetProperty("from").GetString()!, t.GetProperty("to").GetString()!)));
            Assert.All(tasks, t => Assert.Equal(("task", "inspect", 0, 1ul, 7u), (t.GetProperty("type").GetString(), t.GetProperty("task").GetString(),
                t.GetProperty("windowIndex").GetInt32(), t.GetProperty("invocation").GetUInt64(), t.GetProperty("npcId").GetUInt32())));

            // the owner removes: ok at the next revision, with the event; an already-effective remove: ok, revision 0, no event
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Remove(owner, 7, &OnDone, (void*)14, Ticket)));
            kiln.Step();
            Assert.Equal(new Done(*Ticket, true, 2, 7, RoutinesAbi.CodeOk, "", 14), Completed.Last());
            Assert.Null(Inspect(kiln, 7));
            var count = Events.Count;
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Remove(owner, 7, &OnDone, (void*)15, Ticket)));
            kiln.Step();
            Assert.Equal(new Done(*Ticket, true, 0, 7, RoutinesAbi.CodeOk, "", 15), Completed.Last());
            Assert.Equal(count, Events.Count);
        }
        finally
        {
            Unsubscribe(kiln);
        }
    }

    [Fact]
    public void The_257th_queued_change_answers_buffer_too_small_and_a_switched_off_row_unavailable()
    {
        using var kiln = new KilnHarness();
        kiln.Boot();
        kiln.Step();
        for (uint i = 1; i <= 256; i++) Assert.Equal(FakeTables.Ok, Assign(kiln, 77, i, GoldenWire));
        Assert.Equal(FakeTables.BufferTooSmall, Assign(kiln, 77, 1000, GoldenWire));
        Assert.Equal(0ul, *Ticket);
        Assert.Equal(FakeTables.BufferTooSmall, kiln.Run(() => T->Remove(77, 1, &OnDone, null, Ticket)));
        kiln.Step();
        Assert.Equal(256, Completed.Count);
        Assert.All(Completed, c => Assert.True(c.Ok));
        Assert.Equal(FakeTables.Ok, Assign(kiln, 77, 1000, GoldenWire));               // free again after the step
        kiln.Capabilities.Override(RoutinesAbi.Decisions, false);
        Assert.Equal(FakeTables.Unavailable, Assign(kiln, 77, 1001, GoldenWire));
    }

    [Fact]
    public void A_revoked_owner_loses_its_changes_and_routines_at_the_next_step_and_the_harness_end_tears_the_world_down()
    {
        var kiln = new KilnHarness();
        kiln.Boot();
        kiln.Step();
        Subscribe(kiln);
        try
        {
            Assert.Equal(FakeTables.Ok, Assign(kiln, 77, 7, GoldenWire));
            kiln.Step();
            Assert.Equal(FakeTables.Ok, Assign(kiln, 77, 8, GoldenWire, user: 21));
            var revoked = *Ticket;
            T->RevokeOwner(77);                                                           // any thread
            Assert.Single(Completed);                                                     // at the next step
            // the mod's next owner (a reload) can queue its plans at once; the old owner's revoke does not touch them
            Assert.Equal(FakeTables.Ok, Assign(kiln, 79, 9, GoldenWire, user: 22));
            var reloaded = *Ticket;
            kiln.Step();
            Assert.Null(Inspect(kiln, 7));
            Assert.Contains(new Done(revoked, false, 0, 8, RoutinesAbi.CodeRevoked, "Revoked", 21), Completed);
            Assert.Contains(new Done(reloaded, true, 3, 9, RoutinesAbi.CodeOk, "", 22), Completed);
            Assert.Contains(Events.Select(Parse), e => e.GetProperty("reason").ValueKind == JsonValueKind.String && e.GetProperty("reason").GetString() == "OwnerUnloaded");

            // the end of the harness stops the world: the stop's own step still commits what was queued (as on a server; a
            // change queued after that step completes WorldStopped: RoutinesModelTests), then each assignment says goodbye
            Assert.Equal(FakeTables.Ok, Assign(kiln, 79, 10, GoldenWire, user: 23));
            var queued = *Ticket;
            kiln.Dispose();
            Assert.Contains(new Done(queued, true, 4, 10, RoutinesAbi.CodeOk, "", 23), Completed);
            var teardown = Events.Select(Parse).Where(e => e.GetProperty("type").GetString() == "assignment" && e.GetProperty("teardown").GetBoolean()).ToList();
            Assert.Equal([9u, 10u], teardown.Select(e => e.GetProperty("after").GetProperty("npcId").GetUInt32()).Order());
            Assert.All(teardown, e => Assert.Equal(("WorldStopped", "PendingSpawn"), (e.GetProperty("reason").GetString(),
                e.GetProperty("after").GetProperty("assignment").GetString())));
        }
        finally
        {
            // the subscription is the process's: take it back on a tick thread
            kiln.Dispose();
            var handle = *Handle;
            using var again = new KilnHarness();
            again.Boot();
            Assert.Equal(FakeTables.Ok, again.Run(() => T->UnsubscribeEvents(handle)));
        }
    }
}
