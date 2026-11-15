// SPDX-License-Identifier: MIT
// 17b-kiln-quotas: the guided checks of Kiln's own quotas. The buff room of 4,096 clones ships on in 0.2.0-dev.29
// (gamedata.expanded_buff_room) with no run in a real game client: 17-buffs-client is the record of that. It needs the
// fixture mod gamedata-caps (mods/tests/gamedata-test/Caps: 4,097 buff clones, so the room is full) in the hosted
// world; it checks what the code can see (the room, the first and last clone registered, the game's lookup) and asks
// the player what only a player sees: the first and the last clone on the buff bar with their duration, after a rejoin
// and in a second world.
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
}
