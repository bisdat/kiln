// SPDX-License-Identifier: MIT
// The native kiln.abilities table (native/core/abi/services/kiln_abilities.h). Field order and types must match.
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class AbilitiesNative
{
    public const string Name = "kiln.abilities";
    public const uint Version = 1;
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5;

    public const uint EvImpactStarted = 1, EvImpactEnded = 2, EvNative = 3, EvSignal = 4, EvManagedCall = 5;
    public const uint InterestHold = 1u << 31;
    public static uint Bit(uint kind) => 1u << (int)kind;

    public const uint OverrideByConfig = 0, OverrideByName = 1, OverrideByIndex = 2;

    // sequence_play: KILN_SEQUENCE_TRIGGER_* | flags. SequenceHold (0x10000): hold the sequence's keep-alive states
    // (native/services/abilities, "held sequences"; older services answer INVALID_ARGUMENT to a flag).
    public const uint TriggerExternal = 22, TriggerAbortOverride = 24, SequenceHold = 0x10000;

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
    public struct Event
    {
        public uint Size;
        public uint Kind;
        public ulong Step;
        public uint ImpactId;
        public uint ProgramId;
        public uint Self;
        public uint Root;
        public uint NativeHash;
        public uint Site;
        public uint Argc;
        public byte Result, Error, Held, Canceled;
        public fixed uint Args[31];
        public uint Tag;
        public fixed uint ArgTypes[31];
        public uint ValueType;
        public uint ValueSize;
        public fixed uint ArgValues[31];
        public fixed byte ArgOffsets[31];
        public fixed byte ArgSizes[31];
        public fixed byte Reserved0[2];
        public ulong CallId;
        public fixed byte Value[136];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Var
    {
        public uint Index, NameHash, ConfigId, Type;
        public ushort ValueSize, Offset;
        public uint Reserved;
        public fixed byte Name[64];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Override
    {
        public uint Match, Key, Size, Reserved;
        public fixed byte Data[64];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeInfo
    {
        public uint Size, Hash, Argc, Kind;
        public nuint Fn;
        public fixed byte Name[64];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ProgramInfo
    {
        public uint Size, Id;
        public fixed byte Guid[16];
        public uint CodeWords, ShutdownWords, Variables, DataBytes;
        public ushort StackSize;
        public byte Streams, Kiln;
    }

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
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<Status*, int> GetStatus;
        public delegate* unmanaged[Cdecl]<delegate* unmanaged[Cdecl]<Event*, uint, void*, uint>, void*, int> SetListener;
        public delegate* unmanaged[Cdecl]<uint*, uint, int> HoldFilter;
        public delegate* unmanaged[Cdecl]<uint, Var*, uint, uint*, int> InstanceVars;
        public delegate* unmanaged[Cdecl]<uint, uint, void*, uint, uint*, int> InstanceRead;
        public delegate* unmanaged[Cdecl]<uint, uint, void*, uint, int> InstanceWrite;
        public delegate* unmanaged[Cdecl]<uint, uint*, uint*, uint*, int> InstanceInfo;
        public delegate* unmanaged[Cdecl]<uint, Override*, uint, uint, int> EntityOverrides;
        public delegate* unmanaged[Cdecl]<uint> ProgramCount;
        public delegate* unmanaged[Cdecl]<uint, ProgramInfo*, int> ProgramInfoFn;
        public delegate* unmanaged[Cdecl]<uint, Var*, uint, uint*, int> ProgramVars;
        public delegate* unmanaged[Cdecl]<ulong, void*, nuint, uint*, byte*, nuint, int> ProgramRegister;
        public delegate* unmanaged[Cdecl]<uint*, int> ProgramRefresh;
        public delegate* unmanaged[Cdecl]<uint> NativeCount;
        public delegate* unmanaged[Cdecl]<uint, NativeInfo*, int> NativeAt;
        public delegate* unmanaged[Cdecl]<uint, NativeInfo*, int> NativeFind;
        public delegate* unmanaged[Cdecl]<uint, uint, int> NativeDeclare;
        public delegate* unmanaged[Cdecl]<ulong, NativeStr, uint, uint*, int> NativeRegister;
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
    }

    /// <summary>Since 0.2.0-dev.29 (appended to v1; read a member only when <see cref="Has"/> says the native table
    /// reaches it): synchronous natives and the thunks' counters.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct TableV2
    {
        public Table V1;
        public delegate* unmanaged[Cdecl]<ulong, NativeStr, uint, delegate* unmanaged[Cdecl]<NativeCall*, void*, void>, void*, uint*, uint*, int> NativeRegisterSync;
        public delegate* unmanaged[Cdecl]<NativeCounters*, int> NativeCountersFn;
    }

    /// <summary>KilnNativeCounters (since 0.2.0-dev.29).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct NativeCounters
    {
        public uint Size, Inflight, Flags, Reserved;
        public ulong SyncCalls, SyncFaults, SyncSlow, DeadCalls;
    }

    public const uint CountersNatives = 1;  // KILN_NATIVE_COUNTERS_NATIVES: the RegisterNative hook is in

    // KILN_NATIVE_REFUSED_*
    public const uint RefusedNone = 0, RefusedRegistryFull = 1, RefusedNoSlot = 2, RefusedTaken = 3, RefusedVanilla = 4;

    /// <summary>The refusal's words (NativeResult.Reason, AttachResult.Reason).</summary>
    public static string Refusal(uint code) => code switch
    {
        RefusedRegistryFull => "native registry full (the game takes 128 natives, 107 of them its own)",
        RefusedNoSlot => "no free Kiln native slot (160)",
        RefusedTaken => "a Kiln native with this name is registered",
        RefusedVanilla => "the name's hash is one of the game's natives",
        _ => $"refused ({code})",
    };

    private static Table* _t;

    public static Table* T => _t != null ? _t : _t = Services.Get<Table>(Name, Version);

    /// <summary>True when the native table reaches the member <paramref name="field"/> of <see cref="TableV2"/>.</summary>
    public static bool Has(Table* t, string field) =>
        t != null && t->Header.Size >= (uint)Marshal.OffsetOf<TableV2>(field) + (uint)sizeof(nint);

    /// <summary>The table with the dev.29 members up to <paramref name="field"/>, or null (no kiln.abilities, or an
    /// older one).</summary>
    public static TableV2* V2(string field)
    {
        var t = T;
        return Has(t, field) ? (TableV2*)t : null;
    }

    /// <summary>The thunks' counters, or null (no kiln.abilities, or one older than dev.29).</summary>
    public static NativeCounters? Counters()
    {
        var t = V2(nameof(TableV2.NativeCountersFn));
        if (t == null) return null;
        NativeCounters c = default;
        c.Size = (uint)sizeof(NativeCounters);
        return t->NativeCountersFn(&c) == Ok ? c : null;
    }

    public static Table* Require(string api)
    {
        var t = T;
        if (t != null) return t;
        AbilitiesCaps.Ensure();
        throw ServiceCaps.Unavailable(AbilitiesCaps.Service, $"{api}: kiln.abilities is not available on this build");
    }

    public static string Describe(int rc) => rc switch
    {
        Ok => "ok",
        InvalidArgument => "invalid argument",
        NotFound => "not found",
        Unavailable => "unavailable on this build (or the world is not running yet)",
        WrongThread => "not on the tick thread",
        BufferTooSmall => "buffer too small",
        _ => $"error {rc}",
    };

    public static uint Fnv(ReadOnlySpan<byte> bytes)
    {
        uint h = 0x811c9dc5;
        foreach (var b in bytes) h = (h ^ b) * 0x01000193;
        return h;
    }

    public static uint Fnv(string s) => Fnv(System.Text.Encoding.UTF8.GetBytes(s));

    public static string Text(byte* p, int max)
    {
        var n = 0;
        while (n < max && p[n] != 0) n++;
        return System.Text.Encoding.UTF8.GetString(p, n);
    }
}
