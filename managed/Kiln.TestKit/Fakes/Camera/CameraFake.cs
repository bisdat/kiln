// SPDX-License-Identifier: MIT
// The fake kiln.camera (native/core/abi/services/kiln_camera.h, v1) under the real Kiln.Camera: the lease rules of the
// service (native/services/camera/state.cpp) without a backend. A request is applied the step after it was made.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

/// <summary>Mirror of <c>kiln_camera.h</c>. The sizes are pinned by tests to Kiln.Camera's private mirrors.</summary>
internal static unsafe class CameraAbi
{
    public const string Service = "kiln.camera";
    public const uint Version = 1;
    public const uint ServerMode = 1, Local = 2, Remote = 3;
    public const uint PartMode = 1, PartPath = 2, PartProfile = 4, PartShake = 8, PartLens = 16;
    public const uint Applied = 1, Updated = 2, Ended = 3;
    public const uint EndReleased = 1, EndExpired = 2, EndWorldStopped = 5, EndSuperseded = 6, EndOwnerRetired = 8, EndCancelledBeforeApply = 13;
    public const uint StaleWorld = 12;
    public const uint Busy = 1, AlreadyOwned = 2, UnknownLease = 3, ForeignOwner = 4, WrongEndpoint = 5, InputPolicy = 6, LeaseKind = 23;
    public const uint RequiresKilnClient = 7, PeerUnavailable = 8, ClientCapability = 25;
    public const ulong CapRemote = 1ul << 6, LocalParts = (1ul << 1) | (1ul << 2) | (1ul << 3) | (1ul << 4);
    public const uint FlagAlreadyEnded = 2;
    public const int Revoked = 6;

    [StructLayout(LayoutKind.Sequential, Size = 128)]
    public struct Request
    {
        public uint Size, Parts, Endpoint, Player;
        public uint InputPolicy, DurationMs, BlendInMs, BlendOutMs;
        public int Priority;
        public uint LensSource;
        public float FovDegrees;
        public byte CameraId, Transition, Reserved0, Reserved1;
        // the profile (52 bytes), the shake (16), the key count and the keys follow: the fake reads none of them
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Result
    {
        public uint Size, Ok, ReasonCode, CommitScope, Flags, Endpoint, Player, Generation;
        public ulong Lease, Revision, Frame;
        public NativeStr Reason;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Change
    {
        public uint Size, Kind, Endpoint, Player, Generation, EndReason, ReasonCode, Reserved;
        public ulong Lease, Revision, Frame;
        public NativeStr Reason;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<uint, uint, ulong*, int> Capabilities;
        public delegate* unmanaged[Cdecl]<ulong, Request*, ulong*, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, int> Apply;
        public delegate* unmanaged[Cdecl]<ulong, ulong, uint, Request*, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, int> Update;
        public delegate* unmanaged[Cdecl]<ulong, ulong, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, int> End;
        public delegate* unmanaged[Cdecl]<ulong, delegate* unmanaged[Cdecl]<Change*, void*, void>, void*, ulong*, int> Subscribe;
        public delegate* unmanaged[Cdecl]<ulong, int> Unsubscribe;
        public delegate* unmanaged[Cdecl]<void*, uint*, int> GetVisual;
        public delegate* unmanaged[Cdecl]<ulong, void*, int> BindTransport;
        public delegate* unmanaged[Cdecl]<ulong, int> UnbindTransport;
        public delegate* unmanaged[Cdecl]<ulong, ulong, ulong, uint, uint, NativeStr, int> TransportEnded;
    }
}

internal sealed class FakeCameraLease
{
    public ulong Id, Owner, Revision = 1;
    public uint Endpoint, Player;
    public int Priority;
    public bool Applied;   // its apply was answered Ok
    public bool Stopped;   // the world stopped while it lived (HarnessCamera.StopWorld)
}

/// <summary>One harness's kiln.camera: the leases, what waits for the next step, and the record.</summary>
internal sealed class CameraFakeState
{
    public readonly object Lock = new();
    public readonly Dictionary<ulong, FakeCameraLease> Leases = [];
    public readonly Dictionary<ulong, ulong> Ended = [];   // lease -> its owner
    public readonly List<Action> Queue = [];               // answers and events of the next step, in order
    public readonly List<FakeCameraCall> Calls = [];
    public readonly HashSet<(uint Endpoint, uint Player)> InFlight = [];  // an apply or update waits for the next step there
    public readonly Queue<uint> Refusals = new();          // HarnessCamera.RefuseNext: the next accepted requests are refused so
    public readonly Dictionary<uint, ulong> KilnClients = [];  // HarnessCamera.KilnClient: player -> the local parts its client offers
}

internal sealed unsafe class CameraFake : IKitFake
{
    // the rows the fake serves (a test can switch one off), and the ones it does not model
    public const string ServiceRow = "camera.service", ModeRow = "camera.server_mode", RemoteRow = "camera.remote";
    public static readonly string[] LocalRows = ["camera.local_pose", "camera.local_fov", "camera.local_profile", "camera.local_shake"];
    public static readonly string[] OfflineRows = ["camera.rendered_snapshot", "camera.resource_profile"];

