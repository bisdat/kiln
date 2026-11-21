// SPDX-License-Identifier: MIT
// Kiln.Markers' anchors on a fake kiln.markers (chapter 04 §5 capture text): the client capture's tickets (posted to the
// client step, completed once, canceled, retired, ended by an epoch change through the stop timer, the waiting limits),
// the server's synchronous capture, the error rule, the JSON anchor form through the per-mod registry, and SetNameplate.
using Kiln;
using Xunit;

namespace MarkersTests;

public sealed class AnchorsTests : MarkersFixture
{
    private readonly List<Action> _posted = new();

    public AnchorsTests()
    {
        MarkerAnchors.PostToClient = _posted.Add;  // the client step runs when a test says so
        MarkerAnchors.TimerPeriod = Timeout.InfiniteTimeSpan;  // CheckEpochs is called by the tests (one test runs the real timer)
        FakeMarkers.ClientEpoch = 5;
        FakeMarkers.ServerEpoch = 3;
    }

    private static void Anchors(bool client = true, bool server = true)
    {
        Capabilities.Report("markers.anchors.client", client ? CapabilityState.Available : CapabilityState.Unavailable,
            client ? CapabilityReason.None : CapabilityReason.SymbolMissing, client ? null : "test: off");
        Capabilities.Report("markers.anchors.server", server ? CapabilityState.Available : CapabilityState.Unavailable,
            server ? CapabilityReason.None : CapabilityReason.SymbolMissing, server ? null : "test: off");
    }

    // a call whose Task is not the point (it throws before any Task exists)
    private static Action Call<T>(Func<T> f) => () => f();

    private void Step()
    {
        var work = _posted.ToArray();
        _posted.Clear();
        foreach (var w in work) w();
    }

