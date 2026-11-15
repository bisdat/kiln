// SPDX-License-Identifier: MIT
// The roster's second unlock page (package 17c; docs/kiln/services/npcs.md "The roster's size"; docs/re/npc-roster.md).
// Player-free lab test, driven by lab.sh: each boot gets a quests/roster.json from gen_roster.py and lab.json says the
// phase. The lab save is used up across the boots (the knowledge granted in one boot is in the next):
//   64      29 new entries (64 in all): no hook, capacity 64; a grant on entry 63 shows in the snapshot one step later
//   65      30 new (65): the page is installed at the start, capacity 128; entry 64 follows its own knowledge
//   129     94 new: refused at the start with the 128 text; the game's 35 are what the world has
//   65-off  30 new with the lab switch kiln/data/kiln.knowledge/roster-64: refused with the 64 text and the reason
//   128     93 new (128): entries 0/64, 1/65, 63/127 set to contradict each other; every entry of page 2 against its
//           knowledge; the one-per-world rule (locked destroyed, unlocked kept) on page 2; the events; the same once more
//           with kiln.world's parallel switch on (kiln/control/systems.parallel; whether the roster programs then ran on
//           a worker is in kiln.log, not here); what it set is written for the next boot
//   128-restart  the same roster after a restart: what the save kept
using System.Text.Json;
using Kiln;
using Kiln.Testing;

namespace RosterCapTest;

public sealed class RosterCapTestMod : Mod
{
    private const string Tag = "roster-cap-test";
    private int _pass, _fail;
    private readonly List<RosterUnlockChangedEvent> _events = [];

    private void Pass(string what) { _pass++; Log.Info($"{Tag}: PASS {what}"); }
    private void Fail(string what, string why) { _fail++; Log.Error($"{Tag}: FAIL {what}: {why}"); }
    private bool Check(bool ok, string what, string why = "")
    {
        if (ok) Pass(what);
        else Fail(what, why);
        return ok;
    }
    private void Note(string s) => Log.Info($"{Tag}: {s}");

    public override void OnLoad()
    {
        Directory.CreateDirectory(Context.DataDirectory);
        Events.On<RosterUnlockChangedEvent>(e => { lock (_events) _events.Add(e); });
        Events.On<ServerStartedEvent>(ev => { _ = Run(); });
    }

    private async Task Steps(int n)
    {
        for (var i = 0; i < n; i++) await Context.Scheduler.NextTick();
    }

    private async Task<bool> WaitFor(Func<bool> cond, int steps)
    {
        for (var i = 0; i < steps; i++)
        {
            if (cond()) return true;
            await Context.Scheduler.NextTick();
        }
        return cond();
    }

    private string Phase()
    {
        var dir = Mods.All.FirstOrDefault(x => x.Id == Tag)?.Directory ?? "";
        var p = Path.Combine(dir, "lab.json");
        if (!File.Exists(p)) return "";
        using var d = JsonDocument.Parse(File.ReadAllText(p));
        return d.RootElement.GetProperty("phase").GetString() ?? "";
    }

    private async Task Run()
    {
        var phase = "";
        try
        {
            phase = Phase();
            Note($"phase {phase}");
            for (var i = 0; i < 2000 && Knowledge.Status?.Applying != true; i++) await Context.Scheduler.NextTick();
            await Steps(5);
            switch (phase)
            {
                case "64": await Fits(); break;
                case "65": await Paged(30); break;
                case "129": await Refused(94, "the roster holds at most 128 NPCs (two 64-bit unlock pages): 35 of the game's + 94 new"); break;
                case "65-off":
                    await Refused(30, "the roster holds at most 64 NPCs (a 64-bit unlock mask): 35 of the game's + 30 new " +
                                      "(the second unlock page is not available here: lab switch kiln/data/kiln.knowledge/roster-64)");
                    break;
                case "128": await Full(); break;
                case "128-restart": await Restarted(); break;
                default: Fail("the phase", $"lab.json says '{phase}'"); break;
            }
        }
        catch (Exception ex)
        {
            Fail("exception", ex.ToString());
        }
        Note($"done: phase {phase}: {_pass} PASS, {_fail} FAIL");
    }