    public string Service => CameraAbi.Service;

    private static CameraAbi.Table* _table;
    // process-wide, as in the service: Kiln.Camera subscribes once, and a lease number is never used twice
    private static readonly object Shared = new();
    private static readonly List<(ulong Handle, ulong Owner, nint Fn, nint User)> Subscribers = [];
    private static ulong _nextLease, _nextHandle;

    public void Register()
    {
        if (_table == null)
        {
            var t = (CameraAbi.Table*)NativeMemory.AllocZeroed((nuint)sizeof(CameraAbi.Table));  // process lifetime, never freed
            t->Header = new ServiceHeader { Size = (uint)sizeof(CameraAbi.Table), Version = CameraAbi.Version };
            t->Capabilities = &Capabilities;
            t->Apply = &Apply;
            t->Update = &Update;
            t->End = &End;
            t->Subscribe = &Subscribe;
            t->Unsubscribe = &Unsubscribe;
            t->GetVisual = &GetVisual;
            t->BindTransport = &BindTransport;
            t->UnbindTransport = &UnbindTransport;
            t->TransportEnded = &TransportEnded;
            _table = t;
        }
        KitBackend.Instance.RegisterTable(CameraAbi.Service, (nint)_table);
        FakeTables.Features([ServiceRow, ModeRow, RemoteRow, .. LocalRows]);
        foreach (var row in OfflineRows) FakeTables.Offline(row, "not modelled by the test kit's fake of kiln.camera");
    }

    public void Start(KilnHarness harness) { }

    // where the service's Pre would run: the answers and events of the calls made since the last step
    public void BeforeStep(KilnHarness harness, ulong step)
    {
        var state = KitFakes.State<CameraFakeState>(harness);
        Action[] due;
        lock (state.Lock)
        {
            // as the service's Pre: the leases of an owner that retired end OwnerRetired (other mods hear it, it does not)
            foreach (var gone in state.Leases.Values.Where(l => harness.CameraOwnerRevoked(l.Owner)).ToList())
            {
                state.Leases.Remove(gone.Id);
                state.Ended[gone.Id] = gone.Owner;
                state.Queue.Add(() => { if (gone.Applied) Raise(gone, CameraAbi.Ended, CameraAbi.EndOwnerRetired); });
            }
            due = [.. state.Queue];
            state.Queue.Clear();
            state.InFlight.Clear();
        }
        foreach (var deliver in due) deliver();
    }

    public void End(KilnHarness harness)
    {
        var state = KitFakes.State<CameraFakeState>(harness);
        lock (state.Lock)
        {
            state.Queue.Clear();
            state.Leases.Clear();
            state.InFlight.Clear();
        }
    }

    public void AddCoverage(List<CoverageRow> rows) =>
        rows.Add(new CoverageRow("Camera", CoverageKind.RecordingFake, [CameraAbi.Service, ServiceRow, ModeRow, RemoteRow, .. LocalRows],
            "Kiln.Camera's own code on the kit's kiln.camera. The lease rules are the service's: one lease per player and endpoint, AlreadyOwned for the same mod, " +
            "Busy unless another mod's priority is higher (the old lease ends superseded before the new one is applied), update and end with their refusals, " +
            "answers and Camera.Changed one step after the call; calls outside mod code (owner 0) are refused, and a mod that unloads has its applied leases end " +
            "OwnerRetired. There is no picture and no clock offline: a request counts as applied the step after it was made, and a lease ends only when the mod " +
            "ends it or the test says so (KilnHarness.Camera.Expire, End, StopWorld; RefuseNext scripts a refusal); blends do nothing. A server's visual request for a " +
            "player's client (camera.remote) reaches only a player the test made a Kiln client (KilnHarness.Camera.KilnClient, with the local parts it offers): " +
            "another joined player is refused RequiresKilnClient, a player nobody joined PeerUnavailable, a missing part ClientCapability; there is no wire, ACK or timeout. Not modelled (the row reads offline): the frame " +
            "snapshot (Camera.GetVisual)."));

