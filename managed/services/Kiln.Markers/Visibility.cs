// SPDX-License-Identifier: MIT
// Markers' visibility events (chapter 04 §4.6): admission per surface at each client step, not pixels.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>Why a requested surface of a marker is or is not admitted (<see cref="MarkerVisibilityChangedEvent"/>).
/// When several apply, the first in this order is given: <see cref="WorldInactive"/>, <see cref="CapabilityOff"/>,
/// <see cref="AnchorEnded"/>, <see cref="FollowMissing"/> or <see cref="SourceStale"/>, <see cref="InvalidPosition"/>,
/// <see cref="CameraUnavailable"/>, <see cref="OutOfRange"/>, <see cref="BehindCamera"/>, <see cref="MapCapacity"/> or
/// <see cref="SurfaceBudget"/>, else <see cref="Ready"/>.</summary>
public enum MarkerVisibilityReason
{
    /// <summary>Admitted: on the game's map list, or in the drawn compass or world labels this step.</summary>
    Ready = 0,
    /// <summary>The game's map list had no room this step (the game's own entries come first); retried every step.</summary>
    MapCapacity = 1,
    /// <summary>Kiln's budget for the surface was spent by markers ranked higher (128 map rows a step, 128 world labels,
    /// 64 compass entries); retried every step.</summary>
    SurfaceBudget = 2,
    /// <summary>No fresh camera or no game window (drawn surfaces, and any marker with a MaxDistance).</summary>
    CameraUnavailable = 3,
    /// <summary>A world label behind the camera.</summary>
    BehindCamera = 4,
    /// <summary>Farther from the camera than the marker's MaxDistance.</summary>
    OutOfRange = 5,
    /// <summary>The followed entity has no position right now (the same anchor recovers by itself).</summary>
    FollowMissing = 6,
    /// <summary>The followed entity was destroyed, recreated or its world ended: the marker stays hidden until its
    /// publisher sets it again with a new anchor.</summary>
    AnchorEnded = 7,
    /// <summary>There is no client world.</summary>
    WorldInactive = 8,
    /// <summary>The surface's capability is off in this game (<c>markers.map</c>, <c>markers.compass</c>, <c>markers.world</c>).</summary>
    CapabilityOff = 9,
    /// <summary>A received followed marker's positions stopped arriving (750 ms).</summary>
    SourceStale = 10,
    /// <summary>The position is not a point of the world.</summary>
    InvalidPosition = 11,
    /// <summary>The marker is gone (removed, cleared, its mod unloaded, its world or link ended): the last event of a row.</summary>
    Removed = 12,
    /// <summary>The marker does not ask for this surface.</summary>
    NotRequested = 13,
}

/// <summary>A marker's admission changed on this client: which of its requested surfaces it has now and why not the
/// others. Admission, not pixels: a map row is on the game's map list even while the map is closed and follows the
/// icon type's own fog, zoom and filter rules; a drawn surface is admitted with this step's camera. Raised once with
/// <see cref="Initial"/> the first step a marker is evaluated, then only when the admitted surfaces or a reason change
/// (a moving followed marker raises nothing while its admission stays), and a last time with <see cref="After"/> empty and
/// <see cref="MarkerVisibilityReason.Removed"/> when an evaluated marker goes. A marker set and removed before a step is
/// never evaluated and raises none. Immutable; safe to read on any thread.</summary>
[ClientOnly, AnyThread]
public sealed class MarkerVisibilityChangedEvent
{
    internal MarkerVisibilityChangedEvent(string owner, string id, MarkerSource audience, EntityId recipient, ulong peerGeneration,
        ulong publisherGeneration, ulong worldEpoch, ulong sequence, bool initial, MarkerSurface requested, MarkerSurface before,
        MarkerSurface after, MarkerVisibilityReason map, MarkerVisibilityReason compass, MarkerVisibilityReason world)
    {
        Owner = owner;
        Id = id;
        Audience = audience;
        Recipient = recipient;
        PeerGeneration = peerGeneration;
        PublisherGeneration = publisherGeneration;
        WorldEpoch = worldEpoch;
        Sequence = sequence;
        Initial = initial;
        Requested = requested;
        Before = before;
        After = after;
        Map = map;
        Compass = compass;
        World = world;
    }

