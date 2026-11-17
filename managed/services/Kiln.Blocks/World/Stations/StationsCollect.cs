// SPDX-License-Identifier: MIT
// Stations.Collect: a station's finished items into a chest through Kiln.Items' move (the game's slot transfer).
namespace Kiln;

internal static partial class StationsCore
{
    // ---- collect (Kiln.Items' move, one output stack after the other) ----
    internal static async Task<StationResult> Collect(EntityId station, ItemId? item, Func<ItemId, EntityId?> target)
    {
        if (station.IsNone) throw new ArgumentException("no station", nameof(station));
        var info = Stations.Read(station);
        if (info is null) return Refusal(StationActionStatus.NotAStation, StationAction.Collect, station, 0, $"entity {station} is no production station");
        var outs = info.Outputs.Where(s => item is null || s.Item == item).ToArray();
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
}
