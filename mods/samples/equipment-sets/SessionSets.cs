// SPDX-License-Identifier: MIT
// Equipment sets: a player's 20 equipment slots changed in one transaction (docs/kiln/services/items.md, "Equipment sets").
//   /set remember <name>   keeps what you wear now, all 20 slots (empty ones too), for this session
//   /set apply <name>      puts the set on again: every slot changes or none does
//   /set check <name>      the same as a dry run: the transaction runs and is rolled back, nothing changes
//   /set list              your sets
// What to take from it:
//   - a set is for the current session only. Its scope names this process, this server world and this life of the
//     player; after a restart, in another world or after the player rejoined it comes back StaleSnapshot and nothing
//     changes. So keep sets in memory, forget a player's when they leave, and never save them;
//   - EquipSet is one request: one PlayerEquippingEvent for the whole set (a mod can veto it), one transaction, one
//     result. A refusal (no room, an item that is gone, a form Kiln does not run yet) is a result with a reason;
//   - the player's own actions between remember and apply are fine: the apply takes a snapshot at that moment and
//     plans from it. Slots a set leaves out stay as they are;
//   - nothing happens to a player's gear unless they ask: no set is applied at load or at join.
using Kiln;

namespace SessionSets;

public sealed class SessionSetsMod : Mod
{
    private const string Usage = "remember <name>, apply <name>, check <name> or list";
    private const int MaxSets = 8, MaxName = 24;
    // player entity -> name -> set. For this session only: a player's sets go when they leave, all of them at unload.
    private readonly Dictionary<EntityId, Dictionary<string, EquipmentSet>> _sets = [];
    private IDisposable? _left;

    public override void OnLoad() => _left = Events.On<PlayerLeftEvent>(e => _sets.Remove(e.Player.Entity));

    public override void OnUnload()
    {
        _left?.Dispose();
        _sets.Clear();  // session-only: nothing is kept for the next world or start
    }

    [Command("set", Description = "remember <name>, apply <name>, check <name> or list: your 20 equipment slots, kept for this session and put on again in one go")]
    private async Task Set(CommandContext c, string action, string? name = null)
    {
        if (!c.Source.IsPlayer) throw new CommandException("only a player wears things");
        if (!Capabilities.Is("items.equip_sets")) throw new CommandException($"this server cannot do that: {Capabilities.Why("items.equip_sets")}");
        var player = c.Source.Player;
        var mine = _sets.TryGetValue(player, out var have) ? have : _sets[player] = [];
        try
        {
            switch (action.ToLowerInvariant())
            {
                case "list":
                    c.Reply(mine.Count == 0 ? "you have no sets in this session" : $"your sets: {string.Join(", ", mine.Keys)}");
                    return;
                case "remember":
                    if (string.IsNullOrWhiteSpace(name) || name.Length > MaxName) throw new CommandException($"a set needs a name of 1 to {MaxName} characters");
                    if (!mine.ContainsKey(name) && mine.Count >= MaxSets) throw new CommandException($"you have {MaxSets} sets already");
                    var captured = Items.CaptureSet(player);
                    if (!captured.Ok) throw new CommandException($"your equipment could not be read: {captured.Reason}");
                    mine[name] = captured.Set!;
                    c.Reply($"set {name}: {captured.Set!.Entries.Count(e => e.Selection is not EmptyEquipment)} slot(s) kept for this session");
                    return;
                case "apply":
                case "check":
                    if (name is null || !mine.TryGetValue(name, out var set)) throw new CommandException($"you have no set {name}");
                    var dry = action.Equals("check", StringComparison.OrdinalIgnoreCase);
                    var r = await Items.EquipSet(player, set, dry ? EquipOptions.DryRun : EquipOptions.None);
                    Log.Info($"set {name}: {r.Status}; dry={r.DryRun}, committed={r.Committed}, reason={r.Reason}");
                    c.Reply(r.Status switch
                    {
                        EquipmentSetStatus.Done => $"set {name} is on ({r.Changes.Count} slot(s) changed)",
                        EquipmentSetStatus.NoChange => $"you wear set {name} already",
                        EquipmentSetStatus.Simulated => $"set {name} would go on",
                        EquipmentSetStatus.StaleSnapshot => $"set {name} is from another session or the items moved: remember it again",
                        _ => $"set {name} was not put on: {r.Reason}",
                    });
                    return;
                default:
                    throw new CommandException(Usage);
            }
        }
        catch (KilnUnavailableException e)
        {
            throw new CommandException($"this server cannot do that right now: {e.Message}");
        }
    }
}
