// SPDX-License-Identifier: MIT
// The fake kiln.routines v1 table (native/core/abi/services/kiln_routines.h) under the real Kiln.Blocks (Routines, the two
// routine events). Its functions answer from the running harness's RoutinesModel (RoutinesFake.cs), and as the native
// service does when it is unavailable otherwise.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln.TestKit;

/// <summary>Mirror of <c>kiln_routines.h</c> (v1). Sizes are pinned by the kit's tests to the header's.</summary>
internal static unsafe class RoutinesAbi
{
    public const string Service = "kiln.routines";
    public const string Decisions = "routines.decisions";
    public const uint Version = 1;
    public const int MaxSubscribers = 8;
    // for the kit's tests, which cannot name Kiln.Api's NativeStr: the sizes the header's mirror has
    public static readonly int ResultSize = sizeof(Result), TableSize = sizeof(Table);

    // KilnRoutineResult.reason_code
    public const uint CodeOk = 0, CodeOwnedBy = 1, CodeWorldStopped = 2, CodeRevoked = 3, CodeNotAssigned = 4;

    [StructLayout(LayoutKind.Sequential)]
    public struct Result
    {
        public uint Size, Ok;
        public ulong Ticket, Revision;
        public uint NpcId, ReasonCode;
        public NativeStr Reason;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, uint, NativeStr, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, ulong*, int> Assign;
        public delegate* unmanaged[Cdecl]<ulong, uint, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, ulong*, int> Remove;
        public delegate* unmanaged[Cdecl]<ulong, uint, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, ulong*, int> Resume;
        public delegate* unmanaged[Cdecl]<uint, byte*, nuint, nuint*, int> Inspect;
        public delegate* unmanaged[Cdecl]<ulong, void> RevokeOwner;
        public delegate* unmanaged[Cdecl]<ulong, delegate* unmanaged[Cdecl]<NativeStr, void*, void>, void*, ulong*, int> SubscribeEvents;
        public delegate* unmanaged[Cdecl]<ulong, int> UnsubscribeEvents;
    }

    /// <summary>The result code of a completion's reason, as the native service sets it.</summary>
    public static uint CodeOf(string? reason) => reason switch
    {
        null or "" => CodeOk,
        RoutinesModel.WorldStoppedReason => CodeWorldStopped,
        RoutinesModel.Revoked => CodeRevoked,
        RoutinesModel.NotAssigned => CodeNotAssigned,
        _ when reason.StartsWith(RoutinesModel.OwnedBy, StringComparison.Ordinal) => CodeOwnedBy,
        _ => CodeOk,
    };
}

/// <summary>The C# functions behind the fake <c>kiln.routines</c> table. Modelled: every function of v1, and the two
/// event functions appended after it. The event subscriptions are the process's, as Kiln.Blocks subscribes once per
/// table; completions and events of a harness go to them from its steps.</summary>
internal static unsafe class RoutinesTable
{
    private static RoutinesAbi.Table* _table;

    private sealed record Subscriber(ulong Handle, ulong Owner, nint Fn, nint User);

    private static readonly object SubscribersLock = new();
    private static readonly List<Subscriber> Subscribers = [];
    private static ulong _nextSubscription;

    public static nint Create()
    {
        if (_table != null) return (nint)_table;
        var t = (RoutinesAbi.Table*)NativeMemory.AllocZeroed((nuint)sizeof(RoutinesAbi.Table));  // process lifetime
        t->Header = new ServiceHeader { Size = (uint)sizeof(RoutinesAbi.Table), Version = RoutinesAbi.Version };
        t->Assign = &Assign;
        t->Remove = &Remove;
        t->Resume = &Resume;
        t->Inspect = &Inspect;
        t->RevokeOwner = &RevokeOwner;
        t->SubscribeEvents = &SubscribeEvents;
        t->UnsubscribeEvents = &UnsubscribeEvents;
        _table = t;
        return (nint)t;
    }

    // the state of the harness that runs now; null: answer as the unavailable service
    private static RoutinesState? State => FakeTables.Active(RoutinesAbi.Service) is { } h ? RoutinesFake.StateOf(h) : null;

