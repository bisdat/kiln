// SPDX-License-Identifier: MIT
// kiln.markers' anchors in a dedicated-server lab (chapter 04 §4.2, M05/M06 server mechanics; no player needed).
//  1. The research probe's run (dev28 out/markers/probes/anchors.cpp, 11 PASS) through the product API: an entity made with
//     only a template and a position (no SpawnTime), Markers.CaptureServerAnchor reads it with the explicit-world reader,
//     the follow read is Ready where it is, destroy ends the incarnation (AnchorEnded) before anything reuses the id, the
//     same id made again is a new incarnation and the old lease stays ended; a Kiln.Spawn entity ends the same way; the
//     lease limit (128 per mod, the next is Full); every lease released.
//  2. The observers on the game's worker threads: with 06a's lab parallel switch on, entities that the game's own
//     lifetime program destroys (a LifeTime component), half of them followed; the counters say on how many threads the
//     create/destroy observers ran and how many of those are not the world's own.
// Lines "markers-test: PASS|FAIL|INFO ..." and a last "markers-test: DONE n PASS, m FAIL".
using System.Runtime.InteropServices;
using Kiln;
using Kiln.Testing;

namespace MarkersTest;

public sealed class MarkersTestMod : Mod
{
    private const string Template = "HintMarker_NE_Mountain_Well";
    private static readonly WorldPosition Here = new(3670, 820, 1500);
    private int _pass, _fail, _step;
    private bool _probeDone, _done;

    // keen::ecs::LifeTime (24 bytes, reflected): lifeTime, endOfLife (ECS ns), enforceDestroy, preventDestroy, triggeredDestroy
    [StructLayout(LayoutKind.Explicit, Size = 24)]
    private struct LifeTime
    {
        [FieldOffset(0)] public long Duration;
        [FieldOffset(8)] public long EndOfLife;
        [FieldOffset(16)] public byte Enforce;
    }

    public override void OnLoad()
    {
        Context.Tick.Post += OnPost;
        Info($"loaded: anchors.server {Row("markers.anchors.server")}, anchors.client {Row("markers.anchors.client")}, lab table {MarkersLab.Available}, " +
             $"parallel switch {LabSystems.Enabled}");
    }

    private static string Row(string name) => Capabilities.Is(name) ? "available" : "off (" + Capabilities.Why(name) + ")";

    private void Info(string text) => Log.Info("markers-test: INFO " + text);

    private bool Check(string name, bool ok, object? actual = null)
    {
        if (ok) _pass++;
        else _fail++;
        if (ok) Log.Info($"markers-test: PASS {name}");
        else Log.Error($"markers-test: FAIL {name}{(actual is null ? "" : ": " + actual)}");
        return ok;
    }

    private void OnPost(in TickInfo tick)
    {
        if (_done || !Spawn.WorldRunning) return;
        _step++;
        try
        {
            if (!_probeDone && _step >= 30)
            {
                _probeDone = true;
                ProbeRun();
                WorkersStart();
            }
            else if (_probeDone) WorkersStep();
        }
        catch (Exception e)
        {
            Check("no exception", false, e);
            Finish();
        }
    }

    private void Finish()
    {
        if (_done) return;
        _done = true;
        foreach (var a in _followed) a.Dispose();
        _followed.Clear();
        Info($"counters at the end: {Counters()}");
        Log.Info($"markers-test: DONE {_pass} PASS, {_fail} FAIL");
    }

    // ---- 1. the probe's run through the product API ----

