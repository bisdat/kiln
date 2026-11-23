// SPDX-License-Identifier: MIT
// Kiln.Camera's own code on the test kit's kiln.camera: what a request must look like, which row each part asks for,
// how the service's answers come back (a task, never inside the call), and what Camera.Changed says and when.
using System.Numerics;
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace CameraTests;

public sealed class CameraProbeMod : Mod
{
    public override void OnLoad() { }
}

public sealed class CameraTests
{
    private static readonly EntityId Player = new(5);
    private static readonly TimeSpan Minute = TimeSpan.FromSeconds(60);

    // A harness whose server world runs, with a mod loaded: camera leases belong to a mod (owner 0 is refused).
    private static KilnHarness Started(out ModHandle mod)
    {
        var kiln = new KilnHarness();
        mod = kiln.Load<CameraProbeMod>(new ModManifest("camera-probe"));
        kiln.Step(2);
        return kiln;
    }

    // The entries throw before they return a task: the call is run for the exception, not awaited.
    private static void Sync(KilnHarness kiln, ModHandle mod, Func<Task<CameraResult>> call) => _ = kiln.As(mod, call);

    private static CameraOverride Lens(float degrees = 40) => new(Lens: new CameraLens(degrees));

    private static CameraPose At(double x) => new(new WorldPosition(x, 10, 0), Quaternion.Identity);

    // Runs `call` as code of a mod context the host would make (owner 77), for a harness that has loaded no mod yet.
    private static T AsModContext<T>(Func<T> call)
    {
        var outer = HostAccess.Current;
        HostAccess.Current = HostAccess.CreateContext(new ModInfo("camera-context", "camera-context", "1.0.0", "no-such-folder"), 77, "no-such-folder");
        try
        {
            return call();
        }
        finally
        {
            HostAccess.Current = outer;
        }
    }

    [Fact]
    public void Writes_outside_mod_code_are_refused_before_the_service_is_asked()
    {
        using var kiln = Started(out var mod);
        var held = kiln.As(mod, () => Camera.SetMode(Player, CameraMode.PlayerCameraBuildingFirstPerson, Minute));
        kiln.Step();
        var lease = held.Result.Lease;
        var calls = kiln.Camera.Calls.Count;
        var mode = new CameraOverride(Mode: new CameraModeRequest(CameraMode.PlayerCameraSitting), InputPolicy: CameraInputPolicy.StockMode);
        foreach (var call in new Func<Task<CameraResult>>[]
                 {
                     () => Camera.SetMode(Player, CameraMode.PlayerCameraSitting, Minute),
                     () => Camera.Set(Player, mode),
                     () => Camera.SetLocal(Lens()),
                     () => Camera.SetLocal(mode),
                     () => Camera.Update(lease, mode),
                     () => Camera.End(lease),
                 })
        {
            var e = Assert.Throws<InvalidOperationException>(() => Sync0(kiln, call));
            Assert.Contains("mod code", e.Message);
        }
        Assert.Equal(calls, kiln.Camera.Calls.Count);   // nothing reached the service
        var end = kiln.As(mod, () => Camera.End(lease));
        kiln.Step();
        Assert.Equal((true, false), (end.Result.Ok, end.Result.AlreadyEnded));   // the mod's own lease was untouched
    }

    // As Kiln itself (no mod): owner 0.
    private static void Sync0(KilnHarness kiln, Func<Task<CameraResult>> call) => _ = kiln.Run(call);

