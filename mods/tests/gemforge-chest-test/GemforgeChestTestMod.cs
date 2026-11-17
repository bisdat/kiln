// SPDX-License-Identifier: MIT
// GitHub issue 61 lab test (0.2.0-dev.29.15), player-free, on a dedicated server: a spawned player character (1_Player:
// PlayerInput, so equipment_actions and kiln_items_gems iterate it) next to a spawned Shroud Forge (Grassland) and four
// spawned chests. Part 1 and 3 from code: a gem in a chest socketed into the player's sword, an unsocket into a chest, an
// upgrade in a chest paid by another chest, a payer short of the price, a recycle whose essence goes into the chest,
// base storage without a base, a stale holder, a carrier and a gem in two chests. Part 2 through the game's own request
// (written into the character's PlayerInput.weaponGemAction as a client's would arrive, offsets of server 26430253): a
// forged slot without an offer, an offered upgrade, the same request again, a stale offer after its gem moved, a gem
// taken from the chest right after the request, an offer whose payer is short, an offered recycle with the essence into
// the chest, a withdrawn offer. Each against what the inventories, sockets and levels hold afterwards. Everything spawned
// is destroyed at the end.
using Kiln;

namespace GemforgeChestTest;

public sealed class GemforgeChestTestMod : Mod
{
    private int _pass, _fail;
    private const string GemName = "T1_Gem_GemOfBleeding", SwordName = "Weapon_T1_1H_Sword_Craft",
        EssenceName = "Material_T1_Fognest_Ressource", ForgeTemplate = "Shroud_Gem_Forge_01_Grassland",
        ChestTemplate = "Prop_Decoration_T1_Storage_24";
    // PlayerInput.weaponGemAction and ServerConsumedPlayerInput.consumedWeaponGemAction on server 26430253 (sigdb gems/input.*)
    private const int InWeapon = 0x260, InSource = 0x264, InForge = 0x26c, InMaterial = 0x270, InVersion = 0x274, InAction = 0x278,
        Consumed = 0x98;
    private readonly List<EntityId> _spawned = new();
    private readonly List<GemCraftedEvent> _crafted = new();

    private void Info(string s) => Log.Info($"gemforge-chest-test: INFO {s}");