    private void ProbeRun()
    {
        Check("markers.anchors.server is available (sigdb feature, CurrentTransform layout checked, observers installed)",
            Capabilities.Is("markers.anchors.server"), Capabilities.Why("markers.anchors.server"));
        var client = Capabilities.Get("markers.anchors.client");
        Check("markers.anchors.client is off on a dedicated server (wrong side)",
            !Capabilities.Is("markers.anchors.client") && client.Reason == CapabilityReason.WrongSide, $"{client.State} {client.Reason} {client.ReasonText}");
        if (!Check("kiln.markers.lab is published on this lab server", MarkersLab.Available)) return;
        var tmpl = Spawn.Lookup(Template);
        if (!Check($"the vanilla template {Template} is known", tmpl is not null)) return;

        var entity = MarkersLab.CreateRaw(0, tmpl!.Id, Here);
        if (!Check("raw CreateEntity returns a non-player id", entity.Value > 1023, entity)) return;
        Check("the capture target has no SpawnTime", Spawn.Inspect(entity) is { SpawnTime: null }, Spawn.Inspect(entity));

        var a = Markers.CaptureServerAnchor(entity);
        if (!Check("Markers.CaptureServerAnchor reads the entity through the explicit-world reader (CurrentTransform)", a.Ok, a.Reason)) return;
        var lease = a.Anchor!;
        var f0 = MarkersLab.FollowNow(lease);
        Check("the follow read is Ready at the created position", f0 is { State: "Ready" } r0 && Near(r0.Position, Here), f0);
        var b = Markers.CaptureServerAnchor(entity);
        var fb = b.Anchor is { } bl ? MarkersLab.FollowNow(bl) : null;
        Check("a second capture of the same incarnation has the same serial (another lease)",
            b.Ok && fb?.Incarnation == f0?.Incarnation && b.Anchor!.Entity == entity, $"{fb?.Incarnation} vs {f0?.Incarnation}");
        b.Anchor?.Dispose();

        Check("destroy the original entity", Spawn.Destroy(entity));
        var f1 = MarkersLab.FollowNow(lease);
        Check("destroy-entry ends the captured incarnation (AnchorEnded)", f1 is { State: "AnchorEnded" }, f1);
        var gone = Markers.CaptureServerAnchor(entity);
        Check("no transform after the destroy: NotFound, no lease", !gone.Ok && gone.Anchor is null && gone.Reason?.StartsWith("NotFound") == true, gone.Reason);

        var again = MarkersLab.CreateRaw(entity.Value, tmpl.Id, Here);
        Check("explicit same-id recreation succeeds", again == entity, again);
        var f2 = MarkersLab.FollowNow(lease);
        Check("the old lease stays ended across same-id reuse", f2 is { State: "AnchorEnded" }, f2);
        var c = Markers.CaptureServerAnchor(entity);
        var fc = c.Anchor is { } cl ? MarkersLab.FollowNow(cl) : null;
        Check("the recreated id is a new incarnation with a transform (a new serial)",
            c.Ok && fc is { State: "Ready" } && fc.Value.Incarnation != f0?.Incarnation, $"{f0?.Incarnation} -> {fc?.Incarnation} ({fc?.State})");
        Info($"serials: the first incarnation {f0?.Incarnation}, the recreated one {fc?.Incarnation}");

        // the lease limit (one entity, many leases of its one incarnation)
        var many = new List<MarkerAnchor>();
        if (c.Anchor is { } held) many.Add(held);
        MarkerAnchorResult last = new(true, null, null);
        lease.Dispose();  // the ended lease goes first: 128 of this mod from here
        while (many.Count < 200 && (last = Markers.CaptureServerAnchor(entity)).Ok) many.Add(last.Anchor!);
        Check("128 leases per mod, the next is Full (nothing evicted)", many.Count == 128 && last.Reason?.StartsWith("Full") == true,
            $"{many.Count} leases, then {last.Reason}");
        foreach (var m in many) m.Dispose();
        Check("cleanup: destroy the recreated entity", Spawn.Destroy(entity));

        // a Kiln.Spawn entity (it has SpawnTime): the same path
        var spawned = Spawn.TryEntity(Template, new WorldPosition(3680, 820, 1500));
        if (Check("Kiln.Spawn spawns a second entity", spawned.Ok, spawned.Reason))
        {
            var id = spawned.Entity.Id;
            var s = Markers.CaptureServerAnchor(id);
            Check("a Kiln.Spawn entity (with SpawnTime) is captured", s.Ok && Spawn.Inspect(id) is { SpawnTime: not null }, s.Reason);
            Spawn.Destroy(id);
            var fs = s.Anchor is { } sl ? MarkersLab.FollowNow(sl) : null;
            Check("Kiln.Spawn's destroy ends it the same way (AnchorEnded)", fs is { State: "AnchorEnded" }, fs);
            s.Anchor?.Dispose();
        }
        var counters = MarkersLab.Counters();
        Check("every lease of the probe run is released", counters is { Leases: 0 }, counters);
    }

