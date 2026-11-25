// SPDX-License-Identifier: MIT
// The fake kiln.skills.client (native/core/abi/services/kiln_skills_client.h, v1, since 0.2.0-dev.31.13) under the real
// Kiln.Skills ClientSkills: the local player's tree, levels and points as a test scripts them, answered as kiln-skills.dll
// answers (spent = the levels' costs, shadow nodes included; available = total - spent, at least 0).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

/// <summary>Mirror of <c>kiln_skills_client.h</c>. The sizes are pinned by Kiln.TestKit.Tests to the header's numbers and to
/// Kiln.Skills' private mirror.</summary>
internal static unsafe class SkillsClientAbi
{
    public const string Service = "kiln.skills.client";
    public const uint Version = 1;
    public const byte FlagUnlocked = 0x1, FlagShadow = 0x2, FlagRecord = 0x4;

    [StructLayout(LayoutKind.Sequential)]
    public struct Node
    {
        public uint Id;
        public ushort Index, Cost;
        public byte Type, ClassAlignment, MaxLevel, Level, Flags, LevelIndex;
        public ushort Reserved;
        public float X, Y;
        public uint Incoming, Outgoing;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Link
    {
        public uint Id, From, To;
        public byte Bidirectional, R0, R1, R2;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct State
    {
        public uint Size;
        public byte On, World, Copying, Reserved0;
        public uint Player;
        public int Total, Spent, Available;
        public uint Nodes, Unlocked, Links, Reserved1;
        public ulong Step;
        public long AgeMs;
        public fixed byte Detail[160];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<State*, int> GetState;
        public delegate* unmanaged[Cdecl]<Node*, uint, uint*, int> Nodes;
        public delegate* unmanaged[Cdecl]<Link*, uint, uint*, int> Links;
    }
}

internal sealed class SkillsClientState
{
    internal readonly object Gate = new();
    internal readonly List<FakeClientSkillNode> Nodes = [];
    internal readonly List<(uint Id, uint From, uint To, bool Both)> Links = [];
    internal EntityId Player = new(1);
    internal int Total;
    internal bool Loaded = true;
    internal int Reads;
}

internal sealed unsafe class SkillsClientFake : IKitFake
{
    public string Service => SkillsClientAbi.Service;
    private static SkillsClientAbi.Table* _table;
    private static KilnHarness? Harness => FakeTables.Active(SkillsClientAbi.Service);
    private static SkillsClientState? StateOf => Harness is { } h ? KitFakes.State<SkillsClientState>(h) : null;

    public void Register()
    {
        if (_table == null)
        {
            _table = (SkillsClientAbi.Table*)NativeMemory.AllocZeroed((nuint)sizeof(SkillsClientAbi.Table));  // process lifetime
            _table->Header = new ServiceHeader { Size = (uint)sizeof(SkillsClientAbi.Table), Version = SkillsClientAbi.Version };
            _table->GetState = &GetState;
            _table->Nodes = &Nodes;
            _table->Links = &Links;
        }
        KitBackend.Instance.RegisterTable(Service, (nint)_table);
        FakeTables.Features([ClientSkills.Capability]);
    }

    public void Start(KilnHarness harness) { }
    public void BeforeStep(KilnHarness harness, ulong step) { }

    public void End(KilnHarness harness)
    {
        var s = KitFakes.State<SkillsClientState>(harness);
        lock (s.Gate)
        {
            s.Nodes.Clear();
            s.Links.Clear();
        }
    }

    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new CoverageRow("The client's skill-tree view (since 0.2.0-dev.31.13)",
        CoverageKind.RecordingFake, [SkillsClientAbi.Service, ClientSkills.Capability],
        "Kiln.Skills' ClientSkills on the kit's kiln.skills.client: the local player's tree a test builds " +
        "(KilnHarness.ClientSkills.AddNode, Link: type, class, cost, levels, screen position, shadow nodes), their levels " +
        "(Unlock, Lock) and total points; spent and available as kiln-skills.dll reads them from the game's screen (the levels' " +
        "costs, shadow nodes included; available at least 0), link counts per node. Loaded = false is the time before the first " +
        "copy (Points null). The row skills.client_view can be switched off. Offline: the game's replication of the character, " +
        "the screen's update timing, the demand window"));