    public static bool EndLease(CameraFakeState state, ulong lease, uint endReason)
    {
        lock (state.Lock)
        {
            if (!state.Leases.TryGetValue(lease, out var l) || !l.Applied) return false;  // only an applied lease ends by itself
            state.Leases.Remove(lease);
            state.Ended[lease] = l.Owner;
            state.Queue.Add(() => Raise(l, CameraAbi.Ended, endReason));
            return true;
        }
    }

    // The world stops: an applied lease ends WorldStopped, a waiting apply or update is answered StaleWorld.
    public static int StopWorld(CameraFakeState state)
    {
        lock (state.Lock)
        {
            var all = state.Leases.Values.ToList();
            foreach (var l in all)
            {
                state.Leases.Remove(l.Id);
                state.Ended[l.Id] = l.Owner;
                l.Stopped = true;
                state.Queue.Add(() => { if (l.Applied) Raise(l, CameraAbi.Ended, CameraAbi.EndWorldStopped); });
            }
            return all.Count;
        }
    }

    // ---- deliveries ----

    // `revision`: the lease's revision in force for a refused update of its owner (the service reports it; 0 otherwise)
    private static void Answer(nint done, nint user, FakeCameraLease lease, ulong id, bool ok, uint reason, uint flags = 0, ulong? revision = null)
    {
        if (done == 0) return;
        var text = ok ? default : Str(reason);
        var r = new CameraAbi.Result
        {
            Size = (uint)sizeof(CameraAbi.Result), Ok = ok ? 1u : 0u, ReasonCode = reason, Flags = flags, Lease = id, Reason = text,
            CommitScope = !ok || flags != 0 ? 0u : lease.Endpoint == CameraAbi.ServerMode ? 1u : 2u,
            Endpoint = lease.Endpoint, Player = lease.Player, Generation = 1, Revision = revision ?? (ok && flags == 0 ? lease.Revision : 0),
        };
        ((delegate* unmanaged[Cdecl]<CameraAbi.Result*, void*, void>)done)(&r, (void*)user);
    }

    private static void Raise(FakeCameraLease lease, uint kind, uint endReason = 0)
    {
        (ulong, ulong, nint, nint)[] subscribers;
        lock (Shared) subscribers = [.. Subscribers];
        var c = new CameraAbi.Change
        {
            Size = (uint)sizeof(CameraAbi.Change), Kind = kind, Endpoint = lease.Endpoint, Player = lease.Player, Generation = 1, EndReason = endReason,
            Lease = lease.Id, Revision = lease.Revision,
        };
        foreach (var (_, _, fn, user) in subscribers) ((delegate* unmanaged[Cdecl]<CameraAbi.Change*, void*, void>)fn)(&c, (void*)user);
    }

    private static readonly Dictionary<uint, NativeStr> Reasons = [];

    private static NativeStr Str(uint reason)
    {
        lock (Reasons)
        {
            if (Reasons.TryGetValue(reason, out var have)) return have;
            var bytes = System.Text.Encoding.UTF8.GetBytes($"refused by the test kit's kiln.camera (reason {reason})");
            var p = (byte*)NativeMemory.Alloc((nuint)bytes.Length);  // process lifetime, as the service's reason strings
            bytes.CopyTo(new Span<byte>(p, bytes.Length));
            return Reasons[reason] = new NativeStr { Ptr = p, Len = (nuint)bytes.Length };
        }
    }

    // ---- the table ----

