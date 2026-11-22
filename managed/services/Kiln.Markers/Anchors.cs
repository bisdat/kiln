// SPDX-License-Identifier: MIT
// Following an entity (chapter 04 §4.2, §5): the caller's leases (MarkerAnchor), the client's asynchronous capture
// (tickets posted to the client world's quiet phase), the server's synchronous capture, the bounded per-mod registry the
// JSON anchor form looks leases up in, and the nameplate convenience. kiln.markers holds the incarnations and the rules;
// this wrapper binds the mod, posts, maps answers by the error rule and keeps the managed objects honest.
using System.Text;

namespace Kiln;

/// <summary>A lease on one incarnation of an entity, for <see cref="MarkerSpec.Follow"/> (from
/// <see cref="Markers.CaptureAnchorAsync"/> on a client, <see cref="Markers.CaptureServerAnchor"/> on the server).
/// Owner-bound and valid for this process and that world only: it is not an entity id and is never saved or sent. An
/// entity destroyed, unloaded or recreated (even with the same id) is a new incarnation: a lease keeps the old one, and a
/// marker that follows it reports <c>AnchorEnded</c> until you capture again and set the marker with the new lease.
/// Disposing it releases the caller's lease; a marker already stored with it keeps following. A disposed lease cannot be
/// used in a new spec.</summary>
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
        MarkerAnchors.Forget(this);
        unsafe
        {
            var t = MarkersNative.T;
            if (t != null && Handle != 0) t->ReleaseAnchor(Owner, Handle);
        }
    }

    /// <summary>The owner unloaded: kiln.markers released its leases already (release_owner).</summary>
    internal void Retired() => Interlocked.Exchange(ref _disposed, 1);
}

/// <summary>What became of a capture, by Kiln's error rule: a bad argument throws <see cref="ArgumentException"/>, a
/// capability that is off throws <see cref="KilnUnavailableException"/> naming it, and a refusal by the current state comes
/// back here with <see cref="Ok"/> false and a <see cref="Reason"/>.</summary>
/// <param name="Ok">True when <paramref name="Anchor"/> holds a new lease.</param>
/// <param name="Reason">Why not, starting with the refusal's name: <c>NotFound</c> (no such entity, or it has no position
/// in that world yet: not created, not replicated to this client, or gone; it is never waited for), <c>NotReady</c> (no
/// world, the world changed before the capture ran, or the mod unloaded), <c>Full</c> (a lease or capture limit);
/// null when <see cref="Ok"/>.</param>
/// <param name="Anchor">The caller's lease when <see cref="Ok"/>; dispose it when it is no longer needed.</param>
public sealed record MarkerAnchorResult(bool Ok, string? Reason, MarkerAnchor? Anchor) : IKilnResult;

public static unsafe partial class Markers
{
    internal const uint WorldServer = 0, WorldClient = 2;

    /// <summary>Captures a lease on the current incarnation of a client-world entity, for a marker that follows it
    /// (<see cref="MarkerSpec.Follow"/>). The capture runs at the client world's next quiet step (between its steps) with
    /// the client world that was current when you called: if that world ends first, the result is <c>NotReady</c>. A capture
    /// produces no marker event.</summary>
    /// <remarks>
    /// <para>Limits (Kiln's policy): 64 captures of one mod and 256 in all may wait for the next client step; a lease
    /// limit of 128 per mod, 256 per world and 512 in all (<c>Full</c>). Leases end with their world; a lease never
    /// follows another entity, also when the game gives the same id to a new one.</para>
    /// <para>Canceling <paramref name="cancel"/> before the capture ran cancels the task and makes no lease; when the
    /// capture already ran, the lease it made is released and the task is canceled. When the mod unloads, its waiting
    /// captures complete with <c>NotReady</c>.</para>
    /// </remarks>
    /// <param name="entity">An entity of the client's own world (for the local player: <c>Client.LocalPlayer</c>).</param>
    /// <param name="cancel">Cancels the capture.</param>
    /// <returns>The lease, or a refusal (<see cref="MarkerAnchorResult"/>).</returns>
    /// <exception cref="ArgumentException"><paramref name="entity"/> is <see cref="EntityId.None"/>.</exception>
    /// <exception cref="KilnUnavailableException"><c>markers.anchors.client</c> is off.</exception>
    /// <exception cref="InvalidOperationException">Not called from a mod's code.</exception>
    [ClientOnly, AnyThread, RequiresCapability("markers.anchors.client")]
    public static Task<MarkerAnchorResult> CaptureAnchorAsync(EntityId entity, CancellationToken cancel = default)
    {
        if (entity.IsNone) throw new ArgumentException("the entity must not be EntityId.None", nameof(entity));
        MarkersCaps.Require(MarkersCaps.AnchorsClient);
        if (cancel.IsCancellationRequested) return Task.FromCanceled<MarkerAnchorResult>(cancel);
        var t = MarkersNative.T;
        if (!Bind(t, nameof(CaptureAnchorAsync), out var owner, out var refusal))
            return Task.FromResult(new MarkerAnchorResult(false, refusal!.Reason, null));
        var epoch = MarkerAnchors.Epoch(WorldClient);
        if (epoch == 0) return Task.FromResult(new MarkerAnchorResult(false, "NotReady: there is no client world", null));
        return MarkerAnchors.Start(ModContext.CurrentMod!, owner, entity, epoch, cancel);
    }

