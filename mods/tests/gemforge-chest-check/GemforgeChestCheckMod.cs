// SPDX-License-Identifier: MIT
using Kiln;

namespace GemforgeChestCheck;

/// <summary>
/// GitHub issue 61's real-client check (docs/kiln/ui-gemforge.md, "Real-client check"). Where the world runs (a dedicated
/// server or a hosting client): when a player's Shroud Forge screen opens (<see cref="GemForgeOpenedEvent"/>), the loose
/// gems of every storage chest within 20 m of the player are offered to that player, paid and returned as
/// <c>/gemchest pay</c> and <c>/gemchest return</c> say; every gem forge event goes into kiln.log as <c>gemchest: ...</c>.
/// <c>/gemchest</c> (admins):
/// <list type="bullet">
/// <item><c>setup</c>: a sword with an empty socket in your backpack, two gems (levels 2 and 3) and 300 essence in the
/// storage chest nearest to you (within 20 m).</item>
/// <item><c>pay player|chest|base|playerthenbase</c>: who pays an Insert or Upgrade of a chest gem (default chest).</item>
/// <item><c>return player|chest</c>: where a Recycle's essence goes (default chest).</item>
/// <item><c>off</c> / <c>on</c>: stop or resume offering (an open forge screen keeps the rows it has until they lapse, 30 s).</item>
/// <item><c>status</c>: the rows, the mode, and the chests near you with their gems and essence.</item>
/// </list>
/// On the Kiln client it only logs the capability rows and the forge screen's openings.
/// </summary>
public sealed class GemforgeChestCheckMod : Mod
{
    private const string Gem = "T1_Gem_GemOfBleeding", Sword = "Weapon_T1_1H_Sword_Craft", Essence = "Material_T1_Fognest_Ressource";
    private const double Radius = 20;
    private string _pay = "chest", _return = "chest";
    private bool _offering = true;
    private readonly List<IDisposable> _subs = new();

    private void Say(string s) => Log.Info($"gemchest: {s}");

