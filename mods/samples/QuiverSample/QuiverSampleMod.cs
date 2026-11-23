// SPDX-License-Identifier: MIT
// Quiver sample: player storage (docs/kiln/modding/player-storage.md, since 0.2.0-dev.30.7, GitHub issues 127 and 128).
//
// Two storages every player gets, kept with the character by Kiln (world saves, rejoins, other worlds, the mod off):
//   quiversample:quiver   18 slots, arrows and spells only, follows the game's tombstone rule at death
//   quiversample:satchel   8 slots, anything (gear keeps its level, durability, dye and gem)
// Commands (server; the storage is the sender's own, so every command checks that it comes from a player):
//   /quiver                  both storages, slot by slot
//   /quiver store [stacks]   ammunition from your backpack into the quiver (3 stacks unless you say)
//   /quiver take <slot>      a quiver stack back into your backpack
//   /quiver use <slot>       equips a quiver stack where it is (Items.Equip with the storage slot: nothing moves)
//   /quiver activate <slot>  swaps a quiver stack into your backpack and equips it there (PlayerStorage.Activate)
//   /quiver stash <name>     the first backpack item whose name contains <name> into the satchel
//   /quiver unstash <slot>   a satchel item back into your backpack
// And the quiver's stacks in the HUD's ammunition and spell lists: PlayerStorage.Client.ProvideAmmo, made on both sides.
// On a player's game it shows the rows from Kiln's copy of their storages; on the server it takes the player's pick of
// such a row, checks it and equips the stack. What to take from it:
//   - register in OnLoad on every side that may run a world (a dedicated server, a game that can host); a game that only
//     joins others' worlds has no storage of its own, and its client half needs only the full key;
//   - a storage's items are real game items: move them with Kiln.Items (Store, Retrieve, Items.Move); never by hand;
//   - every command or message from a player's game is a wish: the storage and slot are the sender's own here because
//     PlayerStorage answers per player, never because the game said so.
using Kiln;

namespace QuiverSample;

public sealed class QuiverSampleMod : Mod
{
    // "<mod id>:<key>": the full key, the storage's name in saves and on the player's game
    private const string QuiverKey = "quiversample:quiver";
    private const int MaxStore = 18;

    private PlayerStorage? _quiver, _satchel;
    private PlayerStorageAmmoProvider? _ammo;

    public override void OnLoad()
    {
        // The server half: the storages. Where player storage is off (a game build Kiln does not know, a game that only
        // joins others' worlds) there is nothing to register; the reason says why.
        try
        {
            _quiver = Items.RegisterPlayerStorage("quiver", 18, StorageFilter.Ammunition(AmmoKinds.Arrow | AmmoKinds.Spell), StorageDeath.FollowGame);
            _satchel = Items.RegisterPlayerStorage("satchel", 8, StorageFilter.Any);
            Log.Info($"quiversample: {_quiver}; {_satchel}");
        }
        catch (KilnUnavailableException e)
        {
            Log.Info($"quiversample: no player storage here: {e.Message}");
        }

        // Both halves of the HUD lists in one call: on a player's game the quiver's arrows and spells as rows, on the
        // server the check and equip of a picked row.
        try
        {
            _ammo = PlayerStorage.Client.ProvideAmmo(QuiverKey, AmmoKinds.Arrow | AmmoKinds.Spell);
            Log.Info($"quiversample: {_ammo}");
        }
        catch (KilnUnavailableException e)
        {
            Log.Info($"quiversample: no HUD rows for the quiver: {e.Message}");
        }

        // What Kiln did with the storages, for kiln.log (the owner's in-game check reads these lines).
        Events.On<PlayerStorageRestoredEvent>(e => { if (Mine(e.Key)) Log.Info($"quiversample: {e}"); });
        Events.On<PlayerStorageParkedEvent>(e => { if (Mine(e.Key)) Log.Info($"quiversample: {e}"); });
        Events.On<PlayerStorageDeathEvent>(e => { if (Mine(e.Key)) Log.Info($"quiversample: {e}"); });
    }

    public override void OnUnload() => _ammo?.Dispose();  // Kiln ends it with the mod anyway

    private static bool Mine(string key) => key.StartsWith("quiversample:", StringComparison.Ordinal);

    // ---- commands (server) ----

    [Command("quiver", Description = "your quiver and satchel; subcommands: store, take, use, activate, stash, unstash")]
    private string Show(CommandContext c)
    {
        var player = Player(c);
        return Describe(Quiver(), player) + "\n" + Describe(Satchel(), player);
    }

    [Command("quiver store", Description = "[stacks]: moves ammunition from your backpack into the quiver (3 stacks unless you say)")]
    private async Task Store(CommandContext c, int stacks = 3)
    {
        var player = Player(c);
        var quiver = Ready(Quiver(), player);
        stacks = Math.Clamp(stacks, 1, MaxStore);
        // Read the backpack once; each Store is its own move, so a stack the player moved meanwhile is refused, not lost.
        var ammo = (Items.Read(player)?.Slots ?? [])
            .Where(s => !s.IsEmpty && !s.IsEquipped && s.Category == InventoryCategory.Generic &&
                        Items.Get(s.Item)?.Category == ItemCategory.Ammunition)
            .Take(stacks).ToList();
        if (ammo.Count == 0) throw new CommandException("no ammunition in your backpack");
        var stored = 0;
        foreach (var s in ammo)
        {
            var r = await quiver.Store(player, s.Slot);
            if (r.Succeeded) stored++;
            else c.Reply($"{Name(s.Item)} stays in your backpack: {r.Status} {r.Message}");
        }
        c.Reply($"{stored} stack(s) into the quiver");
    }

