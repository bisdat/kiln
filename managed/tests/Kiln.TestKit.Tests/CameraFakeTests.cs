// SPDX-License-Identifier: MIT
// The kit's fake kiln.camera (Fakes/Camera/) on its raw table, without Kiln.Camera: the lease rules it answers by must
// stay the native service's (native/services/camera/tests/state_test.cpp).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed unsafe class CameraFakeTests
{
    private static CameraAbi.Table* T => (CameraAbi.Table*)KitBackend.Instance.Service(CameraAbi.Service, 1);

    // what the callbacks saw, tagged by the test's user pointer (the fake's subscribers are process-wide)
    private static readonly object Seen = new();
    private static readonly List<(nint Tag, string What)> Log = [];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnDone(CameraAbi.Result* r, void* user)
    {
        lock (Seen) Log.Add(((nint)user, $"done {r->Lease} ok={r->Ok} reason={r->ReasonCode} flags={r->Flags} rev={r->Revision}"));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnChange(CameraAbi.Change* c, void* user)
    {
        lock (Seen) Log.Add(((nint)user, $"change {c->Lease} kind={c->Kind} end={c->EndReason} rev={c->Revision}"));
    }

    private static string[] Take(nint tag)
    {
        lock (Seen)
        {
            var mine = Log.Where(e => e.Tag == tag).Select(e => e.What).ToArray();
            Log.RemoveAll(e => e.Tag == tag);
            return mine;
        }
    }

    private static CameraAbi.Request Mode(uint player, int priority) => new()
    {
        Size = (uint)sizeof(CameraAbi.Request), Parts = CameraAbi.PartMode, Endpoint = CameraAbi.ServerMode, Player = player,
        InputPolicy = 1, DurationMs = 5000, Priority = priority, CameraId = 34,
    };

    [Fact]
    public void TheTableIsTheHeadersV1()
    {
        Assert.Equal(128, sizeof(CameraAbi.Request));
        Assert.Equal(72, sizeof(CameraAbi.Result));
        Assert.Equal(72, sizeof(CameraAbi.Change));
        Assert.Equal(88, sizeof(CameraAbi.Table));
        Assert.True(T != null);
        Assert.Equal((uint)sizeof(CameraAbi.Table), ((uint*)T)[0]);  // header: size, version
        Assert.Equal(1u, ((uint*)T)[1]);
        Assert.Contains(Coverage.All, r => r.Capabilities.Contains("kiln.camera") && r.Kind == CoverageKind.RecordingFake);
    }

    [Fact]
    public void LeasesAnswerTheStepAfterTheCallInTheServicesOrder()
    {
        using var kiln = new KilnHarness();
        kiln.Boot();
        kiln.Step(2);
        nint tag = 0x3c01;
        // records the fake reads in native memory: a lambda run on the tick thread may not take a local's address
        var mem = (byte*)NativeMemory.AllocZeroed(1024);
        var req = (CameraAbi.Request*)mem;
        var lease = (ulong*)(mem + 256);
        var handle = (ulong*)(mem + 320);
        var mask = (ulong*)(mem + 384);
        try
        {
            Assert.Equal(FakeTables.Ok, T->Subscribe(0, &OnChange, (void*)tag, handle));
            *req = Mode(7, 10);
            Assert.Equal(FakeTables.WrongThread, T->Apply(1, req, lease, &OnDone, (void*)tag));  // not the tick thread
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Apply(1, req, lease, &OnDone, (void*)tag)));
            var first = *lease;
            Assert.NotEqual(0ul, first);
            Assert.Empty(Take(tag));  // nothing before the next step
            kiln.Step();
            Assert.Equal([$"done {first} ok=1 reason=0 flags=0 rev=1", $"change {first} kind=1 end=0 rev=1"], Take(tag));

            // the same owner again: AlreadyOwned; another owner at the same priority: Busy (each still gets a lease id)
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Apply(1, req, lease, &OnDone, (void*)tag)));
            var again = *lease;
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Apply(2, req, lease, &OnDone, (void*)tag)));
            var busy = *lease;
            kiln.Step();
            Assert.Equal([$"done {again} ok=0 reason={CameraAbi.AlreadyOwned} flags=0 rev=0", $"done {busy} ok=0 reason={CameraAbi.Busy} flags=0 rev=0"], Take(tag));

            // a higher priority wins: the old lease ends superseded before the new one is applied
            *req = Mode(7, 11);
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Apply(2, req, lease, &OnDone, (void*)tag)));
            var second = *lease;
            kiln.Step();
            Assert.Equal([$"change {first} kind=3 end={CameraAbi.EndSuperseded} rev=1", $"done {second} ok=1 reason=0 flags=0 rev=1",
                $"change {second} kind=1 end=0 rev=1"], Take(tag));
            Assert.Equal([second], kiln.Camera.Leases);

            // update: a foreign owner is refused; the owner's update raises the revision; while it waits, the slot is busy
            *req = Mode(7, 50);
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Update(1, second, 0, req, &OnDone, (void*)tag)));
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Update(2, second, 0, req, &OnDone, (void*)tag)));
            *req = Mode(7, 60);
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Apply(3, req, lease, &OnDone, (void*)tag)));
            var waiting = *lease;
            kiln.Step();
            Assert.Equal([$"done {second} ok=0 reason={CameraAbi.ForeignOwner} flags=0 rev=0", $"done {second} ok=1 reason=0 flags=0 rev=2",
                $"change {second} kind=2 end=0 rev=2", $"done {waiting} ok=0 reason={CameraAbi.Busy} flags=0 rev=0"], Take(tag));

            // the updated priority (50) holds from the answer on
            *req = Mode(7, 50);
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Apply(3, req, lease, &OnDone, (void*)tag)));
            var low = *lease;
            *req = Mode(7, 51);
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Apply(3, req, lease, &OnDone, (void*)tag)));
            var third = *lease;
            kiln.Step();
            Assert.Equal([$"done {low} ok=0 reason={CameraAbi.Busy} flags=0 rev=0", $"change {second} kind=3 end={CameraAbi.EndSuperseded} rev=2",
                $"done {third} ok=1 reason=0 flags=0 rev=1", $"change {third} kind=1 end=0 rev=1"], Take(tag));

            // end is idempotent for its owner, refused for another, unknown for a number never given
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->End(3, third, &OnDone, (void*)tag)));
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->End(3, third, &OnDone, (void*)tag)));
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->End(4, third, &OnDone, (void*)tag)));
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->End(3, 999_999, &OnDone, (void*)tag)));
            kiln.Step();
            Assert.Equal([$"done {third} ok=1 reason=0 flags=0 rev=1", $"change {third} kind=3 end={CameraAbi.EndReleased} rev=1",
                $"done {third} ok=1 reason=0 flags={CameraAbi.FlagAlreadyEnded} rev=0", $"done {third} ok=0 reason={CameraAbi.ForeignOwner} flags=0 rev=0",
                $"done 999999 ok=0 reason={CameraAbi.UnknownLease} flags=0 rev=0"], Take(tag));
            Assert.Empty(kiln.Camera.Leases);

            // a lease the test lets run out ends expired at the next step
            *req = Mode(8, 1);
            Assert.Equal(FakeTables.Ok, kiln.Run(() => T->Apply(5, req, lease, &OnDone, (void*)tag)));
            var fourth = *lease;
            kiln.Step();
            Take(tag);
            Assert.True(kiln.Camera.Expire(fourth));
            Assert.False(kiln.Camera.Expire(fourth));
            kiln.Step();
            Assert.Equal([$"change {fourth} kind=3 end={CameraAbi.EndExpired} rev=1"], Take(tag));
            Assert.Equal("AAAAUUAAAEEEEA", string.Concat(kiln.Camera.Calls.Select(c => c.Kind.ToString()[0])));
            Assert.Equal([0u, CameraAbi.AlreadyOwned, CameraAbi.Busy, 0u, CameraAbi.ForeignOwner, 0u, CameraAbi.Busy, CameraAbi.Busy, 0u],
                kiln.Camera.Calls.Take(9).Select(c => c.Refusal));

            // capabilities follow the rows; a row switched off turns the request away at once
            Assert.Equal(FakeTables.Ok, T->Capabilities(CameraAbi.ServerMode, 7, mask));
            Assert.Equal(1ul, *mask);
            Assert.Equal(FakeTables.Ok, T->Capabilities(CameraAbi.Local, 0, mask));
            Assert.Equal(2ul | 4 | 8 | 16, *mask);
            kiln.Capabilities.Override(CameraFake.ModeRow, false, "switched off by the test");
            Assert.Equal(FakeTables.Ok, T->Capabilities(CameraAbi.ServerMode, 7, mask));
            Assert.Equal(0ul, *mask);
            var calls = kiln.Camera.Calls.Count;
            Assert.Equal(FakeTables.Unavailable, kiln.Run(() => T->Apply(6, req, lease, &OnDone, (void*)tag)));
            Assert.Equal(calls, kiln.Camera.Calls.Count);
            kiln.Capabilities.ClearOverride(CameraFake.ModeRow);
            Assert.Equal(FakeTables.Unavailable, T->GetVisual(mem + 512, (uint*)(mem + 640)));  // not modelled
            Assert.Equal(FakeTables.Ok, T->Unsubscribe(*handle));
            Assert.Equal(FakeTables.NotFound, T->Unsubscribe(*handle));
        }
        finally
        {
            T->Unsubscribe(*handle);
            NativeMemory.Free(mem);
        }
    }
}