    // ---- reads

    private async Task<CapacityWorldToken?> Token()
    {
        CapacityWorldToken? t = null;
        await WaitFor(() => (t = NativeQuests.GetServerRosterWorld().World) is not null, 200);
        return t;
    }

    private static bool? Read(CapacityWorldToken t, uint id) => NativeQuests.GetUnlock(id, t) is { Ok: true } r ? r.Unlocked : null;

    private void Budget(CapacityWorldToken t, uint capacity, uint kiln, bool expanded)
    {
        var b = NativeQuests.GetRosterBudget(t);
        Note($"RosterBudget Capacity={b.Budget.Capacity} GameUsed={b.Budget.GameUsed} KilnUsed={b.Budget.KilnUsed} Reserved={b.Budget.Reserved} " +
             $"Expanded={b.Budget.Expanded} Readiness={b.Budget.Readiness} World={b.Budget.World}");
        Check(b.Ok && b.Budget.Capacity == capacity && b.Budget.GameUsed == 35 && b.Budget.KilnUsed == kiln && b.Budget.Reserved == 0 &&
              b.Budget.Expanded == expanded && b.Budget.World == t,
            $"budget: capacity {capacity}, the game's 35, Kiln's {kiln}, expanded {expanded}", $"{b}");
    }

    private NativeModInfo? Mine() => NativeQuests.Mod(Tag);

    // The mod's NPC at roster index `index` (the catalog's order is the registry's: the game's 35, then the build's)
    private NativeNpcInfo? At(IReadOnlyList<KnowledgeNpc> catalog, int index) =>
        index < catalog.Count ? Mine()?.Npcs.FirstOrDefault(n => n.NpcId == catalog[index].NpcId) : null;

    private static string Row(string name) => Capabilities.Get(name) is var c ? $"{c.State} {c.Reason} {c.ReasonText}" : "";

    // ---- phases

    private async Task Fits()
    {
        var m = Mine();
        Check(m is { Ok: true } && m.Npcs.Count(n => n.Roster) == 29, $"the build took 29 entries: {m?.Npcs.Count}", string.Join("; ", m?.Errors ?? []));
        Check(Knowledge.Npcs.Count == 64, $"the roster has 64 entries: {Knowledge.Npcs.Count}");
        var row = Capabilities.Get("npcs.expanded_roster");
        Check(row.State == CapabilityState.Available && row.ReasonText.Contains("ready", StringComparison.Ordinal),
            $"npcs.expanded_roster says ready, nothing installed: {Row("npcs.expanded_roster")}");
        if (await Token() is not { } t) { Fail("a server world token", "none"); return; }
        Budget(t, 64, 29, false);
        // entry 63 (the last bit of the game's value): its own knowledge, one step later in the snapshot
        if (At(Knowledge.Npcs, 63) is not { Id: "rc_npc_028" } e63)
        {
            Fail("entry 63 is the mod's 29th", $"{At(Knowledge.Npcs, 63)?.Id}");
            return;
        }
        var before = Read(t, e63.NpcId);
        var v0 = NativeQuests.GetUnlock(e63.NpcId, t).KnowledgeVersion;
        var g = before == true ? await Knowledge.World.Revoke(e63.Unlock) : await Knowledge.World.Grant(e63.Unlock);
        Check(g.Ok && g.Changed, $"entry 63's knowledge {(before == true ? "revoked" : "granted")}", g.Reason);
        Check(await WaitFor(() => Read(t, e63.NpcId) == (before != true), 60), $"entry 63 follows in the snapshot: {Read(t, e63.NpcId)}");
        var after = NativeQuests.GetUnlock(e63.NpcId, t);
        Check(after.KnowledgeVersion != v0, $"with the new knowledge version: {v0} -> {after.KnowledgeVersion}");
        Check(Knowledge.FindNpc("Kiln_RosterCap_028")?.Unlocked == (before != true), $"Kiln's catalog reads it too: {Knowledge.FindNpc("Kiln_RosterCap_028")}");
        Check(await WaitFor(() => { lock (_events) return _events.Any(e => e.NpcId == e63.NpcId && e.After == (before != true)); }, 60),
            "RosterUnlockChangedEvent for it");
    }

