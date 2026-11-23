// SPDX-License-Identifier: MIT
// Camera: camera overrides as leases (docs/kiln/services/camera.md). Wrapper for kiln.camera: every entry checks its
// arguments, asks for the rows its parts need, and hands the request to the service; the answer comes back as a task.
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>
/// Camera overrides. A request that is accepted becomes a <see cref="CameraLease"/>: one wins per player and endpoint,
/// it lasts for its <see cref="CameraOverride.Duration"/> at most, and its owner updates or ends it.
/// </summary>
/// <remarks>
/// <para>Since mod API 0.2.17. Each endpoint requires its own available capability rows.</para>
/// <para><b>Two kinds.</b> A <i>mode lock</i> (<see cref="SetMode"/>) makes the server hold a player in one of the
/// game's own camera modes; it reaches every client, vanilla ones too, and an Ok result says the server's state holds
/// it (<see cref="CameraCommitScope.ServerState"/>). A <i>visual</i> request (<see cref="SetLocal"/>, <see cref="Set"/>)
/// changes only what is drawn, on a client that runs Kiln: the player's input camera and aim never move, and Ok means
/// a frame was drawn with it (<see cref="CameraCommitScope.ClientFrame"/>).</para>
/// <para><b>Results.</b> A request the game or another lease refuses comes back with <c>Ok = false</c> and a
/// <see cref="CameraReason"/>; a malformed one throws <see cref="ArgumentException"/>; a part whose capability row is
/// off throws <see cref="KilnUnavailableException"/> naming the row. The task completes at a later step, never
/// inside the call. Tasks of a mod that unloads are cancelled.</para>
/// </remarks>
public static unsafe class Camera
{
    private sealed class Pending(ModContext? mod)
    {
        public readonly TaskCompletionSource<CameraResult> Tcs = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public readonly ModContext? Mod = mod;
    }

    private sealed class Handler(Action<CameraChange> fn, ModContext? mod)
    {
        public readonly Action<CameraChange> Fn = fn;
        public readonly ModContext? Mod = mod;
    }

    private static readonly object Lock = new();
    private static readonly ConcurrentDictionary<nint, Pending> Waiting = new();
    private static readonly HashSet<ModContext> Watched = new(ReferenceEqualityComparer.Instance);
    private static Handler[] _handlers = [];
    private static long _nextKey;
    private static int _subscribed;  // 0 no, 1 yes

    /// <summary>Locks <paramref name="player"/>'s camera to one of the game's own modes for <paramref name="duration"/>.
    /// Reaches vanilla clients. Only <see cref="CameraMode.PlayerCameraBuildingFirstPerson"/> was seen working in the
    /// real game; the game may not honour every mode in every state of the player.</summary>
    /// <param name="player">The player; not <see cref="EntityId.None"/>.</param>
    /// <param name="mode">The stock camera mode.</param>
    /// <param name="duration">1 ms to 120 s in whole milliseconds; zero means 10 s.</param>
    /// <param name="transition">How the game moves into the mode.</param>
    /// <param name="priority">-1000 to 1000; another mod's lock replaces this one only with a higher number.</param>
    /// <exception cref="ArgumentException">A value is out of range.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or called outside mod code.</exception>
    /// <exception cref="KilnUnavailableException"><c>camera.server_mode</c> is off.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability("camera.server_mode")]
    public static Task<CameraResult> SetMode(EntityId player, CameraMode mode, TimeSpan duration,
        CameraTransitionOverride transition = CameraTransitionOverride.None, int priority = 0)
    {
        const string api = "Camera.SetMode";
        Services.RequireTickThread(api);
        var request = new CameraOverride(Mode: new CameraModeRequest(mode, transition), Duration: duration, Priority: priority, InputPolicy: CameraInputPolicy.StockMode);
        return Apply(api, request, CameraEndpoint.ServerMode, RequirePlayer(player, api));
    }

    /// <summary>A camera request of the server for <paramref name="player"/>: a mode lock (see <see cref="SetMode"/>),
    /// or visual parts for a player whose client runs Kiln.</summary>
    /// <exception cref="ArgumentException">The request is malformed, or there is no player.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or called outside mod code.</exception>
    /// <exception cref="KilnUnavailableException">A row the request needs is off: <c>camera.server_mode</c> for a mode
    /// lock, <c>camera.remote</c> for visual parts.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability("camera.server")]
    public static Task<CameraResult> Set(EntityId player, CameraOverride request)
    {
        const string api = "Camera.Set";
        Services.RequireTickThread(api);
        ArgumentNullException.ThrowIfNull(request);
        return Apply(api, request, request.Mode is not null ? CameraEndpoint.ServerMode : CameraEndpoint.Remote, RequirePlayer(player, api));
    }

