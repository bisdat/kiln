// SPDX-License-Identifier: MIT
using Kiln;

namespace GemsClientCheck;

/// <summary>
/// The gem system's real-client check (docs/kiln/services/gems.md, "Real-client checks"), with GemSample and GemEffects.
/// Where the world runs (server or hosting client): sockets on rings and armour (<see cref="Gems.AllowSockets"/>, no
/// rolls), every GemCraftingEvent, GemCraftedEvent, GemSocketChangedEvent and GemTriggeredEvent in kiln.log as
/// <c>gemcheck: event ...</c>, and <c>/gemcheck</c> (admins):
/// <list type="bullet">
/// <item><c>rings</c>: a Ring of Shock with an empty socket, one made by <see cref="Gems.AddSocket"/> (its result), and a
/// plain Ring of Light (never listed by the forge).</item>
/// <item><c>essence [n]</c>: Shroud essence for the forge's prices (default 500).</item>
/// <item><c>equipped</c>, <c>sockets</c>, <c>catalog</c>: what <see cref="Gems.Equipped"/>, <see cref="Gems.Socket(ItemInstanceRef)"/>
/// on each backpack item and <see cref="Gems.Catalog"/> say.</item>
/// <item><c>cancel</c>: the next Recycle a player starts at a forge is canceled (GemCraftingEvent).</item>
/// <item><c>upgrade</c>: a code Upgrade of the gem in a Ring of Shock in the backpack, at the nearest
/// forge (run it from kiln/control/console while the forge screen is open: its lists must refresh).</item>
/// <item><c>return</c>: <see cref="Gems.ReturnSocketedGems"/> for you.</item>
/// </list>
/// </summary>
public sealed class GemsClientCheckMod : Mod
{
    private const uint RingLight = 0x79442800;  // Ring_T3_Ring_of_Light
    private const uint RingShock = 0x4623bcb0;  // Ring_T6_Ring_of_Shock
    private const string Essence = "Material_T1_Fognest_Ressource";

    private bool _cancelRecycle;
    private readonly List<IDisposable> _subs = new();

    private void Say(string s) => Log.Info($"gemcheck: {s}");

    public override void OnLoad()
    {
        foreach (var row in new[] { "gems.read", "gems.triggers", "gems.craft", "gems.sockets", "ui.gemforge.inspect" })
            Say($"{row} = {(Capabilities.Is(row) ? "on" : "off")} ({Capabilities.Why(row)})");
        if (Capabilities.Is("gems.sockets"))
            Try("AllowSockets", () => Say($"AllowSockets(Rings | Armour): {Gems.AllowSockets(GemSocketKinds.Rings | GemSocketKinds.Armour)}"));
        Try("GemCraftingEvent", () => _subs.Add(Events.On<GemCraftingEvent>(e =>
        {
            Say($"event: {e}");
            if (_cancelRecycle && e.Action == GemCraftAction.Recycle && !e.Player.IsNone)
            {
                _cancelRecycle = false;
                e.Cancel();
                Say("canceled that Recycle (nothing is paid, nothing moves)");
            }
        })));
        Try("GemCraftedEvent", () => _subs.Add(Events.On<GemCraftedEvent>(e => Say($"event: {e}"))));
        Try("GemSocketChangedEvent", () => _subs.Add(Events.On<GemSocketChangedEvent>(e => Say($"event: {e}"))));
        Try("GemTriggeredEvent", () => _subs.Add(Events.On<GemTriggeredEvent>(e => Say($"event: {e}"))));
        Try("commands", () => _subs.Add(Commands.Register("gemcheck", c => _ = Run(c), CommandPermission.Admin,
            usage: "rings | essence [n] | equipped | sockets | catalog | cancel | upgrade | return",
            description: "the gem system's real-client check")));
    }

    public override void OnUnload()
    {
        foreach (var s in _subs) s.Dispose();
        _subs.Clear();
    }

    private void Try(string what, Action a)
    {
        try { a(); }
        catch (Exception e) { Say($"{what} not here: {e.Message}"); }
    }

