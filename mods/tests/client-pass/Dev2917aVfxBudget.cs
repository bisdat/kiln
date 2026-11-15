// SPDX-License-Identifier: MIT
// 17a-vfx-cap S2: the guided check of the VFX resource budget (Vfx.ResourceBudget) and of VFX defined with an answer
// (Blocks.Define(name, VfxBlock, options), Blocks.GetVfxRegistration). Both ship on in 0.2.0-dev.29 with no run in a real game
// client: the host tests run kiln.vfx's ledger and a simulated game, nothing has read the real game's VFX table. A new VFX needs
// "clients": "mod" and this mod says "kiln", so the definitions are its fixture's: vfx-test (mods/tests/vfx-test/Budget.cs) defines
// three VFX in its OnLoad with AllowNextStart and writes the answers to its data folder. The check runs over two starts: at the
// first the three are accepted for the next start (the game lists its VFX once, before mods load); at the next they are registered,
// the budget counts them, and the check plays them in front of the player, who confirms they show.
using System.Text.Json.Nodes;
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    private const int VfxCapOrder = 2000 + (28320 - 28000) * 10;
    private const string VfxTest = "vfx-test";

    [ClientCheck("17-vfx-budget", "The VFX budget, and VFX defined with an answer: registered at the next start and played",
        Area = "Limits", Since = Dev29, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = VfxCapOrder + 2)]
    public async Task VfxBudget(CheckContext c)
    {
        if (!await Need(c, VfxTest) || !await NeedPlayer(c)) return;
        CapacityQueryResult before;
        try
        {
            before = Vfx.ResourceBudget;
        }
        catch (KilnUnavailableException e)
        {
            c.Expect(false, "vfx.budget is available in a game client", e.Message);
            await c.Verdict();
            return;
        }
        var b = before.Budget;
        c.Log.Info($"client-pass: 17-vfx-budget budget {b}");
        c.Expect(true, "the budget as read", before.Ok
            ? $"capacity {b.Capacity}, the game's {b.GameUsed}, Kiln's {b.KilnUsed}, reserved {b.Reserved}, left {b.Available}"
            : before.Reason);
        c.Expect(before.Ok, "the budget is ready in a world (kiln.vfx read the game's VFX table)", before.Reason);
        c.Expect(before.Ok && b.GameUsed > 0 && b.Capacity > b.GameUsed, "the game's own VFX and the table's capacity are counted",
            $"capacity {b.Capacity}, the game's {b.GameUsed}");

        // the fixture's answers at its OnLoad of this start
        var file = Path.Combine(c.Mod.DataDirectory, "..", VfxTest, "budget.json");
        JsonObject? doc = null;
        try
        {
            doc = JsonNode.Parse(File.ReadAllText(file)) as JsonObject;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        {
        }
        var effects = (doc?["effects"] as JsonArray)?.OfType<JsonObject>().ToList() ?? [];
        if (!c.Expect(effects.Count == 3, "vfx-test defined its three VFX with an answer at this start",
                (string?)doc?["off"] ?? (doc == null ? $"no {file}" : $"{effects.Count} answers")))
        {
            await c.Verdict();
            return;
        }
        var never = doc!["requireRegistered"] as JsonObject;
        c.Expect((string?)never?["code"] is "RegistryClosed" or "CapacityExceeded" && never?["ok"]?.GetValue<bool>() == false,
            "a new VFX under RequireRegistered is refused and not stored", $"{never?["state"]} ({never?["code"]}): {never?["reason"]}");

        // what each of the three is now
        var receipts = new List<(string Name, DefinitionRegistrationResult R)>();
        foreach (var e in effects)
            receipts.Add(((string)e["name"]!, Blocks.GetVfxRegistration(Guid.Parse((string)e["registrationId"]!))));
        var said = string.Join("; ", receipts.Select(x => $"{x.Name}: {x.R.State} ({x.R.Code}), {x.R.SlotsConsumed} slot(s)"));
        c.Log.Info($"client-pass: 17-vfx-budget receipts {said}");
        if (receipts.All(x => x.R.State == DefinitionRegistrationState.AcceptedNextStart))
        {
            c.Expect(receipts.All(x => x.R.Ok && x.R.SlotsConsumed == 0), "part 1: the three are accepted for the next start, no slot taken now", said);
            c.Instruct("Part 1 of 2. The test mod vfx-test defined three new VFX, and they were accepted for the next start: the game " +
                       "lists its VFX once, before mods load. Quit the game to the desktop, start it again with the same profile, " +
                       "host this world, and run this check again: then the three are registered and played in front of you. " +
                       "Pass now if every assertion above is ok.");
            await c.Verdict();
            return;
        }
        if (!c.Expect(receipts.All(x => x.R.Ok && x.R.State == DefinitionRegistrationState.Registered && x.R.SlotsConsumed == 1),
                "part 2: the three are registered at this start, 1 slot each", said))
        {
            await c.Verdict();
            return;
        }
        c.Expect(before.Ok && b.KilnUsed >= 3, "the budget counts them as Kiln's", $"Kiln's {b.KilnUsed}");

        if (Ahead(c, 5, right: -2, up: 1) is not { } left || Ahead(c, 5, up: 1) is not { } middle || Ahead(c, 5, right: 2, up: 1) is not { } right)
        {
            c.Expect(false, "the player's place is known");
            await c.Verdict();
            return;
        }
        var at = new[] { left, middle, right };
        c.Instruct("Part 2 of 2. Look 5 m ahead: every 3 seconds three bursts play side by side, left to right red, green and blue: " +
                   "about 40 small sprites each that fly up and fade in about a second. They are VFX a mod defined from code at the " +
                   "last start, registered with the game at this one. Pass when you see all three, each in its colour.");
        await RepeatUntilVerdict(c, TimeSpan.FromSeconds(3), () =>
        {
            for (var i = 0; i < receipts.Count; i++) PlayVfx(c, VfxId.Named(VfxTest, receipts[i].Name), at[i]);
        });
        var after = Vfx.ResourceBudget;
        c.Expect(after.Ok && after.Budget with { Revision = 0 } == b with { Revision = 0 }, "playing them changed nothing in the budget",
            after.Ok ? $"Kiln's {after.Budget.KilnUsed}, reserved {after.Budget.Reserved}" : after.Reason);
    }
}