    /// <summary>A render-only camera for this client's own player: a path, a follow profile, a lens, a shake. Call it
    /// on the client world's thread (<c>Client.Tick</c>, <c>Client.Post</c>). A mode lock is not a local request: it is
    /// answered <see cref="CameraReason.WrongEndpoint"/> (use <see cref="SetMode"/> on the server).</summary>
    /// <exception cref="ArgumentException">The request is malformed.</exception>
    /// <exception cref="InvalidOperationException">Not on the client world's thread, or called outside mod code.</exception>
    /// <exception cref="KilnUnavailableException">A part's row is off: <c>camera.local_pose</c> (path),
    /// <c>camera.local_profile</c>, <c>camera.local_fov</c> (lens), <c>camera.local_shake</c>.</exception>
    [ClientOnly, TickThreadOnly, RequiresCapability("camera.local")]
    public static Task<CameraResult> SetLocal(CameraOverride request)
    {
        const string api = "Camera.SetLocal";
        ArgumentNullException.ThrowIfNull(request);
        if (request.Mode is not null)
        {
            RequireOwner(api);
            CameraRequests.Build(request, CameraEndpoint.Local, EntityId.None, api);  // a malformed one still throws
            return Task.FromResult(Refused(CameraReason.WrongEndpoint, "a mode lock is the server's: use Camera.SetMode in the server world", CameraEndpoint.Local, EntityId.None));
        }
        return Apply(api, request, CameraEndpoint.Local, EntityId.None);
    }

    /// <summary>Replaces the whole request of a lease (not a merge). The lease keeps its player and its kind: a mode
    /// lock stays a mode lock. A refused update leaves the lease as it was.</summary>
    /// <param name="lease">A lease of the calling mod.</param>
    /// <param name="replacement">The new request.</param>
    /// <param name="timing">Whether the lease's clock goes on or starts again.</param>
    /// <exception cref="ArgumentException">The request is malformed, or <paramref name="lease"/> is none.</exception>
    /// <exception cref="InvalidOperationException">Not on the thread of the lease's world, inside a program wrap's
    /// handler, or outside mod code.</exception>
    /// <exception cref="KilnUnavailableException">A row the new request needs is off.</exception>
    [TickThreadOnly, RequiresCapability("camera.service")]
    public static Task<CameraResult> Update(CameraLease lease, CameraOverride replacement, CameraUpdateTiming timing = CameraUpdateTiming.Preserve)
    {
        const string api = "Camera.Update";
        ArgumentNullException.ThrowIfNull(replacement);
        RequireLease(lease, api);
        RequireLeaseThread(lease, api);
        var owner = RequireOwner(api);
        if (!Enum.IsDefined(timing)) throw new ArgumentException($"{api}: timing {timing} is not Preserve or Restart", nameof(timing));
        var built = CameraRequests.Build(replacement, lease.Endpoint, lease.Player, api);
        CameraCaps.Require(CameraCaps.Service);
        var (key, pending) = Register();
        var req = Prepared(built);
        int rc;
        fixed (CameraNative.Key* keys = built.Keys)
        {
            req.Keys = built.Keys.Length == 0 ? null : keys;
            rc = CameraNative.T->Update(owner, lease.Value, (uint)timing, &req, &OnDone, (void*)key);
        }
        return Finish(rc, key, pending, api, lease.Endpoint, lease.Player, built.Parts);
    }

    /// <summary>Ends a lease of the calling mod. Works while a capability is off. A request of the lease that still
    /// waits is cancelled first. Ending a lease that has ended is Ok with <see cref="CameraResult.AlreadyEnded"/>.
    /// The task completes when the camera was let go (after a blend out, if the request had one).</summary>
    /// <exception cref="ArgumentException"><paramref name="lease"/> is none.</exception>
    /// <exception cref="InvalidOperationException">Not on the thread of the lease's world, inside a program wrap's
    /// handler, or outside mod code.</exception>
    [TickThreadOnly, RequiresCapability("camera.service")]
    public static Task<CameraResult> End(CameraLease lease)
    {
        const string api = "Camera.End";
        RequireLease(lease, api);
        RequireLeaseThread(lease, api);
        var owner = RequireOwner(api);
        CameraCaps.Require(CameraCaps.Service);
        var (key, pending) = Register();
        var rc = CameraNative.T->End(owner, lease.Value, &OnDone, (void*)key);
        return Finish(rc, key, pending, api, lease.Endpoint, lease.Player, 0);
    }