    private async Task Paged(int n)
    {
        var m = Mine();
        Check(m is { Ok: true } && m.Npcs.Count(x => x.Roster) == n, $"the build took {n} entries: {m?.Npcs.Count}", string.Join("; ", m?.Errors ?? []));
        Check(Knowledge.Npcs.Count == 35 + n, $"the roster has {35 + n} entries: {Knowledge.Npcs.Count}");
        var row = Capabilities.Get("npcs.expanded_roster");
        Check(row.State == CapabilityState.Available && row.ReasonText.Contains("installed", StringComparison.Ordinal),
            $"npcs.expanded_roster says installed: {Row("npcs.expanded_roster")}");
        if (await Token() is not { } t) { Fail("a server world token", "none"); return; }
        Budget(t, 128, (uint)n, true);
        var e0 = Knowledge.Npcs[0];
        if (At(Knowledge.Npcs, 64) is not { Id: "rc_npc_029" } e64)
        {
            Fail("entry 64 is the mod's 30th", $"{Knowledge.Npcs[64]}");
            return;
        }
        Pass("entry 64 is the mod's 30th");
        var s0 = Read(t, e0.NpcId);
        var s64 = Read(t, e64.NpcId);
        Note($"entry 0 {e0.DebugName} {s0}, entry 64 {s64}");
        var want = s0 != true;  // the opposite of entry 0: aliasing would show entry 0's state
        if (s64 != want)
        {
            var g = want ? await Knowledge.World.Grant(e64.Unlock) : await Knowledge.World.Revoke(e64.Unlock);
            Check(g.Ok, $"entry 64's knowledge set to {want}", g.Reason);
        }
        Check(await WaitFor(() => Read(t, e64.NpcId) == want, 60), $"entry 64 reads {want} (entry 0 reads {s0})", $"{Read(t, e64.NpcId)}");
        Check(Read(t, e0.NpcId) == s0, "entry 0 did not move");
        Check(await WaitFor(() => Knowledge.FindNpc("Kiln_RosterCap_029")?.Unlocked == want, 30), $"Kiln's catalog reads entry 64 from the page: {Knowledge.FindNpc("Kiln_RosterCap_029")}");
    }

    private async Task Refused(int n, string text)
    {
        var manifest = NativeQuests.ManifestJson;
        Check(manifest.Contains(text, StringComparison.Ordinal), $"the build refused {35 + n} entries with: {text}", manifest.Length > 600 ? manifest[..600] : manifest);
        Check(Knowledge.Npcs.Count == 35, $"the world has the game's 35 entries: {Knowledge.Npcs.Count}");
        if (await Token() is { } t) Budget(t, 64, 0, false);
        else Fail("a server world token", "none");
        var row = Capabilities.Get("npcs.expanded_roster");
        Note($"npcs.expanded_roster: {Row("npcs.expanded_roster")}");
        // what a world whose mods' build was refused keeps of their knowledge (an older Kiln does the same with this roster)
        var rc28 = NativeIds.Knowledge(Tag, "rc_28");
        Note($"observed: the knowledge rc_28 (granted in an earlier boot) reads {Knowledge.World.Get(rc28)?.ToString() ?? "unknown to this world"}");
        if (n == 94) Check(row.State == CapabilityState.Available, "npcs.expanded_roster stays available (nothing was installed for a roster it could not hold)");
        else Check(row.State == CapabilityState.Unavailable && row.Reason == CapabilityReason.Policy, "npcs.expanded_roster: unavailable, policy (the lab switch)");
    }

    private readonly Dictionary<uint, bool> _set = [];

    private async Task Set(NativeNpcInfo e, bool on, CapacityWorldToken t)
    {
        if (Read(t, e.NpcId) == on && Knowledge.World.Has(e.Unlock) == on) { _set[e.NpcId] = on; return; }
        var r = on ? await Knowledge.World.Grant(e.Unlock) : await Knowledge.World.Revoke(e.Unlock);
        if (!r.Ok) Fail($"set {e.Id} to {on}", r.Reason);
        _set[e.NpcId] = on;
    }