    private static IEnumerable<string> RowsOf(uint endpoint, uint parts)
    {
        if (endpoint == CameraAbi.ServerMode) yield return ModeRow;
        else if (endpoint == CameraAbi.Remote) yield return "camera.remote";
        else
        {
            if ((parts & CameraAbi.PartPath) != 0) yield return LocalRows[0];
            if ((parts & CameraAbi.PartLens) != 0) yield return LocalRows[1];
            if ((parts & CameraAbi.PartProfile) != 0) yield return LocalRows[2];
            if ((parts & CameraAbi.PartShake) != 0) yield return LocalRows[3];
        }
    }

    private static bool On(string row) => KitBackend.Instance.Capabilities.Is(row);

    // A remote visual request: the player's client must run Kiln and offer every part (the bridge's answer). 0: it does. A
    // Kiln client the test named is connected; any other player must be one the test joined, else nobody is there.
    private static uint Recipient(KilnHarness harness, CameraFakeState state, CameraAbi.Request* req)
    {
        if (req->Endpoint != CameraAbi.Remote || (req->Parts & CameraAbi.PartMode) != 0) return 0;
        if (!state.KilnClients.TryGetValue(req->Player, out var offered))
            return harness.PlayersFake?.Present.Any(p => p.Entity.Value == req->Player) == true ? CameraAbi.RequiresKilnClient : CameraAbi.PeerUnavailable;
        ulong need = 0;
        if ((req->Parts & CameraAbi.PartPath) != 0) need |= 1ul << 1;
        if ((req->Parts & CameraAbi.PartLens) != 0) need |= 1ul << 2;
        if ((req->Parts & CameraAbi.PartProfile) != 0) need |= 1ul << 3;
        if ((req->Parts & CameraAbi.PartShake) != 0) need |= 1ul << 4;
        return (offered & need) == need ? 0 : CameraAbi.ClientCapability;
    }

