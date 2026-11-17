// SPDX-License-Identifier: MIT
// Stations.Collect: a station's finished items into a chest through Kiln.Items' move (the game's slot transfer).
namespace Kiln;

internal static partial class StationsCore
{
    // ---- collect (Kiln.Items' move, one output stack after the other) ----
    internal static async Task<StationResult> Collect(EntityId station, ItemId? item, CollectWhat what, Func<ItemId, EntityId?> target,
        bool toBase = false, EntityId chest = default)
    {
        if (station.IsNone) throw new ArgumentException("no station", nameof(station));
        if ((what & CollectWhat.All) == 0) throw new ArgumentException("collect what: Outputs, Leftovers or All", nameof(what));
        if (Backend.HasCollect) return await CollectNative(station, item, what, toBase, chest);
        var info = Stations.Read(station);
        if (info is null) return Refusal(StationActionStatus.NotAStation, StationAction.Collect, station, 0, $"entity {station} is no production station");
        var pick = ((what & CollectWhat.Outputs) != 0 ? info.Outputs : []).Concat((what & CollectWhat.Leftovers) != 0 ? info.Leftovers : []);
        var outs = pick.Where(s => item is null || s.Item == item).ToArray();
        if (outs.Length == 0)
            return new StationResult(StationActionStatus.Unchanged, StationAction.Collect, station, info.SelectedRecipe, 0, 0, 0, 0, 0, [], info.State,
                info.State, false, "nothing to collect");
        var changes = new List<MoveChange>();
        int moved = 0, refused = 0;
        string last = "";
        foreach (var s in outs)
        {
            var to = target(s.Item);
            if (to is not { IsNone: false } dest)
            {
                refused++;
                last = $"no container with room for {s.Item}";
                continue;
            }
            var m = await Items.Move(station, s.Slot, dest);
            changes.AddRange(m.Changes);
            if (m.Succeeded) moved++;
            if (m.Status != ItemsStatus.Done)
            {
                if (!m.Succeeded) refused++;
                last = m.Message.Length > 0 ? m.Message : m.Status.ToString();
            }
        }
        var status = refused == 0 ? StationActionStatus.Done : moved > 0 ? StationActionStatus.Partial : StationActionStatus.NoRoom;
        var result = new StationResult(status, StationAction.Collect, station, info.SelectedRecipe, 0, 0, 0, 0, 0, changes, info.State, info.State,
            false, status == StationActionStatus.Done ? "" : last);
        if (result.Changed) Changed(result, ModContext.CurrentMod);
        return result;
    }

    // The native collect is the same rules (which stacks, which chests, the result counts) through kiln.items' move.
    private static Task<StationResult> CollectNative(EntityId station, ItemId? item, CollectWhat what, bool toBase, EntityId chest)
    {
        var flags = ((what & CollectWhat.Outputs) != 0 ? StationsNative.CollectOutputs : 0u) |
                    ((what & CollectWhat.Leftovers) != 0 ? StationsNative.CollectLeftovers : 0u) |
                    (toBase ? StationsNative.CollectToBase : 0u);
        var request = new StationsNative.Collect
        {
            Size = (uint)System.Runtime.InteropServices.Marshal.SizeOf<StationsNative.Collect>(),
            Station = station.Value,
            To = toBase ? 0 : chest.Value,
            Item = item?.Value ?? 0,
            Flags = flags,
        };
        return SendCollect(request);
    }

    private static Task<StationResult> SendCollect(in StationsNative.Collect request)
    {
        var mod = ModContext.CurrentMod;
        if (mod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
        var tcs = new TaskCompletionSource<StationResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        long key;
        lock (Gate)
        {
            key = ++_next;
            InFlight[key] = (tcs, mod, StationAction.Collect);
        }
        StartPump();
        var owner = Services.CallerOwner;
        if (owner != 0 && mod is not null) lock (Gate) Owners[owner] = mod.Info.Id;
        var rc = Backend.Collect(request, owner, key);
        if (rc == StationsNative.Ok) return tcs.Task;
        lock (Gate) InFlight.Remove(key);
        var station = new EntityId(request.Station);
        if (rc == StationsNative.BufferTooSmall)
            return Task.FromResult(Refusal(StationActionStatus.QueueFull, StationAction.Collect, station, 0, "1024 station actions are waiting: try again in a step"));
        if (rc == StationsNative.InvalidArgument) throw new ArgumentException("Stations.Collect: kiln.stations refused the request's arguments");
        throw new KilnUnavailableException(Stations.ReadCapability, WhyOff(Backend.Status()));
    }
}