    [Fact]
    public void A_mode_lock_is_answered_a_step_later_and_Changed_follows_the_answer()
    {
        using var kiln = Started(out var mod);
        var seen = new List<(CameraChange Change, bool TaskDone)>();
        Task<CameraResult>? task = null;
        Action<CameraChange> handler = c => seen.Add((c, task!.IsCompleted));
        kiln.Run(() => Camera.Changed += handler);
        try
        {
            task = kiln.As(mod, () => Camera.SetMode(Player, CameraMode.PlayerCameraBuildingFirstPerson, Minute, CameraTransitionOverride.CameraTransitionLerpSpeed, priority: 7));
            Assert.False(task.IsCompleted);   // accepted is not applied: never inside the call
            Assert.Empty(seen);
            var call = Assert.Single(kiln.Camera.Calls);
            Assert.Equal((FakeCameraCallKind.Apply, 1u, Player, 1u, 60000u, 7, 0u), (call.Kind, call.Endpoint, call.Player, call.Parts, call.DurationMs, call.Priority, call.Refusal));
            kiln.Step();
            Assert.True(task.IsCompleted);
            var result = task.Result;
            Assert.True(result.Ok);
            Assert.Null(result.Reason);
            Assert.Equal((CameraReason.None, 1ul, CameraCommitScope.ServerState, false, false), (result.ReasonCode, result.Revision, result.CommitScope, result.MayHaveApplied, result.AlreadyEnded));
            Assert.Equal(new CameraLease(call.Lease, CameraEndpoint.ServerMode, Player, 1), result.Lease);
            var (change, done) = Assert.Single(seen);
            Assert.True(done);   // the request's own answer comes before its event
            Assert.Equal((result.Lease, 1ul, CameraChangeKind.Applied, CameraEndReason.None), (change.Lease, change.Revision, change.Kind, change.EndReason));
            kiln.Step(3);
            Assert.Single(seen);   // once
        }
        finally
        {
            kiln.Run(() => Camera.Changed -= handler);
        }
    }

    [Fact]
    public void Update_and_end_follow_the_lease_and_a_second_end_says_already_ended()
    {
        using var kiln = Started(out var mod);
        var seen = new List<CameraChange>();
        Action<CameraChange> handler = seen.Add;
        kiln.Run(() => Camera.Changed += handler);
        try
        {
            var applied = kiln.As(mod, () => Camera.SetMode(Player, CameraMode.PlayerCameraBuildingFirstPerson, Minute));
            kiln.Step();
            var lease = applied.Result.Lease;
            var updated = kiln.As(mod, () => Camera.Update(lease, new CameraOverride(Mode: new CameraModeRequest(CameraMode.PlayerCameraSitting), Duration: Minute, InputPolicy: CameraInputPolicy.StockMode),
                CameraUpdateTiming.Restart));
            kiln.Step();
            Assert.Equal((true, 2ul, CameraCommitScope.ServerState), (updated.Result.Ok, updated.Result.Revision, updated.Result.CommitScope));
            // a mode lease stays a mode lease; a lease of another player is another endpoint
            var visual = kiln.As(mod, () => Camera.Update(lease with { Endpoint = CameraEndpoint.ServerMode }, Lens()));
            var other = kiln.As(mod, () => Camera.Update(lease with { Player = new EntityId(6) },
                new CameraOverride(Mode: new CameraModeRequest(CameraMode.PlayerCameraSitting), InputPolicy: CameraInputPolicy.StockMode)));
            var unknown = kiln.As(mod, () => Camera.End(lease with { Value = 987654 }));
            kiln.Step();
            Assert.Equal((false, CameraReason.LeaseKind), (visual.Result.Ok, visual.Result.ReasonCode));
            Assert.Equal((false, CameraReason.WrongEndpoint), (other.Result.Ok, other.Result.ReasonCode));
            Assert.Equal((false, CameraReason.UnknownLease), (unknown.Result.Ok, unknown.Result.ReasonCode));
            Assert.NotNull(unknown.Result.Reason);   // a refusal always has words
            var ended = kiln.As(mod, () => Camera.End(lease));
            Assert.False(ended.IsCompleted);
            kiln.Step();
            Assert.Equal((true, false), (ended.Result.Ok, ended.Result.AlreadyEnded));
            var again = kiln.As(mod, () => Camera.End(lease));
            kiln.Step();
            Assert.Equal((true, true, CameraReason.None), (again.Result.Ok, again.Result.AlreadyEnded, again.Result.ReasonCode));
            Assert.Equal([CameraChangeKind.Applied, CameraChangeKind.Updated, CameraChangeKind.Ended], seen.Select(c => c.Kind));
            Assert.Equal((CameraEndReason.Released, 2ul), (seen[2].EndReason, seen[2].Revision));   // one Ended, however often it is ended
            Assert.Empty(kiln.Camera.Leases);
        }
        finally
        {
            kiln.Run(() => Camera.Changed -= handler);
        }
    }

