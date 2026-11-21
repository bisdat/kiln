// SPDX-License-Identifier: MIT
// A server-only author mod for kiln.markers' link (dossier chapter 04 §6, M08 and M10 in a lab): it sends broadcast and
// per-player markers while a receiving lab peer (tools/nettest/markerstest.py: a Kiln client of the lab transport that
// speaks kiln:markers v1 and has NOT this mod) and a vanilla lab peer join. Lines "markers-send-test: PASS <case>" /
// "FAIL <case>" in kiln.log; "markers-send-test done" at the end. Phases advance on what the server sees, never on time
// alone: phase 1 waits for the icon catalog from a verified client, phase 2 for a vanilla peer.
//
// Demo mode for real game clients (off unless asked): with kiln/control/markers-send-test.demo present the nettest phases
// are skipped; once the server world runs and markers.send is available it broadcasts "demo-map" (Map, icon well_plant,
// once the server's catalog is ready), "demo-compass" (Compass, red) and "demo-world" (World, "Server label", height 2)
// near the point the file holds ("x y z"; empty: the first connected player's position, else 3670 820 1500), sends the
// identical specs again every 10 s and logs "markers-send-test: demo ..." with each marker's queued/refused counts.
// kiln/control/markers-send-test.demo-clear clears them once ("markers-send-test: demo cleared") and pauses the demo while
// it exists.
using System.Globalization;
using Kiln;

namespace MarkersSendTest;

public sealed class MarkersSendTestMod : Mod
{
    private const uint WellPlant = 0xffe5e444u;  // chapter 04 §4.3: the alias well_plant (this build's catalog)
    private static readonly WorldPosition At = new(3670, 820, 1500);
    private int _pass, _fail, _phase;
    private DateTime _since = DateTime.UtcNow, _vanillaSeen = DateTime.MinValue;
    private readonly object _lock = new();
    private readonly List<MarkerChangedEvent> _changes = new();
    private readonly List<MarkerCatalogChangedEvent> _catalogs = new();

