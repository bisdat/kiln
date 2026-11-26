// SPDX-License-Identifier: MIT
// Lab-only (see RegistryProofTestMod.cs): the producer's phase 2, once the lab's world runs. It loads drop tables the way the
// game does for a player (a few of the game's loot containers spawned next to a container of the world, else at the lab world's
// spot: their default_inventory program acquires each table), samples the drop rows over them, and drives real loot rolls for
// the loot rows' read times. A separate part of the class because the main part is an unsafe context, where C# allows no
// await.
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Kiln;

namespace RegistryProofTest;

public sealed partial class RegistryProofTestMod
{
    // ---- phase 2: drop tables and loot rolls (the world runs) ----
    private string _noTables = NoDropTables;  // why no drop table was sampled (Kiln words, no parentheses: review3 N2)
    private const string Tag = "registry-proof-test";
    // The game's loot containers whose tables the lab loads: two whose tables hold a random-loot stack (for the rolls) and the
    // scavenger's (the table earlier labs loaded without a player). Their tables carry these templates' names (drop table ids
    // are made from the first template that uses a table).
    private static readonly string[] TableTemplates = { "base_lootchest_random_cairn_sarcophagus_wood", "base_trashheap", "lootcontainer_enemy_scavenger_melee01" };
    private readonly Dictionary<string, string> _tableTemplate = new();  // a loaded drop table's Kiln id -> the template that loaded it
    private static readonly WorldPosition LabSpot = new(3855.8, 834.5, 1399.8);

    private async Task Steps(int n)
    {
        for (var i = 0; i < n; i++) await Context.Scheduler.NextTick();
    }

