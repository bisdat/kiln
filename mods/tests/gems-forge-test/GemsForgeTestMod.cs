// SPDX-License-Identifier: MIT
// Gem wave G3 lab test (gems.craft), player-free: a spawned player character (the game's 1_Player template: it has
// PlayerInput, so equipment_actions and kiln_items_gems iterate it) next to a spawned Shroud Forge (Grassland, maxGemLevel
// 10, Archaic Essence I) gets a sword with an empty socket, a Gem of Bleeding and essence. Then from code: Socket, Upgrade,
// a free Upgrade, Unsocket, an Upgrade of the loose gem with a replaced cost, the refusals (an occupied socket, an empty
// socket, a gem at the forge's maximum level, no essence), and Recycle; each against what the inventories and the socket
// hold afterwards, with GemCraftedEvent for each that happened. Then two "players' actions": the test writes the
// character's PlayerInput.weaponGemAction as a client's input would arrive (offsets of server 26430253), a handler of
// GemCraftingEvent cancels the first (nothing paid, nothing moved, the version consumed) and lets the second run
// (GemCraftedEvent, not by code). Everything spawned is destroyed at the end.
using Kiln;

namespace GemsForgeTest;

public sealed class GemsForgeTestMod : Mod
{
    private int _pass, _fail;
    private const string GemName = "T1_Gem_GemOfBleeding", SwordName = "Weapon_T1_1H_Sword_Craft",
        EssenceName = "Material_T1_Fognest_Ressource", ForgeTemplate = "Shroud_Gem_Forge_01_Grassland";
    // PlayerInput.weaponGemAction and ServerConsumedPlayerInput.consumedWeaponGemAction on server 26430253 (sigdb gems/input.*)
    private const int InWeapon = 0x260, InSource = 0x264, InForge = 0x26c, InMaterial = 0x270, InVersion = 0x274, InAction = 0x278,
        Consumed = 0x98;
    private readonly List<EntityId> _spawned = new();
    private readonly List<GemCraftedEvent> _crafted = new();
    private readonly List<GemCraftingEvent> _crafting = new();
    private int _cancelNext;

    private void Info(string s) => Log.Info($"gems-forge-test: INFO {s}");