    /// <summary>The publisher's namespace (a mod's id).</summary>
    public string Owner { get; }
    /// <summary>The marker's key.</summary>
    public string Id { get; }
    /// <summary>A local marker or a received one.</summary>
    public MarkerSource Audience { get; }
    /// <summary>Always <see cref="EntityId.None"/> on a client (kept for the shape of the server events).</summary>
    public EntityId Recipient { get; }
    /// <summary>The link generation of a received marker; 0 for a local one.</summary>
    public ulong PeerGeneration { get; }
    /// <summary>The publisher's binding generation.</summary>
    public ulong PublisherGeneration { get; }
    /// <summary>kiln.markers' epoch of the client world.</summary>
    public ulong WorldEpoch { get; }
    /// <summary>The sequence of the logical change that set the marker's current value.</summary>
    public ulong Sequence { get; }
    /// <summary>The first evaluation of this marker.</summary>
    public bool Initial { get; }
    /// <summary>The surfaces the marker asks for.</summary>
    public MarkerSurface Requested { get; }
    /// <summary>The surfaces it had before this event (none for <see cref="Initial"/>).</summary>
    public MarkerSurface Before { get; }
    /// <summary>The surfaces it has now.</summary>
    public MarkerSurface After { get; }
    /// <summary>The map surface's reason (<see cref="MarkerVisibilityReason.Ready"/> exactly when <see cref="After"/> has it).</summary>
    public MarkerVisibilityReason Map { get; }
    /// <summary>The compass surface's reason.</summary>
    public MarkerVisibilityReason Compass { get; }
    /// <summary>The world-label surface's reason.</summary>
    public MarkerVisibilityReason World { get; }
}

public static unsafe partial class Markers
{
    /// <summary>A marker's admission on this client changed (<see cref="MarkerVisibilityChangedEvent"/>), raised at the client
    /// world's step on the game's main thread, after that step's <see cref="Changed"/> events; when the client world stops,
    /// the last ones arrive on the stopping thread: do not call game code from the handler then. A handler that throws is
    /// logged and removed; a mod's handlers go when it unloads. Not raised on a dedicated server.</summary>
    [AnyThread]
    public static event Action<MarkerVisibilityChangedEvent>? VisibilityChanged
    {
        add
        {
            VisibilityHandlers.Add(value);
            EnsureSubscribed();
        }
        remove => VisibilityHandlers.Remove(value);
    }

    internal static readonly MarkerHandlers<Action<MarkerVisibilityChangedEvent>> VisibilityHandlers = new("Markers.VisibilityChanged");

    private static MarkerVisibilityReason Reason(uint r) => r <= 13 ? (MarkerVisibilityReason)r : MarkerVisibilityReason.CapabilityOff;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnVisibility(MarkersNative.Visibility* v, void* user)
    {
        try
        {
            if (v == null || v->Size < (uint)sizeof(MarkersNative.Visibility)) return;
            const uint all = (uint)(MarkerSurface.Map | MarkerSurface.Compass | MarkerSurface.World);
            var e = new MarkerVisibilityChangedEvent(MarkersNative.Text(v->Owner, 65), MarkersNative.Text(v->Key, 65),
                v->Audience <= 3 ? (MarkerSource)v->Audience : MarkerSource.Received, new EntityId(v->Recipient), v->PeerGeneration,
                v->PublisherGeneration, v->WorldEpoch, v->Sequence, v->Initial != 0, (MarkerSurface)(v->Requested & all),
                (MarkerSurface)(v->Before & all), (MarkerSurface)(v->After & all), Reason(v->Reason[0]), Reason(v->Reason[1]),
                Reason(v->Reason[2]));
            VisibilityHandlers.Raise(h => h(e));
        }
        catch (Exception x)
        {
            try { Backend.Current.Log(0, LogLevel.Error, $"kiln.markers: a visibility change could not be delivered: {x.Message}"); }
            catch { /* nothing left to say */ }
        }
    }
}