    // The checks every request passes before it is queued, in the service's order. 0: go on.
    private static int Admit(CameraAbi.Request* req, out KilnHarness? harness)
    {
        harness = null;
        if (req == null || req->Size < (uint)sizeof(CameraAbi.Request) || req->Parts == 0 || (req->Parts & ~31u) != 0 || req->Endpoint is < 1 or > 3) return FakeTables.InvalidArgument;
        if (req->Endpoint != CameraAbi.Local && req->Player == 0) return FakeTables.InvalidArgument;
        if (!FakeTables.OnTickThread) return FakeTables.WrongThread;
        if (FakeTables.Active(CameraAbi.Service) is not { } h || !On(ServiceRow)) return FakeTables.Unavailable;
        var mode = (req->Parts & CameraAbi.PartMode) != 0;
        if (mode == (req->Endpoint == CameraAbi.ServerMode) && RowsOf(req->Endpoint, req->Parts).Any(row => !On(row))) return FakeTables.Unavailable;
        if (h.EntitiesFake is not { Started: true }) return FakeTables.NotFound;
        harness = h;
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Capabilities(uint endpoint, uint player, ulong* mask)
    {
        try
        {
            if (mask == null || endpoint is < 1 or > 3) return FakeTables.InvalidArgument;
            *mask = 0;
            if (FakeTables.Active(CameraAbi.Service) is null || !On(ServiceRow)) return FakeTables.Ok;
            if (endpoint == CameraAbi.ServerMode && On(ModeRow)) *mask = 1;
            if (endpoint == CameraAbi.Remote && On(RemoteRow))
            {
                *mask = CameraAbi.CapRemote;
                // with a player: the parts its client offered (as the transport bridge answers)
                if (player != 0 && FakeTables.Active(CameraAbi.Service) is { } h)
                {
                    var state = KitFakes.State<CameraFakeState>(h);
                    lock (state.Lock)
                        if (state.KilnClients.TryGetValue(player, out var parts)) *mask |= parts;
                }
            }
            if (endpoint == CameraAbi.Local)
                for (var i = 0; i < LocalRows.Length; i++)
                    if (On(LocalRows[i])) *mask |= 2ul << i;   // pose 2, fov 4, profile 8, shake 16
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Apply(ulong owner, CameraAbi.Request* req, ulong* outLease, delegate* unmanaged[Cdecl]<CameraAbi.Result*, void*, void> done, void* user)
    {
        try
        {
            if (outLease == null || owner == 0) return FakeTables.InvalidArgument;  // as kiln.camera: a lease belongs to a real owner
            if (Admit(req, out var harness) is var rc and not FakeTables.Ok) return rc;
            var state = KitFakes.State<CameraFakeState>(harness!);
            var lease = new FakeCameraLease { Owner = owner, Endpoint = req->Endpoint, Player = req->Endpoint == CameraAbi.Local ? 0 : req->Player, Priority = req->Priority };
            lock (Shared) lease.Id = ++_nextLease;
            *outLease = lease.Id;
            nint fn = (nint)done, u = (nint)user;
            lock (state.Lock)
            {
                var mode = (req->Parts & CameraAbi.PartMode) != 0;
                var holder = state.Leases.Values.FirstOrDefault(l => l.Endpoint == lease.Endpoint && l.Player == lease.Player);
                uint refusal = 0;
                if (mode != (req->Endpoint == CameraAbi.ServerMode)) refusal = CameraAbi.WrongEndpoint;
                else if (req->InputPolicy == 2) refusal = CameraAbi.InputPolicy;
                else if (holder is not null && holder.Owner == owner) refusal = CameraAbi.AlreadyOwned;
                else if (state.InFlight.Contains((lease.Endpoint, lease.Player))) refusal = CameraAbi.Busy;  // one apply or update per endpoint at a time
                else if (holder is not null && req->Priority <= holder.Priority) refusal = CameraAbi.Busy;
                else if (Recipient(harness!, state, req) is var recipient and not 0) refusal = recipient;
                else if (state.Refusals.Count > 0) refusal = state.Refusals.Dequeue();
                state.Calls.Add(new FakeCameraCall(harness!.CurrentStep, FakeCameraCallKind.Apply, lease.Id, req->Endpoint, new EntityId(lease.Player), req->Parts, req->DurationMs,
                    req->Priority, refusal));
                if (refusal != 0)
                {
                    state.Ended[lease.Id] = owner;
                    state.Queue.Add(() => Answer(fn, u, lease, lease.Id, false, refusal));
                    return FakeTables.Ok;
                }
                if (holder is not null)
                {
                    state.Leases.Remove(holder.Id);
                    state.Ended[holder.Id] = holder.Owner;
                    state.Queue.Add(() => Raise(holder, CameraAbi.Ended, CameraAbi.EndSuperseded));
                }
                state.Leases[lease.Id] = lease;
                state.InFlight.Add((lease.Endpoint, lease.Player));
                var h = harness;
                state.Queue.Add(() =>
                {
                    if (h.CameraOwnerRevoked(lease.Owner)) return;  // the service drops a retired owner's waiting lease without a word
                    if (lease.Stopped)
                    {
                        Answer(fn, u, lease, lease.Id, false, CameraAbi.StaleWorld);
                        return;
                    }
                    lease.Applied = true;
                    Answer(fn, u, lease, lease.Id, true, 0);
                    Raise(lease, CameraAbi.Applied);
                });
            }
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Update(ulong owner, ulong id, uint timing, CameraAbi.Request* req, delegate* unmanaged[Cdecl]<CameraAbi.Result*, void*, void> done, void* user)
    {
        try
        {
            if (id == 0 || owner == 0 || timing > 1) return FakeTables.InvalidArgument;
            if (Admit(req, out var harness) is var rc and not FakeTables.Ok) return rc;
            var state = KitFakes.State<CameraFakeState>(harness!);
            nint fn = (nint)done, u = (nint)user;
            lock (state.Lock)
            {
                state.Leases.TryGetValue(id, out var lease);
                var mode = (req->Parts & CameraAbi.PartMode) != 0;
                uint refusal = 0;
                if (lease is null) refusal = CameraAbi.UnknownLease;
                else if (lease.Owner != owner) refusal = CameraAbi.ForeignOwner;
                else if (req->Endpoint != lease.Endpoint || (req->Endpoint != CameraAbi.Local && req->Player != lease.Player)) refusal = CameraAbi.WrongEndpoint;
                else if (mode != (lease.Endpoint == CameraAbi.ServerMode)) refusal = CameraAbi.LeaseKind;
                else if (req->InputPolicy == 2) refusal = CameraAbi.InputPolicy;
                else if (state.InFlight.Contains((lease.Endpoint, lease.Player))) refusal = CameraAbi.Busy;
                else if (Recipient(harness!, state, req) is var recipient and not 0) refusal = recipient;
                else if (state.Refusals.Count > 0) refusal = state.Refusals.Dequeue();
                state.Calls.Add(new FakeCameraCall(harness!.CurrentStep, FakeCameraCallKind.Update, id, req->Endpoint, new EntityId(req->Player), req->Parts, req->DurationMs,
                    req->Priority, refusal));
                var about = lease is not null && lease.Owner == owner ? lease : new FakeCameraLease { Endpoint = req->Endpoint, Player = req->Player };
                if (refusal != 0)
                {
                    var keeps = lease is not null && lease.Owner == owner ? lease.Revision : 0;
                    state.Queue.Add(() => Answer(fn, u, about, id, false, refusal, revision: keeps));
                    return FakeTables.Ok;
                }
                state.InFlight.Add((lease!.Endpoint, lease.Player));
                var priority = req->Priority;
                state.Queue.Add(() =>
                {
                    if (lease.Stopped || !state.Leases.ContainsKey(lease.Id))
                    {
                        Answer(fn, u, lease, id, false, lease.Stopped ? CameraAbi.StaleWorld : 24u);  // the lease ended first: Cancelled
                        return;
                    }
                    lock (state.Lock)
                    {
                        lease.Revision++;
                        lease.Priority = priority;  // as the service: the new priority holds from the answer on
                    }
                    Answer(fn, u, lease, id, true, 0);
                    Raise(lease, CameraAbi.Updated);
                });
            }
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int End(ulong owner, ulong id, delegate* unmanaged[Cdecl]<CameraAbi.Result*, void*, void> done, void* user)
    {
        try
        {
            if (id == 0 || owner == 0) return FakeTables.InvalidArgument;
            if (!FakeTables.OnTickThread) return FakeTables.WrongThread;
            if (FakeTables.Active(CameraAbi.Service) is not { } harness || !On(ServiceRow)) return FakeTables.Unavailable;
            if (harness.EntitiesFake is not { Started: true }) return FakeTables.NotFound;
            var state = KitFakes.State<CameraFakeState>(harness);
            nint fn = (nint)done, u = (nint)user;
            lock (state.Lock)
            {
                var none = new FakeCameraLease();
                uint refusal = 0;
                if (state.Leases.TryGetValue(id, out var lease))
                {
                    if (lease.Owner != owner) refusal = CameraAbi.ForeignOwner;
                    else
                    {
                        state.Leases.Remove(id);
                        state.Ended[id] = owner;
                        state.Queue.Add(() => Answer(fn, u, lease, id, true, 0));
                        state.Queue.Add(() => Raise(lease, CameraAbi.Ended, CameraAbi.EndReleased));
                    }
                }
                else if (state.Ended.TryGetValue(id, out var was))
                {
                    if (was != owner) refusal = CameraAbi.ForeignOwner;
                    else state.Queue.Add(() => Answer(fn, u, none, id, true, 0, CameraAbi.FlagAlreadyEnded));
                }
                else refusal = CameraAbi.UnknownLease;
                if (refusal != 0) state.Queue.Add(() => Answer(fn, u, none, id, false, refusal));
                state.Calls.Add(new FakeCameraCall(harness.CurrentStep, FakeCameraCallKind.End, id, lease?.Endpoint ?? 0, new EntityId(lease?.Player ?? 0), 0, 0, 0, refusal));
            }
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Subscribe(ulong owner, delegate* unmanaged[Cdecl]<CameraAbi.Change*, void*, void> fn, void* user, ulong* handle)
    {
        if (fn == null || handle == null) return FakeTables.InvalidArgument;
        lock (Shared)
        {
            *handle = ++_nextHandle;
            Subscribers.Add((*handle, owner, (nint)fn, (nint)user));
        }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsubscribe(ulong handle)
    {
        lock (Shared) return Subscribers.RemoveAll(s => s.Handle == handle) > 0 ? FakeTables.Ok : FakeTables.NotFound;
    }

    // The frame snapshot and the transport bridge itself are not modelled (remote requests are answered by the fake).
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GetVisual(void* visual, uint* reason) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int BindTransport(ulong owner, void* table) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int UnbindTransport(ulong owner) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int TransportEnded(ulong owner, ulong lease, ulong revision, uint endReason, uint reasonCode, NativeStr reason) => FakeTables.Unavailable;
}