    public override void OnLoad()
    {
        foreach (var row in new[] { Gems.CraftCapability, Gems.PayersCapability, Gems.SourcesCapability, "ui.gemforge.sources", "ui.gemforge.inspect", "ui.screens" })
            Say($"{row} = {(Capabilities.Is(row) ? "on" : "off")} ({Capabilities.Why(row)})");
        Try("GemForgeOpenedEvent", () => _subs.Add(Events.On<GemForgeOpenedEvent>(OnOpened)));
        Try("GemCraftingEvent", () => _subs.Add(Events.On<GemCraftingEvent>(e => Say($"event: {e}"))));
        Try("GemCraftedEvent", () => _subs.Add(Events.On<GemCraftedEvent>(e => Say($"event: {e}"))));
        Try("commands", () => _subs.Add(Commands.Register("gemchest", c => _ = Run(c), CommandPermission.Admin,
            usage: "setup | pay player|chest|base|playerthenbase | return player|chest | off | on | status",
            description: "chest gems at the Shroud Forge (issue 61 client check)")));
        // the client half: the forge screen's openings, next to Kiln's own lines (ui.gemforge.sources hooks installed ...)
        if (Client.IsClient)
            Try("screens", () => Ui.Screens.Changed += (from, to) =>
            {
                if (to.Hud == HudScreen.ShroudForge || from.Hud == HudScreen.ShroudForge)
                    Say($"client: forge screen {(to.Hud == HudScreen.ShroudForge ? "opened" : "closed")}; ui.gemforge.sources " +
                        $"{(Capabilities.Is("ui.gemforge.sources") ? "on" : "off")} ({Capabilities.Why("ui.gemforge.sources")})");
            });
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

    private GemPayer Payer(EntityId chest) => _pay switch
    {
        "player" => GemPayer.Player,
        "base" => GemPayer.BaseStorage,
        "playerthenbase" => GemPayer.PlayerThenBaseStorage,
        _ => GemPayer.From(chest),
    };

    private static IReadOnlyList<EntityId> ChestsNear(EntityId player)
    {
        if (World.Transform(player) is not { } t) return [];
        return Spawn.FindByName("Storage", t.Position, Radius, 32).Where(e => Items.Read(e) is { Initialized: true, IsPlayer: false, IsNpc: false }).ToArray();
    }

    private void OnOpened(GemForgeOpenedEvent e)
    {
        Say($"event: {e}");
        if (!_offering) return;
        foreach (var chest in ChestsNear(e.Player))
        {
            try
            {
                var o = Gems.OfferChestGems(e.Player, chest, new GemSourceOptions
                {
                    PayFrom = Payer(chest),
                    ReturnTo = _return == "chest" ? chest : EntityId.None,
                });
                if (!e.Refresh || !o.Ok)
                    Say($"offer chest {chest} to {e.Player} (pay {_pay}, return {_return}): {o.Refusal ?? $"{o.Gems.Count} gem(s)"} " +
                        string.Join(", ", o.Gems.Select(g => $"{g.Slot} L{g.Level}")));
            }
            catch (Exception x) when (x is KilnUnavailableException or ArgumentException or InvalidOperationException)
            {
                Say($"offer chest {chest}: {x.Message}");
            }
        }
    }

    private async Task Run(CommandContext c)
    {
        var who = c.Source.Player;
        if (who.IsNone && Players.All is [var only]) who = only.Entity;
        if (who.IsNone)
        {
            c.Reply("run it as a player (or from the console with exactly one player in the world)");
            return;
        }
        void Out(string s)
        {
            c.Reply(s);
            Say(s);
        }
        var n = c.Count;
        string Arg(int i) => i < n ? c.Args[i].ToLowerInvariant() : "";
        switch (n > 0 ? Arg(0) : "status")
        {
            case "setup":
            {
                var chest = ChestsNear(who).FirstOrDefault();
                if (chest.IsNone)
                {
                    Out("no storage chest within 20 m: place one next to the Shroud Forge first");
                    return;
                }
                Out($"sword: {await Items.Give(who, Sword, 1, new Gear(null, ItemRarity.Common) { GemSlot = true })}");
                Out($"gem L2 into chest {chest}: {await Items.Give(chest, Gem, 1, new Gear(2))}");
                Out($"gem L3 into chest {chest}: {await Items.Give(chest, Gem, 1, new Gear(3))}");
                Out($"essence into chest {chest}: {await Items.Give(chest, Essence, 300)}");
                break;
            }
            case "pay" when Arg(1) is "player" or "chest" or "base" or "playerthenbase":
                _pay = Arg(1);
                Out($"who pays an Insert or Upgrade of a chest gem: {_pay} (reopen the forge screen)");
                break;
            case "return" when Arg(1) is "player" or "chest":
                _return = Arg(1);
                Out($"where a Recycle's essence goes: {_return} (reopen the forge screen)");
                break;
            case "off":
                _offering = false;
                Out("offers stopped (rows already sent lapse within 30 s; reopen the forge screen)");
                break;
            case "on":
                _offering = true;
                Out("offering again (reopen the forge screen)");
                break;
            default:
                Out($"offering {(_offering ? "on" : "off")}, pay {_pay}, return {_return}; rows: {Gems.SourcesCapability} " +
                    $"{(Capabilities.Is(Gems.SourcesCapability) ? "on" : "off")}, {Gems.PayersCapability} {(Capabilities.Is(Gems.PayersCapability) ? "on" : "off")}");
                var essence = Items.Lookup(Essence)?.Id ?? ItemId.None;
                foreach (var chest in ChestsNear(who))
                {
                    var inv = Items.Read(chest)!;
                    var gems = inv.Slots.Where(s => !s.IsEmpty && Items.Get(s.Item)?.Category == ItemCategory.WeaponGem)
                        .Select(s => $"{s.Slot} {s.Item} L{s.Level}");
                    Out($"chest {chest}: essence {inv.Count(essence)}; gems {string.Join(", ", gems)}");
                }
                var mine = Items.Read(who);
                if (mine is not null) Out($"you: essence {mine.Count(essence)}");
                break;
        }
    }
}