    [Fact]
    public async Task A_client_capture_runs_at_the_next_client_step_and_completes_once()
    {
        Anchors();
        var mod = Mod("plates.mod", 77);
        var task = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(2022)));
        Assert.False(task.IsCompleted);
        Assert.Empty(FakeMarkers.Captures);  // nothing is read off the client world's thread
        Assert.Equal(1, MarkerAnchors.WaitingCount);
        Assert.True(MarkerAnchors.TimerRunning);  // the stop timer only while captures wait
        var post = _posted.Single();
        Step();
        var r = await task;
        Assert.True(r.Ok);
        Assert.Null(r.Reason);
        Assert.Equal((new EntityId(2022), 2u, 5ul), (r.Anchor!.Entity, r.Anchor.WorldKind, r.Anchor.WorldEpoch));
        Assert.Equal((77ul, 2u, 5ul, 2022u), FakeMarkers.Captures.Single());  // the epoch read at the call
        Assert.Equal(0, MarkerAnchors.WaitingCount);
        Assert.False(MarkerAnchors.TimerRunning);
        post();  // a stale copy of the posted work runs again: nothing happens
        Assert.Single(FakeMarkers.Captures);
        Assert.Equal(1, MarkerAnchors.Registered(77));
        r.Anchor.Dispose();
        Assert.Equal(0, MarkerAnchors.Registered(77));
        Assert.Contains("release_anchor 77 257", FakeMarkers.Calls);
    }

    [Fact]
    public async Task Canceled_before_the_step_cancels_the_task_and_makes_no_lease()
    {
        Anchors();
        var mod = Mod("cancel.mod", 77);
        using var cts = new CancellationTokenSource();
        var task = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(2022), cts.Token));
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => task);
        Step();
        Assert.Empty(FakeMarkers.Captures);
        Assert.Equal(0, MarkerAnchors.WaitingCount);
        // canceled already: no ticket at all
        var again = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(2022), cts.Token));
        Assert.True(again.IsCanceled);
        Assert.Empty(_posted);
    }

    [Fact]
    public async Task Canceled_while_it_captures_releases_the_lease_and_cancels()
    {
        Anchors();
        var mod = Mod("late.cancel", 77);
        using var cts = new CancellationTokenSource();
        var task = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(2022), cts.Token));
        FakeMarkers.DuringCapture = cts.Cancel;
        Step();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => task);
        Assert.Single(FakeMarkers.Captures);
        Assert.Single(FakeMarkers.Calls, c => c.StartsWith("release_anchor 77 ", StringComparison.Ordinal));
        Assert.Equal(0, MarkerAnchors.Registered(77));
    }

    [Fact]
    public async Task A_mod_that_unloads_completes_its_waiting_captures()
    {
        Anchors();
        var mod = Mod("leaving.mod", 77);
        var other = Mod("staying.mod", 78);
        var mine = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(2022)));
        var theirs = As(other, () => Markers.CaptureAnchorAsync(new EntityId(2023)));
        mod.Retire();
        var r = await mine;
        Assert.False(r.Ok);
        Assert.StartsWith("NotReady", r.Reason);
        Assert.Contains("unloaded", r.Reason);
        Step();
        Assert.Equal((78ul, 2u, 5ul, 2023u), FakeMarkers.Captures.Single());  // the retired one never captures
        Assert.True((await theirs).Ok);
    }

    [Fact]
    public async Task A_world_that_changes_or_ends_before_the_step_completes_the_capture_NotReady()
    {
        Anchors();
        var mod = Mod("epoch.mod", 77);
        var a = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(1)));
        var b = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(2)));
        MarkerAnchors.CheckEpochs();  // the same world: nothing
        Assert.False(a.IsCompleted);
        FakeMarkers.ClientEpoch = 6;  // a new client world
        MarkerAnchors.CheckEpochs();
        Assert.Contains("changed", (await a).Reason);
        Assert.False((await b).Ok);
        Assert.False(MarkerAnchors.TimerRunning);
        Step();
        Assert.Empty(FakeMarkers.Captures);  // the posted closures saw completed tickets: no lease in the next world
        var c = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(3)));
        FakeMarkers.ClientEpoch = 0;
        MarkerAnchors.CheckEpochs();
        Assert.Contains("ended", (await c).Reason);
        Step();
        Assert.Empty(FakeMarkers.Captures);
        // no client world at the call: NotReady at once, nothing posted
        var d = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(4)));
        Assert.True(d.IsCompleted);
        Assert.StartsWith("NotReady", (await d).Reason);
        Assert.Empty(_posted);
    }

    [Fact]
    public async Task The_stop_timer_runs_only_while_captures_wait()
    {
        Anchors();
        MarkerAnchors.TimerPeriod = TimeSpan.FromMilliseconds(20);
        var mod = Mod("timer.mod", 77);
        Assert.False(MarkerAnchors.TimerRunning);
        var task = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(1)));
        Assert.True(MarkerAnchors.TimerRunning);
        FakeMarkers.ClientEpoch = 9;  // the world went; no client step will come
        var done = await Task.WhenAny(task, Task.Delay(5000));
        Assert.Same(task, done);
        Assert.StartsWith("NotReady", (await task).Reason);
        Assert.False(MarkerAnchors.TimerRunning);
    }

    [Fact]
    public async Task Waiting_captures_are_limited_per_mod_and_in_all()
    {
        Anchors();
        var mods = Enumerable.Range(0, 5).Select(i => Mod($"many{i}.mod", 100 + (ulong)i)).ToArray();
        var tasks = new List<Task<MarkerAnchorResult>>();
        for (var i = 0; i < MarkerAnchors.PendingPerMod; i++) tasks.Add(As(mods[0], () => Markers.CaptureAnchorAsync(new EntityId(1000 + (uint)i))));
        var over = await As(mods[0], () => Markers.CaptureAnchorAsync(new EntityId(9999)));
        Assert.False(over.Ok);
        Assert.StartsWith("Full", over.Reason);
        for (var m = 1; m < 4; m++)
            for (var i = 0; i < MarkerAnchors.PendingPerMod; i++) tasks.Add(As(mods[m], () => Markers.CaptureAnchorAsync(new EntityId(2000 + (uint)i))));
        Assert.Equal(MarkerAnchors.PendingTotal, MarkerAnchors.WaitingCount);
        var all = await As(mods[4], () => Markers.CaptureAnchorAsync(new EntityId(9999)));
        Assert.StartsWith("Full", all.Reason);
        Step();
        Assert.All(await Task.WhenAll(tasks), r => Assert.True(r.Ok));
        Assert.Equal(0, MarkerAnchors.WaitingCount);
    }

    [Fact]
    public async Task Native_answers_follow_the_error_rule()
    {
        Anchors();
        var mod = Mod("rule.mod", 77);
        FakeMarkers.Missing.Add(404);
        var missing = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(404)));
        Step();
        var r = await missing;
        Assert.Equal(new MarkerAnchorResult(false, "NotFound: no entity 404", null), r);
        Assert.IsAssignableFrom<IKilnResult>(r);
        FakeMarkers.NextCapture = (3, 0, "markers.anchors.client");
        var off = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(1)));
        Step();
        Assert.Equal("markers.anchors.client", (await Assert.ThrowsAsync<KilnUnavailableException>(() => off)).Capability);
        FakeMarkers.NextCapture = (1, 0, "entity 0 is no entity");
        var bad = As(mod, () => Markers.CaptureAnchorAsync(new EntityId(1)));
        Step();
        await Assert.ThrowsAsync<ArgumentException>(() => bad);
        // checked before anything waits
        Assert.Throws<ArgumentException>(Call(() => As(mod, () => Markers.CaptureAnchorAsync(EntityId.None))));
        Anchors(client: false);
        Assert.Equal("markers.anchors.client",
            Assert.Throws<KilnUnavailableException>(Call(() => As(mod, () => Markers.CaptureAnchorAsync(new EntityId(1))))).Capability);
        Assert.Throws<InvalidOperationException>(Call(() => As(null, () => { Anchors(); return Markers.CaptureAnchorAsync(new EntityId(1)); })));
    }

    [Fact]
    public void The_server_capture_is_synchronous_on_the_tick_thread()
    {
        Anchors();
        var mod = Mod("server.mod", 77);
        var r = As(mod, () => Markers.CaptureServerAnchor(new EntityId(2022)));
        Assert.True(r.Ok);
        Assert.Equal((new EntityId(2022), 0u, 3ul), (r.Anchor!.Entity, r.Anchor.WorldKind, r.Anchor.WorldEpoch));
        Assert.Equal((77ul, 0u, 3ul, 2022u), FakeMarkers.Captures.Single());
        Assert.Empty(_posted);
        FakeMarkers.Missing.Add(5);
        Assert.StartsWith("NotFound", As(mod, () => Markers.CaptureServerAnchor(new EntityId(5))).Reason);
        FakeMarkers.ServerEpoch = 0;
        Assert.StartsWith("NotReady", As(mod, () => Markers.CaptureServerAnchor(new EntityId(2022))).Reason);
        Assert.Throws<ArgumentException>(() => As(mod, () => Markers.CaptureServerAnchor(EntityId.None)));
        Anchors(server: false);
        Assert.Equal("markers.anchors.server", Assert.Throws<KilnUnavailableException>(() => As(mod, () => Markers.CaptureServerAnchor(new EntityId(1)))).Capability);
        r.Anchor.Dispose();
        r.Anchor.Dispose();
        Assert.Single(FakeMarkers.Calls, c => c.StartsWith("release_anchor", StringComparison.Ordinal));
    }

    [Fact]
    public void JSON_names_a_live_lease_of_the_calling_mod_and_gets_the_same_object()
    {
        Anchors();
        var mod = Mod("json.mod", 77);
        var other = Mod("other.mod", 78);
        var lease = As(mod, () => Markers.CaptureServerAnchor(new EntityId(2022))).Anchor!;
        var spec = new MarkerSpec("f", default, "", MarkerIcon.None, MarkerSurface.World, lease);
        var json = spec.ToJson();
        Assert.Contains($"\"follow\":{{\"anchor\":\"{lease.Handle:x16}\"}}", json);
        var back = As(mod, () => MarkerSpec.FromJson(json));
        Assert.Same(lease, back.Follow);  // the same lease, not a second disposable one
        Assert.Equal(spec, back);
        Assert.Throws<ArgumentException>(() => As(other, () => MarkerSpec.FromJson(json)));  // another mod's lease
        Assert.Throws<InvalidOperationException>(() => As(null, () => MarkerSpec.FromJson(json)));
        Assert.Throws<ArgumentException>(() => As(mod, () => MarkerSpec.FromJson(json.Replace(lease.Handle.ToString("x16"), "00000000000000ff"))));
        Assert.Throws<ArgumentException>(() => As(mod, () => MarkerSpec.FromJson(json.Replace(lease.Handle.ToString("x16"), "12"))));
        FakeMarkers.ServerEpoch = 4;  // its world ended
        Assert.Throws<ArgumentException>(() => As(mod, () => MarkerSpec.FromJson(json)));
        FakeMarkers.ServerEpoch = 3;
        lease.Dispose();
        Assert.Throws<ArgumentException>(() => As(mod, () => MarkerSpec.FromJson(json)));  // disposed
        // the registry stays bounded: leases of an ended world go at the next capture
        var a = As(mod, () => Markers.CaptureServerAnchor(new EntityId(1))).Anchor!;
        FakeMarkers.ServerEpoch = 8;
        var b = As(mod, () => Markers.CaptureServerAnchor(new EntityId(2))).Anchor!;
        Assert.Equal(1, MarkerAnchors.Registered(77));
        Assert.Equal(b.Handle, As(mod, () => MarkerSpec.FromJson(new MarkerSpec("g", default, "", MarkerIcon.None, MarkerSurface.World, b).ToJson())).Follow!.Handle);
        // unloading the mod forgets its leases (kiln.markers released them)
        mod.Retire();
        Assert.Equal(0, MarkerAnchors.Registered(77));
        Assert.True(b.IsDisposed);  // a went from the registry with its world: its lease is gone natively
        Assert.False(a.IsDisposed);
    }

    [Fact]
    public async Task SetNameplate_captures_sets_a_world_label_as_the_mod_and_releases_its_own_lease()
    {
        Anchors();
        var mod = Mod("plates.mod", 77);
        var task = As(mod, () => Markers.SetNameplate("boss", new EntityId(2022), "The Fell Thunderbrute", 2.5f));
        Assert.Empty(FakeMarkers.Specs);  // nothing is stored while the entity is awaited
        Step();
        var r = await task;
        Assert.True(r.Ok);
        var s = Assert.Single(FakeMarkers.Specs);
        Assert.Equal(new FakeSpec(77, 96, 4, "boss", 0, 0, 0, 0x101, 2.5f, 0, 0, 0xffffffff, 0, 0, "The Fell Thunderbrute"), s);
        Assert.Contains("release_anchor 77 257", FakeMarkers.Calls);  // its own lease went after the set
        Assert.Equal(0, MarkerAnchors.Registered(77));
        // a refused capture stores nothing
        FakeMarkers.Missing.Add(404);
        var refused = As(mod, () => Markers.SetNameplate("gone", new EntityId(404), "x"));
        Step();
        Assert.Equal(new MarkerResult(false, "NotFound: no entity 404", false, 0, 0, 0), await refused);
        Assert.Single(FakeMarkers.Specs);
        // bad arguments throw at once
        Assert.Throws<ArgumentNullException>(Call(() => As(mod, () => Markers.SetNameplate(null!, new EntityId(1), "x"))));
        Assert.Throws<ArgumentNullException>(Call(() => As(mod, () => Markers.SetNameplate("k", new EntityId(1), null!))));
        Assert.Throws<ArgumentException>(Call(() => As(mod, () => Markers.SetNameplate("k", new EntityId(1), ""))));
        Assert.Throws<ArgumentException>(Call(() => As(mod, () => Markers.SetNameplate("bad key", new EntityId(1), "x"))));
        Assert.Throws<ArgumentException>(Call(() => As(mod, () => Markers.SetNameplate("k", new EntityId(1), "x", float.NaN))));
        Assert.Throws<ArgumentException>(Call(() => As(mod, () => Markers.SetNameplate("k", new EntityId(1), "x", 200))));
        Assert.Throws<ArgumentException>(Call(() => As(mod, () => Markers.SetNameplate("k", EntityId.None, "x"))));
        Assert.Throws<InvalidOperationException>(Call(() => As(null, () => Markers.SetNameplate("k", new EntityId(1), "x"))));
        Assert.Empty(_posted);
    }
}
