// SPDX-License-Identifier: MIT
// MarkerAnchor: the caller's lease on an entity a marker follows (chapter 04 §4.2, §5). Leases are made by the anchor
// slice's capture calls; until then no MarkerAnchor can exist and a spec that names one is refused (markers.anchors.client).
namespace Kiln;

/// <summary>A lease on one incarnation of an entity, for <see cref="MarkerSpec.Follow"/>. Owner-bound and valid for this
/// process and that world only; it is not an entity id and is never saved or sent. Disposing it releases the caller's
/// lease; a marker already stored with it keeps following. A disposed lease cannot be used in a new spec.</summary>
public sealed class MarkerAnchor : IDisposable
{
    private int _disposed;

    internal MarkerAnchor(ulong handle, ulong owner, EntityId entity, uint worldKind, ulong worldEpoch)
    {
        Handle = handle;
        Owner = owner;
        Entity = entity;
        WorldKind = worldKind;
        WorldEpoch = worldEpoch;
    }

    internal ulong Handle { get; }
    internal ulong Owner { get; }
    internal bool IsDisposed => Volatile.Read(ref _disposed) != 0;

    /// <summary>The entity at capture (diagnostic only: the lease follows that incarnation, not the id).</summary>
    public EntityId Entity { get; }

    /// <summary>The world kind it belongs to: 0 the server world, 2 the client's world (diagnostic).</summary>
    public uint WorldKind { get; }

    /// <summary>kiln.markers' epoch of that world (diagnostic).</summary>
    public ulong WorldEpoch { get; }

    /// <summary>Releases the caller's lease. Idempotent; any thread.</summary>
    [AnyThread]
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        unsafe
        {
            var t = MarkersNative.T;
            if (t != null && Handle != 0) t->ReleaseAnchor(Owner, Handle);
        }
    }
}