    private void Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"gems-forge-test: PASS {what}"); }
        else { _fail++; Log.Error($"gems-forge-test: FAIL {what}"); }
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
        Log.Info($"gems-forge-test done: {_pass} PASS, {_fail} FAIL");
    }

    private static int Count(EntityId p, ItemId item) => Items.Read(p)?.Slots.Where(s => !s.IsEmpty && s.Item == item).Sum(s => s.Count) ?? -1;

    private static InventorySlot? SlotOf(EntityId p, EntityId entity) =>
        Items.Read(p)?.Slots.FirstOrDefault(s => !s.IsEmpty && s.ItemEntity == entity);

    private static (uint Item, uint Pide) SocketOf(EntityId carrier)
    {
        var b = World.ReadBytes(carrier, "WeaponGemSlot");
        if (b is null || b.Length < 12) return (0xFFFFFFFF, 0xFFFFFFFF);
        var pide = BitConverter.ToUInt32(b, 8);
        return BitConverter.ToUInt32(b, 4) == 0 ? (0, 0) : (BitConverter.ToUInt32(b, 0), pide == 0xFFFFFFFF ? 0 : pide);
    }

    private static int LevelOf(EntityId p, EntityId gem) => SlotOf(p, gem)?.Level ?? -1;

    private string Gear(EntityId p) =>
        Items.Read(p) is { } c
            ? string.Join(" ", c.Slots.Where(s => !s.IsEmpty).Select(s => $"{s.Slot}={s.Item.Value:x8}x{s.Count}#{s.ItemEntity}L{s.Level}"))
            : "unreadable";

    private async Task Suite()
    {
        Check(Capabilities.Is(Gems.CraftCapability), $"gems.craft is available ({Capabilities.Why(Gems.CraftCapability)})");
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
        var p = await Spawn.Entity("1_Player", new WorldPosition(a.X + 40, a.Y + 0.5, a.Z + 40), 0,
            new SpawnOptions { Tag = "gemsforge", Flags = SpawnFlags.Transient });
        _spawned.Add(p);
        var forge = await Spawn.Entity(ForgeTemplate, new WorldPosition(a.X + 43, a.Y + 0.5, a.Z + 40), 0,
            new SpawnOptions { Tag = "gemsforge", Flags = SpawnFlags.Transient });
        _spawned.Add(forge);
        await Steps(30);
        Check(!p.IsNone && World.Has(p, "PlayerInput") && Items.Read(p) is { Initialized: true }, $"player character {p} with PlayerInput and an inventory");
        Check(!forge.IsNone && Spawn.Exists(forge), $"a {ForgeTemplate} spawned: {forge}");
        if (p.IsNone || forge.IsNone) return;

        Info($"give: {await Items.Give(p, SwordName, 1, new Gear(null, ItemRarity.Common) { GemSlot = true })}");
        Info($"give: {await Items.Give(p, GemName, 1, new Gear(5))}");
        Info($"give: {await Items.Give(p, EssenceName, 300)}");
        await Steps(10);
        var sword = Items.Read(p)!.Slots.First(s => !s.IsEmpty && s.Item == swordId);
        var gem = Items.Read(p)!.Slots.First(s => !s.IsEmpty && s.Item == gemId);
        Info($"sword {sword.Ref} at {sword.Slot}, gem {gem.Ref} level {gem.Level} at {gem.Slot}, essence {Count(p, essence)}: {Gear(p)}");
        Check(SocketOf(sword.ItemEntity) == (0, 0), "the sword has an empty socket");

        // ---- code: Socket (the game's cost) ----
        var e0 = Count(p, essence);
        _crafted.Clear();
        var sock = await Gems.Socket(p, sword.Ref, gem.Ref, forge);
        await Steps(2);
        var e1 = Count(p, essence);
        Info($"Socket: {sock}");
        Check(sock.Ok && SocketOf(sword.ItemEntity) == (gemId.Value, gem.ItemEntity.Value),
            $"Socket: the sword's socket holds the gem {gem.ItemEntity} ({SocketOf(sword.ItemEntity)})");
        Check(SlotOf(p, gem.ItemEntity) is null, "Socket: the gem left its backpack slot");
        Check(sock.Paid.Count == e0 - e1 && (sock.Paid.IsFree || sock.Paid.Item == essence),
            $"Socket: paid {sock.Paid}, the inventory lost {e0 - e1} essence");
        Check(_crafted.Count(c => c.ByCode && c.Action == GemCraftAction.Socket) == 1, $"Socket: one GemCraftedEvent by code ({_crafted.Count})");

        // ---- code: Upgrade of the socketed gem (the game's cost) ----
        _crafted.Clear();
        var up = await Gems.Upgrade(p, sword.Ref, forge);
        await Steps(2);
        var e2 = Count(p, essence);
        Info($"Upgrade: {up}");
        Check(up.Ok && up.LevelBefore == gem.Level && up.LevelAfter == gem.Level + 1, $"Upgrade: level {up.LevelBefore} -> {up.LevelAfter}");
        Check(up.Paid.Count == e1 - e2 && up.Paid.Count > 0 && up.Paid.Item == essence, $"Upgrade: paid {up.Paid}, the inventory lost {e1 - e2}");
        Check(_crafted.Count(c => c.ByCode && c.Action == GemCraftAction.Upgrade && c.LevelAfter == up.LevelAfter) == 1, "Upgrade: GemCraftedEvent");

        // ---- code: a free Upgrade ----
        var free = await Gems.Upgrade(p, sword.Ref, forge, new GemCraftOptions { Cost = GemCost.Free });
        await Steps(2);
        var e3 = Count(p, essence);
        Info($"free Upgrade: {free}");
        Check(free.Ok && free.LevelAfter == up.LevelAfter + 1 && e3 == e2 && free.Paid.IsFree, $"free Upgrade: level {free.LevelAfter}, essence {e2} -> {e3}");

        // ---- refusals that change nothing ----
        var occupied = await Gems.Socket(p, sword.Ref, sword.Ref, forge);
        Check(!occupied.Ok && occupied.Status == GemCraftStatus.NotFound, $"Socket of a sword into a sword is refused: {occupied.Status} ({occupied.Reason})");

        // ---- code: Unsocket ----
        _crafted.Clear();
        var un = await Gems.Unsocket(p, sword.Ref, forge);
        await Steps(2);
        var e4 = Count(p, essence);
        Info($"Unsocket: {un}");
        var back = SlotOf(p, gem.ItemEntity);
        Check(un.Ok && SocketOf(sword.ItemEntity) == (0, 0) && back is not null, $"Unsocket: the socket is empty, the gem is back at {back?.Slot}");
        Check(un.Paid.Count == e3 - e4, $"Unsocket: paid {un.Paid}, the inventory lost {e3 - e4}");
        Check(back is { } bk && bk.Level == free.LevelAfter, $"Unsocket: the gem kept its level {back?.Level}");
        var empty = await Gems.Unsocket(p, sword.Ref, forge);
        Check(empty.Status == GemCraftStatus.Occupied, $"Unsocket of an empty socket: {empty.Status} ({empty.Reason})");

        // ---- code: Upgrade of the loose gem with a replaced cost (1 essence) ----
        var gemNow = SlotOf(p, gem.ItemEntity)!;
        var repl = await Gems.Upgrade(p, gemNow.Ref, forge, new GemCraftOptions { Cost = new GemCost(essence, 1) });
        await Steps(2);
        var e5 = Count(p, essence);
        Info($"replaced-cost Upgrade of the loose gem: {repl}");
        Check(repl.Ok && repl.LevelAfter == free.LevelAfter + 1 && e4 - e5 == 1 && repl.Paid == new GemCost(essence, 1),
            $"replaced cost: level {repl.LevelAfter}, essence {e4} -> {e5}, paid {repl.Paid}");

        // ---- up to the forge's maximum (10), then refused ----
        var level = repl.LevelAfter;
        for (var i = 0; i < 12 && level < 10; i++)
        {
            var r = await Gems.Upgrade(p, gemNow.Ref, forge, new GemCraftOptions { Cost = GemCost.Free });
            if (!r.Ok) { Info($"upgrade to the maximum stopped: {r}"); break; }
            level = r.LevelAfter;
        }
        var max = await Gems.Upgrade(p, gemNow.Ref, forge);
        Check(level == 10 && max.Status == GemCraftStatus.MaxLevel && LevelOf(p, gem.ItemEntity) == 10,
            $"at the forge's maximum (10) an Upgrade is refused: {max.Status} ({max.Reason}), level {LevelOf(p, gem.ItemEntity)}");

        // ---- players' actions: the game's own path, held, one canceled, one allowed ----
        Events.On<GemCraftingEvent>(e =>
        {
            _crafting.Add(e);
            Info($"event: {e} (cost {e.Cost}, version {e.Version})");
            if (_cancelNext > 0) { _cancelNext--; e.Cancel(); }
        });
        await Steps(2);
        gemNow = SlotOf(p, gem.ItemEntity)!;
        async Task<uint> Inject(GemCraftAction action, EntityId weapon, SlotId source)
        {
            var consumed = World.ReadBytes(p, "ServerConsumedPlayerInput");
            var v = (consumed is { Length: > Consumed + 4 } ? BitConverter.ToUInt32(consumed, Consumed) : 0) + 1;
            World.WriteRaw(p, "PlayerInput", InWeapon, weapon.Value);
            World.WriteRaw(p, "PlayerInput", InSource, source.Value);
            World.WriteRaw(p, "PlayerInput", InForge, forge.Value);
            World.WriteRaw(p, "PlayerInput", InMaterial, essence.Value);
            World.WriteRaw(p, "PlayerInput", InAction, (byte)action);
            World.WriteRaw(p, "PlayerInput", InVersion, v);
            await Steps(6);
            return v;
        }
        uint ConsumedNow() => World.ReadBytes(p, "ServerConsumedPlayerInput") is { Length: > Consumed + 4 } c ? BitConverter.ToUInt32(c, Consumed) : 0;

        var e6 = Count(p, essence);
        _crafted.Clear();
        _crafting.Clear();
        _cancelNext = 1;
        var v1 = await Inject(GemCraftAction.Socket, sword.ItemEntity, gemNow.Slot);
        Check(_crafting.Count == 1 && _crafting[0].Version == v1 && _crafting[0].Action == GemCraftAction.Socket &&
              _crafting[0].Gem.Entity.Id == gem.ItemEntity && _crafting[0].Carrier.Entity.Id == sword.ItemEntity && _crafting[0].Forge == forge,
            $"player's Socket: one GemCraftingEvent with the action, carrier, gem and forge ({_crafting.Count})");
        Check(ConsumedNow() == v1, $"canceled: the game consumed version {v1} ({ConsumedNow()})");
        Check(SocketOf(sword.ItemEntity) == (0, 0) && SlotOf(p, gem.ItemEntity) is not null && Count(p, essence) == e6 && _crafted.Count == 0,
            $"canceled: nothing moved, nothing paid ({Count(p, essence)} essence, socket {SocketOf(sword.ItemEntity)}, {_crafted.Count} crafted)");

        _crafting.Clear();
        var v2 = await Inject(GemCraftAction.Socket, sword.ItemEntity, gemNow.Slot);
        var socketCost = _crafting.FirstOrDefault()?.Cost ?? default;
        Check(_crafting.Count == 1 && _crafting[0].Version == v2, $"player's second Socket: GemCraftingEvent, not canceled (cost {socketCost})");
        Check(SocketOf(sword.ItemEntity) == (gemId.Value, gem.ItemEntity.Value), $"allowed: the game socketed the gem ({SocketOf(sword.ItemEntity)})");
        Check(_crafted.Count == 1 && !_crafted[0].ByCode && _crafted[0].Action == GemCraftAction.Socket && _crafted[0].Paid.Count == e6 - Count(p, essence),
            $"allowed: one GemCraftedEvent of the player's action, paid {(_crafted.Count > 0 ? _crafted[0].Paid : default)} ({e6 - Count(p, essence)} counted)");
        Check(socketCost.Count == e6 - Count(p, essence), $"the cost GemCraftingEvent announced ({socketCost}) is what the game charged ({e6 - Count(p, essence)})");

        // ---- code: Recycle of the socketed gem ----
        Events.On<GemCraftingEvent>(_ => { });  // keep holding (a handler listens) while code runs: code is never held
        var e7 = Count(p, essence);
        _crafted.Clear();
        var rec = await Gems.Recycle(p, sword.Ref, forge);
        await Steps(2);
        var e8 = Count(p, essence);
        Info($"Recycle: {rec}");
        Check(rec.Ok && SocketOf(sword.ItemEntity) == (0, 0) && SlotOf(p, gem.ItemEntity) is null, "Recycle: the gem is gone, the socket empty");
        Check(rec.Received.Item == essence && rec.Received.Count == e8 - e7 && rec.Received.Count > 0,
            $"Recycle: received {rec.Received}, the inventory gained {e8 - e7}");
        Check(_crafted.Count(c => c.ByCode && c.Action == GemCraftAction.Recycle) == 1, "Recycle: GemCraftedEvent");

        await ReviewChecks(p, forge, sword, gemId, essence);

        // ---- no essence: an Upgrade cannot be paid ----
        await Items.Give(p, GemName, 1, new Gear(3));
        await Steps(4);
        var gem2 = Items.Read(p)!.Slots.First(s => !s.IsEmpty && s.Item == gemId);
        var all = Count(p, essence);
        if (all > 0) await Items.Take(p, essence, all);
        await Steps(2);
        var poor = await Gems.Upgrade(p, gem2.Ref, forge);
        Check(poor.Status == GemCraftStatus.NotEnough && LevelOf(p, gem2.ItemEntity) == gem2.Level,
            $"no essence: Upgrade refused ({poor.Status}: {poor.Reason}), level still {LevelOf(p, gem2.ItemEntity)}");
        var noForge = await Gems.Upgrade(p, gem2.Ref, p);
        Check(noForge.Status == GemCraftStatus.NoForge, $"a player is no forge: {noForge.Status} ({noForge.Reason})");
        Info($"inventory at the end: {Gear(p)}");
    }

    // ---- review (G3 review-and-fix): the forgeless return for G4, the native non-gem refusal, stale refs, gear costs ----
    private static readonly Type GemCraftingType = typeof(Gems).Assembly.GetType("Kiln.GemCrafting", throwOnError: true)!;

    // internal GemCrafting.UnsocketWithoutForge(EntityId owner, ItemInstanceRef carrier): what Gems.ReturnSocketedGems calls
    private static Task<(bool Ok, string? Reason)> ReturnGem(EntityId owner, ItemInstanceRef carrier) =>
        (Task<(bool Ok, string? Reason)>)GemCraftingType.GetMethod("UnsocketWithoutForge",
                System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic, [typeof(EntityId), typeof(ItemInstanceRef)])!
            .Invoke(null, [owner, carrier])!;

    // a request straight to kiln.gemcraft, past Kiln.Abilities' own checks (what a buggy or hostile native caller can send)
    private static Task<GemCraftResult> Raw(GemCraftAction action, EntityId player, uint carrier, ulong gemSlot, uint gem, EntityId forge)
    {
        var rt = typeof(Gems).Assembly.GetType("Kiln.GemcraftNative+Request", throwOnError: true)!;
        var req = Activator.CreateInstance(rt)!;
        void Set(string f, object v) => rt.GetField(f)!.SetValue(req, v);
        Set("Size", 56u);
        Set("Action", (uint)action);
        Set("Player", player.Value);
        Set("Carrier", carrier);
        Set("GemSlot", gemSlot);
        Set("Gem", gem);
        Set("Forge", forge.Value);
        var send = GemCraftingType.GetMethod("Send", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)!;
        return (Task<GemCraftResult>)send.Invoke(null, [req, action, player, ItemInstanceRef.None, ItemInstanceRef.None, forge, null, "lab"])!;
    }

    private async Task ReviewChecks(EntityId p, EntityId forge, InventorySlot sword, ItemId gemId, ItemId essence)
    {
        // a gem in the sword's socket (code, free), then out again without any forge
        Info($"give: {await Items.Give(p, GemName, 1, new Gear(4))}");
        await Steps(4);
        var g4 = Items.Read(p)!.Slots.First(s => !s.IsEmpty && s.Item == gemId && s.Level == 4);
        var put = await Gems.Socket(p, sword.Ref, g4.Ref, forge, new GemCraftOptions { Cost = GemCost.Free });
        await Steps(2);
        Check(put.Ok && SocketOf(sword.ItemEntity) == (gemId.Value, g4.ItemEntity.Value), $"review: a level 4 gem socketed for the return ({put.Status})");
        var e0 = Count(p, essence);
        _crafted.Clear();
        var (ok, why) = await ReturnGem(p, sword.Ref);
        await Steps(2);
        var back = SlotOf(p, g4.ItemEntity);
        Info($"review: UnsocketWithoutForge: {ok} {why}; {Gear(p)}");
        Check(ok && why is null, $"review: UnsocketWithoutForge answered ({ok}, {why})");
        Check(SocketOf(sword.ItemEntity) == (0, 0) && back is not null && back.Level == 4,
            $"review: no forge: the socket is empty, the same gem entity {g4.ItemEntity} is back at {back?.Slot} level {back?.Level}");
        Check(Count(p, essence) == e0, $"review: nothing paid ({e0} -> {Count(p, essence)} essence)");
        Check(_crafted.Count(c => c.ByCode && c.Action == GemCraftAction.Unsocket && c.Forge.IsNone && c.Gem.Entity.Id == g4.ItemEntity) == 1,
            $"review: one GemCraftedEvent (Unsocket, by code, no forge) ({_crafted.Count})");
        var (again, againWhy) = await ReturnGem(p, sword.Ref);
        Check(!again && againWhy is { } aw && aw.Contains("empty"), $"review: the empty socket: ({again}, {againWhy})");
        var (chest, chestWhy) = await ReturnGem(forge, sword.Ref);
        Check(!chest && chestWhy is { } cw && cw.Contains("player"), $"review: a non-player owner is refused at once: ({chest}, {chestWhy})");

        // the game's handler recycles whatever item entity the request's slot holds: kiln.gemcraft refuses a non-gem
        var swordNow = SlotOf(p, sword.ItemEntity)!;
        var e1 = Count(p, essence);
        var raw = await Raw(GemCraftAction.Recycle, p, 0, swordNow.Slot.Value, sword.ItemEntity.Value, forge);
        await Steps(2);
        Info($"review: a raw Recycle of the sword's slot: {raw}");
        Check(raw.Status == GemCraftStatus.NotFound && SlotOf(p, sword.ItemEntity) is not null && Count(p, essence) == e1,
            $"review: Recycle of a sword named as a loose gem: {raw.Status} ({raw.Reason}); the sword is still there, no essence given");
        var rawUp = await Raw(GemCraftAction.Upgrade, p, 0, swordNow.Slot.Value, sword.ItemEntity.Value, forge);
        Check(rawUp.Status == GemCraftStatus.NotFound && SlotOf(p, sword.ItemEntity)?.Level == swordNow.Level,
            $"review: Upgrade of a sword's level through the forge: {rawUp.Status}; level {SlotOf(p, sword.ItemEntity)?.Level}");

        // a stale ref (the same entity id with another stamp) acts on nothing
        var loose = SlotOf(p, g4.ItemEntity)!;
        if (loose.Ref.Entity.Serial != 0)
        {
            var stale = new ItemInstanceRef(new EntityRef(loose.ItemEntity, loose.Ref.Entity.Serial ^ 1), loose.Item);
            var st = await Gems.Recycle(p, stale, forge);
            Check(st.Status == GemCraftStatus.NotFound && SlotOf(p, g4.ItemEntity) is not null, $"review: a stale gem ref: {st.Status} ({st.Reason})");
        }
        else
        {
            Info("review: the gem's ref has no stamp (items.instances off): stale-ref check skipped");
        }

        // a replaced price that is gear (a gem) throws
        try
        {
            await Gems.Upgrade(p, loose.Ref, forge, new GemCraftOptions { Cost = new GemCost(gemId, 1) });
            Check(false, "review: a gem as the replaced price was accepted");
        }
        catch (ArgumentException e)
        {
            Check(SlotOf(p, g4.ItemEntity)?.Level == 4, $"review: a gem as the replaced price throws ({e.Message})");
        }
    }
}