    /// <summary>What the camera service can do for <paramref name="endpoint"/>. A bit says the capability exists, not
    /// that a player or a frame is there now. <see cref="CameraEndpoint.Remote"/> with a player: what that player's
    /// client offered.</summary>
    /// <exception cref="ArgumentException"><paramref name="endpoint"/> is not an endpoint.</exception>
    [AnyThread, RequiresCapability("camera.service")]
    public static CameraCapability GetCapabilities(CameraEndpoint endpoint, EntityId player = default)
    {
        if (!Enum.IsDefined(endpoint)) throw new ArgumentException($"Camera.GetCapabilities: {endpoint} is not an endpoint", nameof(endpoint));
        CameraCaps.Require(CameraCaps.Service);
        ulong mask = 0;
        return CameraNative.T->Capabilities((uint)endpoint, player.Value, &mask) == CameraNative.Ok ? (CameraCapability)mask : CameraCapability.None;
    }

    /// <summary>The camera the newest frame was drawn with. Not Ok when there is none to give: no frame yet, a
    /// snapshot older than 250 ms, or one being written this instant.</summary>
    /// <exception cref="KilnUnavailableException"><c>camera.rendered_snapshot</c> is off.</exception>
    [ClientOnly, AnyThread, RequiresCapability("camera.rendered_snapshot")]
    public static CameraVisualResult GetVisual()
    {
        CameraCaps.Require(CameraCaps.RenderedSnapshot);
        var v = new CameraNative.Visual { Size = (uint)sizeof(CameraNative.Visual) };
        uint reason = 0;
        switch (CameraNative.T->GetVisual(&v, &reason))
        {
            case CameraNative.Ok: break;
            case CameraNative.Unavailable: throw CameraCaps.Off(CameraCaps.RenderedSnapshot);
            case var rc: throw new InvalidOperationException($"Camera.GetVisual: kiln.camera failed ({rc})");
        }
        if (v.Valid == 0)
        {
            var code = reason == 0 ? CameraReason.NoCameraFrame : (CameraReason)reason;
            return new CameraVisualResult(false, code.ToString(), code, null);
        }
        var pose = new CameraPose(new WorldPosition(v.Pose.X, v.Pose.Y, v.Pose.Z), new System.Numerics.Quaternion(v.Pose.Qx, v.Pose.Qy, v.Pose.Qz, v.Pose.Qw));
        var age = TimeSpan.FromTicks(Math.Max(0, (MonotonicNs() - (long)v.CapturedNs) / 100));
        return new CameraVisualResult(true, null, CameraReason.None,
            new CameraVisualSnapshot(pose, v.FovRadians * (180f / MathF.PI), v.Aspect, v.Near, v.Far, v.Frame, v.TargetSampleStep, v.Generation, age));
    }

