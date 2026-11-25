// SPDX-License-Identifier: MIT
// The kit's content lookups (0.2.0-dev.31.13, mod API 0.2.22): the game's texts per language and its voxel materials, as the
// test scripts them (KilnHarness.GameTexts, KilnHarness.VoxelMaterials). Not a native table: Kiln.Api's GameTextStore,
// Kiln.Items' GameItemTexts and Kiln.Content's VoxelMaterialData read from these sources instead of the game's resources, so
// Content.TextIn / TryGetTexts / LoadTextsAsync, Items.Description / DescriptionIn / DisplayNameIn / TextTags and
// Content.TryGetVoxelMaterials / LoadVoxelMaterialsAsync run their own code. A dedicated-server harness has no texts, as a
// dedicated server has none (the same reason text); its voxel materials are there, as on a server.
namespace Kiln.TestKit;

/// <summary>One harness's scripted game data for the content lookups.</summary>
internal sealed class GameLookupsState
{
    public readonly object Gate = new();
    public readonly List<string> Languages = [];
    public readonly Dictionary<string, Dictionary<uint, string>> Texts = new(StringComparer.Ordinal);
    public readonly Dictionary<uint, ItemTextTags> ItemTags = new();
    public readonly SortedDictionary<int, VoxelMaterial> Materials = new();
    public int TextReads, MaterialReads;
}

internal sealed class GameLookupsFake : IKitFake
{
    internal const string Service = "kiln.content.lookups";

    string IKitFake.Service => Service;

    public void Register() { }

    public void Start(KilnHarness harness)
    {
        GameTextStore.UseSource(new TextSource(harness));
        GameItemTexts.ResetForTests();
        var state = StateOf(harness);
        GameItemTexts.Override = item =>
        {
            lock (state.Gate) return state.ItemTags.TryGetValue(item.Value, out var t) ? t : null;
        };
        VoxelMaterialData.UseSource(new MaterialSource(harness));
        ServiceCapRows.RefreshStarted();
    }

    public void BeforeStep(KilnHarness harness, ulong step) { }

    public void End(KilnHarness harness)
    {
        GameTextStore.UseSource(null);
        GameItemTexts.ResetForTests();
        VoxelMaterialData.UseSource(null);
    }

    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new CoverageRow("Content lookups (game texts per language, voxel materials)",
        CoverageKind.RecordingFake, [Content.GameTextsCapability, Content.VoxelMaterialsCapability],
        "KilnHarness.GameTexts scripts the game's text tables per language and items' text tags; KilnHarness.VoxelMaterials the voxel " +
        "material rows (what the game's terraforming registry, world table and items give). Kiln's own code answers from them: " +
        "Content.TextLanguages, TextIn, TryGetTexts, LoadTextsAsync, Items.TextTags, Description, DescriptionIn, DisplayNameIn, " +
        "Content.TryGetVoxelMaterials and LoadVoxelMaterialsAsync with their labels. A dedicated-server harness (the default) has no game " +
        "texts, as a dedicated server: the same wrong-side reason; English from a Kiln catalog is not modelled. Not modelled: the game's " +
        "loca bytes and resource reads (host tests cover the readers), Content.Text (kiln.content's main-thread lookup), new items' texts"));

    internal static GameLookupsState StateOf(KilnHarness harness) => KitFakes.State<GameLookupsState>(harness);

    private sealed class TextSource(KilnHarness harness) : IGameTextSource
    {
        private GameLookupsState State => StateOf(harness);

        public string? Unavailable =>
            harness.Options.Side == HarnessSide.DedicatedServer ? GameTextStore.ServerReason
            : FakeTables.Off(Content.GameTextsCapability) ? "switched off by the test"
            : null;

        public IReadOnlyList<string> Languages
        {
            get
            {
                if (Unavailable != null) return [];
                lock (State.Gate) return [.. State.Languages];
            }
        }

        public Dictionary<uint, string>? Read(string language, out string reason)
        {
            if (Unavailable is { } why)
            {
                reason = why;
                return null;
            }
            var s = State;
            lock (s.Gate)
            {
                s.TextReads++;
                if (s.Texts.TryGetValue(language, out var t))
                {
                    reason = "";
                    return new Dictionary<uint, string>(t);
                }
                reason = GameTextStore.NotShipped(language, s.Languages);
                return null;
            }
        }
    }

    private sealed class MaterialSource(KilnHarness harness) : IVoxelMaterialSource
    {
        public string? Unavailable => FakeTables.Off(Content.VoxelMaterialsCapability) ? "switched off by the test" : null;

        public IReadOnlyList<VoxelMaterial>? Read(out string reason)
        {
            var s = StateOf(harness);
            lock (s.Gate)
            {
                s.MaterialReads++;
                reason = "";
                return [.. s.Materials.Values];
            }
        }
    }
}
