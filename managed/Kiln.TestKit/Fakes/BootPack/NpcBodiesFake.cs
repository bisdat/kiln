// SPDX-License-Identifier: MIT
// The boot build's NPC body rows (package 16f, chapter 16f §5.4): kiln-quests builds roster bodies at the game's start and lists each in
// kiln.knowledge's native_manifest ("bodies": ready, failed with its refusal, removed since the last start). The kit has no boot build and
// no kiln.knowledge table, so a test scripts the rows (KilnHarness.NpcBodies) and NpcBodies.Status, NpcBodyBuildChangedEvent and the
// handshake's kiln.npcbodies read them as in a game. No table is registered: the rest of kiln.knowledge stays unavailable offline.
using System.Text.Json.Nodes;

namespace Kiln.TestKit;

/// <summary>One harness's scripted body rows, in the order they were set.</summary>
internal sealed class NpcBodiesFakeState
{
    public readonly object Lock = new();
    public readonly List<JsonObject> Rows = [];
    /// <summary>The manifest's text, made again only after a change (NpcBodyRows parses it again only when it changes).</summary>
    public string? Text;
}

internal sealed class NpcBodiesFake : IKitFake
{
    /// <summary>The service whose member is modelled (its native_manifest's "bodies" rows only).</summary>
    internal const string Service = "kiln.knowledge";
    internal const string Row = "blocks.npc_body";

    string IKitFake.Service => Service;

    public void Register()
    {
        FakeTables.Features([Row]);
        // Kiln.Blocks reports the row from its dependencies (kiln.knowledge, quests.boot_pack, ...), which the kit does not have: here it
        // is the kit's row, which a test switches off with Capabilities.Override
        BlockCapabilities.NpcBodyKit = static () => new BlockCapabilities.Row(CapabilityState.Available, CapabilityReason.None, null);
        // Status reads the test's switch from the kit's own registry (deterministic), not only from the process snapshot
        NpcBodies.KitOff = static () => FakeTables.Off(Row) ? KitBackend.Instance.Capabilities.Why(Row) : null;
    }

    public void Start(KilnHarness harness)
    {
        NpcBodies.ResetForTests();
        NpcBodyRows.TestSource = () => Manifest(harness);
    }

    public void BeforeStep(KilnHarness harness, ulong step) => NpcBodies.Kick();

    public void End(KilnHarness harness)
    {
        NpcBodyRows.TestSource = null;
        NpcBodies.ResetForTests();
    }

    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new CoverageRow("NPC bodies (NpcBodies.Status)", CoverageKind.RecordingFake, [Row],
        "the boot build's rows of NPC blocks with a body of their own, scripted with KilnHarness.NpcBodies (Ready, Failed with a reason, " +
        "Removed): NpcBodies.Status by handle and by mod:block key, Failed for a body defined from code (built from blocks/*.json only), RemovalPendingRestart " +
        "after the mod unloads, one NpcBodyBuildChangedEvent per change at the next step, and the handshake's kiln.npcbodies. Not modelled: " +
        "the native boot build itself (cooking, admission, the closure hash: the test gives them), the rest of kiln.knowledge"));

    internal static NpcBodiesFakeState StateOf(KilnHarness harness) => KitFakes.State<NpcBodiesFakeState>(harness);

    private static string Manifest(KilnHarness harness)
    {
        var s = StateOf(harness);
        lock (s.Lock)
            return s.Text ??= new JsonObject
            {
                ["mods"] = new JsonArray(), ["bodies"] = new JsonArray(s.Rows.Select(r => (JsonNode)r.DeepClone()).ToArray()),
            }.ToJsonString();
    }

    internal static void Set(KilnHarness harness, JsonObject row)
    {
        var s = StateOf(harness);
        lock (s.Lock)
        {
            s.Rows.RemoveAll(r => (string?)r["mod"] == (string?)row["mod"] && (string?)r["npc"] == (string?)row["npc"]);
            s.Rows.Add(row);
            s.Text = null;
        }
    }

    internal static void Clear(KilnHarness harness)
    {
        var s = StateOf(harness);
        lock (s.Lock)
        {
            s.Rows.Clear();
            s.Text = null;
        }
    }
}