    /// <summary>Every change of every camera lease: applied, updated, ended (with the reason). Raised in the step of
    /// the lease's world, after the task of the request that caused it completed; when a world stops, its leases end
    /// with <see cref="CameraEndReason.WorldStopped"/> on the thread that tears the world down, so a handler must not
    /// touch the game for that reason. A handler that throws is logged. Handlers of a mod go with the mod.</summary>
    [AnyThread]
    public static event Action<CameraChange>? Changed
    {
        add
        {
            if (value is null) return;
            var mod = ModContext.CurrentMod;
            if (mod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
            lock (Lock) _handlers = [.. _handlers, new Handler(value, mod)];
            Watch(mod);
            EnsureSubscribed();
        }
        remove
        {
            if (value is null) return;
            lock (Lock)
            {
                var i = Array.FindLastIndex(_handlers, h => h.Fn == value);
                if (i >= 0) _handlers = [.. _handlers[..i], .. _handlers[(i + 1)..]];
            }
        }
    }

    // ---- requests ----

    private static Task<CameraResult> Apply(string api, CameraOverride request, CameraEndpoint endpoint, EntityId player)
    {
        var owner = RequireOwner(api);
        var built = CameraRequests.Build(request, endpoint, player, api);
        foreach (var row in CameraCaps.RowsOf(endpoint, built.Parts)) CameraCaps.Require(row);
        CameraCaps.Require(CameraCaps.Service);
        var (key, pending) = Register();
        var req = Prepared(built);
        ulong lease = 0;
        int rc;
        fixed (CameraNative.Key* keys = built.Keys)
        {
            req.Keys = built.Keys.Length == 0 ? null : keys;
            rc = CameraNative.T->Apply(owner, &req, &lease, &OnDone, (void*)key);
        }
        return Finish(rc, key, pending, api, endpoint, player, built.Parts);
    }

    private static CameraNative.Request Prepared(in CameraBuilt built)
    {
        var req = built.Request;
        req.Size = (uint)sizeof(CameraNative.Request);
        req.Profile.Size = (uint)sizeof(CameraNative.Profile);
        req.KeyCount = (uint)built.Keys.Length;
        return req;
    }

    private static (nint Key, Pending Pending) Register()
    {
        var mod = ModContext.CurrentMod;
        if (mod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
        var pending = new Pending(mod);
        var key = (nint)Interlocked.Increment(ref _nextKey);
        Waiting[key] = pending;
        Watch(mod);
        return (key, pending);
    }

    // The service's immediate answer. KILN_OK: the task waits for `done`. Everything else: no callback will come.
    private static Task<CameraResult> Finish(int rc, nint key, Pending pending, string api, CameraEndpoint endpoint, EntityId player, uint parts)
    {
        if (rc == CameraNative.Ok) return pending.Tcs.Task;
        Waiting.TryRemove(key, out _);
        switch (rc)
        {
            case CameraNative.InvalidArgument:
                throw new ArgumentException($"{api}: kiln.camera refused the request as malformed");
            case CameraNative.WrongThread:
                throw new InvalidOperationException(endpoint == CameraEndpoint.Local
                    ? $"{api} must be called on the client world's thread (Client.Tick, Client.Post)"
                    : $"{api} must be called on the tick thread (use Scheduler.Post)");
            case CameraNative.Unavailable:
            {
                // name a row that is off now: one of the request's parts, or for an end every row of the lease's endpoint;
                // when none reads off (the service turned it away for a reason no row shows), the service row
                throw CameraCaps.OffOneOf(CameraCaps.RowsOf(endpoint, parts != 0 ? parts : CameraNative.PartPath | CameraNative.PartProfile | CameraNative.PartLens | CameraNative.PartShake));
            }
            case CameraNative.NotFound:
                // no world, or no such player in it: no step would ever bring an answer
                return Task.FromResult(endpoint == CameraEndpoint.Local
                    ? Refused(CameraReason.NoCameraFrame, "no client world with a local player is running", endpoint, player)
                    : Refused(CameraReason.PeerUnavailable, "the player is not in a running server world", endpoint, player));
            case CameraNative.BufferTooSmall:
                return Task.FromResult(Refused(CameraReason.RateLimit, "too many camera requests wait already: ask again in a later step", endpoint, player));
            case CameraNative.Revoked:
                throw new InvalidOperationException("the mod is unloading");
            default:
                throw new InvalidOperationException($"{api}: kiln.camera failed ({rc})");
        }
    }

    private static CameraResult Refused(CameraReason code, string text, CameraEndpoint endpoint, EntityId player) =>
        new(false, text, code, new CameraLease(0, endpoint, player, 0), 0, CameraCommitScope.None);

    // A lease belongs to a mod: kiln.camera refuses owner 0 (no mod), which never retires and which every caller
    // outside mod code would share.
    private static ulong RequireOwner(string api) =>
        Services.CallerOwner is var owner and not 0 ? owner : throw new InvalidOperationException($"{api}: call it from mod code (a camera lease belongs to a mod)");

    private static EntityId RequirePlayer(EntityId player, string api) =>
        player.IsNone ? throw new ArgumentException($"{api}: a player is required", nameof(player)) : player;

    private static void RequireLease(CameraLease lease, string api)
    {
        if (lease.Value == 0) throw new ArgumentException($"{api}: the lease is none", nameof(lease));
        if (!Enum.IsDefined(lease.Endpoint)) throw new ArgumentException($"{api}: the lease's endpoint {lease.Endpoint} is not an endpoint", nameof(lease));
    }

    // A server lease (mode, remote) is the tick thread's, as SetMode and Set are. A local lease is the client world's:
    // kiln.camera checks that thread; here only the program wrap's handler is refused, which runs inside a step even on
    // the world's own thread (and may cancel a write its update holds).
    private static void RequireLeaseThread(CameraLease lease, string api)
    {
        if (lease.Endpoint != CameraEndpoint.Local)
            Services.RequireTickThread(api);
        else if (SliceGuard.Depth > 0)
            throw new InvalidOperationException($"{api} cannot be called from a program wrap's handler (it runs inside the world's step, maybe on a worker); use slice.Post");
    }

    // QueryPerformanceCounter as nanoseconds: the clock kiln.camera stamps a snapshot with.
    private static long MonotonicNs() => (long)(System.Diagnostics.Stopwatch.GetTimestamp() * (1e9 / System.Diagnostics.Stopwatch.Frequency));

    // ---- callbacks from the service ----

    // On the lease's world thread in its Pre; for a world that stops, on the thread that destroys it. The task's
    // continuations never run in here (RunContinuationsAsynchronously): they go where the awaiting code asked.
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnDone(CameraNative.Result* r, void* user)
    {
        try
        {
            if (r == null || !Waiting.TryRemove((nint)user, out var pending)) return;  // its mod went: nobody waits
            var ok = r->Ok != 0;
            var code = (CameraReason)r->ReasonCode;
            var text = ok ? null : r->Reason.ToString() is { Length: > 0 } words ? words : code.ToString();
            pending.Tcs.TrySetResult(new CameraResult(ok, text, code,
                new CameraLease(r->Lease, (CameraEndpoint)r->Endpoint, new EntityId(r->Player), r->Generation), r->Revision, (CameraCommitScope)r->CommitScope,
                (r->Flags & CameraNative.MayHaveApplied) != 0, (r->Flags & CameraNative.AlreadyEnded) != 0));
        }
        catch
        {
            // never unwind into the game
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnChanged(CameraNative.Change* c, void* user)
    {
        var outer = ModContext.CurrentMod;
        try
        {
            var handlers = Volatile.Read(ref _handlers);
            if (c == null || handlers.Length == 0) return;
            var text = c->Reason.ToString();
            var change = new CameraChange(new CameraLease(c->Lease, (CameraEndpoint)c->Endpoint, new EntityId(c->Player), c->Generation), c->Revision,
                (CameraChangeKind)c->Kind, (CameraEndReason)c->EndReason, (CameraReason)c->ReasonCode, text.Length > 0 ? text : null, c->Frame);
            foreach (var h in handlers)
            {
                if (h.Mod is { Alive: false }) continue;
                ModContext.CurrentMod = h.Mod;
                try
                {
                    h.Fn(change);
                }
                catch (Exception e)
                {
                    Report(h.Mod, $"Camera.Changed handler {h.Fn.Method.DeclaringType?.Name}.{h.Fn.Method.Name} threw", e);
                }
            }
        }
        catch (Exception e)
        {
            Report(null, "Camera.Changed dispatch failed", e);
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    private static void EnsureSubscribed()
    {
        if (Volatile.Read(ref _subscribed) != 0 || CameraNative.T == null) return;
        lock (Lock)
        {
            if (_subscribed != 0) return;
            ulong handle = 0;
            // owner 0: the handler is Kiln.Camera's own code, which stays loaded; each mod's handlers go in Forget
            if (CameraNative.T->Subscribe(0, &OnChanged, null, &handle) == CameraNative.Ok) _subscribed = 1;
        }
    }

    // ---- mods that go ----

    private static void Watch(ModContext? mod)
    {
        if (mod is null) return;
        lock (Lock)
            if (!Watched.Add(mod)) return;
        mod.Retiring += () => Forget(mod);
    }

    // The mod's handlers are removed and its waiting tasks cancelled: the service calls nothing of a retired owner.
    private static void Forget(ModContext mod)
    {
        lock (Lock)
        {
            _handlers = Array.FindAll(_handlers, h => !ReferenceEquals(h.Mod, mod));
            Watched.Remove(mod);
        }
        foreach (var (key, pending) in Waiting)
            if (ReferenceEquals(pending.Mod, mod) && Waiting.TryRemove(key, out _)) pending.Tcs.TrySetCanceled();
    }

    private static void Report(ModContext? mod, string what, Exception e)
    {
        try
        {
            if (mod != null) mod.Log.Error(e, what);
            else Backend.Current.Log(0, LogLevel.Error, $"{what}: {e}");
        }
        catch
        {
            // logging must never unwind into the game
        }
    }
}
