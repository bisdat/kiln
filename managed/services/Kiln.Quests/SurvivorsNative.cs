// SPDX-License-Identifier: MIT
// The native mirror of kiln.survivors (native/core/abi/services/kiln_survivors.h, new in 0.2.0-dev.31). Internal: the
// public surface is Survivors.cs, SurvivorsTypes.cs and SurvivorsEvents.cs.
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

internal static unsafe class SurvivorsNative
{
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4;
    public const uint OpRecruit = 1, OpSummon = 2, OpAssignBed = 3, OpUnassignBed = 4;
    public const uint GiveTool = 1;
    public const uint StateInWorld = 1, StateHasHome = 2, StateHasBase = 4;
    public const uint EvRecruited = 1, EvToolGiven = 2, EvSummoned = 3, EvAssignmentChanged = 4;

    [StructLayout(LayoutKind.Sequential)]
    internal struct Vec3
    {
        public double X, Y, Z;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Request
    {
        public uint Size, Op, NpcId, Flags, Authorization, Reserved;
        public uint Actor, Recipient, Bed, Reserved1;
        public fixed byte BaseId[16];
        public Vec3 Position;
        public float YawDegrees;
        public uint Reserved2;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct State
    {
        public uint Size, NpcId, Entity, Bed, SummonItem, Unlocked, Flags, Index;
        public fixed byte BaseId[16];
        public Vec3 Home;
        public fixed byte Name[96];
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Result
    {
        public uint Size, Status;
        public ulong Request, Step;
        public uint Effects, AfterKnown, DisplacedEntity, DisplacedNpc, Op, Steps;
        public State Before, After;
        public fixed byte Reason[160];
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Progress
    {
        public uint Size, State;
        public ulong Request, ElapsedMs;
        public uint Effects, InFlight;
        public fixed byte Reason[160];
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Event
    {
        public uint Size, Kind;
        public ulong Sequence, Request, Step;
        public State Before, After;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Status
    {
        public uint Size;
        public byte Read, Recruit, Summon, Assign;
        public uint Pending, Reserved;
        public ulong Submitted, Ended, Events;
        public fixed byte Detail[160];
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<Status*, int> GetStatus;
        public delegate* unmanaged[Cdecl]<uint> Count;
        public delegate* unmanaged[Cdecl]<uint, State*, int> At;
        public delegate* unmanaged[Cdecl]<uint, State*, int> Get;
        public delegate* unmanaged[Cdecl]<uint, uint*, int> OfEntity;
        public delegate* unmanaged[Cdecl]<uint, uint*, int> BedOccupant;
        public delegate* unmanaged[Cdecl]<Vec3, byte*, uint*, uint*, int> BaseAt;
        public delegate* unmanaged[Cdecl]<ulong, Request*, ulong*, int> Submit;
        public delegate* unmanaged[Cdecl]<ulong, Result*, int> GetResult;
        public delegate* unmanaged[Cdecl]<ulong, Progress*, int> GetProgress;
        public delegate* unmanaged[Cdecl]<ulong, Event*, uint, uint*, int> Events;
    }

    /// <summary>The structs as the header has them (its static asserts).</summary>
    internal static bool LayoutMatches =>
        sizeof(Request) == 88 && sizeof(State) == 168 && sizeof(Result) == 544 && sizeof(Progress) == 192 &&
        sizeof(Event) == 368 && sizeof(Status) == 200;

    private static Table* _table;
    private static bool _looked;

    internal static Table* T
    {
        get
        {
            if (_table != null || _looked) return _table;
            _looked = true;
            if (!LayoutMatches) return null;  // layout drift between this assembly and the header: refuse rather than misread
            var t = Services.Get<Table>("kiln.survivors", 1);
            if (t != null && t->Header.Size < (uint)sizeof(Table)) t = null;
            _table = t;
            return _table;
        }
    }

    internal static string Text(byte* p, int max)
    {
        var n = 0;
        while (n < max && p[n] != 0) n++;
        return Encoding.UTF8.GetString(p, n);
    }

    internal static Guid ReadGuid(byte* b)
    {
        Span<byte> s = stackalloc byte[16];
        for (var i = 0; i < 16; i++) s[i] = b[i];
        return new Guid(s);
    }

    internal static void WriteGuid(Guid g, byte* b)
    {
        Span<byte> s = stackalloc byte[16];
        g.TryWriteBytes(s);
        for (var i = 0; i < 16; i++) b[i] = s[i];
    }

    internal static SurvivorInfo Info(in State s)
    {
        fixed (State* p = &s)
        {
            var inWorld = (s.Flags & StateInWorld) != 0;
            return new SurvivorInfo(new SurvivorId(s.NpcId), Text(p->Name, 96), s.Unlocked == 1, new ItemId(s.SummonItem),
                inWorld ? new EntityId(s.Entity) : EntityId.None, inWorld ? new EntityId(s.Bed) : EntityId.None,
                (s.Flags & StateHasBase) != 0 ? ReadGuid(p->BaseId) : null,
                (s.Flags & StateHasHome) != 0 ? new WorldPosition(s.Home.X, s.Home.Y, s.Home.Z) : null);
        }
    }

    internal static SurvivorResult ToResult(in Result r)
    {
        fixed (Result* p = &r)
        {
            var completion = (SurvivorCompletion)Math.Min(r.Status, (uint)SurvivorCompletion.WorldEnded);
            var ok = completion is SurvivorCompletion.Done or SurvivorCompletion.NoChange;
            var reason = Text(p->Reason, 160);
            return new SurvivorResult(ok, ok ? null : (reason.Length > 0 ? reason : completion.ToString()), Info(r.Before), Info(r.After),
                r.AfterKnown != 0, (SurvivorEffects)r.Effects, completion,
                r.DisplacedEntity != 0 ? new EntityId(r.DisplacedEntity) : EntityId.None, (int)r.Steps);
        }
    }
}
