// SPDX-License-Identifier: MIT
// The fake kiln.stations.client (native/core/abi/services/kiln_stations_client.h, v1, since 0.2.0-dev.31.13) under the real
// Kiln.Blocks ClientStations: the stations a test scripts as the client's station cache would hold them, the views
// computed as kiln-items.dll computes them (client_stations_logic.h's arithmetic, ported: ClientStationMath).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

/// <summary>Mirror of <c>kiln_stations_client.h</c>. The sizes are pinned by Kiln.TestKit.Tests to the header's numbers and to
/// Kiln.Blocks' mirror (ClientStationsNative).</summary>
internal static unsafe class StationsClientAbi
{
    public const string Service = "kiln.stations.client";
    public const uint Version = 1;
    public const int MaxOutputs = 8;
    public const uint FlagRecipe = 0x1, FlagTime = 0x2, FlagOutputs = 0x4, FlagEnergy = 0x8;

    [StructLayout(LayoutKind.Sequential)]
    public struct View
    {
        public uint Size, Entity, Workshop, SelectedRecipe;
        public fixed byte Base[16];
        public float X, Y, Z;
        public byte State, Needs, UseAllOptions, Reserved0;
        public fixed uint Choices[8];  // {u32 category, u8 index, 3 reserved} x 4
        public uint CraftsLoaded;
        public int Water, WaterMax;
        public uint Flags;
        public long RecipeStartNs, NowNs, CraftNs, CraftRemainingNs, RemainingNs;
        public float Progress;
        public uint OutputCount;
        public fixed uint Outputs[MaxOutputs * 2];
        public ulong Step;
        public fixed uint Reserved[4];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Status
    {
        public uint Size;
        public byte On, World, Copying, Reserved0;
        public uint Rows, Reserved1;
        public ulong Step;
        public long AgeMs;
        public fixed byte Detail[160];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<Status*, int> GetStatus;
        public delegate* unmanaged[Cdecl]<View*, uint, uint*, int> Stations;
        public delegate* unmanaged[Cdecl]<uint, View*, int> Station;
    }
}

/// <summary>client_stations_logic.h's game arithmetic in C#: the HUD's remaining time (client 26430625 0xb2d6e0, saturating
/// at the i64 extremes) and the scaled craft. Parity with the native host test: Kiln.TestKit.Tests ClientViewsFakeTests.</summary>
internal static class ClientStationMath
{
    public const long Max = long.MaxValue, Min = long.MinValue;

    public static long ScaledNs(long durationNs, float factor)
    {
        if (!(factor > 0f) || float.IsInfinity(factor) || durationNs <= 0) return 0;
        var sec = (float)(durationNs * 1e-9);
        var ns = (double)(sec * factor) * 1e9;
        if (!(ns < 9.2233720368547758e18) || !(ns >= -9.2233720368547758e18)) return Min;
        return (long)ns;
    }

    public static long GameRemaining(long craft, uint crafts, long start, long now)
    {
        unchecked
        {
            long total = crafts == 0 ? 0 : craft is Max or Min ? craft : (long)((ulong)craft * crafts);
            long elapsed = now == Max ? (start != Max ? Max : 0)
                : now == Min ? (start != Min ? Min : 0)
                : start == Max ? Min
                : start == Min ? Max
                : (long)((ulong)now - (ulong)start);
            if (total == Max) total = craft != Min ? Max : 0;
            else if (total == Min) total = craft != Max ? Min : 0;
            else if (craft == Max) return elapsed != Max ? Max : 0;
            else if (craft == Min) return elapsed != Min ? Min : 0;
            else total = (long)((ulong)total + (ulong)craft);
            if (total == Max) return elapsed != Max ? Max : 0;
            if (total == Min) return elapsed != Min ? Min : 0;
            if (elapsed == Max) return Min;
            if (elapsed == Min) return Max;
            return (long)((ulong)total - (ulong)elapsed);
        }
    }
}

internal sealed class StationsClientState
{
    internal readonly object Gate = new();
    internal readonly List<FakeClientStation> Stations = [];
    internal readonly Dictionary<uint, (long DurationNs, uint[] Outputs, uint[] Inputs)> Recipes = [];
    internal long ClockNs;
    internal float Factor = 1f;
    internal int Reads;
}

internal sealed unsafe class StationsClientFake : IKitFake
{
    public string Service => StationsClientAbi.Service;
    private static StationsClientAbi.Table* _table;
    private static KilnHarness? Harness => FakeTables.Active(StationsClientAbi.Service);
    internal static StationsClientState? State => Harness is { } h ? KitFakes.State<StationsClientState>(h) : null;