    private string ControlDir => Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "..", "control"));
    private string DemoFile => Path.Combine(ControlDir, "markers-send-test.demo");
    private string DemoClearFile => Path.Combine(ControlDir, "markers-send-test.demo-clear");
    private bool _demo, _demoCleared;
    private WorldPosition? _demoAt;
    private DateTime _demoNext = DateTime.MinValue, _demoLook = DateTime.MinValue;

    public override void OnLoad()
    {
        Markers.Changed += e =>
        {
            lock (_lock) _changes.Add(e);
        };
        Markers.CatalogChanged += e =>
        {
            lock (_lock) _catalogs.Add(e);
        };
        Context.Tick.Pre += OnTick;
        Info("loaded: waiting for the server world");
    }

    public override void OnUnload() => Info($"unloaded: {_pass} PASS, {_fail} FAIL");

    private void Next(int phase)
    {
        _phase = phase;
        _since = DateTime.UtcNow;
        Info($"phase {phase}");
    }

    private double Elapsed => (DateTime.UtcNow - _since).TotalSeconds;

    private void OnTick(in TickInfo tick)
    {
        if (!Lifecycle.IsServerRunning) return;
        if (DateTime.UtcNow >= _demoLook)
        {
            _demoLook = DateTime.UtcNow.AddSeconds(1);
            if (!_demo && File.Exists(DemoFile))
            {
                _demo = true;
                Info($"demo mode ({DemoFile}): the nettest phases are skipped");
            }
        }
        if (_demo)
        {
            try
            {
                Demo();
            }
            catch (Exception e)
            {
                Log.Error($"markers-send-test: demo step threw: {e.GetType().Name}: {e.Message}");
                _demoNext = DateTime.UtcNow.AddSeconds(10);
            }
            return;
        }
        if (_phase >= 9) return;
        try
        {
            Step();
        }
        catch (Exception e)
        {
            Check(false, $"phase {_phase} threw: {e.GetType().Name}: {e.Message}");
            Done();
        }
    }

    private MarkerSpec Spec(string id, string text, MarkerSurface surfaces = MarkerSurface.World | MarkerSurface.Compass, uint icon = 0) =>
        new(id, At, text, new MarkerIcon(icon), surfaces, Height: 2);

    private int Count(string id, MarkerChangeKind kind)
    {
        lock (_lock) return _changes.Count(c => c.Id == id && c.Kind == kind && c.Owner == "markers-send-test");
    }

    private void Step()
    {
        switch (_phase)
        {
            case 0:
            {
                if (!Capabilities.Is("markers.send"))
                {
                    if (Elapsed > 60) Fail("markers.send.available", $"{Capabilities.Get("markers.send")}");
                    return;
                }
                Pass("markers.send.available");
                var r = Markers.Send(EntityId.None, Spec("lab_a", "Lab A"));
                Check(r.Ok && r.Committed && r.Queued == 0 && r.Refused == 0, $"broadcast.no_recipients_is_ok ({r})");
                r = Markers.Send(EntityId.None, Spec("lab_map", "Well", MarkerSurface.Map, WellPlant));
                Check(!r.Ok && !r.Committed && r.Reason?.StartsWith("CatalogNotReady", StringComparison.Ordinal) == true,
                    $"map.before_the_catalog_is_not_stored ({r})");
                r = Markers.Send(new EntityId(999), Spec("lab_t", "T"));
                Check(!r.Ok && !r.Committed && r.Reason?.StartsWith("NotFound", StringComparison.Ordinal) == true, $"target.not_connected_is_refused ({r})");
                Check(!Capabilities.Is("markers.catalog"), $"catalog.off_before_a_verified_client ({Capabilities.Get("markers.catalog")})");
                Next(1);
                return;
            }
            case 1:  // the receiving peer's verified catalog
            {
                MarkerCatalogChangedEvent? c;
                lock (_lock) c = _catalogs.FirstOrDefault(x => x.Role == 0 && x.Ready);
                if (c is null)
                {
                    if (Elapsed > 180) Fail("catalog.changed_role0", "no CatalogChanged in 180 s");
                    return;
                }
                Check(c.Count == 43 && c.Generation == 1, $"catalog.changed_role0_count43 (count {c.Count}, generation {c.Generation})");
                Check(Markers.Icons.IsReady && Markers.Icons.All.Count == 43, $"catalog.ready_on_the_dedicated_server ({Markers.Icons.All.Count})");
                var icon = Markers.Icons.Resolve("well_plant");
                Check(icon.Ok && icon.Icon!.Icon.Id == WellPlant, $"catalog.resolve_well_plant ({icon})");
                var ready = Network.Peers.Count(p => p.State == NetPeerState.Ready && p.HasChannel("kiln:markers"));
                var r = Markers.Send(EntityId.None, Spec("lab_map", "Well", MarkerSurface.Map | MarkerSurface.Compass, WellPlant));
                Check(r.Ok && r.Committed && r.Queued == ready && ready >= 1, $"map.after_the_catalog_is_queued ({r}, {ready} ready peer(s))");
                Next(2);
                return;
            }
            case 2:  // a vanilla player next to the receiving one: M10's partial counts
            {
                // a vanilla game client is judged Vanilla after the handshake time; a lab vanilla peer stays Connecting
                var vanilla = Network.Peers.Any(p => !p.HasChannel("kiln:markers") &&
                                                     (p.State == NetPeerState.Vanilla || (p.IsLab && p.State == NetPeerState.Connecting)));
                if (!vanilla)
                {
                    _vanillaSeen = DateTime.MinValue;
                    if (Elapsed > 180) Fail("partial.vanilla_peer", "no vanilla peer in 180 s");
                    return;
                }
                if (_vanillaSeen == DateTime.MinValue) _vanillaSeen = DateTime.UtcNow;
                if ((DateTime.UtcNow - _vanillaSeen).TotalSeconds < 2) return;  // the bridge's next sync has it
                var r = Markers.Send(EntityId.None, Spec("lab_b", "Lab B"));
                Check(!r.Ok && r.Committed && r.Queued == 1 && r.Refused == 1 && r.Reason?.StartsWith("Partial", StringComparison.Ordinal) == true,
                    $"m10.partial_queued1_refused1 ({r})");
                Next(3);
                return;
            }
            case 3:  // the identical retry once the receiving peer has it: no event, nothing queued again, the vanilla one refused
            {
                if (Elapsed < 3) return;
                var again = Markers.Send(EntityId.None, Spec("lab_b", "Lab B"));
                Check(again.Committed && again.Affected == 0 && again.Queued == 0 && again.Refused == 1, $"m10.identical_retry ({again})");
                Next(4);
                return;
            }
            case 4:  // the events of phase 0-3 went out; then remove one and clear the rest
            {
                if (Elapsed < 3) return;
                Check(Count("lab_a", MarkerChangeKind.Added) == 1 && Count("lab_b", MarkerChangeKind.Added) == 1 && Count("lab_map", MarkerChangeKind.Added) == 1,
                    $"events.one_added_per_committed_mutation (lab_a {Count("lab_a", MarkerChangeKind.Added)}, lab_b {Count("lab_b", MarkerChangeKind.Added)}, lab_map {Count("lab_map", MarkerChangeKind.Added)})");
                MarkerChangedEvent? first;
                lock (_lock) first = _changes.FirstOrDefault(c => c.Id == "lab_a");
                Check(first is { Audience: MarkerSource.ServerBroadcast, Cause: "Api" }, $"events.broadcast_audience ({first?.Audience}, {first?.Cause})");
                var r = Markers.Unsend(EntityId.None, "lab_a");
                Check(r.Committed && r.Affected == 1, $"unsend.broadcast ({r})");
                Next(5);
                return;
            }
            case 5:
            {
                if (Elapsed < 3) return;
                var r = Markers.ClearSent();
                Check(r.Committed && r.Affected == 2, $"clear_sent.all ({r})");
                Next(6);
                return;
            }
            case 6:
            {
                if (Elapsed < 3) return;
                Check(Count("lab_a", MarkerChangeKind.Removed) == 1, "events.removed_once");
                int cleared;
                lock (_lock) cleared = _changes.Count(c => c.Kind == MarkerChangeKind.Cleared && c.Affected == 2);
                Check(cleared == 1, "events.one_cleared_for_clear_sent");
                Done();
                return;
            }
        }
    }

    // ---- demo mode (real game clients) ----

    private WorldPosition DemoPoint()
    {
        try
        {
            var parts = File.ReadAllText(DemoFile).Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 3 && double.TryParse(parts[0], NumberStyles.Float, CultureInfo.InvariantCulture, out var x) &&
                double.TryParse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture, out var y) &&
                double.TryParse(parts[2], NumberStyles.Float, CultureInfo.InvariantCulture, out var z))
                return new WorldPosition(x, y, z);
        }
        catch (IOException)
        {
        }
        foreach (var p in Network.Peers)
        {
            if (p.Player.IsNone) continue;
            try
            {
                if (Players.PositionOf(p.Player) is { } at) return at;
            }
            catch (Exception)
            {
                // kiln.players cannot say: the next player, then the fixed point
            }
        }
        return At;
    }

    private void Demo()
    {
        if (!Capabilities.Is("markers.send")) return;
        if (File.Exists(DemoClearFile))
        {
            if (_demoCleared) return;
            var c = Markers.ClearSent();
            _demoCleared = true;
            Log.Info($"markers-send-test: demo cleared ({c.Affected} marker(s), {c.Reason ?? "ok"})");
            return;
        }
        if (_demoCleared)
        {
            _demoCleared = false;  // the clear file is gone: the demo shows again
            _demoAt = null;
        }
        if (DateTime.UtcNow < _demoNext) return;
        _demoNext = DateTime.UtcNow.AddSeconds(10);
        var at = _demoAt ??= DemoPoint();
        var near = new WorldPosition(at.X + 20, at.Y, at.Z);
        var label = new WorldPosition(at.X, at.Y, at.Z + 10);
        var line = new List<string>();
        void One(string id, MarkerSpec spec)
        {
            var r = Markers.Send(EntityId.None, spec);
            line.Add($"{id} {r.Queued}/{r.Refused}{(r.Committed ? "" : $" ({r.Reason})")}");
        }
        if (Capabilities.Is("markers.catalog") && Markers.Icons.IsReady)
            One("demo-map", new MarkerSpec("demo-map", at, "Server map marker", new MarkerIcon(WellPlant), MarkerSurface.Map));
        else line.Add("demo-map waits for the server's icon catalog (a verified Kiln client)");
        One("demo-compass", new MarkerSpec("demo-compass", near, "Server compass", MarkerIcon.None, MarkerSurface.Compass, Rgba: 0xff0000ffu));
        One("demo-world", new MarkerSpec("demo-world", label, "Server label", MarkerIcon.None, MarkerSurface.World, Height: 2));
        Log.Info($"markers-send-test: demo at {at.X:0.#} {at.Y:0.#} {at.Z:0.#} queued/refused per marker: {string.Join(", ", line)}");
    }

    private void Done()
    {
        _phase = 9;
        Log.Info($"markers-send-test done: {_pass} PASS, {_fail} FAIL");
    }

    private void Pass(string what) => Check(true, what);
    private void Fail(string what, string detail)
    {
        Check(false, $"{what}: {detail}");
        Done();
    }

    private bool Check(bool ok, string what)
    {
        if (ok)
        {
            _pass++;
            Log.Info($"markers-send-test: PASS {what}");
        }
        else
        {
            _fail++;
            Log.Error($"markers-send-test: FAIL {what}");
        }
        return ok;
    }

    private void Info(string s) => Log.Info($"markers-send-test: INFO {s}");
}