    private void Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"gemforge-chest-test: PASS {what}"); }
        else { _fail++; Log.Error($"gemforge-chest-test: FAIL {what}"); }
    }

    public override void OnLoad()
    {
        if (!Client.IsDedicatedServer) return;
        _ = Run();
    }

    private async Task Steps(int n)
    {
        for (var i = 0; i < n; i++) await Context.Scheduler.NextTick();
    }

    private async Task Run()
    {
        try
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (!Spawn.WorldRunning && sw.Elapsed.TotalSeconds < 240) await Context.Scheduler.NextTick();
            await Steps(60);
            await Suite();
        }
        catch (Exception e)
        {
            Check(false, $"exception: {e}");
        }
        foreach (var e in _spawned) Spawn.Destroy(e);
        await Steps(3);
        Log.Info($"gemforge-chest-test done: {_pass} PASS, {_fail} FAIL");
    }

    private static int Count(EntityId e, ItemId item) => Items.Read(e)?.Slots.Where(s => !s.IsEmpty && s.Item == item).Sum(s => s.Count) ?? -1;

    private static InventorySlot? SlotOf(EntityId holder, EntityId item) =>
        Items.Read(holder)?.Slots.FirstOrDefault(s => !s.IsEmpty && s.ItemEntity == item);

    private static (uint Item, uint Pide) SocketOf(EntityId carrier)
    {
        var b = World.ReadBytes(carrier, "WeaponGemSlot");
        if (b is null || b.Length < 12) return (0xFFFFFFFF, 0xFFFFFFFF);
        var pide = BitConverter.ToUInt32(b, 8);
        return BitConverter.ToUInt32(b, 4) == 0 ? (0, 0) : (BitConverter.ToUInt32(b, 0), pide == 0xFFFFFFFF ? 0 : pide);
    }

    private static string Gear(EntityId e) =>
        Items.Read(e) is { } c
            ? string.Join(" ", c.Slots.Where(s => !s.IsEmpty).Select(s => $"{s.Slot}={s.Item.Value:x8}x{s.Count}#{s.ItemEntity}L{s.Level}"))
            : "unreadable";

    private static int GemsIn(EntityId e, ItemId gem) => Items.Read(e)?.Slots.Count(s => !s.IsEmpty && s.Item == gem) ?? -1;

    private async Task<InventorySlot> GiveGem(EntityId to, ItemId gemId, int level)
    {
        var before = Items.Read(to)?.Slots.Where(s => !s.IsEmpty && s.Item == gemId).Select(s => s.ItemEntity).ToHashSet() ?? [];
        Info($"give {GemName} L{level} to {to}: {await Items.Give(to, GemName, 1, new Gear(level))}");
        await Steps(4);
        return Items.Read(to)!.Slots.First(s => !s.IsEmpty && s.Item == gemId && !before.Contains(s.ItemEntity));
    }

    private async Task Suite()
    {
        foreach (var row in new[] { Gems.CraftCapability, Gems.PayersCapability, Gems.SourcesCapability })
            Check(Capabilities.Is(row), $"{row} is available ({Capabilities.Why(row)})");
        if (!Capabilities.Is(Gems.CraftCapability)) return;
        Events.On<GemCraftedEvent>(e =>
        {
            _crafted.Add(e);
            Info($"event: {e}");
        });
        var gemId = Items.Lookup(GemName)!.Id;
        var swordId = Items.Lookup(SwordName)!.Id;
        var essence = Items.Lookup(EssenceName)!.Id;

        WorldPosition? anchor = null;
        foreach (var f in new[] { "FlameAltar", "Altar", "Townsfolk", "NPC_" })
        {
            var id = Spawn.FindByName(f, max: 1).FirstOrDefault();
            if (!id.IsNone && Spawn.Inspect(id) is { } inf)
            {
                anchor = inf.Position;
                break;
            }
        }
        if (anchor is not { } a)
        {
            Check(false, "no anchor entity in the world");
            return;
        }
        async Task<EntityId> Put(string template, double dx, double dz)
        {
            var e = await Spawn.Entity(template, new WorldPosition(a.X + dx, a.Y + 0.5, a.Z + dz), 0,
                new SpawnOptions { Tag = "gemforge-chest", Flags = SpawnFlags.Transient });
            _spawned.Add(e);
            return e;
        }
        var p = await Put("1_Player", 40, 40);
        var forge = await Put(ForgeTemplate, 43, 40);
        var chestA = await Put(ChestTemplate, 38, 43);
        var chestB = await Put(ChestTemplate, 36, 43);
        var chestC = await Put(ChestTemplate, 34, 43);  // stays without essence: the short payer
        var chestD = await Put(ChestTemplate, 32, 43);
        await Steps(30);
        Check(!p.IsNone && World.Has(p, "PlayerInput") && Items.Read(p) is { Initialized: true }, $"player character {p}");
        Check(!forge.IsNone && Spawn.Exists(forge), $"a {ForgeTemplate} spawned: {forge}");
        foreach (var c in new[] { chestA, chestB, chestC, chestD })
            Check(!c.IsNone && Items.Read(c) is { Initialized: true, IsPlayer: false }, $"chest {c} with an inventory");
        if (p.IsNone || forge.IsNone || chestA.IsNone || chestB.IsNone || chestC.IsNone || chestD.IsNone) return;

        Info($"give: {await Items.Give(p, SwordName, 1, new Gear(null, ItemRarity.Common) { GemSlot = true })}");
        Info($"give: {await Items.Give(p, EssenceName, 300)}");
        Info($"give: {await Items.Give(chestB, EssenceName, 400)}");
        await Steps(6);
        var sword = Items.Read(p)!.Slots.First(s => !s.IsEmpty && s.Item == swordId);
        Check(SocketOf(sword.ItemEntity) == (0, 0), "the sword has an empty socket");
        var gA = await GiveGem(chestA, gemId, 3);
        var gB = await GiveGem(chestA, gemId, 4);
        var gD = await GiveGem(chestA, gemId, 2);
        Info($"player {Gear(p)} | chest A {Gear(chestA)} | chest B {Gear(chestB)}");

        // ================= part 1: the gem in a chest, the carrier with the player, one transaction =================
        var pe0 = Count(p, essence);
        _crafted.Clear();
        var sock = await Gems.Socket(p, sword.Ref, gA.Ref, forge, new GemCraftOptions { GemHolder = chestA });
        await Steps(2);
        Info($"Socket from chest A: {sock}");
        Check(sock.Ok && SocketOf(sword.ItemEntity) == (gemId.Value, gA.ItemEntity.Value),
            $"part 1: a gem in chest A socketed into the player's sword ({SocketOf(sword.ItemEntity)})");
        Check(SlotOf(chestA, gA.ItemEntity) is null && GemsIn(p, gemId) == 0, "part 1: the gem left chest A and never passed the backpack");
        Check(sock.Paid.Count == pe0 - Count(p, essence) && sock.Paid.Count >= 0, $"part 1: the player paid {sock.Paid} ({pe0 - Count(p, essence)} counted)");
        Check(_crafted.Count(c => c.ByCode && c.Action == GemCraftAction.Socket) == 1, "part 1: GemCraftedEvent by code");

        // ReturnTo: the unsocketed gem goes into chest B, not the backpack
        var un = await Gems.Unsocket(p, sword.Ref, forge, new GemCraftOptions { ReturnTo = chestB });
        await Steps(2);
        Info($"Unsocket into chest B: {un}");
        Check(un.Ok && SocketOf(sword.ItemEntity) == (0, 0) && SlotOf(chestB, gA.ItemEntity) is { Level: 3 } && GemsIn(p, gemId) == 0,
            $"part 3: Unsocket with ReturnTo chest B: the gem is in chest B ({SlotOf(chestB, gA.ItemEntity)?.Slot}), not in the backpack");

        // PayFrom: an upgrade in chest A paid by chest B
        var pe1 = Count(p, essence);
        var be1 = Count(chestB, essence);
        var up = await Gems.Upgrade(p, gB.Ref, forge, new GemCraftOptions { GemHolder = chestA, PayFrom = GemPayer.From(chestB) });
        await Steps(2);
        Info($"Upgrade in chest A paid by chest B: {up}");
        Check(up.Ok && up.LevelAfter == 5 && SlotOf(chestA, gB.ItemEntity) is { Level: 5 }, $"part 3: the gem in chest A went 4 -> {up.LevelAfter}");
        Check(Count(p, essence) == pe1 && be1 - Count(chestB, essence) == up.Paid.Count && up.Paid.Count > 0,
            $"part 3: chest B paid {up.Paid} ({be1} -> {Count(chestB, essence)}), the player nothing ({pe1} -> {Count(p, essence)})");

        // a payer short of the price: nothing paid, nothing moved
        var short1 = await Gems.Upgrade(p, gB.Ref, forge, new GemCraftOptions { GemHolder = chestA, PayFrom = GemPayer.From(chestC) });
        await Steps(2);
        Check(short1.Status == GemCraftStatus.NotEnough && SlotOf(chestA, gB.ItemEntity) is { Level: 5 } && Count(p, essence) == pe1 &&
              Count(chestB, essence) == be1 - up.Paid.Count, $"part 3: chest C has no essence: {short1.Status} ({short1.Reason}); level 5 kept, nobody paid");

        // base storage: the spawned character is no logged-in player with the right to use base inventories
        var bs = await Gems.Upgrade(p, gB.Ref, forge, new GemCraftOptions { GemHolder = chestA, PayFrom = GemPayer.BaseStorage });
        await Steps(2);
        Check(bs.Status == GemCraftStatus.NotEnough && SlotOf(chestA, gB.ItemEntity) is { Level: 5 },
            $"part 3: base storage for a character outside a base (or without the right): {bs.Status} ({bs.Reason})");
        var ptb = await Gems.Upgrade(p, gB.Ref, forge, new GemCraftOptions { GemHolder = chestA, PayFrom = GemPayer.PlayerThenBaseStorage });
        await Steps(2);
        Check(ptb.Ok && ptb.LevelAfter == 6 && pe1 - Count(p, essence) == ptb.Paid.Count,
            $"part 3: player then base storage: the player paid {ptb.Paid} (no base) and the gem went to {ptb.LevelAfter}");

        // Recycle with the essence into chest A
        var ae0 = Count(chestA, essence);
        var pe2 = Count(p, essence);
        var rec = await Gems.Recycle(p, gD.Ref, forge, new GemCraftOptions { GemHolder = chestA, ReturnTo = chestA });
        await Steps(2);
        Info($"Recycle in chest A into chest A: {rec}");
        Check(rec.Ok && SlotOf(chestA, gD.ItemEntity) is null && rec.Received.Count > 0 && Count(chestA, essence) - ae0 == rec.Received.Count &&
              Count(p, essence) == pe2, $"part 3: Recycle: the gem is gone, chest A got {rec.Received} ({ae0} -> {Count(chestA, essence)}), the player nothing");

        // a holder that does not hold the gem (it is in chest B now)
        var stale = await Gems.Upgrade(p, gA.Ref, forge, new GemCraftOptions { GemHolder = chestA });
        Check(stale.Status == GemCraftStatus.NotFound && SlotOf(chestB, gA.ItemEntity) is { Level: 3 }, $"part 1: the gem is not in the named holder: {stale.Status} ({stale.Reason})");

        // a carrier and a gem in two chests
        Info($"give: {await Items.Give(chestD, SwordName, 1, new Gear(null, ItemRarity.Common) { GemSlot = true })}");
        await Steps(4);
        var sword2 = Items.Read(chestD)!.Slots.First(s => !s.IsEmpty && s.Item == swordId);
        var two = await Gems.Socket(p, sword2.Ref, gA.Ref, forge, new GemCraftOptions { GemHolder = chestB, CarrierHolder = chestD, Cost = GemCost.Free });
        await Steps(2);
        Check(two.Ok && SocketOf(sword2.ItemEntity) == (gemId.Value, gA.ItemEntity.Value) && SlotOf(chestB, gA.ItemEntity) is null,
            $"part 1: the gem in chest B into the sword in chest D ({two.Status}, {SocketOf(sword2.ItemEntity)})");

        // ================= part 2: the game's own request, through an offer =================
        uint ConsumedNow() => World.ReadBytes(p, "ServerConsumedPlayerInput") is { Length: > Consumed + 4 } c ? BitConverter.ToUInt32(c, Consumed) : 0;
        async Task<uint> Inject(GemCraftAction action, EntityId weapon, SlotId source, Func<Task>? right = null)
        {
            var v = ConsumedNow() + 1;
            World.WriteRaw(p, "PlayerInput", InWeapon, weapon.Value);
            World.WriteRaw(p, "PlayerInput", InSource, source.Value);
            World.WriteRaw(p, "PlayerInput", InForge, forge.Value);
            World.WriteRaw(p, "PlayerInput", InMaterial, essence.Value);
            World.WriteRaw(p, "PlayerInput", InAction, (byte)action);
            World.WriteRaw(p, "PlayerInput", InVersion, v);
            if (right is not null) await right();
            await Steps(10);
            return v;
        }
        var gC = await GiveGem(chestA, gemId, 5);
        var offer = Gems.OfferChestGems(p, chestA, new GemSourceOptions { PayFrom = GemPayer.From(chestB), ReturnTo = chestA });
        Info($"offer of chest A: {offer.Refusal ?? "ok"}: {string.Join(", ", offer.Gems.Select(g => $"{g.Slot} {g.Gem} L{g.Level} token {g.Token:x}"))}");
        Check(offer.Ok && offer.Gems.Count == 2 && offer.Gems.All(g => g.Token != 0) && offer.Gems.Select(g => g.Token).Distinct().Count() == 2,
            $"part 2: chest A's two loose gems offered with two tokens ({offer.Gems.Count})");
        var swordNow = SlotOf(p, sword.ItemEntity)!;

        // a forged request: a chest gem that was never offered (the sword's gem in chest D's sword is not loose; chest B holds
        // essence only now: give it a gem first)
        var gE = await GiveGem(chestB, gemId, 1);
        _crafted.Clear();
        var vf = await Inject(GemCraftAction.Socket, sword.ItemEntity, gE.Slot);
        Check(ConsumedNow() == vf && SocketOf(sword.ItemEntity) == (0, 0) && SlotOf(chestB, gE.ItemEntity) is not null && _crafted.Count == 0,
            $"part 2: a forged request (chest B's gem, no offer) is consumed and nothing moves ({SocketOf(sword.ItemEntity)}, {_crafted.Count} crafted)");

        // the offered upgrade: chest B pays, the gem stays in chest A
        var cOffer = offer.Gems.First(g => g.Gem.Entity.Id == gC.ItemEntity);
        var be2 = Count(chestB, essence);
        var pe3 = Count(p, essence);
        _crafted.Clear();
        var vu = await Inject(GemCraftAction.Upgrade, EntityId.None, cOffer.Slot);
        Check(ConsumedNow() == vu, $"part 2: the player's request was consumed ({ConsumedNow()} / {vu})");
        Check(SlotOf(chestA, gC.ItemEntity) is { Level: 6 }, $"part 2: the offered gem in chest A went 5 -> {SlotOf(chestA, gC.ItemEntity)?.Level}");
        Check(Count(p, essence) == pe3 && Count(chestB, essence) < be2, $"part 2: chest B paid ({be2} -> {Count(chestB, essence)}), the player nothing");
        Check(_crafted.Count(c => !c.ByCode && c.Action == GemCraftAction.Upgrade && c.Gem.Entity.Id == gC.ItemEntity) == 1,
            $"part 2: one GemCraftedEvent of the player's action ({_crafted.Count})");

        // the same request again: the offer was used (and the level changed)
        var vd = await Inject(GemCraftAction.Upgrade, EntityId.None, cOffer.Slot);
        Check(ConsumedNow() == vd && SlotOf(chestA, gC.ItemEntity) is { Level: 6 }, "part 2: the same request again is refused (the offer is used once)");

        // a stale offer: offered again, then the gem moved to chest D before the request
        var again = Gems.OfferChestGems(p, chestA, new GemSourceOptions { PayFrom = GemPayer.From(chestB) });
        var bOffer = again.Gems.First(g => g.Gem.Entity.Id == gB.ItemEntity);
        var moved = await Items.Move(chestA, bOffer.Slot, chestD);
        await Steps(4);
        Info($"moved gem B to chest D: {moved}");
        var vs = await Inject(GemCraftAction.Socket, sword.ItemEntity, bOffer.Slot);
        Check(ConsumedNow() == vs && SocketOf(sword.ItemEntity) == (0, 0) && SlotOf(chestD, gB.ItemEntity) is not null,
            $"part 2: a stale offer (its gem moved to chest D) moves nothing ({SocketOf(sword.ItemEntity)})");

        // a gem taken from the chest right after the request (another player's take): never lost, never twice
        var third = Gems.OfferChestGems(p, chestA, new GemSourceOptions());
        var cOffer3 = third.Gems.First(g => g.Gem.Entity.Id == gC.ItemEntity);
        var pe4 = Count(p, essence);
        Task<ItemsResult>? take = null;
        await Inject(GemCraftAction.Socket, sword.ItemEntity, cOffer3.Slot, () =>
        {
            take = Items.Take(chestA, gemId, 1, cOffer3.Slot);
            return Task.CompletedTask;
        });
        var took = take is null ? null : await take;
        await Steps(4);
        var inSocket = SocketOf(sword.ItemEntity) == (gemId.Value, gC.ItemEntity.Value);
        var takenOk = took is { Succeeded: true };
        Info($"race: taken {took}; socket {SocketOf(sword.ItemEntity)}; chest A {Gear(chestA)}; player {Gear(p)}");
        Check(inSocket != takenOk && SlotOf(chestA, gC.ItemEntity) is null && GemsIn(p, gemId) == 0,
            $"part 2: the gem is in exactly one place (socketed {inSocket}, taken {takenOk}): nothing duplicated or lost");
        Check(inSocket || Count(p, essence) == pe4, $"part 2: when the take won, nothing was paid ({pe4} -> {Count(p, essence)})");
        if (inSocket)
        {
            var back = await Gems.Unsocket(p, sword.Ref, forge, new GemCraftOptions { ReturnTo = chestA });
            Info($"unsocket for the next checks: {back}");
            await Steps(2);
        }

        // an offer whose payer is short: nothing paid, nothing moved
        var gF = await GiveGem(chestA, gemId, 2);
        var poor = Gems.OfferChestGems(p, chestA, new GemSourceOptions { PayFrom = GemPayer.From(chestC) });
        var fOffer = poor.Gems.First(g => g.Gem.Entity.Id == gF.ItemEntity);
        var pe5 = Count(p, essence);
        await Inject(GemCraftAction.Upgrade, EntityId.None, fOffer.Slot);
        Check(SlotOf(chestA, gF.ItemEntity) is { Level: 2 } && Count(p, essence) == pe5 && Count(chestC, essence) == 0,
            $"part 2: the offer's payer (chest C) is short: level 2 kept, nobody paid");

        // an offered Recycle a GemCraftingEvent handler cancels: posted like any forge action of the player, nothing moves
        var rcOffer = Gems.OfferChestGems(p, chestA, new GemSourceOptions { ReturnTo = chestA }).Gems.First(g => g.Gem.Entity.Id == gF.ItemEntity);
        var seen = new List<GemCraftingEvent>();
        var sub = Events.On<GemCraftingEvent>(e =>
        {
            seen.Add(e);
            Info($"event: {e}");
            if (e.Action == GemCraftAction.Recycle) e.Cancel();
        });
        await Steps(2);
        await Inject(GemCraftAction.Recycle, EntityId.None, rcOffer.Slot);
        sub.Dispose();
        await Steps(2);
        Check(seen.Count(e => e.Action == GemCraftAction.Recycle && e.Gem.Entity.Id == gF.ItemEntity && e.Player == p) == 1 &&
              SlotOf(chestA, gF.ItemEntity) is not null,
            $"part 2: GemCraftingEvent sees the offered Recycle ({seen.Count}); canceled, the gem stays in chest A");

        // the offered Recycle again (a cancel did not use the offer), the essence into chest A
        var ae1 = Count(chestA, essence);
        var pe6 = Count(p, essence);
        await Inject(GemCraftAction.Recycle, EntityId.None, rcOffer.Slot);
        Check(SlotOf(chestA, gF.ItemEntity) is null && Count(chestA, essence) > ae1 && Count(p, essence) == pe6,
            $"part 2: an offered Recycle: the gem is gone, chest A got the essence ({ae1} -> {Count(chestA, essence)}), the player nothing");

        // a withdrawn offer
        var gG = await GiveGem(chestA, gemId, 1);
        var w = Gems.OfferChestGems(p, chestA, new GemSourceOptions()).Gems.First(g => g.Gem.Entity.Id == gG.ItemEntity);
        var withdrawn = Gems.WithdrawChestGems(p, chestA);
        await Inject(GemCraftAction.Socket, sword.ItemEntity, w.Slot);
        Check(withdrawn > 0 && SocketOf(sword.ItemEntity) == (0, 0) && SlotOf(chestA, gG.ItemEntity) is not null,
            $"part 2: after WithdrawChestGems ({withdrawn}) the request moves nothing");

        // the player's own gem is not affected by the offers (the game's own path)
        var own = await GiveGem(p, gemId, 1);
        var pe7 = Count(p, essence);
        await Inject(GemCraftAction.Socket, sword.ItemEntity, own.Slot);
        Check(SocketOf(sword.ItemEntity) == (gemId.Value, own.ItemEntity.Value) && pe7 - Count(p, essence) > 0,
            $"part 2: the player's own gem still sockets through the game's path, paid by the player ({SocketOf(sword.ItemEntity)})");
        Info($"at the end: player {Gear(p)} | A {Gear(chestA)} | B {Gear(chestB)} | C {Gear(chestC)} | D {Gear(chestD)}");
    }
}
