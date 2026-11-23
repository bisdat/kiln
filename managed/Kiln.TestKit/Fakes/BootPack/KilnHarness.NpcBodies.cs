// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;

namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessNpcBodies? _npcBodies;

    /// <summary>The boot build's NPC body rows this harness's game reports: script what the start built, then test
    /// <c>NpcBodies.Status</c> and <c>NpcBodyBuildChangedEvent</c>. Since 0.2.0-dev.31.</summary>
    public HarnessNpcBodies NpcBodies => _npcBodies ??= new HarnessNpcBodies(this);
}

/// <summary>
/// What the game's start built of NPC blocks with a body of their own (<see cref="KilnHarness.NpcBodies"/>). Since 0.2.0-dev.31.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> The rows a start's boot build reports, one per NPC block (<c>mod</c>, block name): Ready with the clone, model,
/// graph, closure hash and generation it built, Failed with the refusal, Removed (built at the previous start, gone now).
/// <c>NpcBodies.Status</c> answers from them as in a game, with what the harness's mods did since: a body defined from code is
/// Failed (this version builds authored bodies from blocks/*.json only), a Ready body of a mod that unloads is RemovalPendingRestart. <c>NpcBodyBuildChangedEvent</c> comes once per change at
/// the next step. The row <c>blocks.npc_body</c> switched off (<c>Capabilities.Override</c>) makes <c>Status</c> throw.</para>
/// <para><b>Not modelled</b> (offline): the boot build itself (cooking, admission, the motion closure and its hash): nothing is built
/// from a mod's files, the test says what was.</para>
/// </remarks>
public sealed class HarnessNpcBodies
{
    private readonly KilnHarness _harness;

    internal HarnessNpcBodies(KilnHarness harness) => _harness = harness;

    /// <summary>The start built <paramref name="npc"/> of <paramref name="mod"/>: Ready. <paramref name="closure"/>: the body's closure
    /// hash (64 hex digits; default: derived from the mod and name), what the handshake compares.</summary>
    public void Ready(string mod, string npc, Guid template = default, Guid model = default, Guid graph = default, string? closure = null,
        ulong generation = 1)
    {
        Check(mod, npc);
        closure ??= Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes($"{mod}:{npc}")));
        NpcBodiesFake.Set(_harness, Row(mod, npc, "ready", "", template, model, graph, closure, generation));
    }

    /// <summary>The start's boot build refused <paramref name="npc"/> of <paramref name="mod"/> for <paramref name="reason"/>: Failed.</summary>
    public void Failed(string mod, string npc, string reason, ulong generation = 0)
    {
        Check(mod, npc);
        ArgumentException.ThrowIfNullOrWhiteSpace(reason);
        NpcBodiesFake.Set(_harness, Row(mod, npc, "failed", reason, default, default, default, "", generation));
    }

    /// <summary><paramref name="npc"/> of <paramref name="mod"/> was Ready at the previous start and is gone now: Removed.</summary>
    public void Removed(string mod, string npc, ulong generation = 0)
    {
        Check(mod, npc);
        NpcBodiesFake.Set(_harness, Row(mod, npc, "removed", "", default, default, default, "", generation));
    }

    /// <summary>No rows: a start that built no bodies.</summary>
    public void Clear() => NpcBodiesFake.Clear(_harness);

    private static void Check(string mod, string npc)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(mod);
        ArgumentException.ThrowIfNullOrWhiteSpace(npc);
    }

    private static JsonObject Row(string mod, string npc, string state, string reason, Guid template, Guid model, Guid graph, string closure,
        ulong generation) => new()
    {
        ["mod"] = mod, ["npc"] = npc, ["key"] = npc, ["state"] = state, ["reason"] = reason,
        ["template"] = template == Guid.Empty ? "" : template.ToString(), ["model"] = model == Guid.Empty ? "" : model.ToString(),
        ["graph"] = graph == Guid.Empty ? "" : graph.ToString(), ["closure"] = closure, ["generation"] = generation,
    };
}