    /// <summary>Captures a lease on the current incarnation of a server-world entity, now (the tick thread, a dedicated
    /// server or the host of a local game). For the server's own use, e.g. a marker sent to players that follows it; the
    /// lease is never a client's. A capture produces no marker event.</summary>
    /// <remarks>Limits: 128 leases per mod, 256 on the server world, 512 in all (<c>Full</c>). An entity without a position
    /// is <c>NotFound</c> and is never waited for.</remarks>
    /// <param name="entity">An entity of the server world.</param>
    /// <returns>The lease, or a refusal (<see cref="MarkerAnchorResult"/>).</returns>
    /// <exception cref="ArgumentException"><paramref name="entity"/> is <see cref="EntityId.None"/>.</exception>
    /// <exception cref="KilnUnavailableException"><c>markers.anchors.server</c> is off.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or not called from a mod's code.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability("markers.anchors.server")]
    public static MarkerAnchorResult CaptureServerAnchor(EntityId entity)
    {
        if (entity.IsNone) throw new ArgumentException("the entity must not be EntityId.None", nameof(entity));
        MarkersCaps.Require(MarkersCaps.AnchorsServer);
        Services.RequireTickThread("Markers.CaptureServerAnchor");
        var t = MarkersNative.T;
        if (!Bind(t, nameof(CaptureServerAnchor), out var owner, out var refusal)) return new MarkerAnchorResult(false, refusal!.Reason, null);
        MarkerAnchors.Watch(ModContext.CurrentMod!);  // its leases leave the registry when it unloads
        var epoch = MarkerAnchors.Epoch(WorldServer);
        if (epoch == 0) return new MarkerAnchorResult(false, "NotReady: there is no server world", null);
        MarkersNative.AnchorResult r = default;
        r.Size = (uint)sizeof(MarkersNative.AnchorResult);
        var rc = t->CaptureAnchor(owner, WorldServer, epoch, entity.Value, &r);
        var result = MarkerAnchors.Answer(rc, &r, owner, MarkersCaps.AnchorsServer, out var error);
        if (error != null) throw error;
        return result;
    }

    /// <summary>A text label that follows an entity in the world (a nameplate): captures the entity, then sets the calling
    /// mod's local marker of <paramref name="id"/> as a world label only (no map icon, white, priority 0, no distance
    /// limit) <paramref name="height"/> metres above it, and releases its own capture lease (the stored marker keeps
    /// following). When the entity is destroyed or recreated the marker stays and reports <c>AnchorEnded</c>: call this
    /// again to follow the new one. Nothing is stored when the capture is refused.</summary>
    /// <param name="id">The marker's key: 1 to 64 characters of <c>A-Z a-z 0-9 _ . : -</c>.</param>
    /// <param name="entity">An entity of the client's own world.</param>
    /// <param name="text">The label: not empty, at most 128 bytes of UTF-8 on one line, no control characters.</param>
    /// <param name="height">Metres above the entity, in [-128, 128].</param>
    /// <returns>The result of <see cref="Set"/>, or the capture's refusal (not committed).</returns>
    /// <exception cref="ArgumentNullException"><paramref name="id"/> or <paramref name="text"/> is null.</exception>
    /// <exception cref="ArgumentException">An id outside the key rules, empty text, a height outside [-128, 128], or
    /// <see cref="EntityId.None"/> (thrown at once); text the native rules refuse (the task faults with it).</exception>
    /// <exception cref="KilnUnavailableException"><c>markers.nameplates</c> is off (at once); a row that went off while the
    /// capture waited (the task faults with it).</exception>
    /// <exception cref="InvalidOperationException">Not called from a mod's code.</exception>
    [ClientOnly, AnyThread, RequiresCapability("markers.nameplates")]
    public static Task<MarkerResult> SetNameplate(string id, EntityId entity, string text, float height = 2)
    {
        ArgumentNullException.ThrowIfNull(id);
        ArgumentNullException.ThrowIfNull(text);
        if (!MarkerAnchors.ValidKey(id)) throw new ArgumentException("id must be 1 to 64 characters of A-Z a-z 0-9 _ . : -", nameof(id));
        if (text.Length == 0) throw new ArgumentException("a nameplate needs text", nameof(text));
        if (!float.IsFinite(height) || height < -128 || height > 128) throw new ArgumentException("height must be finite, in [-128, 128] metres", nameof(height));
        if (entity.IsNone) throw new ArgumentException("the entity must not be EntityId.None", nameof(entity));
        MarkersCaps.Require(MarkersCaps.Nameplates);
        var mod = ModContext.CurrentMod ?? throw new InvalidOperationException("Markers.SetNameplate must be called from a mod's code");
        return MarkerNameplates.Set(mod, CaptureAnchorAsync(entity), id, text, height);
    }
}