    // What a change needs before it is queued, as the native service's ready(): the tick thread, the capability, a world.
    private static int Ready(out RoutinesState? state)
    {
        state = State;
        if (!FakeTables.OnTickThread) return FakeTables.WrongThread;
        if (state is null || FakeTables.Off(RoutinesAbi.Decisions)) return FakeTables.Unavailable;
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Assign(ulong owner, uint npcId, NativeStr plan, delegate* unmanaged[Cdecl]<RoutinesAbi.Result*, void*, void> done, void* user,
        ulong* ticket)
    {
        try
        {
            if (ticket != null) *ticket = 0;
            var rc = Ready(out var state);
            if (rc != FakeTables.Ok) return rc;
            if (plan.Ptr == null) return FakeTables.InvalidArgument;
            return state!.Assign(owner, npcId, plan.ToString(), (nint)done, (nint)user, ticket);
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Remove(ulong owner, uint npcId, delegate* unmanaged[Cdecl]<RoutinesAbi.Result*, void*, void> done, void* user, ulong* ticket)
    {
        try
        {
            if (ticket != null) *ticket = 0;
            var rc = Ready(out var state);
            return rc != FakeTables.Ok ? rc : state!.Change(remove: true, owner, npcId, (nint)done, (nint)user, ticket);
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Resume(ulong owner, uint npcId, delegate* unmanaged[Cdecl]<RoutinesAbi.Result*, void*, void> done, void* user, ulong* ticket)
    {
        try
        {
            if (ticket != null) *ticket = 0;
            var rc = Ready(out var state);
            return rc != FakeTables.Ok ? rc : state!.Change(remove: false, owner, npcId, (nint)done, (nint)user, ticket);
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Inspect(uint npcId, byte* json, nuint cap, nuint* outLen)
    {
        try
        {
            if (outLen == null || (cap != 0 && json == null)) return FakeTables.InvalidArgument;
            *outLen = 0;
            if (!FakeTables.OnTickThread) return FakeTables.WrongThread;
            if (State is not { } state) return FakeTables.Unavailable;
            if (state.SnapshotJson(npcId) is not { } text) return FakeTables.NotFound;
            var bytes = Encoding.UTF8.GetBytes(text);
            *outLen = (nuint)bytes.Length;
            if ((nuint)bytes.Length > cap) return FakeTables.BufferTooSmall;
            bytes.CopyTo(new Span<byte>(json, bytes.Length));
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    // any thread; it takes effect at the next Pre, and the owner's event subscriptions go with it (as native)
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void RevokeOwner(ulong owner)
    {
        try
        {
            if (owner == 0) return;
            State?.Revoke(owner);
            lock (SubscribersLock) Subscribers.RemoveAll(s => s.Owner == owner);
        }
        catch
        {
            // a void function of the table: nothing to answer
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SubscribeEvents(ulong owner, delegate* unmanaged[Cdecl]<NativeStr, void*, void> fn, void* user, ulong* handle)
    {
        if (owner == 0 || fn == null || handle == null) return FakeTables.InvalidArgument;
        *handle = 0;
        if (!FakeTables.OnTickThread) return FakeTables.WrongThread;
        lock (SubscribersLock)
        {
            if (Subscribers.Count >= RoutinesAbi.MaxSubscribers) return FakeTables.Unavailable;
            *handle = ++_nextSubscription;
            Subscribers.Add(new Subscriber(*handle, owner, (nint)fn, (nint)user));
        }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int UnsubscribeEvents(ulong handle)
    {
        if (!FakeTables.OnTickThread) return FakeTables.WrongThread;
        lock (SubscribersLock) return Subscribers.RemoveAll(s => s.Handle == handle) > 0 ? FakeTables.Ok : FakeTables.NotFound;
    }

    // ---- what the fake sends (tick thread, no lock held) ----

    /// <summary>Calls one change's <c>done</c> with its result, as the native service does on Post.</summary>
    public static void Complete(nint done, nint user, FakeRoutineCompletion c)
    {
        var reason = Encoding.UTF8.GetBytes(c.Reason ?? "");
        fixed (byte* p = reason)
        {
            var r = new RoutinesAbi.Result
            {
                Size = (uint)sizeof(RoutinesAbi.Result), Ok = c.Ok ? 1u : 0u, Ticket = c.Ticket, Revision = c.Revision, NpcId = c.NpcId,
                ReasonCode = RoutinesAbi.CodeOf(c.Reason), Reason = new NativeStr { Ptr = reason.Length == 0 ? null : p, Len = (nuint)reason.Length },
            };
            ((delegate* unmanaged[Cdecl]<RoutinesAbi.Result*, void*, void>)done)(&r, (void*)user);
        }
    }

    /// <summary>Sends one event document to every subscriber.</summary>
    public static void Send(string json)
    {
        Subscriber[] subs;
        lock (SubscribersLock) subs = [.. Subscribers];
        if (subs.Length == 0) return;
        var bytes = Encoding.UTF8.GetBytes(json);
        fixed (byte* p = bytes)
        {
            var s = new NativeStr { Ptr = p, Len = (nuint)bytes.Length };
            foreach (var sub in subs) ((delegate* unmanaged[Cdecl]<NativeStr, void*, void>)sub.Fn)(s, (void*)sub.User);
        }
    }

    /// <summary>How many event subscriptions the process holds (for the kit's tests).</summary>
    public static int SubscriberCount
    {
        get
        {
            lock (SubscribersLock) return Subscribers.Count;
        }
    }
}