    public void Register()
    {
        if (_table == null)
        {
            _table = (StationsClientAbi.Table*)NativeMemory.AllocZeroed((nuint)sizeof(StationsClientAbi.Table));  // process lifetime
            _table->Header = new ServiceHeader { Size = (uint)sizeof(StationsClientAbi.Table), Version = StationsClientAbi.Version };
            _table->GetStatus = &GetStatus;
            _table->Stations = &Stations;
            _table->Station = &Station;
        }
        KitBackend.Instance.RegisterTable(Service, (nint)_table);
        FakeTables.Features([ClientStations.Capability]);
    }

    public void Start(KilnHarness harness) { }
    public void BeforeStep(KilnHarness harness, ulong step) { }

    public void End(KilnHarness harness)
    {
        var s = KitFakes.State<StationsClientState>(harness);
        lock (s.Gate)
        {
            s.Stations.Clear();
            s.Recipes.Clear();
        }
    }

    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new CoverageRow("The client's station view (since 0.2.0-dev.31.13)",
        CoverageKind.RecordingFake, [StationsClientAbi.Service, ClientStations.Capability],
        "Kiln.Blocks' ClientStations on the kit's kiln.stations.client: the stations a test puts in the client's cache " +
        "(KilnHarness.ClientStations.Add: workshop, base, position, state, selected recipe and choices, crafts loaded, water, " +
        "the cycle's start, the station's inventory or none), the client's recipes (SetRecipe) and clock (Clock, SpeedFactor); " +
        "times, progress and finished outputs computed as kiln-items.dll computes them (the game HUD's arithmetic, only while " +
        "Running). The row stations.client_view can be switched off. Offline: the game's cache itself (which stations the " +
        "server sends, its update timing), the demand window and the copy's age (every read answers the current script)"));

