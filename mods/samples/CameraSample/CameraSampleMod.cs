// SPDX-License-Identifier: MIT
// Since mod API 0.2.17. Mode commands run in the SERVER world. Local console commands on a hosting client post to
// the CLIENT world; they are not remote requests from a dedicated server. No game functions, offsets or aim writes.
using Kiln;

namespace CameraSample;

public sealed class CameraSampleMod : Mod
{
    // Server-thread state, separate from the local handlers' client-thread state below.
    private readonly Dictionary<EntityId, CameraLease> _modes = [];
    private CameraLease _local;
    private Task<CameraResult>? _pending;
    private bool _ending;

    public override void OnLoad()
    {
        Client.Tick += OnClientTick;
        Camera.Changed += OnChanged;  // a lease that ends by itself (its time is up, the world stopped) is forgotten
    }

    // In the lease's world step (or, for WorldStopped, on the thread that stops the world: no game access there, only this
    // mod's own fields). The handler goes with the mod.
    private void OnChanged(CameraChange change)
    {
        if (change.Kind != CameraChangeKind.Ended) return;
        if (change.Lease.Endpoint == CameraEndpoint.Local)
        {
            if (change.Lease.Value == _local.Value) _local = default;
            return;
        }
        lock (_modes)
            foreach (var (player, lease) in _modes)
                if (lease.Value == change.Lease.Value)
                {
                    _modes.Remove(player);
                    break;
                }
    }
    public override void OnUnload()
    {
        Client.Tick -= OnClientTick;
        // The framework's owner-retiring barrier cancels pending work and releases both worlds' leases. Calling a
        // client End here would be wrong: unload need not run on the client world's thread.
    }

    [Command("camera-mode", Permission = CommandPermission.Admin, Description = "Five seconds of stock first-person for you")]
    private async Task Mode(CommandContext c)
    {
        if (!c.Source.IsPlayer) throw new CommandException("a player must request their own stock mode");
        var result = await Camera.SetMode(c.Source.Player, CameraMode.PlayerCameraBuildingFirstPerson, TimeSpan.FromSeconds(5));
        if (result.Ok)
            lock (_modes) _modes[c.Source.Player] = result.Lease;
        c.Reply(result.Ok ? "Stock mode accepted (ServerState, not a visual ACK)." : $"Refused: {result.ReasonCode}: {result.Reason}");
    }

    [Command("camera-mode-off", Permission = CommandPermission.Admin, Description = "End this sample's stock-mode lease")]
    private async Task ModeOff(CommandContext c)
    {
        CameraLease lease;
        bool held;
        lock (_modes) held = _modes.TryGetValue(c.Source.Player, out lease);
        if (!c.Source.IsPlayer || !held)
            throw new CommandException("this sample holds no stock-mode lease for you");
        var result = await Camera.End(lease);
        if (result.Ok)
            lock (_modes) _modes.Remove(c.Source.Player);
        c.Reply(result.Ok ? "Stock mode released." : $"Refused: {result.ReasonCode}: {result.Reason}");
    }

    [Command("camera-lens", Permission = CommandPermission.Admin, Description = "Local console: show a 40-degree lens")]
    private void Lens(CommandContext c) => PostLocal(c, () => StartLocal(LensOnly()));

    [Command("camera-path", Permission = CommandPermission.Admin, Description = "Local console: move the visual camera two metres")]
    private void Path(CommandContext c) => PostLocal(c, () =>
    {
        var view = Camera.GetVisual();
        if (!view.Ok || view.Snapshot is null)
        {
            Log.Warn($"Camera sample: {view.ReasonCode}: {view.Reason}");
            return;
        }
        StartLocal(TwoKeyPath(view.Snapshot.Pose));
    });

    [Command("camera-local-off", Permission = CommandPermission.Admin, Description = "Local console: end the sample's visual lease")]
    private void LocalOff(CommandContext c) => PostLocal(c, EndLocal);

    private void PostLocal(CommandContext c, Action work)
    {
        if (!c.Source.IsHostConsole || !Client.IsHosting)
            throw new CommandException("use the local console of a hosting game client, not server chat or a dedicated server");
        Client.Post(() =>
        {
            try { work(); }
            catch (Exception e) { Log.Warn($"Camera sample: {e.Message}"); }
        });
        c.Reply("Queued for the client world's next step; the result is logged, not assumed.");
    }

    // Lens alone does not need camera.local_pose or a player transform sample.
    private static CameraOverride LensOnly() => new(Lens: new CameraLens(40), Duration: TimeSpan.FromSeconds(5));

    // Explicit poses; no look-at target or collision query is deferred into the render callback. Use a fresh visual
    // pose near this player: the native backend still checks every key and derived position against its 250 m bound.
    private static CameraOverride TwoKeyPath(CameraPose start) => new(
        Path: new CameraPath([
            new CameraKey(TimeSpan.Zero, start),
            new CameraKey(TimeSpan.FromSeconds(2), new CameraPose(
                new WorldPosition(start.Position.X + 2, start.Position.Y, start.Position.Z), start.Rotation)),
        ]),
        Duration: TimeSpan.FromSeconds(3), Blend: new CameraBlend(TimeSpan.FromMilliseconds(200), TimeSpan.FromMilliseconds(300)));

    // Client-thread handlers. The fake tests invoke these on the loaded mod's harness thread: the kit does not fake
    // Client.Post. Keeping the mutation here (not in a server command) matters when the two world threads differ.
    private void StartLocal(CameraOverride request)
    {
        CompleteLocal();
        if (_pending is not null || _local.Value != 0)
        {
            Log.Warn("Camera sample: end the current local lease first (camera-local-off).");
            return;
        }
        _ending = false;
        _pending = Camera.SetLocal(request);
    }

    private void EndLocal()
    {
        CompleteLocal();
        if (_pending is not null) { Log.Warn("Camera sample: a local operation still waits."); return; }
        if (_local.Value == 0) return;
        _ending = true;
        _pending = Camera.End(_local);
    }

    private void OnClientTick(in TickInfo _) => CompleteLocal();

    private void CompleteLocal()
    {
        if (_pending is not { IsCompleted: true } task) return;
        _pending = null;
        if (!task.IsCompletedSuccessfully)
        {
            Log.Warn($"Camera sample: operation cancelled or faulted: {task.Exception?.GetBaseException().Message}");
            return;
        }
        var result = task.Result;
        if (_ending && result.Ok) _local = default;
        else if (!_ending && (result.Ok || result.MayHaveApplied)) _local = result.Lease;
        Log.Info(result.Ok ? $"Camera sample: {(_ending ? "released" : "applied")} ({result.CommitScope})."
            : $"Camera sample: {result.ReasonCode}: {result.Reason}; MayHaveApplied={result.MayHaveApplied}.");
    }
}
