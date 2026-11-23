// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;
using Kiln;
using Kiln.CameraTransport;
using Kiln.Testing;
namespace CameraTest;
public sealed class CameraTestMod : Mod
{
    [DllImport("camera-fixture.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern int camera_fixture_result(uint index);

    // S8: lab transport peers (camnet.py) stand in for players: a lab peer has no game player, this mod names one by the
    // peer's nettest name. A = a Kiln client with every camera part, N = one without camera.local_fov, V = a Kiln client
    // without the channel kiln.camera, S = one with the channel that never says Hello.
    private static readonly Dictionary<string, uint> LabPlayers = new() { ["camtest-A"] = 910001, ["camtest-N"] = 910002, ["camtest-V"] = 910003, ["camtest-S"] = 910004 };
    private static readonly EntityId A = new(910001), N = new(910002), V = new(910003), S = new(910004);
    private static readonly List<CameraChange> Changes = [];

    public override void OnLoad()
    {
        CameraBridgeStartup.LabPlayerOf = static p => p.Properties.TryGetValue("nettest.client", out var n) && LabPlayers.TryGetValue(n, out var e) ? e : 0;
        Camera.Changed += c => { lock (Changes) Changes.Add(c); };
    }

    private static async Task Check(TestContext c, uint index, string name)
    {
        await c.Until(() => camera_fixture_result(index) != 0, TimeSpan.FromSeconds(90), name);
        Assert.Equal(1, camera_fixture_result(index), "native fixture " + name + "; see camera-fixture log");
        c.Log.Info("camera-test PASS " + name);
    }
    [KilnTest("maintained20", Order = 0, TimeoutSeconds = 120)] public Task Maintained(TestContext c) => Check(c, 0, "maintained20");
    [KilnTest("isolated_other_entity", Order = 1)] public Task Isolation(TestContext c) => Check(c, 1, "isolated_other_entity");
    [KilnTest("end_cancels_queued", Order = 2)] public Task Cancel(TestContext c) => Check(c, 2, "end_cancels_queued");
    [KilnTest("no_late_revision", Order = 3)] public Task Revision(TestContext c) => Check(c, 3, "no_late_revision");
    [KilnTest("events_once", Order = 4)] public Task Events(TestContext c) => Check(c, 4, "events_once");
    [KilnTest("pending_cancel", Order = 5)] public Task Pending(TestContext c) => Check(c, 5, "pending_cancel");
    [KilnTest("owner_epoch_incarnation", Order = 6)] public Task Epoch(TestContext c) => Check(c, 6, "owner_epoch_incarnation");
    [KilnTest("unchanged_scope", Order = 7)] public Task Unchanged(TestContext c) => Check(c, 7, "unchanged_scope");

    // ---- S8: the transport bridge in the real server, lab peers as the players' clients ----

    private static CameraOverride Lens(int ms, float degrees = 70, int priority = 10) =>
        new(Lens: new CameraLens(degrees), Duration: TimeSpan.FromMilliseconds(ms), Priority: priority);

    private static async Task<CameraResult> Await(TestContext c, Task<CameraResult> task, string what, int seconds = 10)
    {
        await c.Until(() => task.IsCompleted, TimeSpan.FromSeconds(seconds), what);
        return await task;
    }

    private static CameraChange? Change(CameraLease lease, CameraChangeKind kind)
    {
        lock (Changes) return Changes.LastOrDefault(x => x.Lease.Value == lease.Value && x.Kind == kind);
    }

    [KilnTest("remote_bound", Order = 8, TimeoutSeconds = 150)]
    public async Task RemoteBound(TestContext c)
    {
        await c.Until(() => CameraBridgeStartup.State == "bound", TimeSpan.FromSeconds(30), "the bridge bound");
        Assert.True(Capabilities.Is("camera.remote"), "camera.remote is on once the bridge is bound: " + Capabilities.Why("camera.remote"));
        Assert.True(Capabilities.Is("camera.server"), "camera.server");
        var channel = Network.Channels.FirstOrDefault(ch => ch.Name == "kiln.camera");
        Assert.NotNull(channel, "the channel kiln.camera is registered");
        Assert.Equal("1", channel!.Version, "kiln.camera version");
        Assert.False(channel.Required, "kiln.camera is not required: vanilla and older clients still join");
        Assert.Equal(16384, channel.MaxMessageBytes, "kiln.camera's message limit");
        c.Log.Info("camera-test: waiting for the lab peers (camnet.py)");
        await c.Until(() => Network.Peers.Count(p => p.IsLab && p.State == NetPeerState.Ready) >= 4, TimeSpan.FromSeconds(120), "four lab peers ready");
        await c.Until(() => (Camera.GetCapabilities(CameraEndpoint.Remote, A) & CameraCapability.LocalFov) != 0, TimeSpan.FromSeconds(10), "A negotiated");
        var a = Camera.GetCapabilities(CameraEndpoint.Remote, A);
        var n = Camera.GetCapabilities(CameraEndpoint.Remote, N);
        Assert.Equal(CameraCapability.Remote | CameraCapability.LocalPose | CameraCapability.LocalFov | CameraCapability.LocalProfile | CameraCapability.LocalShake, a, "A's parts");
        await c.Until(() => (Camera.GetCapabilities(CameraEndpoint.Remote, N) & CameraCapability.LocalPose) != 0, TimeSpan.FromSeconds(10), "N negotiated");
        n = Camera.GetCapabilities(CameraEndpoint.Remote, N);
        Assert.Equal(CameraCapability.Remote | CameraCapability.LocalPose | CameraCapability.LocalProfile, n, "N's parts (no lens)");
        Assert.Equal(CameraCapability.Remote, Camera.GetCapabilities(CameraEndpoint.Remote, V), "V offers nothing");
        c.Log.Info($"camera-test PASS remote_bound (camera.remote on, kiln.camera v1 not required 16384 B, A {a}, N {n})");
    }

    [KilnTest("remote_applied_on_client_frame", Order = 9)]
    public async Task RemoteApplied(TestContext c)
    {
        var r = await Await(c, Camera.Set(A, Lens(20000)), "Set(A)");
        Assert.True(r.Ok, $"Set(A) refused: {r.ReasonCode} {r.Reason}");
        Assert.Equal(CameraCommitScope.ClientFrame, r.CommitScope, "a remote Ok is a client frame");
        await c.Until(() => Change(r.Lease, CameraChangeKind.Applied) is not null, TimeSpan.FromSeconds(5), "Applied event");
        Assert.Equal(4242ul, Change(r.Lease, CameraChangeKind.Applied)!.Frame, "the confirming frame is the client's (camnet answers 4242)");
        var u = await Await(c, Camera.Update(r.Lease, Lens(20000, 90), CameraUpdateTiming.Restart), "Update(A)");
        Assert.True(u.Ok && u.Revision == 2, $"Update(A): {u.Ok} {u.ReasonCode} rev {u.Revision}");
        var e = await Await(c, Camera.End(r.Lease), "End(A)");
        Assert.True(e.Ok, $"End(A): {e.ReasonCode}");
        await c.Until(() => Change(r.Lease, CameraChangeKind.Ended) is { EndReason: CameraEndReason.Released }, TimeSpan.FromSeconds(5), "Ended(Released)");
        c.Log.Info($"camera-test PASS remote_applied_on_client_frame (lease {r.Lease.Value}: Applied frame 4242, Updated rev 2, Ended Released)");
    }

    [KilnTest("remote_client_ended", Order = 10)]
    public async Task RemoteClientEnded(TestContext c)
    {
        // camnet ends a lease of 3333 ms by itself right after its Ack (as a client whose lease expired)
        var r = await Await(c, Camera.Set(A, Lens(3333)), "Set(A, 3333 ms)");
        Assert.True(r.Ok, $"Set(A) refused: {r.ReasonCode} {r.Reason}");
        await c.Until(() => Change(r.Lease, CameraChangeKind.Ended) is not null, TimeSpan.FromSeconds(10), "the client's own end");
        var ended = Change(r.Lease, CameraChangeKind.Ended)!;
        Assert.Equal(CameraEndReason.Expired, ended.EndReason, "end reason from the client");
        var again = await Await(c, Camera.End(r.Lease), "End after the client's end");
        Assert.True(again.Ok && again.AlreadyEnded, "End of a lease the client ended: Ok, AlreadyEnded");
        c.Log.Info("camera-test PASS remote_client_ended (transport_ended -> Ended(Expired), End afterwards AlreadyEnded)");
    }

    [KilnTest("remote_refusals", Order = 11, TimeoutSeconds = 60)]
    public async Task RemoteRefusals(TestContext c)
    {
        var n = await Await(c, Camera.Set(N, Lens(5000)), "Set(N)");
        Assert.Equal((false, CameraReason.ClientCapability), (n.Ok, n.ReasonCode), "a client without the lens part");
        var v = await Await(c, Camera.Set(V, Lens(5000)), "Set(V)");
        Assert.Equal((false, CameraReason.RequiresKilnClient), (v.Ok, v.ReasonCode), "a client without kiln.camera");
        var s = await Await(c, Camera.Set(S, Lens(5000)), "Set(S)", 15);
        Assert.Equal((false, CameraReason.Negotiation), (s.Ok, s.ReasonCode), "a client that never says Hello");
        var nobody = await Await(c, Camera.Set(new EntityId(919999), Lens(5000)), "Set(nobody)");
        Assert.Equal((false, CameraReason.PeerUnavailable), (nobody.Ok, nobody.ReasonCode), "no such player connected");
        // the profile part needs camera.local_profile: N has it, so a profile request reaches N and is applied
        var p = await Await(c, Camera.Set(N, new CameraOverride(Profile: new CameraProfile(N, 6f, System.Numerics.Vector3.Zero), Duration: TimeSpan.FromSeconds(5))), "Set(N, profile)");
        Assert.True(p.Ok, $"Set(N, profile): {p.ReasonCode} {p.Reason}");
        await Await(c, Camera.End(p.Lease), "End(N)");
        c.Log.Info("camera-test PASS remote_refusals (ClientCapability, RequiresKilnClient, Negotiation after 2 s, PeerUnavailable; N's profile Ok)");
    }

    [KilnTest("remote_supersede", Order = 12)]
    public async Task RemoteSupersede(TestContext c)
    {
        var low = await Await(c, Camera.Set(A, Lens(20000, 60, 0)), "Set(A, priority 0)");
        Assert.True(low.Ok, $"low: {low.ReasonCode}");
        var same = await Await(c, Camera.Set(A, Lens(20000, 60, 50)), "Set(A) again by the same mod");
        Assert.Equal((false, CameraReason.AlreadyOwned), (same.Ok, same.ReasonCode), "the same owner applies again: AlreadyOwned (use Update)");
        var e = await Await(c, Camera.End(low.Lease), "End(low)");
        Assert.True(e.Ok, "End(low)");
        c.Log.Info("camera-test PASS remote_supersede (AlreadyOwned for the same owner; ended)");
    }
}
