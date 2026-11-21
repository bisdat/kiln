// SPDX-License-Identifier: MIT
// 17b-kiln-quotas: the guided checks of Kiln's own quotas. The buff room of 4,096 clones ships on in 0.2.0-dev.29
// (gamedata.expanded_buff_room) with no run in a real game client: 17-buffs-client is the record of that. It needs the
// fixture mod gamedata-caps (mods/tests/gamedata-test/Caps: 4,097 buff clones, so the room is full) in the hosted
// world; it checks what the code can see (the room, the first and last clone registered, the game's lookup) and asks
// the player what only a player sees: the first and the last clone on the buff bar with their duration, after a rejoin
// and in a second world. 17-programs-vanilla: kiln.world's 64 program slots (32 before) with a game client without Kiln
// joined: more server-only programs change no protocol; the fixture mods world-programs-quota-a and -b (64 declarations)
// fill the slots, the check moves the player with a teleport (a replicated change) and asks whether the game stayed well.
// It confirms nothing is held (the quota ships on); it needs a second Steam account or a second person.
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using Kiln;
using Kiln.Testing;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    private const int QuotasOrder = 2000 + (28330 - 28000) * 10;
    private const string CapsFixture = "gamedata-caps";

    [ClientCheck("17-buffs-client", "Kiln's buff room at 4,096 clones: the first and last clone on the buff bar, rejoin and a second world",
        Area = "Limits", Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = QuotasOrder + 0)]
    public async Task BuffsClient(CheckContext c)
    {
        if (!await Need(c, CapsFixture) || !await NeedPlayer(c)) return;
        var budget = Content.Buffs.GetBudget();
        c.Log.Info($"client-pass: 17-buffs-client budget {budget.Budget}");
        c.Expect(budget.Ok && budget.Budget.Capacity == 4096 && budget.Budget.Expanded, "the buff room is 4,096 clones",
            budget.Ok ? $"capacity {budget.Budget.Capacity}, expanded {budget.Budget.Expanded}" : budget.Reason);
        c.Expect(budget.Ok && budget.Budget.KilnUsed + budget.Budget.Reserved == 4096, "the fixture's clones fill it",
            budget.Ok ? $"Kiln {budget.Budget.KilnUsed}, reserved {budget.Budget.Reserved}" : budget.Reason);
        var defs = Content.Buffs.Definitions().Where(d => d.Name.StartsWith("CapsBuff_", StringComparison.Ordinal)).ToList();
        var firstGuid = Guid.Parse("615e6c08-7d7e-5de3-a6b3-7fa8f90bd2c1");
        var bytes = SHA1.HashData(Encoding.UTF8.GetBytes("kiln-17b-gamedata-caps-4095")).AsSpan(0, 16).ToArray();
        bytes[7] = (byte)((bytes[7] & 15) | 0x50); bytes[8] = (byte)((bytes[8] & 63) | 0x80);
        var lastGuid = new Guid(bytes);
        var first = defs.FirstOrDefault(d => d.Guid == firstGuid && d.Name == "CapsBuff_0000");
        var last = defs.FirstOrDefault(d => d.Guid == lastGuid && d.Name == "CapsBuff_4095");
        if (!c.Expect(first?.State == "registered" && last?.State == "registered", "the first and the last accepted clone are registered",
                $"{first?.Name}: {first?.State}; last: {last?.Name}"))
        {
            await c.Verdict();
            return;
        }
        void CheckLookups(string phase)
        {
            var lookups = JsonNode.Parse(Content.GameDataStatus())?["clones"]?["buffLookups"];
            foreach (var guid in new[] { firstGuid, lastGuid })
                c.Expect((bool?)lookups?[guid.ToString()]?["server"] == true && (bool?)lookups?[guid.ToString()]?["client"] == true,
                    $"{phase}: the ordinary lookup found {guid} in both hosted registries");
        }
        CheckLookups("before rejoin");
        // the player of the world now (a new world has a new player entity), with both boundary buffs for 60 seconds
        EntityId Me() => Players.All.FirstOrDefault(p => !p.Entity.IsNone)?.Entity ?? c.Player;
        async Task ShowBuffs(string phase)
        {
            foreach (var d in new[] { first!, last! })
            {
                try
                {
                    await Abilities.ApplyBuff(Me(), d.Guid, TimeSpan.FromSeconds(60));
                }
                catch (Exception e) when (e is InvalidOperationException or KilnUnavailableException)
                {
                    c.Expect(false, $"{phase}: {d.Name} applied to your character", e.Message);
                }
            }
        }
        // a new world: its generation differs from the one before, a player stands in it, the clones are registered
        async Task<bool> NewWorld(CapacityWorldToken? was, string what)
        {
            var ok = await c.Until(() => World.GetProgramBudget().World is { } now && now != was &&
                                         Players.All.Any(p => !p.Entity.IsNone) &&
                                         Content.Buffs.Definitions().Count(d => (d.Guid == firstGuid || d.Guid == lastGuid) && d.State == "registered") == 2,
                TimeSpan.FromMinutes(10), what);
            if (ok) await c.Delay(TimeSpan.FromSeconds(5));   // the character settles in the loaded world
            return ok;
        }

        // 1) this world
        await ShowBuffs("before rejoin");
        var generation = World.GetProgramBudget().World;
        c.Instruct($"Step 1 of 3. Two buffs of a mod were put on your character: {first!.Name} and {last!.Name}, both copies of " +
                   "the game's poison debuff, for 60 seconds. Look at your buff bar: two poison icons with their time running " +
                   "down; remember whether they showed. Then leave the world and host it again (or save and reload): the check " +
                   "goes on by itself when the world is loaded again.");

        // 2) the same world, rejoined
        var rejoined = await NewWorld(generation, "you left the world and hosted it again");
        c.Expect(World.GetProgramBudget().World is { } after && after != generation, "the world was actually rejoined");
        c.Expect(Content.Buffs.Definitions().Any(d => d.Guid == firstGuid && d.State == "registered") &&
                 Content.Buffs.Definitions().Any(d => d.Guid == lastGuid && d.State == "registered"), "both exact boundary GUIDs registered after rejoin");
        CheckLookups("after rejoin");
        if (rejoined) await ShowBuffs("after rejoin");
        var second = World.GetProgramBudget().World;
        c.Instruct($"Step 2 of 3. The world was loaded again and the same two buffs ({first!.Name} and {last!.Name}) were put on " +
                   "your character for 60 seconds: look at the buff bar again and remember whether both icons and their times " +
                   "showed. Then host a SECOND world with the same mods: the check goes on by itself when it is loaded.");

        // 3) a second world with the same mods
        if (await NewWorld(second, "you hosted a second world with the same mods"))
        {
            CheckLookups("second world");
            await ShowBuffs("second world");
        }
        c.Instruct($"Step 3 of 3. In this second world the same two buffs ({first!.Name} and {last!.Name}) were put on your character " +
                   "for 60 seconds; look at the buff bar. Pass only if all of these held: (1) both poison icons with their time " +
                   "running down showed in the first world, (2) again after you hosted it again, (3) again in the second world, " +
                   "and nothing looked out of place. Otherwise fail with a note naming which one.");
        await c.Verdict();
    }

    [ClientCheck("17-programs-vanilla", "More than 32 of Kiln's world programs on a server, a player without Kiln joined",
        Area = "Limits", Since = Dev29, Tags = [VanillaClient], Needs = ClientCheckNeeds.Joined, Order = QuotasOrder + 1)]
    public async Task ProgramsVanilla(CheckContext c)
    {
        if (!await Need(c, "world-programs-quota-a", "world-programs-quota-b") || !await NeedPlayer(c)) return;
        var budget = World.GetProgramBudget();
        c.Log.Info($"client-pass: 17-programs-vanilla budget Kiln {budget.KilnSlots}, engine {budget.EngineSlots}");
        int taken = 0;
        foreach (var mod in new[] { "world-programs-quota-a", "world-programs-quota-b" })
        {
            var file = Path.Combine(Context.DataDirectory, "..", mod, "quota-proof.json");
            JsonNode? proof = File.Exists(file) ? JsonNode.Parse(File.ReadAllText(file)) : null;
            var current = (uint?)proof?["generation"] == budget.World?.Generation;
            var ran = (int?)proof?["ran"] ?? 0;
            c.Expect(current && ran > 0 && (long?)proof?["minimumRuns"] > 0,
                $"{mod}: callbacks executed in this world ({proof?.ToJsonString() ?? "no proof"})");
            if (current) taken += ran;
        }
        c.Expect(budget.Ok && taken > 32, "more than 32 quota callbacks executed in this world",
            budget.Ok ? $"{taken} of {budget.KilnSlots.Capacity} slots, the world's table {budget.EngineSlots.GameUsed + budget.EngineSlots.KilnUsed + budget.EngineSlots.Reserved} of {budget.EngineSlots.Capacity}" : budget.Reason);
        if (Ahead(c, 4) is { } there)
        {
            var moved = await Teleport.To(c.Player, there);
            c.Expect(moved.Ok, "the player was moved 4 m forward (a replicated change)", moved.ToString());
        }
        c.Instruct($"This check is for a player whose game runs without Kiln. The server runs {taken} of Kiln's world programs " +
                   "(the most it held before was 32). You were just moved about 4 metres forward. Walk around for a minute, open " +
                   "your inventory, pick something up. Pass when you were moved, nothing disconnected you, and the game behaved " +
                   "as on a server without mods.");
        await c.Verdict();
    }
}