    [Fact]
    public void One_owner_updates_another_owner_needs_a_higher_priority_and_the_old_lease_ends_first()
    {
        using var kiln = Started(out var mod);
        var other = kiln.Load<CameraProbeMod>(new ModManifest("camera-probe-2"));
        kiln.Step();
        var seen = new List<CameraChange>();
        Action<CameraChange> handler = seen.Add;
        kiln.Run(() => Camera.Changed += handler);
        try
        {
            var first = kiln.As(mod, () => Camera.SetMode(Player, CameraMode.PlayerCameraBuildingFirstPerson, Minute, priority: 10));
            kiln.Step();
            var same = kiln.As(mod, () => Camera.SetMode(Player, CameraMode.PlayerCameraSitting, Minute, priority: 900));          // the same mod again
            var equal = kiln.As(other, () => Camera.SetMode(Player, CameraMode.PlayerCameraSitting, Minute, priority: 10));      // another mod, not higher
            kiln.Step();
            Assert.Equal((false, CameraReason.AlreadyOwned), (same.Result.Ok, same.Result.ReasonCode));
            Assert.Equal((false, CameraReason.Busy), (equal.Result.Ok, equal.Result.ReasonCode));
            Assert.Single(seen);
            var foreign = kiln.As(other, () => Camera.End(first.Result.Lease));
            var higher = kiln.As(other, () => Camera.SetMode(Player, CameraMode.PlayerCameraSitting, Minute, priority: 11));
            kiln.Step();
            Assert.Equal((false, CameraReason.ForeignOwner), (foreign.Result.Ok, foreign.Result.ReasonCode));
            Assert.True(higher.Result.Ok);
            Assert.Equal([(CameraChangeKind.Applied, first.Result.Lease.Value), (CameraChangeKind.Ended, first.Result.Lease.Value), (CameraChangeKind.Applied, higher.Result.Lease.Value)],
                seen.Select(c => (c.Kind, c.Lease.Value)));
            Assert.Equal(CameraEndReason.Superseded, seen[1].EndReason);
            // the old lease's owner can still "end" it: it has ended, and the successor is not touched
            var late = kiln.As(mod, () => Camera.End(first.Result.Lease));
            kiln.Step();
            Assert.Equal((true, true), (late.Result.Ok, late.Result.AlreadyEnded));
            Assert.Equal([higher.Result.Lease.Value], kiln.Camera.Leases);
            // a lease that runs out says so
            Assert.True(kiln.Camera.Expire(higher.Result.Lease.Value));
            kiln.Step();
            Assert.Equal((CameraChangeKind.Ended, CameraEndReason.Expired), (seen[^1].Kind, seen[^1].EndReason));
        }
        finally
        {
            kiln.Run(() => Camera.Changed -= handler);
        }
    }