/// <summary>SetNameplate's continuation (outside the unsafe Markers class, which cannot await).</summary>
internal static class MarkerNameplates
{
    // The continuation carries the original mod across the await (a pool thread has no current mod of its own).
    internal static async Task<MarkerResult> Set(ModContext mod, Task<MarkerAnchorResult> capture, string id, string text, float height)
    {
        var r = await capture.ConfigureAwait(false);
        if (!r.Ok) return new MarkerResult(false, r.Reason, false, 0, 0, 0);
        using var lease = r.Anchor!;  // the caller's lease goes; the stored marker holds its own reference
        var spec = new MarkerSpec(id, default, text, MarkerIcon.None, MarkerSurface.World, lease, height);
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = mod;
        try
        {
            return Markers.Set(spec);
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }
}

/// <summary>Kiln.Markers' side of capture: the waiting client captures (tickets), their timer, and each mod's registry
/// of live leases for the JSON anchor form.</summary>
internal static unsafe class MarkerAnchors
{
    internal const int PendingPerMod = 64, PendingTotal = 256;

    // Where a client capture runs: the client world's next quiet step (Client.Post). Tests replace it.
    internal static Action<Action> PostToClient = Client.Post;
    internal static TimeSpan TimerPeriod = TimeSpan.FromMilliseconds(100);

    private static readonly object Lock = new();
    private static readonly List<Ticket> Waiting = new();
    private static readonly HashSet<ModContext> Watched = new(ReferenceEqualityComparer.Instance);
    private static readonly Dictionary<ulong, Dictionary<ulong, MarkerAnchor>> Registry = new();
    private static Timer? _timer;

    internal sealed class Ticket(ModContext mod, ulong owner, EntityId entity, ulong epoch, CancellationToken cancel)
    {
        public readonly ModContext Mod = mod;
        public readonly ulong Owner = owner;
        public readonly EntityId Entity = entity;
        public readonly ulong Epoch = epoch;
        public readonly CancellationToken Cancel = cancel;
        public readonly TaskCompletionSource<MarkerAnchorResult> Done = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public CancellationTokenRegistration Registration;
        private int _state;  // 0 waiting, 1 capturing (the posted closure has it), 2 completed

        public bool TryClaim() => Interlocked.CompareExchange(ref _state, 1, 0) == 0;
        public bool TryEnd() => Interlocked.CompareExchange(ref _state, 2, 0) == 0;  // only a waiting ticket ends from outside
        public void Ended() => Volatile.Write(ref _state, 2);
        public bool IsEnded => Volatile.Read(ref _state) == 2;
    }

    /// <summary>kiln.markers' epoch of a world kind now (any thread); 0 without a world or the table.</summary>
    internal static ulong Epoch(uint kind)
    {
        var t = MarkersNative.T;
        ulong epoch = 0;
        return t != null && t->WorldEpoch(kind, &epoch) == MarkersNative.Ok ? epoch : 0;
    }

