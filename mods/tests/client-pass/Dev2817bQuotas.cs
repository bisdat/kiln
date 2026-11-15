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
        var first = defs.FirstOrDefault(d => d.Name == "CapsBuff_0000");
        var last = defs.Where(d => d.State == "registered").OrderBy(d => d.Name, StringComparer.Ordinal).LastOrDefault();
        if (!c.Expect(first?.State == "registered" && last is not null, "the first and the last accepted clone are registered",
                $"{first?.Name}: {first?.State}; last: {last?.Name}"))
        {
            await c.Verdict();
            return;
        }
        foreach (var d in new[] { first!, last! })
        {
            try
            {
                await Abilities.ApplyBuff(c.Player, d.Guid, TimeSpan.FromSeconds(60));
            }
            catch (Exception e) when (e is InvalidOperationException or KilnUnavailableException)
            {
                c.Expect(false, $"{d.Name} applied to your character", e.Message);
            }
        }
        c.Instruct($"Two buffs of a mod were put on your character: {first!.Name} and {last!.Name}, both copies of the game's " +
                   "poison debuff, for 60 seconds. Look at your buff bar: two poison icons with their time running down. " +
                   "Then leave the world and host it again (or save and reload), and run the check list's next step; " +
                   "finally host a second world with the same mods and look at a buff of the mod there too. Pass when " +
                   "the icons and their times showed each time and nothing looked out of place.");
        await c.Verdict();
    }

    [ClientCheck("17-programs-vanilla", "More than 32 of Kiln's world programs on a server, a player without Kiln joined",
        Area = "Limits", Since = Dev29, Tags = [VanillaClient], Needs = ClientCheckNeeds.Joined, Order = QuotasOrder + 1)]
    public async Task ProgramsVanilla(CheckContext c)
    {
        if (!await Need(c, "world-programs-quota-a", "world-programs-quota-b") || !await NeedPlayer(c)) return;
        var budget = World.GetProgramBudget();
        c.Log.Info($"client-pass: 17-programs-vanilla budget Kiln {budget.KilnSlots}, engine {budget.EngineSlots}");
        var taken = budget.KilnSlots.KilnUsed + budget.KilnSlots.Reserved;
        c.Expect(budget.Ok && taken > 32, "the server runs more of Kiln's programs than the old room of 32",
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
