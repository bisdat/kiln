// SPDX-License-Identifier: MIT
// The test kit's kiln.camera as an author's test sees it (KilnHarness.Camera): what a mod that unloads, a stopping
// world, a lease that ends by itself and a refusal of the game look like through Camera's own tasks and Changed.
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace CameraTests;

public sealed class FakeCameraTests
{
    private static readonly EntityId Player = new(5), Other = new(6);
    private static readonly TimeSpan Minute = TimeSpan.FromSeconds(60);

    private static KilnHarness Started(out ModHandle watcher, out ModHandle mod, List<CameraChange> seen)
    {
        var kiln = new KilnHarness();
        watcher = kiln.Load<CameraProbeMod>(new ModManifest("camera-probe"));
        mod = kiln.Load<CameraProbeMod>(new ModManifest("camera-probe-2"));
        kiln.Step(2);
        kiln.As(watcher, () => Camera.Changed += seen.Add);
        return kiln;
    }

    private static CameraLease Held(KilnHarness kiln, ModHandle mod, EntityId player)
    {
        var task = kiln.As(mod, () => Camera.SetMode(player, CameraMode.PlayerCameraSitting, Minute));
        kiln.Step();
        Assert.True(task.Result.Ok);
        return task.Result.Lease;
    }

    [Fact]
    public void A_mod_that_unloads_has_its_applied_lease_end_OwnerRetired_for_the_others()
    {
        var seen = new List<CameraChange>();
        using var kiln = Started(out _, out var mod, seen);
        var lease = Held(kiln, mod, Player);
        var waiting = kiln.As(mod, () => Camera.SetMode(Other, CameraMode.PlayerCameraSitting, Minute));
        kiln.Unload(mod, checkCollected: false);
        Assert.True(waiting.IsCanceled);
        kiln.Step();
        Assert.Equal([(CameraChangeKind.Applied, CameraEndReason.None), (CameraChangeKind.Ended, CameraEndReason.OwnerRetired)],
            seen.Select(c => (c.Kind, c.EndReason)));
        Assert.Equal(lease.Value, seen[^1].Lease.Value);   // the waiting lease never showed: no event of it
        Assert.Empty(kiln.Camera.Leases);
    }

    [Fact]
    public void A_stopping_world_ends_applied_leases_and_refuses_waiting_requests()
    {
        var seen = new List<CameraChange>();
        using var kiln = Started(out _, out var mod, seen);
        var lease = Held(kiln, mod, Player);
        var waiting = kiln.As(mod, () => Camera.SetMode(Other, CameraMode.PlayerCameraSitting, Minute));
        Assert.Equal(2, kiln.Camera.StopWorld());
        kiln.Step();
        Assert.Equal((false, CameraReason.StaleWorld), (waiting.Result.Ok, waiting.Result.ReasonCode));
        Assert.Equal((CameraChangeKind.Ended, CameraEndReason.WorldStopped, lease.Value), (seen[^1].Kind, seen[^1].EndReason, seen[^1].Lease.Value));
        Assert.Equal(2, seen.Count);   // one Applied, one Ended: nothing of the waiting request
        var late = kiln.As(mod, () => Camera.End(lease));
        kiln.Step();
        Assert.Equal((true, true), (late.Result.Ok, late.Result.AlreadyEnded));
    }

    [Fact]
    public void A_lease_ends_by_itself_with_the_reason_the_test_gives()
    {
        var seen = new List<CameraChange>();
        using var kiln = Started(out _, out var mod, seen);
        var lease = Held(kiln, mod, Player);
        Assert.Throws<ArgumentOutOfRangeException>(() => kiln.Camera.End(lease.Value, 1));   // released is the owner's End
        Assert.True(kiln.Camera.End(lease.Value, 4));
        Assert.False(kiln.Camera.End(lease.Value, 4));
        kiln.Step();
        Assert.Equal((CameraChangeKind.Ended, CameraEndReason.PlayerLeft), (seen[^1].Kind, seen[^1].EndReason));
        Assert.Empty(kiln.Camera.Leases);
    }

    [Fact]
    public void RefuseNext_answers_the_next_accepted_request_with_that_reason()
    {
        var seen = new List<CameraChange>();
        using var kiln = Started(out _, out var mod, seen);
        Assert.Throws<ArgumentOutOfRangeException>(() => kiln.Camera.RefuseNext(0));
        kiln.Camera.RefuseNext((uint)CameraReason.FrameFault);
        var refused = kiln.As(mod, () => Camera.SetMode(Player, CameraMode.PlayerCameraSitting, Minute));
        kiln.Step();
        Assert.Equal((false, CameraReason.FrameFault), (refused.Result.Ok, refused.Result.ReasonCode));
        Assert.Empty(seen);
        Assert.Empty(kiln.Camera.Leases);
        var lease = Held(kiln, mod, Player);   // the next one goes through
        kiln.Camera.RefuseNext((uint)CameraReason.PeerUnavailable);
        var update = kiln.As(mod, () => Camera.Update(lease, new CameraOverride(Mode: new CameraModeRequest(CameraMode.PlayerCameraBuildingFirstPerson),
            Duration: Minute, InputPolicy: CameraInputPolicy.StockMode)));
        kiln.Step();
        Assert.Equal((false, CameraReason.PeerUnavailable, 1ul), (update.Result.Ok, update.Result.ReasonCode, update.Result.Revision));
        Assert.Equal([lease.Value], kiln.Camera.Leases);   // a refused update keeps the lease
    }
}