    internal static Task<MarkerAnchorResult> Start(ModContext mod, ulong owner, EntityId entity, ulong epoch, CancellationToken cancel)
    {
        var ticket = new Ticket(mod, owner, entity, epoch, cancel);
        lock (Lock)
        {
            var mine = 0;
            foreach (var w in Waiting)
                if (ReferenceEquals(w.Mod, mod)) mine++;
            if (mine >= PendingPerMod)
                return Task.FromResult(new MarkerAnchorResult(false, $"Full: {PendingPerMod} captures of '{mod.Info.Id}' wait for the next client step", null));
            if (Waiting.Count >= PendingTotal)
                return Task.FromResult(new MarkerAnchorResult(false, $"Full: {PendingTotal} captures wait for the next client step", null));
            Waiting.Add(ticket);
            WatchLocked(mod);
            _timer ??= new Timer(_ => CheckEpochs(), null, TimerPeriod, TimerPeriod);
        }
        if (cancel.CanBeCanceled)
        {
            ticket.Registration = cancel.Register(() => Canceled(ticket));
            // the 100 ms epoch check can end the ticket before the registration above was stored; End then disposed an
            // empty registration, so this one goes here (disposing twice is harmless) (review N2)
            if (ticket.IsEnded) ticket.Registration.Dispose();
        }
        PostToClient(() => Run(ticket));
        return ticket.Done.Task;
    }

    internal static void Watch(ModContext mod)
    {
        lock (Lock) WatchLocked(mod);
    }

    private static void WatchLocked(ModContext mod)
    {
        if (Watched.Add(mod)) mod.Retiring += () => Retire(mod);
    }

    private static void Remove(Ticket ticket)
    {
        lock (Lock)
        {
            Waiting.Remove(ticket);
            if (Waiting.Count == 0 && _timer != null)
            {
                _timer.Dispose();  // the timer runs only while captures wait
                _timer = null;
            }
        }
    }

    private static void End(Ticket ticket, MarkerAnchorResult? result, Exception? error = null, bool canceled = false)
    {
        Remove(ticket);
        ticket.Registration.Dispose();
        if (canceled) ticket.Done.TrySetCanceled(ticket.Cancel);
        else if (error != null) ticket.Done.TrySetException(error);
        else ticket.Done.TrySetResult(result!);
    }

    private static void Canceled(Ticket ticket)
    {
        if (!ticket.TryEnd()) return;  // capturing or done: the posted closure decides
        Remove(ticket);
        ticket.Done.TrySetCanceled(ticket.Cancel);
    }

    // The posted closure, on the client world's thread before its next step.
    private static void Run(Ticket ticket)
    {
        if (!ticket.TryClaim()) return;  // canceled, retired or its world went: never a lease in another world
        try
        {
            var t = MarkersNative.T;
            if (t == null)
            {
                End(ticket, null, MarkersCaps.Off(MarkersCaps.AnchorsClient));
                return;
            }
            MarkersNative.AnchorResult r = default;
            r.Size = (uint)sizeof(MarkersNative.AnchorResult);
            var rc = t->CaptureAnchor(ticket.Owner, Markers.WorldClient, ticket.Epoch, ticket.Entity.Value, &r);
            var result = Answer(rc, &r, ticket.Owner, MarkersCaps.AnchorsClient, out var error);
            if (result.Anchor is { } made && (ticket.Cancel.IsCancellationRequested || !ticket.Mod.Alive))
            {
                made.Dispose();  // no lease for a canceled capture or a mod that went
                if (ticket.Cancel.IsCancellationRequested) End(ticket, null, canceled: true);
                else End(ticket, new MarkerAnchorResult(false, "NotReady: the mod unloaded before the capture ran", null));
                return;
            }
            End(ticket, result, error);
        }
        catch (Exception e)
        {
            End(ticket, null, e);
        }
        finally
        {
            ticket.Ended();
        }
    }

    // Every 100 ms while captures wait: a ticket whose world is no longer the current one completes (NotReady).
    internal static void CheckEpochs()
    {
        Ticket[] waiting;
        lock (Lock) waiting = Waiting.ToArray();
        if (waiting.Length == 0) return;
        var now = Epoch(Markers.WorldClient);
        foreach (var ticket in waiting)
            if (ticket.Epoch != now && ticket.TryEnd())
                End(ticket, new MarkerAnchorResult(false, now == 0
                    ? "NotReady: the client world ended before the capture ran"
                    : "NotReady: the client world changed before the capture ran", null));
    }

    // A mod unloads: its waiting captures complete now; kiln.markers releases its leases (release_owner).
    private static void Retire(ModContext mod)
    {
        Ticket[] mine;
        lock (Lock)
        {
            mine = Waiting.Where(w => ReferenceEquals(w.Mod, mod)).ToArray();
            Watched.Remove(mod);
            if (Registry.Remove(mod.Owner, out var leases))
                foreach (var a in leases.Values) a.Retired();
        }
        foreach (var ticket in mine)
            if (ticket.TryEnd()) End(ticket, new MarkerAnchorResult(false, "NotReady: the mod unloaded before the capture ran", null));
    }