    private async Task Full()
    {
        var m = Mine();
        Check(m is { Ok: true } && m.Npcs.Count(x => x.Roster) == 93, $"the build took 93 entries: {m?.Npcs.Count}", string.Join("; ", m?.Errors ?? []));
        Check(Knowledge.Npcs.Count == 128, $"the roster has 128 entries: {Knowledge.Npcs.Count}");
        if (await Token() is not { } t) { Fail("a server world token", "none"); return; }
        Budget(t, 128, 93, true);
        var b = NativeQuests.GetRosterBudget(t);
        Check(b.Budget.Available == 0, $"no room left: {b.Budget.Available}");
        var order = Knowledge.Npcs;
        NativeNpcInfo E(int index) => At(order, index) ?? throw new InvalidOperationException($"roster index {index} is not the mod's");
        Check(Enumerable.Range(35, 93).All(i => At(order, i)?.Id == $"rc_npc_{i - 35:000}"), "the mod's entries are indices 35 to 127, in key order");
        Note($"observed after the refused boots: entry 63's knowledge (granted in the 64 boot) {Knowledge.World.Has(E(63).Unlock)}, entry 64's (65 boot) {Knowledge.World.Has(E(64).Unlock)}");
        // contradictions across the page boundary: 0/64 and 1/65 against the game's own entries, 63/127 both the mods'
        var s0 = Read(t, Knowledge.Npcs[0].NpcId);
        var s1 = Read(t, Knowledge.Npcs[1].NpcId);
        await Set(E(64), s0 != true, t);
        await Set(E(65), s1 != true, t);
        await Set(E(63), true, t);
        await Set(E(127), false, t);
        foreach (var i in Enumerable.Range(66, 61)) await Set(E(i), i % 3 == 0, t);  // a pattern over the rest of page 2
        await Set(E(100), false, t);
        await Set(E(101), false, t);
        Check(await WaitFor(() => _set.All(kv => Read(t, kv.Key) == kv.Value), 120), "every entry set reads what its knowledge says",
            string.Join(", ", _set.Where(kv => Read(t, kv.Key) != kv.Value).Select(kv => $"0x{kv.Key:x8}={Read(t, kv.Key)}")));
        Check(Read(t, E(64).NpcId) != s0 && Read(t, Knowledge.Npcs[0].NpcId) == s0, $"0/64 differ: {s0} / {Read(t, E(64).NpcId)}");
        Check(Read(t, E(65).NpcId) != s1 && Read(t, Knowledge.Npcs[1].NpcId) == s1, $"1/65 differ: {s1} / {Read(t, E(65).NpcId)}");
        Check(Read(t, E(63).NpcId) == true && Read(t, E(127).NpcId) == false, $"63/127 differ: {Read(t, E(63).NpcId)} / {Read(t, E(127).NpcId)}");
        // every one of page 2 against its knowledge, and Kiln's own catalog against the snapshot
        var bad = new List<string>();
        var catalog = Knowledge.Npcs;
        for (var i = 64; i < 128; i++)
        {
            var e = E(i);
            var know = Knowledge.World.Has(e.Unlock);
            if (Read(t, e.NpcId) != know) bad.Add($"{i}: snapshot {Read(t, e.NpcId)} knowledge {know}");
            if (catalog[i].NpcId != e.NpcId || catalog[i].Unlocked != know) bad.Add($"{i}: catalog {catalog[i]}");
        }
        Check(bad.Count == 0, "page 2: all 64 entries match their knowledge, in the snapshot and in Kiln's catalog", string.Join("; ", bad));
        Check(NativeQuests.GetUnlock(0x7fff1234, t) is { Ok: false, Reason: "UnknownNpc", Unlocked: null }, "an id the roster does not have: UnknownNpc");
        await Steps(10);
        lock (_events)
            Check(_events.Any(e => e.NpcId == E(64).NpcId || e.NpcId == E(66).NpcId), $"RosterUnlockChangedEvent for page-2 entries: {_events.Count}");

        // the game's one-per-world rule reads page 2 through the hook: a locked entry is destroyed, an unlocked one kept
        var unlocked = E(66);  // set unlocked above (66 % 3 == 0)
        var locked = E(127);
        await Uniqueness(unlocked, locked, "serial", 0);

        // once more on the game's workers
        if (LabSystems.Enabled && LabSystems.ForceParallel(1500))
        {
            await Steps(3);
            Note($"forced parallel: players mask {LabSystems.PlayersMask:x}, forced {LabSystems.Forced}");
            await Set(E(100), true, t);
            Check(await WaitFor(() => Read(t, E(100).NpcId) == true, 120), "parallel switch on: entry 100 follows its knowledge");
            await Uniqueness(E(100), E(101), "parallel switch on", 20);
            // whether the roster programs really ran on a worker is only in kiln.log: kiln.knowledge's "roster: ... on thread
            // N, not the server world's tick thread" line, and the "off the tick thread" count of its world-stop line
            LabSystems.StopParallel();
        }
        else Note("the parallel switch is off (kiln/control/systems.parallel): the worker run is skipped");

        // what the next boot must find
        File.WriteAllText(Path.Combine(Context.DataDirectory, "expect.json"), JsonSerializer.Serialize(new
        {
            set = _set.ToDictionary(kv => $"0x{kv.Key:x8}", kv => kv.Value),
            kept = new[] { unlocked.Guid.ToString(), E(100).Guid.ToString() },
        }));
        Note("expect.json written for the restart");
    }

