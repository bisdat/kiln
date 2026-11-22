// SPDX-License-Identifier: MIT
// Kiln.Replication's per-peer filters, touch and the item remap: the records and delegates (the dossier's chapter 15 §5.2;
// DESIGN "Public managed API").
using System.Runtime.CompilerServices;

namespace Kiln;

/// <summary>The replication channels a filter applies to.</summary>
[Flags]
public enum ReplChannels
{
    /// <summary>The client channel (4): what every client keeps of the entities it can see.</summary>
    Client = 1,

    /// <summary>The prediction channel (5): the few entities a client predicts. A filter there needs
    /// <see cref="FilterOptions.AllowPrediction"/>: an edited value makes the client predict from it.</summary>
    Prediction = 2,
}

/// <summary>How a <see cref="Replication.Filter{TComponent}"/> runs.</summary>
public sealed record FilterOptions
{
    /// <summary>The channels it edits. Default: the client channel.</summary>
    public ReplChannels Channels { get; init; } = ReplChannels.Client;

    /// <summary>Required to include <see cref="ReplChannels.Prediction"/>.</summary>
    public bool AllowPrediction { get; init; }

    /// <summary>Higher runs first; equal priorities run in registration order. The item remap always runs after every
    /// filter.</summary>
    public int Priority { get; init; }

    /// <summary>Components of the same entity the filter's output depends on (short names, such as <c>"Fuel"</c>): a
    /// change of their bytes re-sends the entity on the client channel, which the game otherwise re-sends only when the
    /// filtered component itself changes. A dependency on another entity or on something global is yours to signal
    /// with <see cref="Replication.Touch"/>. At most 8.</summary>
    public IReadOnlyList<string> Sources { get; init; } = [];
}

/// <summary>Edits <paramref name="value"/>: one peer's copy of the component as the server sends it. Runs on the game's
/// replication job threads, in parallel: read only what <paramref name="context"/> gives, allocate nothing, call no
/// Kiln API. An exception keeps the game's value for that send and removes the filter (logged once).</summary>
public delegate void ComponentFilter<T>(ref T value, in FilterContext context) where T : unmanaged;

/// <summary>Edits <paramref name="value"/> as this client received it, before the client keeps it (client channel only).</summary>
public delegate void ReceiveFilter<T>(ref T value, EntityId entity) where T : unmanaged;

/// <summary>What a <see cref="ComponentFilter{T}"/> knows about the snapshot it edits. Valid only during the call.</summary>
public readonly unsafe ref struct FilterContext
{
    private readonly ReplNetNative.FilterCtx* _ctx;

    internal FilterContext(ReplNetNative.FilterCtx* ctx) => _ctx = ctx;

    /// <summary>The peer the snapshot is for; null for a hosted world's own client and a lab's unbound fake.</summary>
    public NetPeer? Peer => FilterHost.PeerOfMachine(_ctx->Machine);

    /// <summary>The peer's id as kiln.repl names it (see <see cref="ReplPeerStats.PeerId"/>).</summary>
    public uint PeerId => _ctx->Peer;

    /// <summary>The channel the snapshot goes on.</summary>
    public ReplChannel Channel => (ReplChannel)_ctx->Channel;

    /// <summary>The entity.</summary>
    public EntityId Entity => new(_ctx->Entity);

    /// <summary>The server tick of the snapshot.</summary>
    public uint Tick => _ctx->Tick;

    /// <summary>Reads another component of any entity as the world holds it now (read only; the world does not change
    /// during the send step).</summary>
    /// <returns>False when the entity or the component is not there, or its size is not <typeparamref name="TC"/>'s.</returns>
    public bool TryRead<TC>(EntityId entity, out TC value) where TC : unmanaged, IGameComponent<TC>
    {
        value = default;
        uint size = 0;
        var p = _ctx->Read(_ctx, entity.Value, ComponentHashOf<TC>.Hash, &size);
        if (p == null || size != (uint)sizeof(TC)) return false;
        value = Unsafe.ReadUnaligned<TC>(p);
        return true;
    }
}

/// <summary>The engine's hash of a generated component type, computed once per type.</summary>
internal static class ComponentHashOf<T> where T : unmanaged, IGameComponent<T>
{
    public static readonly uint Hash = ReplNative.ComponentHash(T.QualifiedName);
}

/// <summary>What <see cref="Replication.Touch"/> did.</summary>
/// <param name="Ok">The entity is queued: the next sim step marks it and that frame's send re-sends it to every peer.</param>
/// <param name="Reason">Why not: "entity gone", or "no carrier component" (it has none of CurrentTransform, ItemState,
/// Inventory or NetworkServerProgress, so its replication version cannot be moved).</param>
public sealed record TouchResult(bool Ok, string? Reason) : IKilnResult
{
    /// <summary>The entity asked for.</summary>
    public EntityId Entity { get; init; }
}

/// <summary>What the item remap did since the server started (<see cref="Replication.RemapStats"/>).</summary>
/// <param name="Active">The remap table is installed: Kiln-only items are hidden from the peers that lack them.</param>
/// <param name="Sites">Item-id sites (component fields) the remap edits.</param>
/// <param name="KilnOnlyItems">Kiln-only items known now.</param>
/// <param name="WithoutLookAlike">Of those, items without a look-alike: peers that lack them see an empty slot.</param>
/// <param name="Snapshots">Snapshots of a site component sent to a remapped peer.</param>
/// <param name="Remapped">Ids replaced by their look-alike.</param>
/// <param name="Zeroed">Ids replaced by 0 (no look-alike, or neither a game item nor known).</param>
/// <param name="Faults">Times the remap faulted and every site of the component was zeroed.</param>
public sealed record ReplRemapStats(bool Active, int Sites, int KilnOnlyItems, int WithoutLookAlike, long Snapshots, long Remapped,
    long Zeroed, long Faults);