    /// <summary>A capture's answer by the error rule: a lease (registered for the JSON form), a refusal, or the exception
    /// to throw.</summary>
    internal static MarkerAnchorResult Answer(int rc, MarkersNative.AnchorResult* r, ulong owner, string capability, out Exception? error)
    {
        error = null;
        var reason = MarkersNative.Text(r->Reason, 256);
        switch (rc)
        {
            case MarkersNative.Ok:
                if (r->Status != MarkersNative.Accepted || r->Anchor == 0)
                    return new MarkerAnchorResult(false, reason.Length > 0 ? reason : $"status {r->Status}", null);
                var anchor = new MarkerAnchor(r->Anchor, owner, new EntityId(r->Entity), r->WorldKind, r->WorldEpoch);
                Register(anchor);
                return new MarkerAnchorResult(true, null, anchor);
            case MarkersNative.InvalidArgument:
                error = new ArgumentException(reason.Length > 0 ? reason : "kiln.markers refused an argument");
                break;
            case MarkersNative.Unavailable:
                error = MarkersCaps.Off(reason.StartsWith("markers.", StringComparison.Ordinal) ? reason : capability);
                break;
            case MarkersNative.WrongThread:
                error = new InvalidOperationException($"kiln.markers: this capture is not allowed on this thread ({reason})");
                break;
            default:
                error = new InvalidOperationException($"kiln.markers answered {rc}{(reason.Length > 0 ? ": " + reason : "")}");
                break;
        }
        return new MarkerAnchorResult(false, reason, null);
    }

    // ---- the per-mod registry of live leases (the JSON anchor form) ----

    private static void Register(MarkerAnchor anchor)
    {
        lock (Lock)
        {
            if (!Registry.TryGetValue(anchor.Owner, out var mine)) Registry[anchor.Owner] = mine = new Dictionary<ulong, MarkerAnchor>();
            // bounded: leases of a world that ended go (kiln.markers dropped them), disposed ones went at Dispose
            if (mine.Count > 0)
            {
                var server = Epoch(Markers.WorldServer);
                var client = Epoch(Markers.WorldClient);
                foreach (var stale in mine.Values.Where(a => a.WorldEpoch != (a.WorldKind == Markers.WorldClient ? client : server)).ToArray())
                    mine.Remove(stale.Handle);
            }
            mine[anchor.Handle] = anchor;
        }
    }

    internal static void Forget(MarkerAnchor anchor)
    {
        lock (Lock)
        {
            if (Registry.TryGetValue(anchor.Owner, out var mine) && mine.TryGetValue(anchor.Handle, out var held) && ReferenceEquals(held, anchor))
            {
                mine.Remove(anchor.Handle);
                if (mine.Count == 0) Registry.Remove(anchor.Owner);
            }
        }
    }

    /// <summary>The calling mod's live, not disposed lease of that handle in a world that still runs; null otherwise
    /// (unknown, another mod's, disposed, or of a world that ended).</summary>
    internal static MarkerAnchor? Find(ModContext mod, ulong handle)
    {
        MarkerAnchor? found;
        lock (Lock)
        {
            if (!Registry.TryGetValue(mod.Owner, out var mine) || !mine.TryGetValue(handle, out found)) return null;
        }
        if (found.IsDisposed) return null;
        return Epoch(found.WorldKind) == found.WorldEpoch ? found : null;
    }

    internal static int Registered(ulong owner)
    {
        lock (Lock) return Registry.TryGetValue(owner, out var mine) ? mine.Count : 0;
    }

    internal static int WaitingCount
    {
        get
        {
            lock (Lock) return Waiting.Count;
        }
    }

    internal static bool TimerRunning
    {
        get
        {
            lock (Lock) return _timer != null;
        }
    }

    /// <summary>A marker key: 1 to 64 bytes of A-Z a-z 0-9 _ . : - (the native grammar).</summary>
    internal static bool ValidKey(string s)
    {
        if (s.Length is 0 or > 64) return false;
        foreach (var c in s)
            if (!(char.IsAsciiLetterOrDigit(c) || c is '_' or '.' or ':' or '-')) return false;
        return true;
    }

    /// <summary>Tests: forget every ticket, lease and watch (each test starts clean).</summary>
    internal static void ForgetAll()
    {
        lock (Lock)
        {
            Waiting.Clear();
            Watched.Clear();
            Registry.Clear();
            _timer?.Dispose();
            _timer = null;
        }
        PostToClient = Client.Post;
    }
}