    [Fact]
    public void Each_part_asks_for_its_own_row()
    {
        using var kiln = Started(out var mod);
        // a lens needs no pose row
        kiln.Capabilities.Override("camera.local_pose", false, "switched off by the test");
        kiln.Capabilities.Override("camera.local_profile", false, "switched off by the test");
        var lens = kiln.As(mod, () => Camera.SetLocal(Lens()));
        kiln.Step();
        Assert.Equal((true, CameraCommitScope.ClientFrame, CameraEndpoint.Local), (lens.Result.Ok, lens.Result.CommitScope, lens.Result.Lease.Endpoint));
        var call = kiln.Camera.Calls[^1];
        Assert.Equal((2u, 16u, 10000u), (call.Endpoint, call.Parts, call.DurationMs));   // the local endpoint, the lens part alone, 10 s by default
        var path = new CameraOverride(Path: new CameraPath([new CameraKey(TimeSpan.Zero, At(1)), new CameraKey(TimeSpan.FromSeconds(1), At(2))]));
        Assert.Equal("camera.local_pose", Assert.Throws<KilnUnavailableException>(() => Sync(kiln, mod, () => Camera.SetLocal(path))).Capability);
        var follow = new CameraOverride(Profile: new CameraProfile(EntityId.None, 8, Vector3.Zero), Lens: new CameraLens(70));
        Assert.Equal("camera.local_profile", Assert.Throws<KilnUnavailableException>(() => Sync(kiln, mod, () => Camera.SetLocal(follow))).Capability);
        Assert.Equal(CameraCapability.LocalFov | CameraCapability.LocalShake, kiln.Run(() => Camera.GetCapabilities(CameraEndpoint.Local)));
        kiln.Capabilities.ClearOverride("camera.local_pose");
        kiln.Capabilities.ClearOverride("camera.local_profile");
        Assert.Equal(CameraCapability.LocalPose | CameraCapability.LocalFov | CameraCapability.LocalProfile | CameraCapability.LocalShake,
            kiln.Run(() => Camera.GetCapabilities(CameraEndpoint.Local)));
        Assert.Equal(CameraCapability.ServerMode, kiln.Run(() => Camera.GetCapabilities(CameraEndpoint.ServerMode)));
        Assert.Throws<ArgumentException>(() => kiln.Run(() => Camera.GetCapabilities((CameraEndpoint)9)));

        kiln.Capabilities.Override("camera.local_fov", false, "switched off by the test");
        Assert.Equal("camera.local_fov", Assert.Throws<KilnUnavailableException>(() => Sync(kiln, mod, () => Camera.SetLocal(Lens()))).Capability);
        kiln.Capabilities.Override("camera.server_mode", false, "switched off by the test");
        Assert.Equal("camera.server_mode",
            Assert.Throws<KilnUnavailableException>(() => Sync(kiln, mod, () => Camera.SetMode(Player, CameraMode.PlayerCameraSitting, Minute))).Capability);
        // a server's visual request for a player's client needs the transport: the row is off in the kit, and named
        Assert.Equal("camera.remote", Assert.Throws<KilnUnavailableException>(() => Sync(kiln, mod, () => Camera.Set(Player, Lens()))).Capability);
        Assert.Equal("camera.rendered_snapshot", Assert.Throws<KilnUnavailableException>(() => kiln.Run(Camera.GetVisual)).Capability);
        // an update asks only for camera.service itself; the service's "unavailable" is named by the part row that is off
        Assert.Equal("camera.local_fov", Assert.Throws<KilnUnavailableException>(() => Sync(kiln, mod, () => Camera.Update(lens.Result.Lease, Lens(50)))).Capability);
        // ending still works while a row is off
        var end = kiln.As(mod, () => Camera.End(lens.Result.Lease));
        kiln.Step();
        Assert.True(end.Result.Ok);
    }

    [Fact]
    public void The_aggregate_rows_follow_the_part_rows()
    {
        using var kiln = Started(out var mod);
        kiln.Run(() => Camera.GetCapabilities(CameraEndpoint.Local));   // Kiln.Camera has started: its rows are reported
        bool Is(string row) => kiln.Run(() => Capabilities.Is(row));
        Assert.True(Is("camera.service"));
        Assert.True(Is("camera.local"), kiln.Run(() => Capabilities.Why("camera.local")));
        Assert.True(Is("camera.server"), kiln.Run(() => Capabilities.Why("camera.server")));
        Assert.False(Is("camera.remote"));
        Assert.False(Is("camera.resource_profile"));
        // the local aggregate goes with the last local part; the server aggregate still has the mode lock
        foreach (var row in new[] { "camera.local_pose", "camera.local_fov", "camera.local_profile", "camera.local_shake" })
            kiln.Capabilities.Override(row, false, "switched off by the test");
        kiln.Step(25);   // the rows are reported again every 25 steps
        Assert.False(Is("camera.local"));
        Assert.True(Is("camera.server"));
        kiln.Capabilities.Override("camera.server_mode", false, "switched off by the test");
        kiln.Step(25);
        Assert.False(Is("camera.server"));
    }

