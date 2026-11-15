// SPDX-License-Identifier: MIT
// 17c-roster-cap: the guided check of a roster past 64 entries. Kiln's second unlock page (npcs.expanded_roster,
// npcs.client_roster_read) ships on in 0.2.0-dev.29 with no run in a real game client: 17-roster-client is the record of
// that. It needs the fixture mod roster-cap-fixture (mods/tests/roster-cap-fixture: 93 new roster NPCs, 128 with the
// game's, so the page is installed at the start) in the hosted world. It checks what the code can see in both worlds of
// the hosting game (the page installed; each world's roster budget; entry 64 set against entry 0 and each world's own
// snapshot following it, entry 0 unmoved) and asks the player what only a player sees: entries 0 and 64 apart in the
// Survivors directory, entries 63 and 65 summoned and in beds, all of it again after a rejoin, and a second world in
// which none of it carried over. Each world must have bound its own registry and had a page attached by the game's own
// evaluation (kiln.log's context line per world). 17-roster.refused-start: a start whose roster this game cannot
// hold (kiln/control/roster.fail-install) stopped before any world, with Kiln's message.
using System.Text.Json;
using Kiln;
using Kiln.Testing;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    private const int RosterOrder = 2000 + (28340 - 28000) * 10;
    private const string RosterFixture = "roster-cap-fixture";

    [ClientCheck("17-roster-client", "A roster of 128 NPCs: entries 0 and 64 apart in the Survivors directory, two summoned into beds, a rejoin and a second world",
        Area = "Limits", Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = RosterOrder + 0)]
    public async Task RosterClient(CheckContext c)
    {
        if (!await Need(c, RosterFixture) || !await NeedPlayer(c)) return;
        var row = Capabilities.Get("npcs.expanded_roster");
        c.Expect(row.State == CapabilityState.Available && row.ReasonText.Contains("installed", StringComparison.Ordinal),
            "this game installed the second unlock page", $"{row.State} {row.Reason} {row.ReasonText}");
        RosterWorldResult server = NativeQuests.GetServerRosterWorld(), client = NativeQuests.GetClientRosterWorld();
        await c.Until(() => (server = NativeQuests.GetServerRosterWorld()).Ok && (client = NativeQuests.GetClientRosterWorld()).Ok,
            TimeSpan.FromSeconds(30), "both worlds of this game have a roster snapshot");
        if (server.World is not { } sw || client.World is not { } cw)
        {
            await c.Verdict();
            return;
        }
        foreach (var (what, token) in new[] { ("hosted server world", sw), ("client world", cw) })
        {
            var b = NativeQuests.GetRosterBudget(token);
            c.Log.Info($"client-pass: 17-roster-client {what} budget {b.Budget}");
            c.Expect(b.Ok && b.Budget.Capacity == 128 && b.Budget.Expanded && b.Budget.GameUsed + b.Budget.KilnUsed + b.Budget.Reserved == 128,
                $"the {what}'s roster holds 128 entries", b.Ok ? $"capacity {b.Budget.Capacity}, game {b.Budget.GameUsed}, Kiln {b.Budget.KilnUsed}" : b.Reason);
        }
        var fixture = NativeQuests.Mod(RosterFixture);
        var npcs = Knowledge.Npcs;
        // the fixture's NPC at roster index `index` (the catalog's order is the registry's)
        NativeNpcInfo? Entry(int index) => index < npcs.Count ? fixture?.Npcs.FirstOrDefault(n => n.NpcId == npcs[index].NpcId) : null;
        if (!c.Expect(fixture is { Ok: true } && npcs.Count == 128 && Entry(63) is not null && Entry(64) is not null && Entry(65) is not null,
                "the roster is the fixture's", $"{npcs.Count} entries, fixture {(fixture is null ? "not built" : fixture.Ok ? "built" : string.Join("; ", fixture.Errors))}"))
        {
            await c.Verdict();
            return;
        }
        var e0 = npcs[0];
        bool? On(CapacityWorldToken t, uint id) => t.Kind == CapacityWorldKind.Server
            ? NativeQuests.GetUnlock(id, t) is { Ok: true } s ? s.Unlocked : null
            : NativeQuests.GetClientUnlock(id, t) is { Ok: true } k ? k.Unlocked : null;
        var s0 = On(sw, e0.NpcId);
        c.Expect(s0 is not null && On(cw, e0.NpcId) == s0, "both worlds agree on entry 0", $"{e0.DebugName}: server {s0}, client {On(cw, e0.NpcId)}");
        // entry 64 against entry 0 (a shared bit would make them equal), entries 63 and 65 unlocked for the summons
        var want64 = s0 != true;
        foreach (var (e, on) in new[] { (Entry(64)!, want64), (Entry(63)!, true), (Entry(65)!, true) })
        {
            if (Knowledge.World.Has(e.Unlock) == on) continue;
            var r = on ? await Knowledge.World.Grant(e.Unlock) : await Knowledge.World.Revoke(e.Unlock);
            c.Expect(r.Ok, $"{e.Id}'s unlock {(on ? "granted" : "revoked")}", r.Reason);
        }
        await c.Until(() => On(sw, Entry(64)!.NpcId) == want64 && On(sw, Entry(63)!.NpcId) == true && On(sw, Entry(65)!.NpcId) == true,
            TimeSpan.FromSeconds(20), "the hosted server world's snapshot follows");
        await c.Until(() => On(cw, Entry(64)!.NpcId) == want64 && On(cw, Entry(63)!.NpcId) == true && On(cw, Entry(65)!.NpcId) == true,
            TimeSpan.FromSeconds(30), "the client world's own snapshot follows (its own registry and page)");
        c.Expect(On(sw, e0.NpcId) == s0 && On(cw, e0.NpcId) == s0, "entry 0 did not move in either world", $"server {On(sw, e0.NpcId)}, client {On(cw, e0.NpcId)}");
        // each world bound its own registry and the game's own evaluation of it attached a page (once per process and world)
        var log = ReadKilnLog();
        foreach (var w in new[] { "server", "client" })
            c.Expect(log.Contains($"roster: the {w} world's evaluator context is", StringComparison.Ordinal),
                $"the {w} world bound its registry and got its page from the game's own evaluation (kiln.log)",
                log.Contains($"roster: the {w} world's evaluator context is NOT", StringComparison.Ordinal) ? "the context is not its G38Knowledge singleton" : "no such line");
        foreach (var e in new[] { Entry(63)!, Entry(65)! })
        {
            var tool = $"Kiln_RosterFix_{e.Id["rc_npc_".Length..]}_Tool";
            var g = await Items.Give(c.Player, tool);
            c.Expect(g.Succeeded, $"the tool of {e.Id} given to you", g.ToString());
        }
        var zero = string.IsNullOrEmpty(e0.DebugName) ? "the directory's first NPC" : e0.DebugName;
        c.Instruct($"Open the Survivors directory (the list of NPCs). '{zero}' is {(s0 == true ? "unlocked" : "locked")} and " +
                   $"'Roster entry 64' is {(want64 ? "unlocked" : "locked")}: they differ. 'Roster entry 63' and 'Roster entry 65' are " +
                   "unlocked. Two tools of theirs are in your inventory: use each to call its NPC here, then use it on a bed to " +
                   "send the NPC there. Leave the world and host it again: the directory shows the same, both NPCs are at their " +
                   "beds. Then host a second world with the same mods: there entries 63, 64 and 65 are locked (nothing of this " +
                   "world's roster came along). Pass when each step looked so.");
        await c.Verdict();
    }

    private static string ReadKilnLog()
    {
        try
        {
            using var f = new FileStream(Path.Combine(KilnFolder(), "logs", "kiln.log"), FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var r = new StreamReader(f);
            return r.ReadToEnd();
        }
        catch (IOException) { return ""; }
    }

    [ClientCheck("17-roster.refused-start", "A roster this game cannot hold stops its start before any world, with Kiln's message",
        Area = "Limits", Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = RosterOrder + 1)]
    public async Task RosterRefusedClient(CheckContext c)
    {
        var state = Path.Combine(KilnFolder(), "state");
        var last = Path.Combine(state, "roster-refused.last.json");
        if (!File.Exists(last))
        {
            c.Instruct("Do this first: quit the game; in the game's folder create an empty file kiln/control/roster.fail-install; " +
                       "start the game with the fixture mod roster-cap-fixture enabled. The game must stop at its start, before the " +
                       "main menu's worlds, with a Kiln message 'the NPC roster is too long' naming roster-cap-fixture. Then delete " +
                       "kiln/control/roster.fail-install, start again, host a world and run this check once more. Skip for now.");
            await c.Verdict();
            return;
        }
        using var doc = JsonDocument.Parse(File.ReadAllText(last));
        var root = doc.RootElement;
        var mods = root.TryGetProperty("mods", out var m) ? m.EnumerateArray().Select(x => x.GetString()).ToList() : new List<string?>();
        c.Expect(root.GetProperty("refused").GetBoolean() && root.GetProperty("side").GetString() == "client" && mods.Contains(RosterFixture) &&
                 root.GetProperty("exitCode").GetUInt32() == 0x4B02,
            "the earlier start stopped on this game client, naming the fixture (kiln/state/roster-refused.last.json)", root.GetRawText());
        c.Expect((root.GetProperty("why").GetString() ?? "").Contains("roster.fail-install", StringComparison.Ordinal),
            "it stopped because the second page was not installed (the switch)", root.GetProperty("why").GetString() ?? "");
        c.Expect(!File.Exists(Path.Combine(KilnFolder(), "control", "roster.fail-install")), "kiln/control/roster.fail-install is deleted again");
        var prev = Path.Combine(KilnFolder(), "logs", "kiln.prev.log");
        c.Expect(File.Exists(prev) && File.ReadAllText(prev).Contains("roster: the start is stopped (pid", StringComparison.Ordinal),
            "the stopped start's kiln.log (now kiln.prev.log) has the refusal line", File.Exists(prev) ? "no such line" : "no kiln.prev.log");
        c.Instruct("Pass when the stopped start showed Kiln's message 'the NPC roster is too long' naming roster-cap-fixture, the game " +
                   "closed after it without loading a world, and this world (hosted after deleting the switch) has its NPCs as before.");
        await c.Verdict();
    }
}
