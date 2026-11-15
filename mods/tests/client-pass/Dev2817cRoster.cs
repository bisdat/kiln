// SPDX-License-Identifier: MIT
// 17c-roster-cap: the guided check of a roster past 64 entries. Kiln's second unlock page (npcs.expanded_roster,
// npcs.client_roster_read) ships on in 0.2.0-dev.29 with no run in a real game client: 17-roster-client is the record of
// that. It needs the fixture mod roster-cap-fixture (mods/tests/roster-cap-fixture: 93 new roster NPCs, 128 with the
// game's, so the page is installed at the start) in the hosted world. It checks what the code can see in both worlds of
// the hosting game (the page installed; each world's roster budget; entry 64 set against entry 0 and each world's own
// snapshot following it, entry 0 unmoved) and asks the player what only a player sees: entries 0 and 64 apart in the
// Survivors directory, entries 63 and 65 summoned and in beds, all of it again after a rejoin, and a second world in
// which none of it carried over.
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
}