    internal static (int Spent, int Unlocked) Sums(SkillsClientState s)
    {
        var spent = 0;
        var unlocked = 0;
        foreach (var n in s.Nodes)
        {
            if (n.Level <= 0) continue;
            spent += n.Cost * n.Level;
            unlocked++;
        }
        return (spent, unlocked);
    }

    private static bool Off(SkillsClientState? s) => s is null || FakeTables.Off(ClientSkills.Capability);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GetState(SkillsClientAbi.State* o)
    {
        if (o == null || o->Size != sizeof(SkillsClientAbi.State)) return FakeTables.InvalidArgument;
        var s = StateOf;
        *o = default;
        o->Size = (uint)sizeof(SkillsClientAbi.State);
        var off = Off(s);
        o->On = off ? (byte)0 : (byte)1;
        o->World = Harness is null ? (byte)0 : (byte)1;
        var text = System.Text.Encoding.UTF8.GetBytes(off ? "switched off by the test" : "");
        for (var i = 0; i < Math.Min(text.Length, 159); i++) o->Detail[i] = text[i];
        if (off) return FakeTables.Unavailable;
        lock (s!.Gate)
        {
            s.Reads++;
            o->Copying = 1;
            if (!s.Loaded) return FakeTables.NotFound;
            var (spent, unlocked) = Sums(s);
            o->Player = s.Player.Value;
            o->Total = s.Total;
            o->Spent = spent;
            o->Available = Math.Max(0, s.Total - spent);
            o->Nodes = (uint)s.Nodes.Count;
            o->Unlocked = (uint)unlocked;
            o->Links = (uint)s.Links.Count;
            o->Step = Harness?.CurrentStep ?? 0;
        }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Nodes(SkillsClientAbi.Node* o, uint capacity, uint* count)
    {
        if (count == null || (capacity != 0 && o == null)) return FakeTables.InvalidArgument;
        *count = 0;
        var s = StateOf;
        if (Off(s)) return FakeTables.Unavailable;
        lock (s!.Gate)
        {
            s.Reads++;
            if (!s.Loaded) return FakeTables.Ok;
            *count = (uint)s.Nodes.Count;
            var n = Math.Min(capacity, (uint)s.Nodes.Count);
            for (var i = 0; i < n; i++)
            {
                var x = s.Nodes[i];
                var flags = (byte)((x.Level > 0 ? SkillsClientAbi.FlagUnlocked : 0) | (x.Shadow ? SkillsClientAbi.FlagShadow : 0) | SkillsClientAbi.FlagRecord);
                o[i] = new SkillsClientAbi.Node
                {
                    Id = x.Id, Index = (ushort)i, Cost = (ushort)x.Cost, Type = (byte)x.Type, ClassAlignment = (byte)x.ClassAlignment,
                    MaxLevel = (byte)x.MaxLevel, Level = (byte)x.Level, Flags = flags, X = x.UiX, Y = x.UiY,
                    Incoming = (uint)s.Links.Count(l => l.To == x.Id), Outgoing = (uint)s.Links.Count(l => l.From == x.Id),
                };
            }
            return n < s.Nodes.Count ? FakeTables.BufferTooSmall : FakeTables.Ok;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Links(SkillsClientAbi.Link* o, uint capacity, uint* count)
    {
        if (count == null || (capacity != 0 && o == null)) return FakeTables.InvalidArgument;
        *count = 0;
        var s = StateOf;
        if (Off(s)) return FakeTables.Unavailable;
        lock (s!.Gate)
        {
            s.Reads++;
            if (!s.Loaded) return FakeTables.Ok;
            *count = (uint)s.Links.Count;
            var n = Math.Min(capacity, (uint)s.Links.Count);
            for (var i = 0; i < n; i++)
                o[i] = new SkillsClientAbi.Link { Id = s.Links[i].Id, From = s.Links[i].From, To = s.Links[i].To, Bidirectional = s.Links[i].Both ? (byte)1 : (byte)0 };
            return n < s.Links.Count ? FakeTables.BufferTooSmall : FakeTables.Ok;
        }
    }
}
