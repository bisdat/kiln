// SPDX-License-Identifier: MIT
// GitHub issue 61 lab test (0.2.0-dev.29.15), player-free, on a dedicated server: a spawned player character (1_Player:
// PlayerInput, so equipment_actions and kiln_items_gems iterate it) next to a spawned Shroud Forge (Grassland) and four
// spawned chests. Part 1 and 3 from code: a gem in a chest socketed into the player's sword, an unsocket into a chest, an
// upgrade in a chest paid by another chest, a payer short of the price, a recycle whose essence goes into the chest,
// base storage without a base, a stale holder, a carrier and a gem in two chests. Part 2 through the game's own request
// (written into the character's PlayerInput.weaponGemAction as a client's would arrive, offsets of server 26430253): a
// forged slot without an offer, an offered upgrade, the same request again, a stale offer after its gem moved, a gem
// taken from the chest right after the request, an offer whose payer is short, an offered recycle with the essence into
// the chest, a withdrawn offer. Review 2026-11-17: an offer to a player slot nobody is in, a chest's carrier refused
// (Unslot, Insert), the gem socketed in the held sword upgraded and recycled with an offer live. Each against what the
// inventories, sockets and levels hold afterwards. Everything spawned is destroyed at the end.
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

        // ================= review 2026-11-17 =================
        // (1) an offer belongs to a player's session: a player slot nobody is in game in gets none (no client here, so a
        // leave and a new session in the same slot is the host test's; this is the native session lookup on a server)
        var noOne = Gems.OfferChestGems(new EntityId(16), chestA, new GemSourceOptions());
        Check(!noOne.Ok && noOne.Gems.Count == 0, $"review 1: an offer to player slot 16 (nobody in game) is refused ({noOne.Refusal})");

        // (5) a carrier outside the player's inventories is refused once offers are in use (the game's handler checks no
        // ownership): Unslot of the gem in chest D's sword, and the player's own gem into another sword of chest D
        var gemsBefore = GemsIn(p, gemId);
        var vx = await Inject(GemCraftAction.Unsocket, sword2.ItemEntity, default);
        Check(ConsumedNow() == vx && SocketOf(sword2.ItemEntity) == (gemId.Value, gA.ItemEntity.Value) && GemsIn(p, gemId) == gemsBefore,
            $"review 5: Unslot of a chest's sword is refused, its gem stays socketed ({SocketOf(sword2.ItemEntity)})");
        Info($"give: {await Items.Give(chestD, SwordName, 1, new Gear(null, ItemRarity.Common) { GemSlot = true })}");
        await Steps(4);
        var sword3 = Items.Read(chestD)!.Slots.First(s => !s.IsEmpty && s.Item == swordId && s.ItemEntity != sword2.ItemEntity);
        var own2 = await GiveGem(p, gemId, 1);
        var pe9 = Count(p, essence);
        var vy = await Inject(GemCraftAction.Socket, sword3.ItemEntity, own2.Slot);
        Check(ConsumedNow() == vy && SocketOf(sword3.ItemEntity) == (0, 0) && SlotOf(p, own2.ItemEntity) is not null && Count(p, essence) == pe9,
            $"review 5: the player's own gem into a chest's sword is refused, nothing paid ({SocketOf(sword3.ItemEntity)})");

        // (8) the gem socketed in the sword the player holds (action bar slot 0, the game's main hand) after offers exist:
        // the player's own Upgrade and Recycle still run (the page walk finds the action bar)
        var live = Gems.OfferChestGems(p, chestA, new GemSourceOptions());
        Info($"an offer is live for the next checks: {live.Gems.Count} gem(s) ({live.Refusal ?? "ok"})");
        var sel = World.ReadBytes(p, "SlotSelection");
        var bar0 = sel is { Length: >= 0xa8 } ? new EntityId(BitConverter.ToUInt32(sel, 0xa0)) : EntityId.None;
        var pagesOf = Items.Read(p)!.Slots.Select(x => x.Slot.Inventory).Distinct().ToList();
        Info($"action bar page {bar0}; the player's pages: {string.Join(" ", pagesOf)}");
        Check(!bar0.IsNone && pagesOf.Contains(bar0), "review 8: the action bar is one of the player's pages (Items.Read's walk)");
        // a worn piece (a helmet): its equipment page is one of the player's pages too (a socketed ring or armour worn)
        Info($"give: {await Items.Give(p, "Armor_T1_Warrior_DPS_Helmet", 1)}");
        await Steps(4);
        var helmet = Items.Read(p)!.Slots.FirstOrDefault(x => !x.IsEmpty && Items.Get(x.Item)?.Name == "Armor_T1_Warrior_DPS_Helmet");
        var worn = helmet is null ? null : await Items.Equip(p, helmet.Slot, EquipmentSlot.ArmourHead);
        await Steps(4);
        sel = World.ReadBytes(p, "SlotSelection");
        var equipPages = new HashSet<EntityId>();
        for (var i = 0; i < 20 && sel is { Length: >= 0xa0 }; i++)
            if (BitConverter.ToUInt32(sel, i * 8) is var e && e != 0) equipPages.Add(new EntityId(e));
        pagesOf = Items.Read(p)!.Slots.Select(x => x.Slot.Inventory).Distinct().ToList();
        Info($"helmet worn: {worn}; equipment pages (SlotSelection.equipment): {string.Join(" ", equipPages)}");
        Check(worn is { Ok: true } && equipPages.Count > 0 && equipPages.All(pagesOf.Contains),
            "review 8: the page of a worn piece is one of the player's pages (worn rings and armour count as the player's own)");
        // Items.Give put the sword on the action bar already (a weapon goes there first); else move it there
        if (SlotOf(p, sword.ItemEntity) is { } at && at.Slot.Inventory != bar0)
        {
            var toChest = await Items.Move(p, at.Slot, chestD, 0, MoveOptions.ActionBar);
            await Steps(4);
            var toBar = await Items.Move(chestD, SlotOf(chestD, sword.ItemEntity)!.Slot, p, 0, MoveOptions.ActionBar, SlotId.Of(bar0, 0));
            await Steps(4);
            Info($"sword to chest D {toChest.Status}, to the action bar {toBar.Status}");
        }
        var held = SlotOf(p, sword.ItemEntity);
        var hand = sel is { Length: >= 0xc1 } ? sel[0xc0] : -1;
        Info($"the sword is at {held?.Slot}; the selected action bar slot (SlotSelection +0xc0) is {hand}");
        Check(held is { } h && h.Slot.Inventory == bar0, $"review 8: the sword is in the action bar ({held?.Slot}, selected {hand})");
        var lv0 = Gems.Socket(sword.Ref)?.Level ?? -1;
        var pe10 = Count(p, essence);
        var vv = await Inject(GemCraftAction.Upgrade, sword.ItemEntity, SlotId.Of(sword.ItemEntity, 0));
        var lv1 = Gems.Socket(sword.Ref)?.Level ?? -1;
        Check(ConsumedNow() == vv && lv1 == lv0 + 1 && pe10 - Count(p, essence) > 0,
            $"review 8: Upgrade of the socketed gem in the held sword with an offer live: level {lv0} -> {lv1}, paid {pe10 - Count(p, essence)}");
        var pe11 = Count(p, essence);
        var vr = await Inject(GemCraftAction.Recycle, sword.ItemEntity, SlotId.Of(sword.ItemEntity, 0));
        Check(ConsumedNow() == vr && SocketOf(sword.ItemEntity) == (0, 0) && Count(p, essence) > pe11,
            $"review 8: Recycle of the socketed gem in the held sword with an offer live: socket {SocketOf(sword.ItemEntity)}, essence {pe11} -> {Count(p, essence)}");
        // a second forge no request has used yet: its material can only come from the gem program's own read
        var forge2 = await Put(ForgeTemplate, 46, 40);
        await Steps(10);
        await ServerHalf(p, forge2, chestA, chestB, gemId, essence);
        Info($"at the end: player {Gear(p)} | A {Gear(chestA)} | B {Gear(chestB)} | C {Gear(chestC)} | D {Gear(chestD)}");
    }

    // Review 2026-11-17, the server half without a client: Kiln's own GemSources.Hello (internal, reached by reflection: a
    // lab-only shortcut for the client's hello over kiln:gemsources) on this dedicated server. The forge's material is
    // read by the gem program (kiln.gemcraft forge_material), the payer counts are of it, the hello limit holds across a
    // close, and a player who left loses the offers.
    private async Task ServerHalf(EntityId p, EntityId forge, EntityId chestA, EntityId chestB, ItemId gemId, ItemId essence)
    {
        const System.Reflection.BindingFlags Any = System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Public |
                                                   System.Reflection.BindingFlags.Static;
        var asm = typeof(Gems).Assembly;
        var gs = asm.GetType("Kiln.GemSources");
        var helloType = asm.GetType("Kiln.GemForgeHello");
        var hello = gs?.GetMethod("Hello", Any, [typeof(EntityId), helloType!]);
        var gone = gs?.GetMethod("PlayerGone", Any, [typeof(EntityId)]);
        var sendHook = gs?.GetField("SendHook", Any);
        var forgeHook = gs?.GetField("ForgeNearHook", Any);
        if (gs is null || helloType is null || hello is null || gone is null || sendHook is null || forgeHook is null)
        {
            Check(false, "server half: Kiln.GemSources' internals are not where this test expects them");
            return;
        }
        object Hello(bool open) => Activator.CreateInstance(helloType, open, Array.Empty<uint>())!;
        var sent = new List<object>();
        sendHook.SetValue(null, new Action<EntityId, object>((to, m) => { if (to == p) sent.Add(m); }));
        forgeHook.SetValue(null, new Func<EntityId, EntityId>(_ => forge));  // the forge the screen is at (here: the second one)
        var opened = new List<GemForgeOpenedEvent>();
        var gH = await GiveGem(chestA, gemId, 2);
        var sub = Events.On<GemForgeOpenedEvent>(e =>
        {
            if (e.Player != p) return;
            opened.Add(e);
            Gems.OfferChestGems(p, chestA, new GemSourceOptions { PayFrom = GemPayer.From(chestB) });
        });
        try
        {
            hello.Invoke(null, [p, Hello(true)]);
            await Steps(20);  // the pump sends the rows; the gem program reads the forge's material within a step or two
            Check(opened.Count == 1 && opened[0].Forge == forge && !opened[0].Refresh,
                $"server half: a hello posts GemForgeOpenedEvent with the forge found near the player ({opened.FirstOrDefault()?.Forge}, {forge})");
            static System.Collections.IEnumerable? Prop(object o, string name) => o.GetType().GetProperty(name)?.GetValue(o) as System.Collections.IEnumerable;
            static object? Field(object o, string name) => o.GetType().GetProperty(name)?.GetValue(o);
            var last = sent.LastOrDefault(m => m.GetType().Name == "GemForgeOffers");
            var payers = last is null ? [] : Prop(last, "Payers")!.Cast<object>().ToList();
            var rows = last is null ? [] : Prop(last, "Rows")!.Cast<object>().ToList();
            var counts = sent.Where(m => m.GetType().Name == "GemForgeOffers").Select(m => Prop(m, "Payers")!.Cast<object>().Count()).ToList();
            Info($"server half: {sent.Count} message(s), payer rows per message [{string.Join(" ", counts)}]; rows {rows.Count}, payers {string.Join(", ", payers.Select(x => $"{Field(x, "Item"):x} x{Field(x, "Count")}"))}");
            Check(rows.Any(r => (uint)Field(r, "Pide")! == gH.ItemEntity.Value),
                "server half: the rows list chest A's offered gem");
            Check(payers.Count == 1 && (uint)Field(payers[0], "Item")! == essence.Value && (uint)Field(payers[0], "Count")! == (uint)Count(chestB, essence),
                $"server half: one payer count, of the forge's own upgrade material (read by the gem program), what chest B holds ({Count(chestB, essence)})");
            // the hello limit is the player's and holds across a close: a close and an open inside the second post nothing
            hello.Invoke(null, [p, Hello(false)]);
            hello.Invoke(null, [p, Hello(true)]);
            await Steps(2);
            Check(opened.Count == 1, $"server half: a close and an open inside the second post no event at once ({opened.Count})");
            await Steps(40);
            Check(opened.Count == 2 && !opened[1].Refresh, $"server half: the opening is posted once the second is over ({opened.Count})");
            // a player who left (the next player in the slot gets nothing of theirs): the session and every offer go
            gone.Invoke(null, [p]);
            sent.Clear();
            var stillThere = Gems.WithdrawChestGems(p);
            await Steps(4);
            Check(stillThere == 0 && sent.Count == 0,
                $"server half: after the player left no offer of theirs is left ({stillThere}) and nothing more is sent ({sent.Count})");
        }
        finally
        {
            sub.Dispose();
            sendHook.SetValue(null, null);
            forgeHook.SetValue(null, null);
            hello.Invoke(null, [p, Hello(false)]);
        }
    }
}
