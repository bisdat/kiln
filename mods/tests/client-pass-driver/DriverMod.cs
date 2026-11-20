// SPDX-License-Identifier: MIT
// Test only: the client pass's in-game actions done through Kiln's own API, on the game's own request path, so a tester
// (or a computer-use agent) does only what needs the screen. Every action logs one line "client-pass-driver: ..." in
// kiln.log. Run as the host's player: /drive <action> in the chat, or a line in kiln/control/console/*.cmd.
//
//   /drive where                    the player's place, the held item (gear state) and the offered things near
//   /drive use [radius|e<entity>]   accept the nearest thing's current offer, as pressing E sends it: the game's own
//                                   AcceptOfferedInteractionEvent (Gameplay.Inject.Interact), validated by the game
//                                   (range, the offer being active for you). Anvil (equip.c1), forge (equip.c2), well.
//   /drive anvil [n] / forge        the same, with the nearest Kiln-offered or offered station; anvil repeats n times
//   /drive fill-can [radius]        the same at the nearest well (06b): the game's fill runs, so its GetItemVolume calls
//   /drive craft [item] [n]         n crafts of the item's recipe at the nearest station, each the game's own
//                                   CraftRecipeEvent (Gameplay.Inject.Craft): payment and output by the game, rarity
//                                   rolls on its result (equip.c10; default rarity-anvil.anvil-blade x 10)
//   /drive provoke [radius]         a 1-point hit from you on the nearest enemy, so its own AI attacks you through the
//                                   game's damage path (equip-p7.armor). Not during equip-p7.power: that hit counts there.
//   /drive step-away [metres]       teleport (the game's Teleport) that far from the nearest offered station (equip.c2's
//                                   "more than 6 m"); the pick in the window is still a click
//   /drive held                     the held item's gear state (rarity, level, durability) for the log
//
// Not driven (no public API reaches the path under test): your own weapon swings (equip-p7.power), the workbench's
// enhancement unlock (equip.c3, the game's source), durability wear (equip.c4: Kiln writes only the max), every UI check.
using Kiln;

namespace ClientPassDriver;

public sealed class DriverMod : Mod
{
    private const string Me = "client-pass-driver";
    private const int OfferIdAt = 0x28, OfferActiveAt = 0x2d;
    private const string DefaultCraft = "rarity-anvil.anvil-blade";

    public override void OnLoad() => Line("loaded: /drive where | use | anvil | forge | fill-can | craft | provoke | step-away | held");

    private void Line(string s) => Log.Info($"{Me}: {s}");

    private string Say(CommandContext c, string s)
    {
        Line(s);
        c.Reply(s);
        return s;
    }

    private async Task Steps(int n)
    {
        for (var i = 0; i < n; i++) await Context.Scheduler.NextTick();
    }

