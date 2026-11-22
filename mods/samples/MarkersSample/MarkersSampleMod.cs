// SPDX-License-Identifier: MIT
using System.Text.Json;
using Kiln;

namespace MarkersSample;

/// <summary>
/// Kiln.Markers on a client, in three parts:
///  - fixed markers from <c>markers.json</c> (the same shape as <see cref="MarkerSpec"/>), set in every world once the
///    icon catalog is ready (markers are session only: they end with the world);
///  - Ctrl+Shift+M drops a pin where your character stands (compass and a world label, no map icon: no catalog needed);
///  - Ctrl+Alt+M puts a label over your own character that follows it (<see cref="Markers.SetNameplate"/>; the same as
///    <see cref="Markers.CaptureAnchorAsync"/> and a spec with <c>Follow</c>).
/// Visibility changes are written to the log, so you can see why a marker is not shown.
/// </summary>
public sealed class MarkersSampleMod : Mod
{
    private uint _appliedSession;
    private int _pins;

    public override void OnLoad()
    {
        if (!Capabilities.Is("markers.local"))
        {
            Log.Info($"markers-sample: no markers here: {Capabilities.Why("markers.local")}");
            return;
        }
        Markers.CatalogChanged += e => Log.Info($"markers-sample: icon catalog ready={e.Ready} ({e.Count} icons) {e.Reason}");
        Markers.VisibilityChanged += e =>
            Log.Info($"markers-sample: {e.Id}: {e.After} of {e.Requested} (map {e.Map}, compass {e.Compass}, world {e.World})");
        Client.Tick += OnTick;
        KeyBindings.Register("markers-sample.pin", "Ctrl+Shift+M", DropPin, "Drop a marker where your character stands");
        KeyBindings.Register("markers-sample.nameplate", "Ctrl+Alt+M", () => _ = Nameplate(), "A label over your character that follows it");
    }

    // Once per client world: the file's markers (map icons need the catalog; Icons.IsReady says when it is there).
    private void OnTick(in TickInfo _)
    {
        var session = Client.ClientWorldSession;
        if (session == 0 || session == _appliedSession || !Client.HasLocalPlayer || !Markers.Icons.IsReady) return;
        _appliedSession = session;
        using var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(Context.Info.Directory, "markers.json")));
        foreach (var item in doc.RootElement.EnumerateArray())
        {
            var spec = MarkerSpec.FromJson(item);
            var r = Markers.Set(spec);
            Log.Info($"markers-sample: {spec.Id}: {(r.Ok ? "set" : r.Reason)}");
        }
    }

    private void DropPin()
    {
        var cam = Render.Camera;  // its pivot is about the character's head
        if (!Client.HasLocalPlayer || !cam.Valid)
        {
            Hud.Toast("Join or host a world first", ToastKind.Warning);
            return;
        }
        var at = cam.Pivot with { Y = cam.Pivot.Y - 1.6 };
        var id = $"pin{++_pins}";
        var r = Markers.Set(new MarkerSpec(id, at, $"Pin {_pins}", MarkerIcon.None, MarkerSurface.Compass | MarkerSurface.World,
            Height: 1.5f, Rgba: 0xffb030ff));
        Hud.Toast(r.Ok ? $"{id} dropped" : $"{id}: {r.Reason}", r.Ok ? ToastKind.Info : ToastKind.Warning);
    }

    private async Task Nameplate()
    {
        if (!Capabilities.Is("markers.nameplates"))
        {
            Hud.Toast(Capabilities.Why("markers.nameplates"), ToastKind.Warning);
            return;
        }
        var r = await Markers.SetNameplate("me", Client.LocalPlayer, Context.Info.Name);
        Hud.Toast(r.Ok ? "a label follows you now" : $"nameplate: {r.Reason}", r.Ok ? ToastKind.Info : ToastKind.Warning);
    }
}