    private async Task Uniqueness(NativeNpcInfo unlocked, NativeNpcInfo locked, string how, int dx)
    {
        var at = new WorldPosition(3890 + dx, 834, 1490);
        foreach (var old in Spawn.Find(unlocked.Guid)) Spawn.Destroy(old);
        await Steps(10);
        var l = await Spawn.Entity(locked.Guid, at, 0, new SpawnOptions { Flags = SpawnFlags.NoTrack });
        await Steps(40);
        Check(!Spawn.Exists(l), $"{how}: the game destroyed the locked page-2 entry's NPC {locked.Id} ({l})");
        var u = await Spawn.Entity(unlocked.Guid, new WorldPosition(at.X + 3, at.Y, at.Z), 0, new SpawnOptions { Flags = SpawnFlags.NoTrack });
        await Steps(40);
        Check(Spawn.Exists(u), $"{how}: the unlocked page-2 entry's NPC {unlocked.Id} lives ({u})");
        var dup = await Spawn.Entity(unlocked.Guid, new WorldPosition(at.X + 6, at.Y, at.Z), 0, new SpawnOptions { Flags = SpawnFlags.NoTrack });
        await Steps(40);
        Check(!Spawn.Exists(dup) && Spawn.Exists(u), $"{how}: one per world: its second ({dup}) was destroyed");
    }

    private async Task Restarted()
    {
        var p = Path.Combine(Context.DataDirectory, "expect.json");
        if (!File.Exists(p)) { Fail("expect.json", "the 128 boot did not write it"); return; }
        using var d = JsonDocument.Parse(File.ReadAllText(p));
        if (await Token() is not { } t) { Fail("a server world token", "none"); return; }
        var bad = new List<string>();
        var n = 0;
        foreach (var kv in d.RootElement.GetProperty("set").EnumerateObject())
        {
            var id = Convert.ToUInt32(kv.Name, 16);
            n++;
            if (Read(t, id) != kv.Value.GetBoolean()) bad.Add($"{kv.Name}: {Read(t, id)} (was {kv.Value.GetBoolean()})");
        }
        Check(bad.Count == 0 && n > 0, $"after the restart: all {n} entries set before it read the same (pages computed again from the saved knowledge)",
            string.Join("; ", bad));
        foreach (var g in d.RootElement.GetProperty("kept").EnumerateArray())
        {
            var guid = Guid.Parse(g.GetString()!);
            Check(await WaitFor(() => Spawn.Find(guid).Count == 1, 300), $"the kept page-2 NPC {guid} came back from the save: {Spawn.Find(guid).Count}");
        }
        Budget(t, 128, 93, true);
    }
}