    [Command("quiver take", Description = "<slot>: a quiver stack back into your backpack")]
    private async Task<string> Take(CommandContext c, int slot)
    {
        var player = Player(c);
        var r = await Ready(Quiver(), player).Retrieve(player, Slot(Quiver(), slot));
        return r.Succeeded ? $"{r.Done} item(s) back in your backpack" : $"nothing moved: {r.Status} {r.Message}";
    }

    [Command("quiver use", Description = "<slot>: equips a quiver stack where it is (nothing moves)")]
    private async Task<string> Use(CommandContext c, int slot)
    {
        var player = Player(c);
        var quiver = Ready(Quiver(), player);
        var at = quiver.Slot(player, Slot(quiver, slot));
        var item = quiver.Read(player)?.Slots.FirstOrDefault(s => s.Slot == at);
        if (item is null || item.IsEmpty) throw new CommandException($"quiver slot {slot} holds nothing");
        // no equipment slot named: the item goes to its own (arrows to the arrow slot, a spell to the spell slot)
        var r = await Items.Equip(player, at, null, EquipOptions.None, item.ItemEntity);
        return r.Ok ? $"{Name(item.Item)} equipped from the quiver" : $"not equipped: {r.Reason}";
    }

    [Command("quiver activate", Description = "<slot>: swaps a quiver stack into your backpack and equips it there")]
    private async Task<string> Activate(CommandContext c, int slot)
    {
        var player = Player(c);
        var r = await Ready(Quiver(), player).Activate(player, Slot(Quiver(), slot));
        return r.Ok ? $"{Name(r.Item)} is your active ammunition (from the backpack)" : $"not activated: {r.Reason}";
    }

    [Command("quiver stash", Description = "<name>: the first backpack item whose name contains <name> into the satchel")]
    private async Task<string> Stash(CommandContext c, [Greedy] string name)
    {
        var player = Player(c);
        var satchel = Ready(Satchel(), player);
        var found = (Items.Read(player)?.Slots ?? [])
            .FirstOrDefault(s => !s.IsEmpty && !s.IsEquipped && s.Category == InventoryCategory.Generic &&
                                 Name(s.Item).Contains(name, StringComparison.OrdinalIgnoreCase));
        if (found is null) throw new CommandException($"nothing called '{name}' in your backpack");
        var r = await satchel.Store(player, found.Slot);
        return r.Succeeded ? $"{Name(found.Item)} is in the satchel" : $"{Name(found.Item)} stays: {r.Status} {r.Message}";
    }

    [Command("quiver unstash", Description = "<slot>: a satchel item back into your backpack")]
    private async Task<string> Unstash(CommandContext c, int slot)
    {
        var player = Player(c);
        var r = await Ready(Satchel(), player).Retrieve(player, Slot(Satchel(), slot));
        return r.Succeeded ? "back in your backpack" : $"nothing moved: {r.Status} {r.Message}";
    }

    // ---- helpers ----

    private static EntityId Player(CommandContext c) =>
        c.Source.IsPlayer ? c.Source.Player : throw new CommandException("only a player has a quiver");

    private PlayerStorage Quiver() => _quiver ?? throw new CommandException("player storage is off on this server");
    private PlayerStorage Satchel() => _satchel ?? throw new CommandException("player storage is off on this server");

    // Ready: restored for this player (Kiln restores at the join; parked or locked storages stay as they are).
    private static PlayerStorage Ready(PlayerStorage s, EntityId player) =>
        s.IsReady(player) ? s : throw new CommandException($"your {s.Key} is not ready: {s.Status(player).State}");

    private static int Slot(PlayerStorage s, int slot) =>
        slot >= 0 && slot < s.Slots ? slot : throw new CommandException($"{s.Key} slots are 0 to {s.Slots - 1}");

    private static string Describe(PlayerStorage s, EntityId player)
    {
        var status = s.Status(player);
        if (!status.Restored) return $"{s.Key}: {status.State}" + (status.Parked > 0 ? $", {status.Parked} stack(s) parked" : "");
        var rows = new List<string>();
        var contents = s.Read(player);
        for (var i = 0; contents is not null && i < s.Slots; i++)
        {
            var at = s.Slot(player, i);  // slot i: page i / 8, cell i % 8
            if (contents.Slots.FirstOrDefault(x => x.Slot == at) is { IsEmpty: false } item)
                rows.Add($"  {i}: {Name(item.Item)} x{item.Count}" + (item.IsGear ? $" (level {item.Level})" : ""));
        }
        var head = $"{s.Key} ({s.Slots} slots, {(status.Paired ? "paired" : "unpaired")}, from {status.Source})";
        return rows.Count == 0 ? head + ": empty" : head + ":\n" + string.Join("\n", rows);
    }

    private static string Name(ItemId item) => Items.Get(item)?.Label ?? item.ToString();
}
