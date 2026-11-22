// SPDX-License-Identifier: MIT
// The fake kiln.abilities table (native/core/abi/services/kiln_abilities.h, v1 with the dev.29 and dev.30.3 members) under the real
// Kiln.Abilities. Only its natives are modelled (06c: same-step natives, attachments, typed declarations, the ECS bridge);
// every other function answers as the native service does when it is unavailable.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

/// <summary>Mirror of <c>kiln_abilities.h</c>: the members the fake answers and the layouts they read or write. Sizes are
/// pinned by the kit's tests to the header's.</summary>
internal static unsafe class AbilitiesAbi
{
    public const string Service = "kiln.abilities";
    public const uint Version = 1;

    /// <summary>The sigdb row of the RegisterNative hook (the dedicated server's): the one game feature the fake serves.</summary>
    public const string NativesFeature = "abilities.natives";

    /// <summary>kiln.abilities' other sigdb rows: not modelled.</summary>
    public static readonly string[] OfflineFeatures = ["abilities.attributes", "abilities.instances", "abilities.programs", "abilities.sequences"];

    /// <summary>The ECS bridge's managed row (Kiln.Abilities reports it from the counters).</summary>
    public const string EcsRow = "abilities.natives.ecs";

    // KILN_NATIVE_KIND_*, _STATE_*, _ATTACH_*, _REFUSED_*, KILN_NATIVE_BEFORE/INSTEAD/AFTER, KILN_NATIVE_ARG_*
    public const uint KindVanilla = 0, KindWrapped = 1, KindSync = 4;
    public const uint StateNone = 0, StateWaiting = 1, StateLive = 2, StateRefused = 3, StateDead = 4;
    public const uint AttachGone = 0, AttachPending = 1, AttachActive = 2, AttachOff = 3;
    public const uint RefusedNone = 0, RefusedRegistryFull = 1, RefusedNoSlot = 2, RefusedTaken = 3, RefusedVanilla = 4,
        RefusedProgramNotLoaded = 5, RefusedSiteNotFound = 6, RefusedSecondInstead = 7, RefusedNotANative = 8;
    public const int Before = 0, Instead = 1, After = 2;
    public const byte ArgOptional = 1, ArgRaw = 2;
    public const uint CountersNatives = 1;
    public const uint EcsPending = 0, EcsOn = 1, EcsOff = 2;
    public const uint EcsAttribute = 1;

    // natives_core.h: the answers of a call no handler gave
    public const byte Undecided = 0xFF, ErrInternal = 0x11, ErrNoConnection = 0x18, ErrUnknownNative = 0x1F, ErrTypeMismatch = 0x2C;

    // KILN_IMPACT_TYPE_* and the other type hashes a typed declaration names (FNV-1a-32 of the impact type's name)
    public const uint TypeSint32 = 0x9859f31e, TypeUint32 = 0x32940bec, TypeFloat32 = 0xe89f7410, TypeBool = 0xc894953d,
        TypeTime = 0xdfe4e404, TypeEntityId = 0x737c8f86, TypeFloat3 = 0x3d1f8582;

