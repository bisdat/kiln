// SPDX-License-Identifier: MIT
// Lab servers only: kiln.markers.lab (native/core/abi/services/kiln_markers_lab.h), for mods/tests/markers-test. Published by
// kiln-markers.dll only when kiln/control/markers.lab exists when a dedicated server starts.
using System.ComponentModel;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>Lab only: what a lab test needs to prove kiln.markers' anchors on a dedicated server (the follow read of a
/// lease now, the observers' counters and threads, an entity created with an explicit id and only a position). Present
/// only on a lab server started with the file <c>kiln/control/markers.lab</c>; every member answers null (or
/// <see cref="EntityId.None"/>) anywhere else. Not part of the mod API.</summary>
[Experimental("KILNX001", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/analyzers.md#kilnx001")]
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class MarkersLab
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct Follow
    {
        public uint Size, Reason;
        public ulong Incarnation, WorldEpoch;
        public uint WorldKind, Entity;
        public MarkersNative.Vec3 Position;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Stats
    {
        public uint Size, Threads, OtherThreads, Watched, Leases, References;
        public ulong Creates, Destroys, NetworkCreates, Ended, Serials;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, ulong, Follow*, int> FollowNow;
        public delegate* unmanaged[Cdecl]<Stats*, int> Stats;
        public delegate* unmanaged[Cdecl]<uint, byte*, MarkersNative.Vec3, uint*, int> CreateRaw;
    }

    private static Table* T => Services.Get<Table>("kiln.markers.lab", 1);

    /// <summary>Lab only: true on a lab server with kiln.markers.lab.</summary>
    [Experimental("KILNX001", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/analyzers.md#kilnx001")]
    [EditorBrowsable(EditorBrowsableState.Never)]
    [AnyThread]
    public static bool Available => T != null;

    /// <summary>Lab only: reads a lease's incarnation now, as a world's Post does for a stored marker, on the lease's world
    /// thread: <c>Ready</c> (with where it is), <c>FollowMissing</c>, <c>InvalidPosition</c> or <c>AnchorEnded</c>, with
    /// its incarnation (a new capture of a recreated entity has another). Null without the lab table, for a released or
    /// foreign lease, or off the world's thread.</summary>
    [Experimental("KILNX001", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/analyzers.md#kilnx001")]
    [EditorBrowsable(EditorBrowsableState.Never)]
    [ServerOnly, TickThreadOnly]
    public static (string State, ulong Incarnation, EntityId Entity, WorldPosition Position)? FollowNow(MarkerAnchor anchor)
    {
        ArgumentNullException.ThrowIfNull(anchor);
        var t = T;
        if (t == null || anchor.IsDisposed) return null;
        var f = new Follow { Size = (uint)sizeof(Follow) };
        if (t->FollowNow(anchor.Owner, anchor.Handle, &f) != MarkersNative.Ok) return null;
        var state = f.Reason switch
        {
            0 => "Ready",           // KILN_MARKER_READY
            6 => "FollowMissing",   // KILN_MARKER_FOLLOW_MISSING
            7 => "AnchorEnded",     // KILN_MARKER_ANCHOR_ENDED
            11 => "InvalidPosition", // KILN_MARKER_BAD_POSITION
            _ => $"reason {f.Reason}",
        };
        return (state, f.Incarnation, new EntityId(f.Entity), new WorldPosition(f.Position.X, f.Position.Y, f.Position.Z));
    }

    /// <summary>Lab only: the anchors' counters: the threads the create/destroy observers ran on (and how many of them are
    /// not a world's own thread: the game's workers), incarnations watched, leases, stored references, observer calls,
    /// incarnations ended and minted. Null without the lab table.</summary>
    [Experimental("KILNX001", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/analyzers.md#kilnx001")]
    [EditorBrowsable(EditorBrowsableState.Never)]
    [AnyThread]
    public static (int Threads, int OtherThreads, int Watched, int Leases, int References, ulong Creates, ulong Destroys, ulong NetworkCreates, ulong Ended,
        ulong Serials)? Counters()
    {
        var t = T;
        if (t == null) return null;
        var s = new Stats { Size = (uint)sizeof(Stats) };
        if (t->Stats(&s) != MarkersNative.Ok) return null;
        return ((int)s.Threads, (int)s.OtherThreads, (int)s.Watched, (int)s.Leases, (int)s.References, s.Creates, s.Destroys, s.NetworkCreates, s.Ended,
            s.Serials);
    }

    /// <summary>Lab only: the game's immediate create on the server world with <paramref name="requestedId"/> (0: the
    /// engine picks), the template and only a position: no SpawnTime, nothing Kiln.Spawn adds. Not tracked: destroy it
    /// yourself. <see cref="EntityId.None"/> when refused or without the lab table.</summary>
    [Experimental("KILNX001", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/analyzers.md#kilnx001")]
    [EditorBrowsable(EditorBrowsableState.Never)]
    [ServerOnly, TickThreadOnly]
    public static EntityId CreateRaw(uint requestedId, Guid template, WorldPosition position)
    {
        var t = T;
        if (t == null) return EntityId.None;
        Services.RequireTickThread("MarkersLab.CreateRaw");
        var guid = template.ToByteArray();  // the game's byte order
        uint id = 0;
        int rc;
        fixed (byte* g = guid) rc = t->CreateRaw(requestedId, g, new MarkersNative.Vec3 { X = position.X, Y = position.Y, Z = position.Z }, &id);
        return rc == MarkersNative.Ok ? new EntityId(id) : EntityId.None;
    }
}