    internal static StationsClientAbi.View ViewOf(FakeClientStation s, StationsClientState st, ulong step)
    {
        StationsClientAbi.View v = default;
        v.Size = (uint)sizeof(StationsClientAbi.View);
        v.Entity = s.Station.Value;
        v.Workshop = s.Workshop;
        v.SelectedRecipe = s.Recipe;
        var g = s.Base.Value.ToByteArray();
        for (var i = 0; i < 16; i++) v.Base[i] = g[i];
        // the cache's 0.25 m steps
        v.X = (float)(Math.Round(s.Position.X * 4) / 4);
        v.Y = (float)(Math.Round(s.Position.Y * 4) / 4);
        v.Z = (float)(Math.Round(s.Position.Z * 4) / 4);
        v.State = (byte)s.State;
        v.Needs = (byte)s.Needs;
        v.UseAllOptions = s.UseAllOptions ? (byte)1 : (byte)0;
        var slot = 0;
        foreach (var x in s.Choices.Take(4))
        {
            v.Choices[2 * slot] = x.Category;
            v.Choices[2 * slot + 1] = (byte)x.Index;
            slot++;
        }
        v.CraftsLoaded = (uint)Math.Max(0, s.CraftsLoaded);
        v.Water = s.Water;
        v.WaterMax = s.WaterMax;
        v.RecipeStartNs = s.Started.Ticks * 100;
        v.NowNs = st.ClockNs;
        v.Step = step;
        if (s.NeedsEnergy) v.Flags |= StationsClientAbi.FlagEnergy;
        (long DurationNs, uint[] Outputs, uint[] Inputs) recipe = default;
        var found = s.Recipe != 0 && st.Recipes.TryGetValue(s.Recipe, out recipe);
        if (found)
        {
            var craft = ClientStationMath.ScaledNs(recipe.DurationNs, st.Factor);
            if (craft > 0)
            {
                v.Flags |= StationsClientAbi.FlagRecipe;
                v.CraftNs = craft;
                if (s.State == StationState.Running)
                {
                    var all = ClientStationMath.GameRemaining(craft, v.CraftsLoaded, v.RecipeStartNs, v.NowNs);
                    var one = ClientStationMath.GameRemaining(craft, 0, v.RecipeStartNs, v.NowNs);
                    if (all is not (long.MaxValue or long.MinValue) && one is not (long.MaxValue or long.MinValue))
                    {
                        v.Flags |= StationsClientAbi.FlagTime;
                        v.RemainingNs = Math.Max(0, all);
                        v.CraftRemainingNs = Math.Max(0, one);
                        v.Progress = (float)Math.Clamp(1.0 - (double)v.CraftRemainingNs / craft, 0.0, 1.0);
                    }
                }
            }
            if (s.Inventory is { } inv)
            {
                v.Flags |= StationsClientAbi.FlagOutputs;
                uint n = 0;
                foreach (var item in recipe.Outputs)
                {
                    if (item == 0 || recipe.Inputs.Contains(item) || n == StationsClientAbi.MaxOutputs) continue;
                    var seen = false;
                    for (var k = 0; k < n; k++) seen |= v.Outputs[2 * k] == item;
                    if (seen) continue;
                    var sum = inv.Where(x => x.Item.Value == item).Sum(x => (long)Math.Max(0, x.Count));
                    if (sum == 0) continue;
                    v.Outputs[2 * n] = item;
                    v.Outputs[2 * n + 1] = (uint)Math.Min(sum, uint.MaxValue);
                    n++;
                }
                v.OutputCount = n;
            }
        }
        return v;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GetStatus(StationsClientAbi.Status* o)
    {
        if (o == null || o->Size != sizeof(StationsClientAbi.Status)) return FakeTables.InvalidArgument;
        var h = Harness;
        var st = State;
        var off = FakeTables.Off(ClientStations.Capability) || st is null;
        *o = default;
        o->Size = (uint)sizeof(StationsClientAbi.Status);
        o->On = off ? (byte)0 : (byte)1;
        o->World = h is null ? (byte)0 : (byte)1;
        if (st is not null)
            lock (st.Gate)
            {
                o->Copying = st.Reads > 0 ? (byte)1 : (byte)0;
                o->Rows = (uint)st.Stations.Count;
            }
        o->Step = h?.CurrentStep ?? 0;
        o->AgeMs = st is null ? -1 : 0;
        var text = off ? "switched off by the test" : "";
        var bytes = System.Text.Encoding.UTF8.GetBytes(text);
        for (var i = 0; i < Math.Min(bytes.Length, 159); i++) o->Detail[i] = bytes[i];
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Stations(StationsClientAbi.View* o, uint capacity, uint* count)
    {
        if (count == null || (capacity != 0 && o == null)) return FakeTables.InvalidArgument;
        *count = 0;
        for (var i = 0; i < capacity; i++)
            if (o[i].Size != sizeof(StationsClientAbi.View)) return FakeTables.InvalidArgument;
        var st = State;
        if (st is null || FakeTables.Off(ClientStations.Capability)) return FakeTables.Unavailable;
        var step = Harness?.CurrentStep ?? 0;
        lock (st.Gate)
        {
            st.Reads++;
            *count = (uint)st.Stations.Count;
            var n = Math.Min(capacity, (uint)st.Stations.Count);
            for (var i = 0; i < n; i++) o[i] = ViewOf(st.Stations[i], st, step);
            return n < st.Stations.Count ? FakeTables.BufferTooSmall : FakeTables.Ok;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Station(uint entity, StationsClientAbi.View* o)
    {
        if (o == null || o->Size != sizeof(StationsClientAbi.View)) return FakeTables.InvalidArgument;
        var st = State;
        if (st is null || FakeTables.Off(ClientStations.Capability)) return FakeTables.Unavailable;
        lock (st.Gate)
        {
            st.Reads++;
            foreach (var s in st.Stations)
                if (s.Station.Value == entity)
                {
                    *o = ViewOf(s, st, Harness?.CurrentStep ?? 0);
                    return FakeTables.Ok;
                }
        }
        return FakeTables.NotFound;
    }
}
