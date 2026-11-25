// SPDX-License-Identifier: MIT
// Lab-only (see RegistryProofTestMod.cs): the producer's phase 2, once the lab's world runs. It loads drop tables the way the
// game does for a player (a few of the game's loot containers spawned next to a container of the world: their default_inventory
// program acquires each table), samples the drop rows over them, and drives real loot rolls for the loot rows' read times. A
// separate part of the class because the main part is an unsafe context, where C# allows no await.
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
            if (loaded > 0) await Rolls();
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
        try
        {
            foreach (var c in Items.FindInventories(InventoryKinds.Containers | InventoryKinds.NonEmpty))
                if (Spawn.Inspect(c) is { } i) { _at = i.Position with { Y = i.Position.Y + 1.5 }; break; }
        }
        catch (KilnUnavailableException) { }
        if (_at is not { } at) { _noTables = "the lab's world has no container to load a drop table next to"; return LoadedTables().Count; }
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

    private sealed record Rolled(uint Item, int Count, bool Gear, int? Rarity, int Container);

    /// <summary>Fresh containers of <paramref name="template"/>, each rolling the table's random stack once; what came out
    /// that is not one of the table's own items (gear per slot, other items summed per container).</summary>
    private async Task<List<Rolled>> Roll(string template, HashSet<uint> own, int n)
    {
        var made = new List<EntityId>();
        var at = _at!.Value;
        for (var i = 0; i < n; i++)
        {
            try { made.Add(await Spawn.Entity(template, at with { X = at.X + 2.0 * (i % 5), Z = at.Z + 3.0 + 2.0 * (i / 5) }, options: new SpawnOptions { Tag = Tag, Flags = SpawnFlags.Transient })); }
            catch (Exception x) { Log.Info($"registry-proof-test: roll: spawning {template} failed: {x.Message}"); }
        }
        for (var t = 0; t < 200 && made.Any(e => Items.Read(e) is not { UsedSlots: > 0 }); t++) await Context.Scheduler.NextTick();
        await Steps(5);
        var rolled = new List<Rolled>();
        for (var c = 0; c < made.Count; c++)
        {
            if (Items.Read(made[c]) is not { } inv) continue;
            var slots = inv.Slots.Where(x => !x.IsEmpty && !own.Contains(x.Item.Value)).ToList();
            foreach (var g in slots.Where(x => x.IsGear)) rolled.Add(new Rolled(g.Item.Value, g.Count, true, g.Rarity is { } r ? (int)r : null, c));
            foreach (var g in slots.Where(x => !x.IsGear).GroupBy(x => x.Item.Value)) rolled.Add(new Rolled(g.Key, g.Sum(x => x.Count), false, null, c));
        }
        foreach (var e in made) try { Spawn.Destroy(e); } catch (Exception) { }
        return rolled;
    }

    private void Effect(string field, string setting, bool ok, string observed, bool inconclusive = false)
    {
        _effects.Add(new JsonObject { ["field"] = field, ["setting"] = setting, ["observed"] = observed, ["result"] = inconclusive ? "inconclusive" : ok ? "followed" : "not followed" });
        if (inconclusive) Log.Info($"registry-proof-test: INCONCLUSIVE roll {field} ({setting}): {observed}");
        else Check(ok, $"roll {field} ({setting}): {observed}");
    }

    private static string Show(IEnumerable<Rolled> r) => string.Join(" ", r.Select(x => $"{x.Item:x8}x{x.Count}{(x.Gear ? "/r" + (x.Rarity?.ToString() ?? "?") : "")}"));

    /// <summary>The rarity a gear item of <paramref name="item"/> shows when created at <paramref name="asked"/> (its perk
    /// slots and type rarity read from the game's item data).</summary>
    private static int Shown(uint item, int asked)
    {
        var e = Content.Items.Get(item);
        if (e is null) return -1;
        var slots = Enumerable.Range(0, 5).Select(k => { try { return e.Get($"perkIds[{k}]").Integer != 0; } catch (Exception) { return false; } }).ToArray();
        int type;
        try { type = (int)e.Get("rarity").Integer; } catch (Exception) { type = -1; }
        return ShownRarity(asked, slots, type);
    }

    /// <summary>The loot rolls: when the game reads stack.scalable, stack.maxScaled, rarities and fixedRarity. A loaded drop
    /// table with a random-loot stack is made to roll at every application (its chance 1, reverted after), each step gives
    /// every loot entry its values through one Manual kiln.registry.live change (reverted after), and fresh containers of the
    /// table's template roll. The results go into the suite's "effects" and the log; kilnctl does not judge them (when the
    /// game reads a field is regmap/common's, from these labs and the RE notes).</summary>
    private async Task Rolls()
    {
        string? tableId = null, template = null;
        var random = -1;
        foreach (var (id, tpl) in _tableTemplate)
        {
            if (Read("drops", id, "entries") is not { } text || JsonNode.Parse(text) is not JsonArray list) continue;
            for (var i = 0; i < list.Count && random < 0; i++)
                if (list[i] is JsonObject el && (string?)el["item"] is { } item && item.StartsWith('#')) random = i;
            if (random >= 0) { tableId = id; template = tpl; break; }
        }
        if (tableId is null || template is null)
        {
            Effect("loot", "a table that rolls random loot", false, "no loaded drop table has a random-loot stack, so no roll was driven", inconclusive: true);
            return;
        }
        var own = new HashSet<uint>();
        foreach (var e in Content.Drops)
            if (((string?)_ids["drops"]?[e.Guid.ToString()] ?? (string?)_ids["drops"]?[BytesText(e.Guid)]) == tableId)
                foreach (var st in e.Stacks) if (!st.Item.IsNone) own.Add(st.Item.Value);
        var always = Change("drops", tableId, $"entries[{random}].chance", JsonValue.Create(1.0), out var alwaysHandle);
        Log.Info($"registry-proof-test: rolls: {template} ({tableId}), its random stack entries[{random}] at chance 1: {(always.Ok ? "applied" : always.Why)}");
        if (!always.Ok) { Effect("drops.entries[].chance", "1 for the random stack", false, always.Why); return; }
        try
        {
            async Task<List<Rolled>?> Step(string what, JsonArray edits)
            {
                var c = Change(edits, out var h);
                if (!c.Ok) { Effect("loot", what, false, "the loot change was refused: " + c.Why); return null; }
                try { return await Roll(template, own, 10); }
                finally { Finish(h, true); }
            }
            var all = "*";
            JsonArray Stack(int n, bool scalable, int maxScaled) => new(Edit("loot", all, "stack.min", JsonValue.Create(n)), Edit("loot", all, "stack.max", JsonValue.Create(n)),
                Edit("loot", all, "stack.scalable", JsonValue.Create(scalable)), Edit("loot", all, "stack.maxScaled", JsonValue.Create(maxScaled)));
            var baseline = await Roll(template, own, 10);
            Log.Info($"registry-proof-test: rolls: baseline {baseline.Count} rolled stacks: {Show(baseline)}");
            // counts: only items whose stacks hold the numbers (a max stack below 8 would cut them)
            bool Stackable(uint item) => Content.Items.Get(item) is { } e && e.GetNumber("maxStackSize") >= 8;
            List<int> Counts(List<Rolled> r) => r.Where(x => !x.Gear && Stackable(x.Item)).Select(x => x.Count).ToList();
            var control = await Step("stack 3, not scalable", Stack(3, false, 1000));
            var c1 = control is null ? new List<int>() : Counts(control);
            var factor = c1.Count > 0 && c1.Distinct().Count() == 1 && c1[0] % 3 == 0 ? c1[0] / 3 : 0;  // what the table's application multiplies by
            Effect("loot.stack.min, loot.stack.max", "3..3, not scalable", factor > 0, $"{c1.Count} stackable rolls, counts {string.Join(",", c1)}", inconclusive: c1.Count == 0);
            var scaled = await Step("stack 3, scalable, maxScaled 1000", Stack(3, true, 1000));
            var c2 = scaled is null ? new List<int>() : Counts(scaled);
            Effect("loot.stack.scalable", "true (3..3, maxScaled 1000)", c2.Count > 0 && factor > 0 && c2.Distinct().Count() == 1 && c2[0] != 3 * factor,
                $"{c2.Count} stackable rolls, counts {string.Join(",", c2)} (not scalable: {3 * factor})", inconclusive: c2.Count == 0 || factor == 0);
            var amount = factor > 0 && c2.Count > 0 ? c2[0] / (double)factor / 3 : 0;  // the world's loot amount
            foreach (var cap in new[] { 4, 5 })
            {
                var r = await Step($"stack 3, scalable, maxScaled {cap}", Stack(3, true, cap));
                var c = r is null ? new List<int>() : Counts(r);
                var want = factor * ScaledCount(3, amount, cap);
                Effect("loot.stack.maxScaled", $"{cap} (3..3, scalable)", c.Count > 0 && c.All(x => x == want), $"{c.Count} stackable rolls, counts {string.Join(",", c)} (expected {want})",
                    inconclusive: c.Count == 0 || factor == 0 || ScaledCount(3, amount, 1000) <= cap);
            }
            async Task Rarity(string field, string setting, JsonArray edits, int asked)
            {
                var r = await Step(setting, edits);
                var gear = r?.Where(x => x.Gear).ToList() ?? new List<Rolled>();
                var wrong = gear.Where(x => x.Rarity != Shown(x.Item, asked)).ToList();
                Effect(field, setting, gear.Count > 0 && wrong.Count == 0, $"{gear.Count} gear rolls: {Show(gear)}" + (wrong.Count > 0 ? $"; {wrong.Count} not at the asked rarity's shown rarity: {Show(wrong)}" : ""),
                    inconclusive: gear.Count == 0);
            }
            await Rarity("loot.fixedRarity", "legendary for every entry", new JsonArray(Edit("loot", all, "fixedRarity", JsonValue.Create("legendary"))), 4);
            await Rarity("loot.fixedRarity", "common for every entry", new JsonArray(Edit("loot", all, "fixedRarity", JsonValue.Create("common"))), 0);
            JsonArray Mask(string r) => new(Edit("loot", all, "fixedRarity", JsonValue.Create("none")), Edit("loot", all, "rarities", new JsonArray(JsonValue.Create(r))));
            await Rarity("loot.rarities", "rare only, no fixed rarity", Mask("rare"), 2);
            await Rarity("loot.rarities", "uncommon only, no fixed rarity", Mask("uncommon"), 1);
        }
        finally { Finish(alwaysHandle, true); }
    }
}