    private static bool Near(WorldPosition p, WorldPosition q) => p.DistanceTo(q) < 0.01;

    private static string Counters() => MarkersLab.Counters() is { } c
        ? $"observers on {c.Threads} thread(s), {c.OtherThreads} not a world's own; {c.Creates} creates, {c.Destroys} destroys, " +
          $"{c.NetworkCreates} network creates seen; {c.Watched} watched, {c.Leases} leases, {c.References} references; " +
          $"{c.Serials} incarnations minted, {c.Ended} ended"
        : "no lab table";

    // ---- 2. the observers on the game's workers ----

    private readonly List<MarkerAnchor> _followed = new();
    private int _workerStart, _spawned;
    private (int Threads, int OtherThreads, int Watched, int Leases, int References, ulong Creates, ulong Destroys, ulong NetworkCreates, ulong Ended,
        ulong Serials)? _before;

    private void WorkersStart()
    {
        _workerStart = _step;
        if (!LabSystems.Enabled)
        {
            Info("the lab parallel switch is off (no kiln/control/systems.parallel): the worker run is skipped");
            Finish();
            return;
        }
        Check("the lab parallel switch forces the parallel path", LabSystems.ForceParallel(1500), LabSystems.PlayersMask);
        _before = MarkersLab.Counters();
        Info($"before the worker run: {Counters()}");
        var life = SpawnComponent.Of("LifeTime", new LifeTime { Duration = 2_000_000_000, EndOfLife = 0, Enforce = 1 });
        for (var i = 0; i < 40; i++)
        {
            var r = Spawn.TryEntity(Template, new WorldPosition(3600 + i * 3, 820, 1400),
                options: new SpawnOptions { Flags = SpawnFlags.NoTrack, Components = [life] });
            if (!r.Ok)
            {
                Info($"spawn with LifeTime refused: {r.Reason}");
                continue;
            }
            _spawned++;
            if (i % 2 == 0 && Markers.CaptureServerAnchor(r.Entity.Id) is { Ok: true, Anchor: { } a }) _followed.Add(a);
        }
        Info($"worker run: {_spawned} entities with a 2 s LifeTime, {_followed.Count} of them followed; the switch holds the parallel path");
    }

    private void WorkersStep()
    {
        var waited = _step - _workerStart;
        var ended = _followed.Count(a => MarkersLab.FollowNow(a) is { State: "AnchorEnded" });
        if (ended < _followed.Count && waited < 900) return;  // up to about 30 s
        LabSystems.StopParallel();
        var c = MarkersLab.Counters();
        Info($"worker run after {waited} steps: {ended} of {_followed.Count} followed entities ended; {Counters()}");
        Check("the game's own lifetime program destroyed every followed entity and each incarnation ended", _followed.Count > 0 && ended == _followed.Count,
            $"{ended} of {_followed.Count}");
        // The game decides where it calls CreateEntity/DestroyEntity; this line reports what it did in this window.
        var creates = c?.Creates - _before?.Creates;
        var destroys = c?.Destroys - _before?.Destroys;
        if (c is { OtherThreads: > 0 })
            Check("the create/destroy observers ran on the game's worker threads (threads that are not the world's own)", true);
        else
            Info($"not shown here: with the parallel path forced the game made its {creates} creates and {destroys} destroys of this window on " +
                 $"{c?.Threads} thread(s), none of them a worker (the observers' thread safety is the host test's four-thread run)");
        Finish();
    }
}