    [Command("drive", Description = "client-pass driver (test only): where | use | anvil | forge | fill-can | craft | provoke | step-away | held",
        Permission = CommandPermission.Admin)]
    private async Task Drive(CommandContext c)
    {
        if (!c.Source.IsPlayer) throw new CommandException("run it as a player (the console runs as the host's player)");
        EntityId p = c.Source.Player;
        var a = c.Args;
        var verb = a.Count > 0 ? a[0].ToLowerInvariant() : "help";
        double Num(int i, double fallback) => a.Count > i && double.TryParse(a[i], System.Globalization.NumberStyles.Float,
            System.Globalization.CultureInfo.InvariantCulture, out var v) ? v : fallback;
        await Steps(1);
        try
        {
            switch (verb)
            {
                case "where": Where(c, p); break;
                case "held": Held(c, p); break;
                case "use":
                    if (a.Count > 1 && a[1].StartsWith('e') && uint.TryParse(a[1].AsSpan(1), out var id)) await Use(c, p, 64, new EntityId(id), "use", null);
                    else await Use(c, p, Num(1, 4), null, "use", null);
                    break;
                case "anvil":
                {
                    var n = (int)Math.Clamp(Num(1, 1), 1, 10);
                    for (var i = 1; i <= n; i++)
                    {
                        await Use(c, p, 5, null, $"anvil {i}/{n}", KilnOffered);
                        if (i < n) await Steps(45);
                    }
                    break;
                }
                case "forge": await Use(c, p, 5, null, "forge", KilnOffered); break;
                case "fill-can": await Use(c, p, Num(1, 5), null, "fill-can", e => !KilnOffered(e)); break;
                case "craft": await Craft(c, p, a.Count > 1 && !double.TryParse(a[1], out _) ? a[1] : DefaultCraft,
                    (int)Math.Clamp(Num(a.Count > 1 && !double.TryParse(a[1], out _) ? 2 : 1, 10), 1, 50)); break;
                case "provoke": await Provoke(c, p, Num(1, 25)); break;
                case "step-away": await StepAway(c, p, Num(1, 8)); break;
                default:
                    c.Reply("/drive where | held | use [radius|e<entity>] | anvil [n] | forge | fill-can [radius] | craft [item] [n] | provoke [radius] | step-away [m]");
                    break;
            }
        }
        catch (CommandException) { throw; }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            throw new CommandException(Say(c, $"{verb}: FAILED: {e.GetType().Name}: {e.Message}"));
        }
    }

    private static bool KilnOffered(EntityId e) => Offers.Available && Offers.List().Any(o => o.Entity == e);

    private static string NameOf(EntityId e) => Spawn.Inspect(e)?.TemplateName ?? "?";

    private static double Dist(WorldPosition a, WorldPosition b) => a.DistanceTo(b);

    // Entities with the game's InteractionOfferRuntime near the player, nearest first.
    private static List<(EntityId E, double D)> Offered(WorldPosition at, double radius, Func<EntityId, bool>? filter) =>
        World.Find("InteractionOfferRuntime")
            .Where(e => !e.IsPlayer && (filter == null || filter(e)))
            .Select(e => (e, d: World.Position(e) is { } q ? Dist(q, at) : double.MaxValue))
            .Where(x => x.d <= radius).OrderBy(x => x.d).ToList();

    private void Where(CommandContext c, EntityId p)
    {
        var at = World.Position(p);
        Say(c, $"where: player {p} at {at}; held {HeldText(p)}");
        if (at is not { } here) return;
        foreach (var (e, d) in Offered(here, 10, null).Take(8))
        {
            var rt = World.ReadBytes(e, "InteractionOfferRuntime");
            var (id, on) = rt is { Length: > OfferActiveAt } ? (BitConverter.ToUInt32(rt, OfferIdAt), rt[OfferActiveAt]) : (0u, (byte)0);
            Say(c, $"where:   offered {e} {NameOf(e)} at {d:0.0} m: offer id {id}, active {on}{(KilnOffered(e) ? ", Kiln offer" : "")}");
        }
        foreach (var e in Creatures.Near(here, 25, CreatureKind.Enemy).Take(3))
            Say(c, $"where:   enemy {e} {NameOf(e)} at {(World.Position(e) is { } q ? Dist(q, here) : -1):0.0} m");
    }

    private static string HeldText(EntityId p)
    {
        var held = Items.Held(p);
        if (held.IsNone) return "nothing";
        var g = Items.GearOf(held);
        return $"{held} {g}";
    }

    private void Held(CommandContext c, EntityId p) => Say(c, $"held: {HeldText(p)}");

    // The game's own accept: the same AcceptOfferedInteractionEvent a client sends when its player presses E. The game
    // checks it as a client's (the offer must be active for this player, in range); nothing here forces the offer on.
    private async Task Use(CommandContext c, EntityId p, double radius, EntityId? only, string what, Func<EntityId, bool>? filter)
    {
        if (!Gameplay.Available || !Gameplay.Enable()) throw new CommandException(Say(c, $"{what}: Kiln.Gameplay is not available"));
        var at = World.Position(p) ?? throw new CommandException(Say(c, $"{what}: the player's place is unknown"));
        var host = only ?? Offered(at, radius, filter).Select(x => x.E).FirstOrDefault();
        if (host.IsNone) throw new CommandException(Say(c, $"{what}: nothing with an offer within {radius:0.#} m of {at}"));
        var rt = World.ReadBytes(host, "InteractionOfferRuntime");
        if (rt is not { Length: > OfferActiveAt })
            throw new CommandException(Say(c, $"{what}: {host} {NameOf(host)} has no InteractionOfferRuntime"));
        var offerId = BitConverter.ToUInt32(rt, OfferIdAt);
        var active = rt[OfferActiveAt];
        var d = World.Position(host) is { } hp ? Dist(hp, at) : -1;
        if (offerId == 0 || active == 0)
            throw new CommandException(Say(c, $"{what}: {host} {NameOf(host)} at {d:0.0} m is not offering to you now (offer id {offerId}, " +
                                              $"active {active}): stand next to it, facing it, until the prompt shows"));
        var heldBefore = HeldText(p);
        bool? accepted = null;
        using var sub = Events.On<InteractionResultEvent>(e =>
        {
            if (e.Guest == p && e.Host == host) accepted = e.Accepted;
        });
        Gameplay.Inject.Interact(p, host, offerId);
        for (var i = 0; i < 30 && accepted == null; i++) await Steps(1);
        await Steps(10);
        Say(c, $"{what}: injected the game's AcceptOfferedInteractionEvent, player {p} -> {host} {NameOf(host)} at {d:0.0} m " +
               $"(offer id {offerId}{(KilnOffered(host) ? ", Kiln offer" : ", game offer")}): the game " +
               $"{(accepted switch { true => "accepted it", false => "declined it", _ => "sent no answer in 40 steps" })}; " +
               $"held before {heldBefore}, after {HeldText(p)}");
    }

    // n crafts, each the game's own CraftRecipeEvent from the player at the nearest station with the item's recipe.
    private async Task Craft(CommandContext c, EntityId p, string itemName, int n)
    {
        if (!Gameplay.Available || !Gameplay.Enable()) throw new CommandException(Say(c, "craft: Kiln.Gameplay is not available"));
        var item = Items.Lookup(itemName) ?? throw new CommandException(Say(c, $"craft: {itemName} is not an item of this game"));
        var at = World.Position(p) ?? throw new CommandException(Say(c, "craft: the player's place is unknown"));
        EntityId station = EntityId.None;
        GameplayRecipe? recipe = null;
        double best = 10;
        foreach (var s in World.Find("Crafting").Where(e => !e.IsPlayer))
        {
            if (World.Position(s) is not { } sp || Dist(sp, at) > best) continue;
            var b = World.ReadBytes(s, "Crafting");
            if (b is not { Length: >= 0x14 }) continue;
            var r = Gameplay.WorkshopRecipes(BitConverter.ToUInt32(b, 0x10)).Select(Gameplay.Recipe)
                .FirstOrDefault(x => x is not null && x.Outputs.Any(o => new ItemId(o.ItemId) == item.Id));
            if (r is null) continue;
            (station, recipe, best) = (s, r, Dist(sp, at));
        }
        if (recipe is null)
            throw new CommandException(Say(c, $"craft: no station within 10 m has a recipe for {itemName} (stand at the workbench)"));
        int Count() => Items.Read(p)?.Count(item.Id) ?? 0;
        var start = Count();
        Say(c, $"craft: {n} x recipe 0x{recipe.Id:x8} ({itemName}) at {station} {NameOf(station)} {best:0.0} m away, " +
               $"inputs {string.Join(", ", recipe.Inputs.Select(i => $"{i.Count} x 0x{i.ItemId:x8}"))}; {start} held before");
        for (var k = 1; k <= n; k++)
        {
            var before = Count();
            Gameplay.Inject.Craft(p, station, recipe.Id, 1);
            var i = 0;
            for (; i < 60 && Count() == before; i++) await Steps(1);
            if (Count() == before)
            {
                Say(c, $"craft {k}/{n}: the game made nothing in 60 steps (materials? the station's own checks?); stopping");
                break;
            }
            await Steps(4);
        }
        Say(c, $"craft: done, {Count() - start} new {itemName} (the check reads their rarities)");
    }

    // One point of damage from the player on the nearest enemy: from then on its own AI attacks the player, and the hits
    // the player takes are the game's (its armour, the item's multiplier) and not Kiln's.
    private async Task Provoke(CommandContext c, EntityId p, double radius)
    {
        var at = World.Position(p) ?? throw new CommandException(Say(c, "provoke: the player's place is unknown"));
        var target = Creatures.Near(at, radius, CreatureKind.Enemy).FirstOrDefault();
        if (target.IsNone) throw new CommandException(Say(c, $"provoke: no enemy within {radius:0.#} m"));
        var r = await Combat.Damage(target, 1, p, new DamageOptions { Aggro = 50f });
        var d = World.Position(target) is { } tp ? Dist(tp, at) : -1;
        Say(c, $"provoke: 1 damage from player {p} to enemy {target} {NameOf(target)} at {d:0.0} m ({r}); it should now attack you " +
               "(creative's invulnerability blocks the hits: turn creative off for equip-p7.armor)");
    }

    // The game's teleport, away from the nearest offered station along the line from it to the player.
    private async Task StepAway(CommandContext c, EntityId p, double metres)
    {
        var at = World.Position(p) ?? throw new CommandException(Say(c, "step-away: the player's place is unknown"));
        var near = Offered(at, 8, KilnOffered).Select(x => x.E).FirstOrDefault();
        if (near.IsNone) near = Offered(at, 8, null).Select(x => x.E).FirstOrDefault();
        if (near.IsNone || World.Position(near) is not { } sp) throw new CommandException(Say(c, "step-away: no offered station within 8 m"));
        double dx = at.X - sp.X, dz = at.Z - sp.Z, len = Math.Sqrt(dx * dx + dz * dz);
        if (len < 0.1) (dx, dz, len) = (1, 0, 1);
        var to = new WorldPosition(sp.X + dx / len * metres, at.Y, sp.Z + dz / len * metres);
        var r = await Teleport.To(p, to, new TeleportOptions { FindGround = true });
        var now = World.Position(p);
        Say(c, $"step-away: teleported player {p} from {at} to {now} ({r}); {(now is { } q ? Dist(q, sp) : -1):0.0} m from {near} {NameOf(near)}");
    }
}