    private async Task Later()
    {
        try
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (!(Spawn.Available && Spawn.WorldRunning && Items.Available && Items.Status() is { Read: true }) && sw.Elapsed.TotalSeconds < 300)
                await Context.Scheduler.NextTick();
            var loaded = 0;
            if (!Spawn.Available || !Spawn.WorldRunning) _noTables = "the lab's world did not run, so the lab loaded no drop table";
            else
            {
                await Steps(60);
                loaded = await LoadDropTables();
            }
            SampleRegistry("drops");
            if (_at is not null) await Rolls();
        }
        catch (Exception x) { Check(false, x.ToString()); }
        finally
        {
            foreach (var e in _spawned) try { Spawn.Destroy(e); } catch (Exception) { }
            try { WriteSuite(); } catch (Exception x) { Check(false, "writing the suite: " + x.Message); }
            Summary();
        }
    }

    /// <summary>The Kiln ids of the drop tables the world has loaded (the ones Content lists).</summary>
    private HashSet<string> LoadedTables()
    {
        var table = _ids["drops"]?.AsObject();
        var ids = new HashSet<string>();
        if (table is null) return ids;
        foreach (var e in Content.Drops)
            if (((string?)table[e.Guid.ToString()] ?? (string?)table[BytesText(e.Guid)]) is { } id) ids.Add(id);
        return ids;
    }

    /// <summary>Loads drop tables the way the game does for a player: the game's loot containers spawned next to a container of
    /// the lab's world (the server runs the area around it), whose default_inventory program acquires their tables (kiln.content's
    /// acquire hook registers each, and kiln.registry.live binds it). How many tables the world has loaded then.</summary>
    private async Task<int> LoadDropTables()
    {
        // the world's containers become readable a while after the world runs: look for one for up to two minutes
        for (var t = 0; t < 120 && _at is null; t++)
        {
            try
            {
                foreach (var kinds in new[] { InventoryKinds.Containers | InventoryKinds.NonEmpty, InventoryKinds.Containers })
                {
                    foreach (var c in Items.FindInventories(kinds))
                        if (Spawn.Inspect(c) is { } i) { _at = i.Position with { Y = i.Position.Y + 1.5 }; break; }
                    if (_at is not null) break;
                }
            }
            catch (KilnUnavailableException) { }
            if (_at is null) await Steps(25);
        }
        if (_at is null)
        {
            _at = LabSpot;  // the spot the lab world's other spawn tests use (blocks-loot-test)
            Log.Info($"registry-proof-test: drops: no container of the world found; spawning at the lab world's {LabSpot}");
        }
        var at = _at.Value;
        var tableIds = _ids["drops"]?.AsObject().Select(p => (string?)p.Value).Where(v => v != null).Cast<string>().ToHashSet() ?? new HashSet<string>();
        var byName = new Dictionary<string, TemplateInfo>(StringComparer.Ordinal);
        foreach (var t in Spawn.Templates) byName.TryAdd(t.Name.ToLowerInvariant(), t);
        var names = TableTemplates.Where(n => byName.ContainsKey(n) && tableIds.Contains("game:" + n)).ToList();
        if (names.Count == 0) names = byName.Keys.Where(n => n.StartsWith("lootcontainer_", StringComparison.Ordinal) && tableIds.Contains("game:" + n)).Order().Take(3).ToList();
        for (var k = 0; k < names.Count; k++)
        {
            var before = LoadedTables();
            var template = byName[names[k]].Name;
            EntityId e;
            try { e = await Spawn.Entity(template, at with { X = at.X + 2.5 * k }, options: new SpawnOptions { Tag = Tag, Flags = SpawnFlags.Transient }); }
            catch (Exception x) { Log.Info($"registry-proof-test: spawning {template} failed: {x.Message}"); continue; }
            _spawned.Add(e);
            for (var i = 0; i < 200 && LoadedTables().Count == before.Count; i++) await Context.Scheduler.NextTick();
            var added = LoadedTables().Except(before).ToList();
            foreach (var id in added) _tableTemplate.TryAdd(id, template);
            Log.Info($"registry-proof-test: drops: {template} spawned as {e}; drop tables {before.Count} -> {before.Count + added.Count} ({string.Join(", ", added)})");
        }
        var n = LoadedTables().Count;
        if (n == 0) _noTables = "the game loaded no drop table for the loot containers the lab spawned";
        Check(n > 0, $"drop tables loaded in the lab's world without a player: {n}");
        return n;
    }

    private sealed record Rolled(uint Item, int Count, int Rarity);

    // keen::ecs::PickupLootSlot on 26430253 (32 B): what a random pickup spawner rolled (World.TryRead; lab-only layout)
    [StructLayout(LayoutKind.Explicit, Size = 32)]
    private struct PickupLootSlot
    {
        [FieldOffset(0x00)] public Guid GenericPickupItem;  // the TemplateReference of the pickup a rolled item spawns as
        [FieldOffset(0x10)] public uint SpawnedPickupEntity;
        [FieldOffset(0x14)] public byte ShouldSpawnItem;
        [FieldOffset(0x18)] public uint SpawnItem;
        [FieldOffset(0x1c)] public byte SpawnItemLevel;
        [FieldOffset(0x1d)] public byte SpawnItemRarity;
        [FieldOffset(0x1e)] public ushort SpawnItemAmount;
    }

    // The game's random pickup spawner (random_pickup_loot_spawner rolls it through the single-slot roll: LootFind, the rarity
    // mask, the fixed rarity, the stack counts) and the loot labels it rolls from: a creature template's (RandomLoot.defaultLabels;
    // the spawner template has none of its own, world placements give theirs).
    private const string Spawner = "Base_LootPickup_RandomLoot_Spawner";
    // review registry-proof: the spawner program (0x1abdef on 26430253) returns before any roll while the slot's
    // genericPickupItem (+0x00) is empty, so the harness gives the generic random-loot pickup in the slot it spawns with. In
    // the 31.x lab that did not make a spawner roll either (the loot generation version check 0x16c050 is left; docs/re/content.md
    // section 11): the slot is logged after the wait, and the roll stays INCONCLUSIVE
    private const string GenericPickup = "Base_LootPickup_RandomLoot_Generic";
    private byte[]? _labels;
    private string _labelsFrom = "";
    private bool _slotLogged;

    /// <summary>Fresh random pickup spawners, each rolling once; what each rolled (item, count, rarity) as its PickupLootSlot
    /// records it. The pickups they spawn are removed with them.</summary>
    private async Task<List<Rolled>> Roll(int n)
    {
        var made = new List<EntityId>();
        var at = _at!.Value;
        var loot = new byte[36];  // keen::ecs::RandomLoot: disableRandomLoot, defaultLabels, forwardSpecialItemLabels
        _labels!.CopyTo(loot, 4);
        var components = new List<SpawnComponent> { SpawnComponent.Of("RandomLoot", loot) };
        if (Spawn.Templates.FirstOrDefault(t => t.Name == GenericPickup) is { } generic)
        {
            var slot = new byte[32];  // keen::ecs::PickupLootSlot: genericPickupItem, the rest as a fresh slot has it (zero)
            generic.Id.ToByteArray().CopyTo(slot, 0);
            components.Add(SpawnComponent.Of("PickupLootSlot", slot));
        }
        var options = new SpawnOptions { Tag = Tag, Flags = SpawnFlags.Transient, Components = components.ToArray() };
        for (var i = 0; i < n; i++)
        {
            try { made.Add(await Spawn.Entity(Spawner, at with { X = at.X + 2.0 * (i % 5), Z = at.Z + 4.0 + 2.0 * (i / 5) }, options: options)); }
            catch (Exception x) { Log.Info($"registry-proof-test: roll: spawning {Spawner} failed: {x.Message}"); break; }
        }
        bool Rolled(EntityId e) => World.TryRead<PickupLootSlot>(e, "PickupLootSlot", out var v) && (v.SpawnItem != 0 || v.SpawnedPickupEntity != 0);
        for (var t = 0; t < 150 && made.Any(e => !Rolled(e)); t++) await Context.Scheduler.NextTick();
        if (!_slotLogged && made.Count > 0)
        {
            // what the spawner's slot holds after the wait: why a spawner did not roll, when none did
            _slotLogged = true;
            var ok = World.TryRead<PickupLootSlot>(made[0], "PickupLootSlot", out var s0);
            Log.Info($"registry-proof-test: roll: {Spawner} {made[0]}'s PickupLootSlot after the wait: " + (ok
                ? $"genericPickupItem {s0.GenericPickupItem}, shouldSpawnItem {s0.ShouldSpawnItem}, spawnItem {s0.SpawnItem:x8}, spawned {s0.SpawnedPickupEntity}, rarity {s0.SpawnItemRarity}, amount {s0.SpawnItemAmount}"
                : "not readable"));
        }
        var rolled = new List<Rolled>();
        foreach (var e in made)
        {
            if (World.TryRead<PickupLootSlot>(e, "PickupLootSlot", out var v) && v.SpawnItem != 0) rolled.Add(new Rolled(v.SpawnItem, v.SpawnItemAmount, v.SpawnItemRarity));
            if (v.SpawnedPickupEntity != 0) try { Spawn.Destroy(new EntityId(v.SpawnedPickupEntity)); } catch (Exception) { }
            try { Spawn.Destroy(e); } catch (Exception) { }
        }
        if (rolled.Count < made.Count) Log.Info($"registry-proof-test: roll: {made.Count - rolled.Count} of {made.Count} spawners rolled nothing");
        return rolled;
    }

    private void Effect(string field, string setting, bool ok, string observed, bool inconclusive = false)
    {
        _effects.Add(new JsonObject { ["field"] = field, ["setting"] = setting, ["observed"] = observed, ["result"] = inconclusive ? "inconclusive" : ok ? "followed" : "not followed" });
        if (inconclusive) Log.Info($"registry-proof-test: INCONCLUSIVE roll {field} ({setting}): {observed}");
        else Check(ok, $"roll {field} ({setting}): {observed}");
    }

    private static string Show(IEnumerable<Rolled> r) => string.Join(" ", r.Select(x => $"{x.Item:x8}x{x.Count}/r{x.Rarity}"));

    /// <summary>The loot rolls: when the game reads stack.scalable, stack.maxScaled, rarities and fixedRarity. Each step gives
    /// every loot entry its values through one Manual kiln.registry.live change (reverted after) and fresh random pickup spawners
    /// roll (their PickupLootSlot records item, count and rarity). A drop table's random-loot stack is not used: the game's
    /// default_inventory program has no loot generation system, so a default inventory never rolls one (docs/re/content.md
    /// section 11). The results go into the suite's "effects" and the log; kilnctl does not judge them (when the game reads a
    /// field is regmap/common's, from these labs and the RE notes).</summary>
    private async Task Rolls()
    {
        if (!Spawn.SupportsComponents) { Effect("loot", "random pickup spawners", false, "kiln.spawn cannot give a spawn its components on this build", inconclusive: true); return; }
        if (!Spawn.Templates.Any(t => t.Name == Spawner)) { Effect("loot", "random pickup spawners", false, $"no {Spawner} template on this build", inconclusive: true); return; }
        // loot labels that roll: the first creature template's whose labels give a baseline roll
        List<Rolled> baseline = new();
        try
        {
            foreach (var c in Creatures.Templates.Where(c => c.LootLabels != Guid.Empty).GroupBy(c => c.LootLabels).Select(g => g.First()).Take(8))
            {
                _labels = c.LootLabels.ToByteArray();
                _labelsFrom = c.Name;
                baseline = await Roll(10);
                if (baseline.Count > 0) break;
            }
        }
        catch (KilnUnavailableException x) { Effect("loot", "random pickup spawners", false, "no creature catalog for loot labels: " + x.Message, inconclusive: true); return; }
        Log.Info($"registry-proof-test: rolls: {Spawner} with {_labelsFrom}'s loot labels, baseline {baseline.Count} rolls: {Show(baseline)}");
        if (baseline.Count == 0) { Effect("loot", "random pickup spawners", false, "no spawner rolled with any of the creatures' loot labels tried", inconclusive: true); return; }
        async Task<List<Rolled>?> Step(string what, JsonArray edits)
        {
            var c = Change(edits, out var h);
            if (!c.Ok) { Effect("loot", what, false, "the loot change was refused: " + c.Why); return null; }
            try { return await Roll(10); }
            finally { Finish(h, true); }
        }
        const string all = "*";
        JsonArray Stack(int n, bool scalable, int maxScaled) => new(Edit("loot", all, "stack.min", JsonValue.Create(n)), Edit("loot", all, "stack.max", JsonValue.Create(n)),
            Edit("loot", all, "stack.scalable", JsonValue.Create(scalable)), Edit("loot", all, "stack.maxScaled", JsonValue.Create(maxScaled)));
        List<int> Counts(List<Rolled>? r) => r?.Select(x => x.Count).ToList() ?? new List<int>();
        string Seen(List<int> c) => $"{c.Count} rolls, counts {string.Join(",", c)}";
        var c1 = Counts(await Step("stack 3, not scalable", Stack(3, false, 1000)));
        Effect("loot.stack.min, loot.stack.max", "3..3, not scalable", c1.Count > 0 && c1.All(x => x == 3), Seen(c1), inconclusive: c1.Count == 0);
        var c2 = Counts(await Step("stack 3, scalable, maxScaled 1000", Stack(3, true, 1000)));
        var amount = c2.Count > 0 ? c2.Average() / 3 : 0;  // the world's loot amount (the scale the roll applies)
        Effect("loot.stack.scalable", "true (3..3, maxScaled 1000)", c2.Count > 0 && c2.All(x => x == c2[0]) && c2[0] != 3,
            Seen(c2) + $" (not scalable: 3; the world's loot amount {amount:0.##})", inconclusive: c2.Count == 0 || amount == 1);
        foreach (var cap in new[] { 4, 5 })
        {
            var c = Counts(await Step($"stack 3, scalable, maxScaled {cap}", Stack(3, true, cap)));
            var want = ScaledCount(3, amount, cap);
            Effect("loot.stack.maxScaled", $"{cap} (3..3, scalable)", c.Count > 0 && c.All(x => x == want), Seen(c) + $" (expected {want})",
                inconclusive: c.Count == 0 || ScaledCount(3, amount, 1000) <= cap);
        }
        async Task Rarity(string field, string setting, JsonArray edits, int want)
        {
            var r = await Step(setting, edits) ?? new List<Rolled>();
            var wrong = r.Where(x => x.Rarity != want).ToList();
            Effect(field, setting, r.Count > 0 && wrong.Count == 0, $"{r.Count} rolls: {Show(r)}" + (wrong.Count > 0 ? $"; {wrong.Count} not at rarity {want}" : ""), inconclusive: r.Count == 0);
        }
        await Rarity("loot.fixedRarity", "legendary for every entry", new JsonArray(Edit("loot", all, "fixedRarity", JsonValue.Create("legendary"))), 4);
        await Rarity("loot.fixedRarity", "common for every entry", new JsonArray(Edit("loot", all, "fixedRarity", JsonValue.Create("common"))), 0);
        JsonArray Mask(string r) => new(Edit("loot", all, "fixedRarity", JsonValue.Create("none")), Edit("loot", all, "rarities", new JsonArray(JsonValue.Create(r))));
        await Rarity("loot.rarities", "rare only, no fixed rarity", Mask("rare"), 2);
        await Rarity("loot.rarities", "uncommon only, no fixed rarity", Mask("uncommon"), 1);
    }
}