    private async Task Run(CommandContext c)
    {
        var who = c.Source.Player;
        if (who.IsNone)
        {
            c.Reply("run it as a player");
            return;
        }
        void Out(string s)
        {
            c.Reply(s);
            Say(s);
        }
        try
        {
            switch (c.Count > 0 ? c.Args[0].ToLowerInvariant() : "")
            {
                case "rings":
                {
                    var socketed = await Items.Give(who, new ItemId(RingShock), 1, new Gear(20, ItemRarity.Rare) { GemSlot = true }, GiveOptions.NoMerge);
                    Out($"a Ring of Shock with an empty socket: {socketed.Status}");
                    var plain = await Items.Give(who, new ItemId(RingShock), 1, new Gear(20, ItemRarity.Epic) { Unlocked = 2 }, GiveOptions.NoMerge);
                    await Context.Scheduler.NextTick();
                    await Context.Scheduler.NextTick();
                    var slot = plain.Changes.Count > 0 ? SlotOf(who, plain.Changes[0].ItemEntity) : null;
                    if (slot is null) Out($"a plain Ring of Shock for AddSocket: {plain.Status} {plain.Message}");
                    else
                    {
                        var add = await Gems.AddSocket(slot.Ref);
                        Out($"AddSocket on an epic Ring of Shock (level 20, 2 upgrades): {add} (missing: {string.Join(", ", add.Missing)})");
                    }
                    var none = await Items.Give(who, new ItemId(RingLight), 1, null, GiveOptions.NoMerge);
                    Out($"a Ring of Light without a socket: {none.Status}");
                    break;
                }
                case "essence":
                {
                    var n = c.Count > 1 && int.TryParse(c.Args[1], out var x) ? Math.Clamp(x, 1, 5000) : 500;
                    Out($"essence x{n}: {await Items.Give(who, Essence, n)}");
                    break;
                }
                case "equipped":
                {
                    var eq = Gems.Equipped(who);
                    Out($"Gems.Equipped: {eq.Count}");
                    foreach (var g in eq) Out($"  {g}");
                    break;
                }
                case "sockets":
                {
                    foreach (var s in Items.Read(who)?.Slots.Where(s => !s.IsEmpty) ?? [])
                        if (Gems.Socket(s.Ref) is { } sg) Out($"  {s.Item} {(s.IsEquipped ? "(worn)" : "")}: {sg}");
                    break;
                }
                case "catalog":
                {
                    var all = Gems.Catalog;
                    Out($"Gems.Catalog: {all.Count} gems, {all.Count(g => g.FromMod)} from mods");
                    foreach (var g in all.Where(g => g.FromMod)) Out($"  {g}");
                    break;
                }
                case "cancel":
                    _cancelRecycle = true;
                    Out("the next Recycle a player starts at a forge is canceled");
                    break;
                case "upgrade":
                {
                    // the Ring of Shock /gemcheck rings gave, with a gem in it
                    var carrier = Items.Read(who)?.Slots.FirstOrDefault(s => !s.IsEmpty && !s.IsEquipped && s.Item == new ItemId(RingShock)
                        && Gems.Socket(s.Ref) is not null);
                    if (carrier is null) { Out("no Ring of Shock with a gem in the backpack"); break; }
                    var forge = NearestForge(who);
                    if (forge.IsNone) { Out("no Shroud Forge near you"); break; }
                    Out($"code Upgrade of {carrier.Item} at forge {forge}: {await Gems.Upgrade(who, carrier.Ref, forge)}");
                    break;
                }
                case "return":
                {
                    var back = await Gems.ReturnSocketedGems(who);
                    Out($"ReturnSocketedGems: {back.Count}");
                    foreach (var r in back) Out($"  {r}");
                    break;
                }
                default:
                    Out("/gemcheck rings | essence [n] | equipped | sockets | catalog | cancel | upgrade | return");
                    break;
            }
        }
        catch (Exception e)
        {
            Out($"failed: {e.GetType().Name}: {e.Message}");
        }
    }

    private static InventorySlot? SlotOf(EntityId holder, EntityId pide) =>
        Items.Read(holder)?.Slots.FirstOrDefault(s => !s.IsEmpty && s.ItemEntity == pide);

    private static EntityId NearestForge(EntityId player)
    {
        if (World.Position(player) is not { } at) return EntityId.None;
        var best = EntityId.None;
        var bestD = 30.0 * 30.0;
        foreach (var id in Spawn.FindByName("Gem_Forge", max: 64))
        {
            if (World.Position(id) is not { } p) continue;
            var d = (p.X - at.X) * (p.X - at.X) + (p.Y - at.Y) * (p.Y - at.Y) + (p.Z - at.Z) * (p.Z - at.Z);
            if (d < bestD) { bestD = d; best = id; }
        }
        return best;
    }
}
