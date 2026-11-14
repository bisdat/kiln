// SPDX-License-Identifier: MIT
// kiln.world v4's game-program members (native/core/abi/services/kiln_world.h). Field order and types must match the
// C header; native/services/world/tests/abi.json is the record Kiln.Systems.Tests checks these mirrors against.
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class SystemsNative
{
    public const string Name = "kiln.world";
    public const uint Version = 4;

    // KilnResult
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5,
        Internal = 7;

    // KILN_WRAP_* stages, states and reasons
    public const int Before = 0, Replace = 1, After = 2;
    public const uint Pending = 0, Active = 1, Removed = 2, Refused = 3;
    public const uint ReasonFault = 1, ReasonBudget = 2, ReasonNativeSet = 3, ReasonForeign = 4, ReasonNoProgram = 5,
        ReasonNoUpdate = 6, ReasonPoolFull = 7, ReasonUnwrapped = 8, ReasonOwnerGone = 9, ReasonReplaceTaken = 10;

    // KILN_GAME_PROGRAM_* and KILN_GAME_LIST_*
    public const uint ProgramGame = 1, ProgramNoUpdate = 2, ProgramNoView = 4;
    public const int ListTpl = 0, ListTplOpt = 1, ListRead = 2, ListReadOpt = 3, ListWrite = 4, ListWriteOpt = 5,
        ListSysRead = 6, ListSysWrite = 7, Lists = 16;

    [StructLayout(LayoutKind.Sequential)]
    public struct ProgramInfo
    {
        public uint Size, WorldKind;
        public int Index;
        public ushort Id, Reserved;
        public NativeStr Name;
        public uint DescFlags, EntryFlags;
        public ulong UpdateRva;
        public uint Flags, Wraps, Faults, Reserved2;
        public ulong Slices, Entities, Forwarded;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ProgramsStatus
    {
        public uint Size;
        public fixed byte Directory[3];
        public byte Threads;
        public fixed uint Programs[3];
        public fixed uint GamePrograms[3];
        public fixed uint NoUpdate[3];
        public ushort PlayersMask;
        public byte ParallelForced, ParallelSwitch;
        public uint ParallelStepsLeft, WrapsActive, WrapsPending;
        public fixed byte Detail[160];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Slice
    {
        public uint Size, WorldKind;
        public void* ProgramCtx;
        public uint Count, ThreadIsWorld;
        public uint* EntityIds;
        public uint EntityStride;
        public int ProgramIndex;
        public ulong Step;
        public NativeStr Program;
        public int Stage;
        public uint Reserved;
        public void* Internal;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WrapInfo
    {
        public uint Size, State, Disabled, Reason, WorldKind;
        public int Stage, Priority, ProgramIndex;
        public NativeStr Program;
        public ulong Calls, Entities, Faults, Overruns, TotalNs;
        public uint Threads, FaultCode;
        public byte RemoveQueued, Gone;
        public ushort Reserved;
        public uint Running;
        public fixed byte Detail[128];
    }

    // KilnWorldApi up to game_slice_attr_changed: the v3 table (header + 34 entries) and v4.
    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public fixed long V3[34];
        public delegate* unmanaged[Cdecl]<uint, NativeStr, int> GameProgramFind;
        public delegate* unmanaged[Cdecl]<uint, int, ProgramInfo*, int> GameProgramInfo;
        public delegate* unmanaged[Cdecl]<ProgramsStatus*, int> GameProgramsStatus;
        public nint LabParallel;
        public delegate* unmanaged[Cdecl]<ulong, uint, int, int, int, delegate* unmanaged[Cdecl]<Slice*, void*, void>, void*, ulong*, int> GameProgramWrap;
        public delegate* unmanaged[Cdecl]<ulong, int> GameProgramUnwrap;
        public delegate* unmanaged[Cdecl]<ulong, WrapInfo*, int> GameWrapInfo;
        public delegate* unmanaged[Cdecl]<ulong, uint*, int> GameWrapsRelease;
        public delegate* unmanaged[Cdecl]<Slice*, int> GameSliceRunOriginal;
        public nint WrapOriginal;
        public delegate* unmanaged[Cdecl]<uint, int, int, NativeStr*, uint, int> GameProgramList;
        public delegate* unmanaged[Cdecl]<Slice*, delegate* unmanaged[Cdecl]<ulong*, uint, uint, void*, uint>, void*, int> GameSliceIterate;
        public delegate* unmanaged[Cdecl]<Slice*, ulong*, int, int, uint*, byte*, void*> GameSliceSlot;
        public delegate* unmanaged[Cdecl]<Slice*, ulong*, int, int, int> GameSliceMark;
        public delegate* unmanaged[Cdecl]<Slice*, nuint*, int> GameSliceArchetype;
        public delegate* unmanaged[Cdecl]<uint, nuint, uint, int> GameArchetypeHas;
        public delegate* unmanaged[Cdecl]<Slice*, ulong*, int, int, int> GameSliceAttrChanged;
    }

    private static Table* _table;
    private static bool _looked;

    /// <summary>The table, or null when kiln.world v4 is not here.</summary>
    public static Table* T
    {
        get
        {
            if (_table != null || _looked) return _table;
            _table = Services.Get<Table>(Name, Version);
            _looked = true;
            return _table;
        }
    }

    /// <summary>Tests: ask for the table again.</summary>
    internal static void Reset()
    {
        _table = null;
        _looked = false;
    }

    public static string Text(byte* p, int capacity)
    {
        var n = 0;
        while (n < capacity && p[n] != 0) n++;
        return System.Text.Encoding.UTF8.GetString(p, n);
    }
}