    [Fact]
    public void A_mode_lock_is_not_a_local_request_and_the_reserved_policy_is_refused()
    {
        using var kiln = Started(out var mod);
        var mode = new CameraOverride(Mode: new CameraModeRequest(CameraMode.PlayerCameraSitting), InputPolicy: CameraInputPolicy.StockMode);
        var local = kiln.As(mod, () => Camera.SetLocal(mode));
        Assert.True(local.IsCompleted);   // nothing was asked of the service
        Assert.Equal((false, CameraReason.WrongEndpoint, 0ul), (local.Result.Ok, local.Result.ReasonCode, local.Result.Lease.Value));
        Assert.NotNull(local.Result.Reason);
        Assert.Empty(kiln.Camera.Calls);
        var aim = kiln.As(mod, () => Camera.SetLocal(Lens() with { InputPolicy = CameraInputPolicy.AimToo }));
        kiln.Step();
        Assert.Equal((false, CameraReason.InputPolicyUnsupported), (aim.Result.Ok, aim.Result.ReasonCode));
        Assert.Empty(kiln.Camera.Leases);
        // the server's entry takes a mode lock as SetMode does
        var set = kiln.As(mod, () => Camera.Set(Player, mode));
        kiln.Step();
        Assert.Equal((true, CameraEndpoint.ServerMode), (set.Result.Ok, set.Result.Lease.Endpoint));
    }

    [Fact]
    public void Without_a_world_the_answer_is_at_once_and_not_a_wait()
    {
        using var kiln = new KilnHarness();   // no step yet: no world runs, and no mod is loaded (mods load at the first step)
        var (mode, local) = kiln.Run(() => AsModContext(() =>
            (Camera.SetMode(Player, CameraMode.PlayerCameraSitting, Minute), Camera.SetLocal(Lens()))));
        Assert.True(mode.IsCompleted);
        Assert.Equal((false, CameraReason.PeerUnavailable), (mode.Result.Ok, mode.Result.ReasonCode));
        Assert.True(local.IsCompleted);
        Assert.Equal((false, CameraReason.NoCameraFrame), (local.Result.Ok, local.Result.ReasonCode));
    }

    [Fact]
    public void A_mod_that_unloads_has_its_waiting_request_cancelled()
    {
        using var kiln = Started(out var stays);
        var mod = kiln.Load<CameraProbeMod>(new ModManifest("camera-probe-2"));
        kiln.Step();
        var waiting = kiln.As(mod, () => Camera.SetMode(Player, CameraMode.PlayerCameraSitting, Minute));
        var calls = 0;
        kiln.As(mod, () => Camera.Changed += _ => calls++);
        kiln.Unload(mod, checkCollected: false);
        Assert.True(waiting.IsCanceled);
        kiln.Step(2);   // the service's late answer finds nobody, and the mod's handler is gone
        Assert.Equal(0, calls);
        Assert.Throws<ArgumentException>(() => Sync(kiln, stays, () => Camera.End(default)));
    }

    [Fact]
    public void A_handler_that_throws_does_not_stop_the_others()
    {
        using var kiln = Started(out var mod);
        var calls = 0;
        Action<CameraChange> bad = _ => throw new InvalidOperationException("the test's handler throws");
        Action<CameraChange> good = _ => calls++;
        kiln.Run(() => { Camera.Changed += bad; Camera.Changed += good; });
        try
        {
            var task = kiln.As(mod, () => Camera.SetMode(Player, CameraMode.PlayerCameraSitting, Minute));
            kiln.Step();
            Assert.True(task.Result.Ok);
            Assert.Equal(1, calls);
        }
        finally
        {
            kiln.Run(() => { Camera.Changed -= bad; Camera.Changed -= good; });
        }
    }
}