    [StructLayout(LayoutKind.Sequential)]
    public struct Str
    {
        public byte* Ptr;
        public nuint Len;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Status
    {
        public uint Size;
        public byte Natives, Programs, Instances, Attributes;
        public uint VanillaNatives, NativeCapacity, KilnNatives, ProgramsTotal, ProgramsKiln, HeldNow, Interest;
        public ulong InstancesCreated, InstancesHeld, OverridesApplied, OverridesMismatched, Runs, NativeCalls,
            NativeCallsObserved, Signals, ManagedCalls, ManagedSuspends, EventsDelivered, EventsDropped, WrapCalls,
            ProgramsRefused;
        public ulong Step;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeInfo
    {
        public uint Size, Hash, Argc, Kind;
        public nuint Fn;
        public fixed byte Name[64];
    }

    /// <summary>KilnNativeCall: one call as a handler gets it (104 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct NativeCall
    {
        public uint Size, Hash, Argc, Reserved0;
        public uint* Args;
        public byte* Layout;
        public ulong LayoutCount;
        public byte* Data;
        public ulong DataSize;
        public uint ImpactId, ProgramId, Site, Self;
        public byte OutError, OutValue, OriginalCalled, Reserved1;
        public uint Reserved2;
        public void* Engine0, Engine1, Engine2;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeArg
    {
        public uint Type;
        public ushort Size;
        public byte Dir, Flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeFilter
    {
        public uint Size, ProgramId, Site, Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeCounters
    {
        public uint Size, Inflight, Flags, Reserved;
        public ulong SyncCalls, SyncFaults, SyncSlow, DeadCalls, AttachEntered, AttachSkipped, TypedRefused, TypedUnconnected;
        public ulong RetireDeferred, WatchdogReports;
        public uint RetirePending, AttachPending, AttachActive, AttachOff;
        public ulong LockBypass;
        public uint EcsState, EcsReason;
    }

    /// <summary>ImpactVariable (engine.h): one entry of a program's variable layout, 24 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Variable
    {
        public uint Name, ConfigId, Type;
        public ushort Size, Offset;
        public int DbgOff;
        public uint DbgLen;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<Status*, int> GetStatus;
        public delegate* unmanaged[Cdecl]<delegate* unmanaged[Cdecl]<void*, uint, void*, uint>, void*, int> SetListener;
        public delegate* unmanaged[Cdecl]<uint*, uint, int> HoldFilter;
        public delegate* unmanaged[Cdecl]<uint, void*, uint, uint*, int> InstanceVars;
        public delegate* unmanaged[Cdecl]<uint, uint, void*, uint, uint*, int> InstanceRead;
        public delegate* unmanaged[Cdecl]<uint, uint, void*, uint, int> InstanceWrite;
        public delegate* unmanaged[Cdecl]<uint, uint*, uint*, uint*, int> InstanceInfo;
        public delegate* unmanaged[Cdecl]<uint, void*, uint, uint, int> EntityOverrides;
        public delegate* unmanaged[Cdecl]<uint> ProgramCount;
        public delegate* unmanaged[Cdecl]<uint, void*, int> ProgramInfo;
        public delegate* unmanaged[Cdecl]<uint, void*, uint, uint*, int> ProgramVars;
        public delegate* unmanaged[Cdecl]<ulong, void*, nuint, uint*, byte*, nuint, int> ProgramRegister;
        public delegate* unmanaged[Cdecl]<uint*, int> ProgramRefresh;
        public delegate* unmanaged[Cdecl]<uint> NativeCount;
        public delegate* unmanaged[Cdecl]<uint, NativeInfo*, int> NativeAt;
        public delegate* unmanaged[Cdecl]<uint, NativeInfo*, int> NativeFind;
        public delegate* unmanaged[Cdecl]<uint, uint, int> NativeDeclare;
        public delegate* unmanaged[Cdecl]<ulong, Str, uint, uint*, int> NativeRegister;
        public delegate* unmanaged[Cdecl]<uint, int> NativeUnregister;
        public delegate* unmanaged[Cdecl]<ulong, byte, byte, int> NativeComplete;
        public delegate* unmanaged[Cdecl]<uint, uint, int> NativeWrap;
        public delegate* unmanaged[Cdecl]<uint, uint, int> NativeObserve;
        public delegate* unmanaged[Cdecl]<delegate* unmanaged[Cdecl]<NativeCall*, void*, void>, void*, int> SetWrapDispatcher;
        public delegate* unmanaged[Cdecl]<NativeCall*, void> CallOriginal;
        public delegate* unmanaged[Cdecl]<ulong, int> ReleaseOwner;
        public delegate* unmanaged[Cdecl]<uint, uint, uint, int*, int> AttrGet;
        public delegate* unmanaged[Cdecl]<uint, uint, uint, int, int> AttrAdd;
        public delegate* unmanaged[Cdecl]<uint, uint*, long*, uint*, int> SequenceState;
        public delegate* unmanaged[Cdecl]<uint, uint, uint, int> SequencePlay;
        // since 0.2.0-dev.29
        public delegate* unmanaged[Cdecl]<ulong, Str, uint, delegate* unmanaged[Cdecl]<NativeCall*, void*, void>, void*, uint*, uint*, int> NativeRegisterSync;
        public delegate* unmanaged[Cdecl]<NativeCounters*, int> NativeCounters;
        public delegate* unmanaged[Cdecl]<ulong, uint, NativeFilter*, int, int, delegate* unmanaged[Cdecl]<NativeCall*, void*, void>, void*, ulong*, uint*, int> NativeAttach;
        public delegate* unmanaged[Cdecl]<ulong, int> NativeDetach;
        public delegate* unmanaged[Cdecl]<uint, uint, NativeArg*, int> NativeDeclareTyped;
        public delegate* unmanaged[Cdecl]<uint, uint*, int> NativeState;
        public delegate* unmanaged[Cdecl]<ulong, uint*, int> NativeAttachState;
        public delegate* unmanaged[Cdecl]<NativeCall*, Str, uint, void*, uint, int> NativeEcsRead;
        public delegate* unmanaged[Cdecl]<NativeCall*, Str, uint, void*, uint, uint, void*, void*, int> NativeEcsWrite;
        // since 0.2.0-dev.30.3: item_use (KilnItemUse*)
        public delegate* unmanaged[Cdecl]<void*, int> ItemUse;
    }

    public static uint Fnv(ReadOnlySpan<byte> bytes)
    {
        uint h = 0x811c9dc5;
        foreach (var b in bytes) h = (h ^ b) * 0x01000193;
        return h;
    }

    public static uint Fnv(string s) => Fnv(System.Text.Encoding.UTF8.GetBytes(s));

    public static string Text(Str s) => s.Ptr == null ? "" : System.Text.Encoding.UTF8.GetString(s.Ptr, (int)Math.Min(s.Len, 4096));
}

/// <summary>The C# functions behind the fake <c>kiln.abilities</c> table.</summary>
internal static unsafe class AbilitiesTable
{
    private static AbilitiesAbi.Table* _table;

    // Kiln.Abilities installs its listener and wrap dispatcher once per process (AbilitiesDispatch.Ensure)
    private static delegate* unmanaged[Cdecl]<void*, uint, void*, uint> _listener;
    private static void* _listenerUser;

    public static nint Create()
    {
        if (_table != null) return (nint)_table;
        var t = (AbilitiesAbi.Table*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesAbi.Table));  // process lifetime
        t->Header = new ServiceHeader { Size = (uint)sizeof(AbilitiesAbi.Table), Version = AbilitiesAbi.Version };
        t->GetStatus = &GetStatus;
        t->SetListener = &SetListener;
        t->HoldFilter = &HoldFilter;
        t->InstanceVars = &InstanceVars;
        t->InstanceRead = &InstanceRead;
        t->InstanceWrite = &InstanceWrite;
        t->InstanceInfo = &InstanceInfo;
        t->EntityOverrides = &EntityOverrides;
        t->ProgramCount = &ProgramCount;
        t->ProgramInfo = &ProgramInfo;
        t->ProgramVars = &ProgramVars;
        t->ProgramRegister = &ProgramRegister;
        t->ProgramRefresh = &ProgramRefresh;
        t->NativeCount = &NativeCount;
        t->NativeAt = &NativeAt;
        t->NativeFind = &NativeFind;
        t->NativeDeclare = &NativeDeclare;
        t->NativeRegister = &NativeRegister;
        t->NativeUnregister = &NativeUnregister;
        t->NativeComplete = &NativeComplete;
        t->NativeWrap = &NativeWrap;
        t->NativeObserve = &NativeObserve;
        t->SetWrapDispatcher = &SetWrapDispatcher;
        t->CallOriginal = &CallOriginal;
        t->ReleaseOwner = &ReleaseOwner;
        t->AttrGet = &AttrGet;
        t->AttrAdd = &AttrAdd;
        t->SequenceState = &SequenceState;
        t->SequencePlay = &SequencePlay;
        t->NativeRegisterSync = &NativeRegisterSync;
        t->NativeCounters = &NativeCounters;
        t->NativeAttach = &NativeAttach;
        t->NativeDetach = &NativeDetach;
        t->NativeDeclareTyped = &NativeDeclareTyped;
        t->NativeState = &NativeState;
        t->NativeAttachState = &NativeAttachState;
        t->NativeEcsRead = &NativeEcsRead;
        t->NativeEcsWrite = &NativeEcsWrite;
        t->ItemUse = &ItemUse;
        _table = t;
        return (nint)t;
    }

    /// <summary>Kiln.Abilities' Pre (its listener): what the native service calls at the start of each step, with no
    /// events offline (detaches and retirements that waited, setup refusals, NativeContext's write events, its tick).</summary>
    public static void RunListener()
    {
        var l = _listener;
        if (l != null) l(null, 0, _listenerUser);
    }

    // the model of the harness that runs now; null: answer as the unavailable service
    private static AbilitiesModel? Model => FakeTables.Active(AbilitiesAbi.Service) is { } h ? AbilitiesFake.ModelOf(h) : null;

    private static int Guard(Func<AbilitiesModel, int> body)
    {
        try
        {
            return Model is { } m ? body(m) : FakeTables.Unavailable;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    // ---- what the fake models ----

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GetStatus(AbilitiesAbi.Status* s) =>
        s == null || s->Size < (uint)sizeof(AbilitiesAbi.Status) ? FakeTables.InvalidArgument : Guard(m => m.Status(s));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SetListener(delegate* unmanaged[Cdecl]<void*, uint, void*, uint> fn, void* user)
    {
        _listener = fn;
        _listenerUser = user;
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SetWrapDispatcher(delegate* unmanaged[Cdecl]<AbilitiesAbi.NativeCall*, void*, void> fn, void* user) =>
        FakeTables.Ok;  // the global wrap dispatcher of older builds: Kiln.Abilities attaches its wraps (Attach) instead

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint NativeCount()
    {
        try
        {
            return Model is { } m ? m.NativeCount() : 0;
        }
        catch
        {
            return 0;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeAt(uint index, AbilitiesAbi.NativeInfo* o) =>
        o == null || o->Size < (uint)sizeof(AbilitiesAbi.NativeInfo) ? FakeTables.InvalidArgument : Guard(m => m.NativeAt(index, o));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeFind(uint hash, AbilitiesAbi.NativeInfo* o) => Guard(m => m.NativeFind(hash, o));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeDeclare(uint hash, uint argc) => Guard(m => m.Declare(hash, argc));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeUnregister(uint hash) => Guard(m => m.Unregister(hash));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void CallOriginal(AbilitiesAbi.NativeCall* c)
    {
        try
        {
            AbilitiesModel.CallOriginal(c);
        }
        catch
        {
            // never throw into the caller
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReleaseOwner(ulong owner) => Guard(m => m.ReleaseOwner(owner));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeRegisterSync(ulong owner, AbilitiesAbi.Str name, uint argc,
        delegate* unmanaged[Cdecl]<AbilitiesAbi.NativeCall*, void*, void> fn, void* user, uint* hash, uint* refusal)
    {
        if (refusal != null) *refusal = AbilitiesAbi.RefusedNone;
        var text = AbilitiesAbi.Text(name);
        var f = (nint)fn;
        return Guard(m => m.RegisterSync(owner, text, argc, f, (nint)user, hash, refusal));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeCounters(AbilitiesAbi.NativeCounters* c) =>
        c == null || c->Size < 16 ? FakeTables.InvalidArgument : Guard(m => m.Counters(c));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeAttach(ulong owner, uint hash, AbilitiesAbi.NativeFilter* filter, int stage, int priority,
        delegate* unmanaged[Cdecl]<AbilitiesAbi.NativeCall*, void*, void> fn, void* user, ulong* handle, uint* refusal)
    {
        if (refusal != null) *refusal = AbilitiesAbi.RefusedNone;
        if (handle != null) *handle = 0;
        var f = (nint)fn;
        return Guard(m => m.Attach(owner, hash, filter, stage, priority, f, (nint)user, handle, refusal));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeDetach(ulong handle) => Guard(m => m.Detach(handle));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeDeclareTyped(uint hash, uint argc, AbilitiesAbi.NativeArg* args) => Guard(m => m.DeclareTyped(hash, argc, args));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeState(uint hash, uint* o) => o == null ? FakeTables.InvalidArgument : Guard(m => m.State(hash, o));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeAttachState(ulong handle, uint* o) => o == null ? FakeTables.InvalidArgument : Guard(m => m.AttachState(handle, o));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeEcsRead(AbilitiesAbi.NativeCall* call, AbilitiesAbi.Str component, uint entity, void* o, uint size)
    {
        if (o == null || size == 0) return FakeTables.InvalidArgument;
        var name = AbilitiesAbi.Text(component);
        return Guard(m => m.EcsRead(call, name, entity, (byte*)o, size));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeEcsWrite(AbilitiesAbi.NativeCall* call, AbilitiesAbi.Str component, uint entity, void* value, uint size,
        uint flags, void* before, void* after)
    {
        if (value == null || size == 0) return FakeTables.InvalidArgument;
        var name = AbilitiesAbi.Text(component);
        return Guard(m => m.EcsWrite(call, name, entity, (byte*)value, size, flags, (byte*)before, (byte*)after));
    }

    // ---- not modelled offline: answered as the native service does when it is unavailable ----

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int HoldFilter(uint* ids, uint n) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int InstanceVars(uint id, void* o, uint cap, uint* n) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int InstanceRead(uint id, uint var, void* o, uint size, uint* n) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int InstanceWrite(uint id, uint var, void* v, uint size) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int InstanceInfo(uint id, uint* program, uint* self, uint* step) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int EntityOverrides(uint entity, void* o, uint n, uint program) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint ProgramCount() => 0;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ProgramInfo(uint index, void* o) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ProgramVars(uint id, void* o, uint cap, uint* n) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ProgramRegister(ulong owner, void* blob, nuint size, uint* id, byte* error, nuint cap) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ProgramRefresh(uint* n) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeRegister(ulong owner, AbilitiesAbi.Str name, uint argc, uint* hash) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeComplete(ulong call, byte error, byte value) => FakeTables.NotFound;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeWrap(uint hash, uint on) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeObserve(uint hash, uint on) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AttrGet(uint entity, uint component, uint attr, int* value) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AttrAdd(uint entity, uint component, uint attr, int delta) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SequenceState(uint entity, uint* sequence, long* time, uint* flags) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SequencePlay(uint entity, uint sequence, uint trigger) => FakeTables.Unavailable;

    // an item use starts a sequence: offline like sequence_play (abilities.sequences is an offline row of the kit)
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ItemUse(void* use) => FakeTables.Unavailable;
}
