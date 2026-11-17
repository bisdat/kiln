// SPDX-License-Identifier: MIT
// The native table of kiln.stations (native/core/abi/services/kiln_stations.h, in kiln-items.dll). Blittable; field order
// is the ABI (sizes asserted in native/services/items/tests/stations_test.cpp and in StationsTests).
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class StationsNative
{
    public const string Name = "kiln.stations";
    public const uint Version = 1;
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5;
    // result statuses (KilnStationResult.status) the managed side looks at
    public const int StLocked = 9, StUnavailable = 11, StDropped = 12, StTimedOut = 13, StCanceled = 15;
    public const int MaxBases = 16, MaxAltars = 16, MaxInputs = 32, MaxOutputs = 8, MaxChanges = 32, MaxKeep = 64;

    // actions
    public const uint Fill = 1, Stop = 2, Resume = 3, Select = 4;
    // request flags
    public const uint FillAdd = 0x1, FillExact = 0x2, DryRun = 0x4, NoWater = 0x8, AfterCraft = 0x10, KeepStopped = 0x20,
        AllowLocked = 0x40;
    // event kinds
    public const uint EvState = 1, EvPlaced = 2, EvRemoved = 3, EvBases = 4;
    // KilnStation.flags
    public const uint FlagStopPending = 0x1;

    [StructLayout(LayoutKind.Sequential)]
    public struct BaseId
    {
        public fixed byte Guid[16];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Choice
    {
        public uint Category;
        public byte Index, R0, R1, R2;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Stack
    {
        public uint Item, Count;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Vec3
    {
        public double X, Y, Z;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Base
    {
        public uint Size, Index;
        public BaseId Id;
        public uint AltarCount;
        public fixed uint Altars[16];
        public fixed byte AltarIds[16 * 16];
        public uint StorageCount, WaterCount, ZoneCount, StationCount, Version;
        public Vec3 Min, Max;
        public fixed uint Reserved[4];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Station
    {
        public uint Size, Entity, Workshop;
        public int BaseIndex;
        public BaseId Base;
        public Vec3 Position;
        public byte State, StoppedByPlayer, UseAllOptions, Needs;
        public uint RunningRecipe, SelectedRecipe;
        public Choice R0, R1, R2, R3, S0, S1, S2, S3;
        public uint CraftsLoaded;
        public long RecipeStartNs, PauseNs, NowNs, DurationNs;
        public float Progress;
        public int Water, WaterMax;
        public uint Flags;
        public fixed uint Reserved[4];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Input
    {
        public uint Item, Count, Category, CategoryCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Recipe
    {
        public uint Size, Id, Workshop, InputCount, OutputCount, WaterCharges;
        public long DurationNs;
        public uint KnowledgeId, KnowledgeValue;
        public byte KnowledgeCompare, KnowledgeType, KnowledgePlayerOnly, Reserved0;
        public uint Reserved1;
        public fixed uint Inputs[MaxInputs * 4];   // Input x 32
        public fixed uint Outputs[MaxOutputs * 2]; // Stack x 8
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Request
    {
        public uint Size, Action, Flags, Station, Crafts, RecipeId;
        public Choice C0, C1, C2, C3;
        public uint Player, KeepCount;
        public Stack* Keep;
        public fixed uint Reserved[4];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MoveChange
    {
        public ulong FromSlot, ToSlot;
        public uint Item, Amount, Pide, Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Result
    {
        public uint Size;
        public int Status;
        public uint Action, Flags, Station, RecipeId, CraftsRequested, CraftsBefore, CraftsAfter, CraftsAdded;
        public int GameError;
        public uint ChangeCount;
        public byte StateBefore, StateAfter, StoppedBefore, StoppedAfter;
        public byte LimitedBy, R0, R1, R2;
        public uint ShortItem, Reserved;
        public ulong Ticket, Step;
        public fixed byte Changes[MaxChanges * 32];  // MoveChange x 32
        public fixed byte Message[128];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Plan
    {
        public uint Size;
        public int Status;
        public uint Station, RecipeId;
        public int BaseIndex;
        public uint CraftsLoaded, CraftsAffordable, CraftsPlanned, InputCount, ShortItem;
        public byte LimitedBy, UseAllOptions, R0, R1;
        public uint WaterPerCraft, WaterHave, WaterStock;
        public fixed uint Inputs[MaxInputs * 2];  // Stack x 32
        public fixed uint Have[MaxInputs];
        public fixed uint Stock[MaxInputs];
        public fixed byte Message[128];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Event
    {
        public uint Size, Kind;
        public ulong Seq, Step;
        public uint Station, Workshop;
        public byte StateBefore, StateAfter, StoppedBefore, StoppedAfter;
        public uint SelectedBefore, SelectedAfter, RunningBefore, RunningAfter;
        public int BaseIndex;
        public uint Reserved0;
        public ulong By;
        public Vec3 Position;
        public BaseId Base;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Status
    {
        public uint Size;
        public byte Read, Fill, Control, Events, Program, Watching, Knowledge, Scanned;
        public uint Bases, Stations, Pending, Done;
        public fixed byte Detail[160];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<Status*, int> GetStatus;
        public delegate* unmanaged[Cdecl]<uint*, int> BaseCount;
        public delegate* unmanaged[Cdecl]<uint, Base*, int> BaseAt;
        public delegate* unmanaged[Cdecl]<uint, Base*, int> BaseOf;
        public delegate* unmanaged[Cdecl]<Vec3*, Base*, int> BaseAtPosition;
        public delegate* unmanaged[Cdecl]<uint, uint*, uint, uint*, int> BaseStorage;
        public delegate* unmanaged[Cdecl]<uint, uint*, uint, uint*, int> BaseWater;
        public delegate* unmanaged[Cdecl]<uint, Stack*, uint, uint*, int> BaseStock;
        public delegate* unmanaged[Cdecl]<int, uint*, uint, uint*, int> Stations;
        public delegate* unmanaged[Cdecl]<uint, Station*, int> StationRead;
        public delegate* unmanaged[Cdecl]<uint, Recipe*, int> RecipeInfo;
        public delegate* unmanaged[Cdecl]<uint, Choice*, Stack*, uint, uint*, int> RecipeInputs;
        public delegate* unmanaged[Cdecl]<uint, uint*, float*, uint, uint*, int> Category;
        public delegate* unmanaged[Cdecl]<uint, uint*, int> CategoryAt;
        public delegate* unmanaged[Cdecl]<uint, uint*, uint, uint*, int> WorkshopRecipes;
        public delegate* unmanaged[Cdecl]<Request*, Plan*, int> MakePlan;
        public delegate* unmanaged[Cdecl]<ulong, Request*, delegate* unmanaged[Cdecl]<Result*, void*, void>, void*, ulong*, int> Submit;
        public delegate* unmanaged[Cdecl]<ulong, int> Cancel;
        public delegate* unmanaged[Cdecl]<uint, int> Watch;
        public delegate* unmanaged[Cdecl]<ulong, Event*, uint, uint*, int> Events;
        public delegate* unmanaged[Cdecl]<ulong> LastSeq;
    }

    public static string Text(byte* p, int max)
    {
        var n = 0;
        while (n < max && p[n] != 0) n++;
        return System.Text.Encoding.UTF8.GetString(p, n);
    }

    public static Guid GuidOf(in BaseId id)
    {
        fixed (byte* g = id.Guid) return new Guid(new ReadOnlySpan<byte>(g, 16));
    }

    public static Guid AltarIdOf(in Base b, int i)
    {
        fixed (byte* g = b.AltarIds) return new Guid(new ReadOnlySpan<byte>(g + 16 * i, 16));
    }

    public static BaseId IdOf(Guid g)
    {
        BaseId id = default;
        Span<byte> b = stackalloc byte[16];
        g.TryWriteBytes(b);
        for (var i = 0; i < 16; i++) id.Guid[i] = b[i];
        return id;
    }
}
