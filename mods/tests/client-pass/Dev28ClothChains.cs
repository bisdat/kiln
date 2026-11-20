// SPDX-License-Identifier: MIT
// 16e-cloth-chains: the guided check of cloth and bone chains that Kiln's cook wrote into a mod's models (id 16e-cc16e, docs/kiln/modding/models.md#physics).
// The fixture is mods/tests/model-physics-client: a banner to place (cloth, a collider), a worn cape cut down its middle for its UVs (cloth on the
// Chest joint) and a creature wearing a three-link pendant (a chain). What the code can see is asserted: the fixture is cooked, and the game's cloth
// and chain code ran (the fixture's read-only counts on the sigdb anchors models/…). What only a player can judge is asked: the pins hold, the
// loose parts move without exploding, the seam stays joined, the collider sits where it should, and nothing is left behind.
// Confirms modelcook.cloth and modelcook.chains2 (on since 0.2.0-dev.31, never held: NeedOpen passes unless a release holds them again).
using System.Text.Json;
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    private const string PhysicsFixture = "model-physics-client";
    private const int ClothChainsOrder = 2000 + (28290 - 28000) * 10;

    /// <summary>The fixture's counts of the game's cloth and chain functions (kiln/data/model-physics-client/counts.json), empty when it wrote none.</summary>
    private Dictionary<string, long> PhysicsCounts()
    {
        var file = Path.Combine(Path.GetDirectoryName(Context.DataDirectory.TrimEnd('/', '\\'))!, PhysicsFixture, "counts.json");
        try { return JsonSerializer.Deserialize<Dictionary<string, long>>(File.ReadAllText(file)) ?? []; }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException) { return []; }
    }

    private static long Grew(Dictionary<string, long> before, Dictionary<string, long> after, string key) =>
        after.GetValueOrDefault(key) - before.GetValueOrDefault(key);

    [ClientCheck("16e-cc16e", "Cloth and chains: a banner, a cape and a pendant cooked by Kiln hang and swing", Area = "Models", Since = Dev31,
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = ClothChainsOrder)]
    public async Task ClothAndChainsOnScreen(CheckContext c)
    {
        if (!await Need(c, PhysicsFixture) || !await NeedOpen(c, "modelcook.cloth", "modelcook.chains2") || !await NeedPlayer(c)) return;
        var dir = Mods.Get(PhysicsFixture)?.Directory ?? "";
        var expected = Path.Combine(dir, "assets", "cooked", "expected.json");
        if (!c.Expect(File.Exists(expected), "the fixture is cooked (build-assets.py ran)", expected)) { await c.Verdict(); return; }
        c.Log.Info($"client-pass: 16e-cc16e what the writer made: {File.ReadAllText(expected).ReplaceLineEndings(" ")}");
        var before = PhysicsCounts();
        c.Log.Info($"client-pass: 16e-cc16e counts before: {string.Join(", ", before.Select(k => $"{k.Key}={k.Value}"))}");

        await GiveBlock(c, PhysicsFixture, "physics-banner");
        await GiveBlock(c, PhysicsFixture, "physics-cape");
        if (Ahead(c, 4) is { } at) await SpawnHere(c, $"{PhysicsFixture}:pendant-stand", at);
        c.Instruct("1. Banner: place the Physics Banner on open ground. Its top edge stays where it is; the cloth below hangs down and sways when you " +
                   "walk past and in the wind, and drapes over an invisible ball a little in front of its middle. Walk round it and look from every side. " +
                   "2. Cape: put the Physics Cape into its vanity slot. Stand, walk, run, jump and turn on the spot. Its top stays on your shoulders and " +
                   "follows you; the rest swings behind and does not pass through your back; the cut down its middle never opens into a gap. " +
                   "3. Pendant: the Pendant Stand stands in front of you. Its three small rings hang below the larger one in a line and swing when it " +
                   "turns; they never fly off or stretch apart. 4. Travel: teleport (a fast travel point or the map) with the cape on, come back to the " +
                   "banner. 5. Take the cape off and put it on again. 6. Save, quit to the menu, load the world again and look at all three once more.");
        var after = PhysicsCounts();
        c.Log.Info($"client-pass: 16e-cc16e counts after: {string.Join(", ", after.Select(k => $"{k.Key}={k.Value}"))}");
        if (after.Count == 0)
            c.Log.Info("client-pass: 16e-cc16e the fixture wrote no counts (its sigdb features are off on this build): only the player's answers count");
        else
        {
            c.Expect(Grew(before, after, "models/ClothInstance") > 0, "the game set up cloth instances while the banner and cape were out",
                     $"ClothInstance +{Grew(before, after, "models/ClothInstance")}, ClothStep +{Grew(before, after, "models/ClothStep")}");
            c.Expect(Grew(before, after, "models/Chain2Init") > 0, "the game set up chains while the pendant was out",
                     $"Chain2Init +{Grew(before, after, "models/Chain2Init")}, direct steps +{Grew(before, after, "models/Chain2DirectStep")}, queued steps +{Grew(before, after, "models/Chain2Step")}");
        }
        c.Instruct("Did the banner's top edge and the cape's top stay put while their loose parts hung and swayed, did the cape stay outside your " +
                   "body with its seam closed, did the pendant's rings hang and swing as a chain, did nothing jitter, explode, stretch or vanish " +
                   "through the teleport, the re-equip and the reload, and did the cape come off cleanly with nothing left floating?");
        await c.Verdict();
    }
}
