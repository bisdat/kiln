// SPDX-License-Identifier: MIT
// Kiln.Stations offline, on a fake kiln.stations: the native mirrors' layouts, the requests built from the options (fill,
// stop, resume, select), results and plans from native records, the bases and stations read, the event pump (the watch
// follows the subscriptions, records become events, a mod's own write is named), a finished cycle and a player's station
// button typed, and the argument, thread and capability errors. What the game does is the lab test
// (mods/tests/stations-test); the native logic is native/services/items/tests/stations_test.cpp.
using System.Runtime.InteropServices;
using Kiln;
using Kiln.TestKit;
using Xunit;

[assembly: CollectionBehavior(DisableTestParallelization = true)]
namespace Kiln.StationsTests;

public sealed unsafe class StationsTests : IDisposable
{
    internal static readonly Guid AltarGuid = new("0b0c0d0e-0102-0304-0506-0708090a0b0c");

    // A fake kiln.stations: what Kiln asked, and what it answers.
    internal sealed class Fake : IStationsBackend
    {
        public (bool Read, bool Fill, bool Control, bool Events, string Detail)? State = (true, true, true, true, "");
        public readonly List<StationsNative.Base> BaseRows = [];
        public readonly Dictionary<uint, StationsNative.Station> StationRows = new();
        public readonly Dictionary<uint, StationsNative.Recipe> Recipes = new();
        public readonly Dictionary<uint, uint[]> Storage = new(), Water = new();
        public StationsNative.Stack[] Stock = [];
        public readonly List<(StationsNative.Request Request, StationsNative.Stack[] Keep, ulong Owner, long Key)> Submitted = [];
        public readonly List<bool> Watches = [];
        public readonly List<StationsNative.Event> Ring = [];
        public StationsNative.Plan NextPlan;
        public StationsNative.Request LastPlanned;
        public int SubmitResult;

        public (bool Read, bool Fill, bool Control, bool Events, string Detail)? Status() => State;

        public int BaseCount(out uint n)
        {
            n = (uint)BaseRows.Count;
            return 0;
        }

        public int BaseAt(uint index, out StationsNative.Base b)
        {
            b = index < BaseRows.Count ? BaseRows[(int)index] : default;
            return index < BaseRows.Count ? 0 : StationsNative.NotFound;
        }

        public int BaseOf(uint entity, out StationsNative.Base b)
        {
            b = default;
            if (!StationRows.TryGetValue(entity, out var s) || s.BaseIndex < 0) return StationsNative.NotFound;
            return BaseAt((uint)s.BaseIndex, out b);
        }

        public int BaseAtPosition(WorldPosition p, out StationsNative.Base b)
        {
            foreach (var x in BaseRows)
                if (p.X >= x.Min.X && p.X < x.Max.X && p.Z >= x.Min.Z && p.Z < x.Max.Z)
                {
                    b = x;
                    return 0;
                }
            b = default;
            return StationsNative.NotFound;
        }

        public (int Rc, uint[] Ids) BaseList(uint index, bool water) =>
            (water ? Water : Storage).TryGetValue(index, out var ids) ? (0, ids) : (StationsNative.NotFound, []);

        public (int Rc, StationsNative.Stack[] Rows) BaseStock(uint index) => (0, Stock);

        public (int Rc, uint[] Ids) StationIds(int baseIndex) =>
            (0, StationRows.Values.Where(s => baseIndex < 0 || s.BaseIndex == baseIndex).Select(s => s.Entity).ToArray());

        public int StationRead(uint entity, out StationsNative.Station s) =>
            StationRows.TryGetValue(entity, out s) ? 0 : StationsNative.NotFound;

        public int Recipe(uint id, out StationsNative.Recipe r) => Recipes.TryGetValue(id, out r) ? 0 : StationsNative.NotFound;

        public (int Rc, StationsNative.Stack[] Rows) RecipeInputs(uint id, StationsNative.Choice[] choices)
        {
            if (!Recipes.TryGetValue(id, out var r)) return (StationsNative.NotFound, []);
            var rows = new List<StationsNative.Stack>();
            for (var i = 0; i < r.InputCount; i++)
            {
                var cat = r.Inputs[i * 4 + 2];
                var item = cat == 0 ? r.Inputs[i * 4] : 0x7000u + choices.FirstOrDefault(c => c.Category == cat).Index;
                rows.Add(new StationsNative.Stack { Item = item, Count = cat == 0 ? r.Inputs[i * 4 + 1] : r.Inputs[i * 4 + 3] });
            }
            return (0, rows.ToArray());
        }

        public (int Rc, (uint Item, float Scaling)[] Options) Category(uint id) =>
            id == 77 ? (0, [(0x7000u, 1f), (0x7001u, 0.5f)]) : (StationsNative.NotFound, []);

        public int CategoryAt(uint index, out uint id)
        {
            id = index == 0 ? 77u : 0;
            return index == 0 ? 0 : StationsNative.NotFound;
        }

        public (int Rc, uint[] Ids) WorkshopRecipes(uint workshop) => (0, Recipes.Values.Where(r => r.Workshop == workshop).Select(r => r.Id).ToArray());

        public int Plan(in StationsNative.Request r, StationsNative.Stack[] keep, out StationsNative.Plan p)
        {
            LastPlanned = r;
            p = NextPlan;
            return 0;
        }

        public int Submit(in StationsNative.Request r, StationsNative.Stack[] keep, ulong owner, long key)
        {
            if (SubmitResult != 0) return SubmitResult;
            Submitted.Add((r, keep, owner, key));
            return 0;
        }

        public int Watch(bool on)
        {
            Watches.Add(on);
            return 0;
        }

        public ulong LastSeq() => Ring.Count == 0 ? 0 : Ring[^1].Seq;

        public int Events(ulong after, Span<StationsNative.Event> into, out uint count)
        {
            var rest = Ring.Where(e => e.Seq > after).ToArray();
            var n = Math.Min(rest.Length, into.Length);
            for (var i = 0; i < n; i++) into[i] = rest[i];
            count = (uint)n;
            return rest.Length > n ? StationsNative.BufferTooSmall : 0;
        }

        public void Push(uint kind, uint station, byte before = 0, byte after = 0, ulong by = 0, int baseIndex = 0)
        {
            var seq = (ulong)Ring.Count + 1;
            Ring.Add(new StationsNative.Event
            {
                Size = (uint)sizeof(StationsNative.Event), Kind = kind, Seq = seq, Step = 100 + seq, Station = station, Workshop = 0x51,
                StateBefore = before, StateAfter = after, StoppedAfter = (byte)(after == (byte)StationState.Stopped ? 1 : 0),
                SelectedAfter = 0x1234, BaseIndex = baseIndex, By = by,
            });
        }
    }

    private readonly Fake _fake = new();

    public StationsTests()
    {
        StationsCore.UseBackend(_fake);
        var b = new StationsNative.Base
        {
            Size = (uint)sizeof(StationsNative.Base), Index = 0, AltarCount = 2, StorageCount = 2, WaterCount = 1, ZoneCount = 2, StationCount = 1,
            Min = new StationsNative.Vec3 { X = -10, Y = -5, Z = -10 }, Max = new StationsNative.Vec3 { X = 10, Y = 30, Z = 10 },
        };
        b.Id = StationsNative.IdOf(AltarGuid);
        b.Altars[0] = 900;
        b.Altars[1] = 901;
        _fake.BaseRows.Add(b);
        _fake.Storage[0] = [700, 701];
        _fake.Water[0] = [702];
        _fake.Stock = [new StationsNative.Stack { Item = 0x10, Count = 40 }, new StationsNative.Stack { Item = 0x11, Count = 3 }];
        var s = new StationsNative.Station
        {
            Size = (uint)sizeof(StationsNative.Station), Entity = 3300, Workshop = 0x51, BaseIndex = 0, State = (byte)StationState.Running,
            Needs = 0, RunningRecipe = 0x1234, SelectedRecipe = 0x1234, CraftsLoaded = 3, DurationNs = 30_000_000_000, Progress = 0.5f,
            Position = new StationsNative.Vec3 { X = 1, Y = 2, Z = 3 }, S0 = new StationsNative.Choice { Category = 77, Index = 1 },
        };
        s.Base = b.Id;
        _fake.StationRows[3300] = s;
        var r = new StationsNative.Recipe
        {
            Size = (uint)sizeof(StationsNative.Recipe), Id = 0x1234, Workshop = 0x51, InputCount = 2, OutputCount = 1, WaterCharges = 0,
            DurationNs = 30_000_000_000,
        };
        r.Inputs[0] = 0x10;
        r.Inputs[1] = 2;
        r.Inputs[4 + 2] = 77;
        r.Inputs[4 + 3] = 1;
        r.Outputs[0] = 0x20;
        r.Outputs[1] = 1;
        _fake.Recipes[0x1234] = r;
    }

    public void Dispose() => StationsCore.UseBackend(null);

    // The calls return a task but throw before one exists: run them as a plain call.
    private static void Call(Func<Task> f) => _ = f();

    private static KilnHarness Kiln(bool caps = true)
    {
        var k = new KilnHarness();
        if (caps)
            foreach (var c in new[] { Stations.ReadCapability, Stations.FillCapability, Stations.ControlCapability, Stations.EventsCapability })
                k.Capabilities.Override(c, true);
        return k;
    }

    [Fact]
    public void TheMirrorsKeepTheNativeLayouts()
    {
        // native/core/abi/services/kiln_stations.h; stations_test.cpp asserts the same sizes and offsets
        Assert.Equal(16, sizeof(StationsNative.BaseId));
        Assert.Equal(8, sizeof(StationsNative.Choice));
        Assert.Equal(144, sizeof(StationsNative.Base));
        Assert.Equal(80, (int)Marshal.OffsetOf<StationsNative.Base>("Min"));
        Assert.Equal(200, sizeof(StationsNative.Station));
        Assert.Equal(16, sizeof(StationsNative.Input));
        Assert.Equal(624, sizeof(StationsNative.Recipe));
        Assert.Equal(88, sizeof(StationsNative.Request));
        Assert.Equal(24, (int)Marshal.OffsetOf<StationsNative.Request>("C0"));
        Assert.Equal(64, (int)Marshal.OffsetOf<StationsNative.Request>("Keep"));
        Assert.Equal(32, sizeof(StationsNative.MoveChange));
        Assert.Equal(1224, sizeof(StationsNative.Result));
        Assert.Equal(56, (int)Marshal.OffsetOf<StationsNative.Result>("Ticket"));
        Assert.Equal(72, (int)Marshal.OffsetOf<StationsNative.Result>("Changes"));
        Assert.Equal(1096, (int)Marshal.OffsetOf<StationsNative.Result>("Message"));
        Assert.Equal(680, sizeof(StationsNative.Plan));
        Assert.Equal(40, (int)Marshal.OffsetOf<StationsNative.Plan>("Inputs"));
        Assert.Equal(96, sizeof(StationsNative.Event));
        Assert.Equal(64, (int)Marshal.OffsetOf<StationsNative.Event>("By"));
        Assert.Equal(72, (int)Marshal.OffsetOf<StationsNative.Event>("Position"));
        Assert.Equal(188, sizeof(StationsNative.Status));
        Assert.Equal(8 + 21 * 8, sizeof(StationsNative.Table));
    }

    [Fact]
    public void TheEnumsKeepTheNativeValues()
    {
        Assert.Equal(new[] { 0, 1, 2, 3, 4, 5 }, Enum.GetValues<StationState>().Select(v => (int)v));
        Assert.Equal(12, (int)StationActionStatus.Dropped);
        Assert.Equal(9, (int)StationActionStatus.Locked);
        Assert.Equal(StationActionStatus.Refused, StationsCore.StatusOf(99));
        Assert.Equal(StopWhen.Now, default(StopWhen));
    }

    [Fact]
    public void TheBaseIdKeepsTheGuidBytes()
    {
        var id = StationsNative.IdOf(AltarGuid);
        Assert.Equal(AltarGuid, StationsNative.GuidOf(id));
        Assert.True(BaseId.None.IsNone);
        Assert.Equal("none", BaseId.None.ToString());
    }

    [Fact]
    public void BasesAreReadWithTheirAltarsChestsWaterAndStock()
    {
        using var kiln = Kiln();
        var all = kiln.Run(() => Bases.All);
        var b = Assert.Single(all);
        Assert.Equal(new BaseId(AltarGuid), b.Id);
        Assert.Equal([new EntityId(900), new EntityId(901)], b.Altars);
        Assert.Equal((2, 1, 2, 1), (b.StorageCount, b.WaterCount, b.ZoneCount, b.StationCount));
        Assert.True(b.InBox(new WorldPosition(0, 0, 0)));
        Assert.False(b.InBox(new WorldPosition(20, 0, 0)));
        Assert.Equal([new EntityId(700), new EntityId(701)], kiln.Run(() => Bases.Storage(b.Id)));
        Assert.Equal([new EntityId(702)], kiln.Run(() => Bases.WaterSources(b.Id)));
        var stock = kiln.Run(() => Bases.Stock(b.Id));
        Assert.Equal(40, stock[new ItemId(0x10)]);
        Assert.Equal(3, kiln.Run(() => Bases.Stock(b.Id, new ItemId(0x11))));
        Assert.Equal(0, kiln.Run(() => Bases.Stock(b.Id, new ItemId(0x99))));
        Assert.Equal(b.Id, kiln.Run(() => Bases.Of(new EntityId(3300)))!.Id);
        Assert.Equal(b.Id, kiln.Run(() => Bases.At(new WorldPosition(1, 0, 1)))!.Id);
        Assert.Null(kiln.Run(() => Bases.At(new WorldPosition(100, 0, 1))));
        Assert.Null(kiln.Run(() => Bases.Get(new BaseId(Guid.NewGuid()))));
        Assert.Throws<KeyNotFoundException>(() => kiln.Run(() => Bases.Storage(new BaseId(Guid.NewGuid()))));
    }

    [Fact]
    public void AStationIsReadWithItsRecipeAndChoices()
    {
        using var kiln = Kiln();
        var s = kiln.Run(() => Stations.Read(new EntityId(3300)))!;
        Assert.Equal((0x51u, StationState.Running, 3, 0x1234u), (s.Workshop, s.State, s.CraftsLoaded, s.SelectedRecipe));
        Assert.Equal(new BaseId(AltarGuid), s.Base);
        Assert.Equal(TimeSpan.FromSeconds(30), s.Duration);
        Assert.Equal([new CategoryChoice(77, 1)], s.SelectedChoices);
        Assert.Null(s.Water);
        Assert.True(s.IsRunning);
        Assert.Null(kiln.Run(() => Stations.Read(new EntityId(5))));
        Assert.True(kiln.Run(() => Stations.IsStation(new EntityId(3300))));
        Assert.False(kiln.Run(() => Stations.IsStation(new EntityId(5))));
        Assert.Equal([new EntityId(3300)], kiln.Run(() => Stations.All()));
        Assert.Equal([new EntityId(3300)], kiln.Run(() => Stations.InBase(new BaseId(AltarGuid))));
        Assert.Equal([0x1234u], kiln.Run(() => Stations.RecipesOf(new EntityId(3300))));

        var r = kiln.Run(() => Stations.Recipe(0x1234))!;
        Assert.Equal(2, r.Inputs.Count);
        Assert.False(r.Inputs[0].IsCategory);
        Assert.True(r.Inputs[1].IsCategory);
        Assert.Equal(new StationStack(new ItemId(0x20), 1), Assert.Single(r.Outputs));
        Assert.True(r.AlwaysUnlocked);
        Assert.Null(kiln.Run(() => Stations.Recipe(0)));
        Assert.Null(kiln.Run(() => Stations.Recipe(0x9999)));

        var cat = Assert.Single(kiln.Run(() => Stations.Categories));
        Assert.Equal(77u, cat.Id);
        Assert.Equal(new IngredientOption(new ItemId(0x7001), 0.5f), cat.Options[1]);
        var inputs = kiln.Run(() => Stations.ResolveInputs(0x1234, [new CategoryChoice(77, 1)]));
        Assert.Equal([new StationStack(new ItemId(0x10), 2), new StationStack(new ItemId(0x7001), 1)], inputs);
        Assert.Throws<KeyNotFoundException>(() => kiln.Run(() => Stations.ResolveInputs(0x9999)));
    }

    [Fact]
    public void OutputSlotsAreTheRecipesOutputsThatAreNoInputs()
    {
        var recipe = new StationRecipe(1, 0x51, [new StationInput(new ItemId(0x10), 2, 0, 0), new StationInput(default, 0, 77, 1)],
            [new StationStack(new ItemId(0x20), 1), new StationStack(new ItemId(0x10), 1)], TimeSpan.FromSeconds(1), 0, true);
        var holder = new EntityId(3300);
        InventorySlot Slot(int i, uint item, int count) => new(SlotId.Of(holder, i), new ItemId(item), count, default, 1, ItemRarity.Common, default, 0, true, false, false, 0);
        var slots = new[] { Slot(0, 0x10, 4), Slot(1, 0x20, 5), Slot(2, 0, 0) };
        var outs = StationsCore.OutputSlots(slots, recipe, null);
        Assert.Equal(new ItemId(0x20), Assert.Single(outs).Item);
        Assert.Empty(StationsCore.OutputSlots(slots, null, null));
    }

    [Fact]
    public void FillOptionsBecomeTheRequest()
    {
        var (r, keep) = StationsCore.FillRequest(new EntityId(3300), new FillOptions
        {
            Crafts = 10, Mode = FillMode.Add, Exact = true, DryRun = true, Water = false, Recipe = 0x1234,
            Choices = [new CategoryChoice(77, 1)], Player = new EntityId(1),
            Keep = new Dictionary<ItemId, long> { [new ItemId(0x10)] = 20, [new ItemId(0x11)] = 0 },
        });
        Assert.Equal(StationsNative.Fill, r.Action);
        Assert.Equal(StationsNative.FillAdd | StationsNative.FillExact | StationsNative.DryRun | StationsNative.NoWater, r.Flags);
        Assert.Equal((3300u, 10u, 0x1234u, 1u), (r.Station, r.Crafts, r.RecipeId, r.Player));
        Assert.Equal((77u, (byte)1), (r.C0.Category, r.C0.Index));
        Assert.Equal(0u, r.C1.Category);
        var k = Assert.Single(keep);  // a reserve of 0 is no reserve
        Assert.Equal((0x10u, 20u), (k.Item, k.Count));
        Assert.Equal(1u, r.KeepCount);

        var (d, _) = StationsCore.FillRequest(new EntityId(3300), new FillOptions());
        Assert.Equal((0u, 1u, 0u, 0u), (d.Flags, d.Crafts, d.RecipeId, d.Player));  // up to 1 craft, water on, no player's bag

        Assert.Throws<ArgumentException>(() => StationsCore.FillRequest(new EntityId(3300), new FillOptions { Crafts = 0 }));
        Assert.Throws<ArgumentException>(() => StationsCore.FillRequest(new EntityId(3300), new FillOptions { Crafts = 1001 }));
        Assert.Throws<ArgumentException>(() => StationsCore.FillRequest(EntityId.None, new FillOptions()));
        Assert.Throws<ArgumentException>(() => StationsCore.FillRequest(new EntityId(3300), new FillOptions { Choices = [new CategoryChoice(77, 0)] }));
        Assert.Throws<ArgumentException>(() => StationsCore.FillRequest(new EntityId(3300), new FillOptions
        {
            Keep = Enumerable.Range(1, 65).ToDictionary(i => new ItemId((uint)i), _ => 1L),
        }));
        Assert.Throws<ArgumentException>(() => StationsCore.ChoicesOf([new(1, 0), new(2, 0), new(3, 0), new(4, 0), new(5, 0)]));
        Assert.Throws<ArgumentException>(() => StationsCore.ChoicesOf([new(1, 256)]));
    }

    [Fact]
    public void APlanIsReadFromTheNativeRecord()
    {
        using var kiln = Kiln();
        var p = new StationsNative.Plan
        {
            Size = (uint)sizeof(StationsNative.Plan), Status = (int)StationActionStatus.Partial, Station = 3300, RecipeId = 0x1234, CraftsLoaded = 3,
            CraftsAffordable = 4, CraftsPlanned = 4, InputCount = 1,
        };
        p.Inputs[0] = 0x10;
        p.Inputs[1] = 2;
        p.Have[0] = 6;
        p.Stock[0] = 8;
        _fake.NextPlan = p;
        var plan = kiln.Run(() => Stations.Plan(new EntityId(3300), new FillOptions { Crafts = 10 }));
        Assert.Equal(10u, _fake.LastPlanned.Crafts);
        Assert.True(plan.Ok);
        Assert.Null(plan.Reason);
        Assert.Equal((3, 4, 4), (plan.CraftsLoaded, plan.CraftsAffordable, plan.CraftsPlanned));
        Assert.Equal(new FillInput(new ItemId(0x10), 2, 6, 8), Assert.Single(plan.Inputs));
        var none = p;
        none.Status = (int)StationActionStatus.NotEnough;
        var why = System.Text.Encoding.UTF8.GetBytes("2 of 9 x 0x00000011 for one craft");
        for (var i = 0; i < why.Length; i++) none.Message[i] = why[i];
        _fake.NextPlan = none;
        var refused = kiln.Run(() => Stations.Plan(new EntityId(3300)));
        Assert.False(refused.Ok);
        Assert.Equal("2 of 9 x 0x00000011 for one craft", refused.Reason);
    }

    [Fact]
    public void StopResumeAndSelectSendTheButtonsRequests()
    {
        using var kiln = Kiln();
        var station = new EntityId(3300);
        Call(() => kiln.Run(() => Stations.Stop(station)));
        Call(() => kiln.Run(() => Stations.Stop(station, StopWhen.AfterCurrentCraft)));
        Call(() => kiln.Run(() => Stations.Resume(station)));
        Call(() => kiln.Run(() => Stations.Select(station, 0x1234, [new CategoryChoice(77, 1)], keepStopped: true)));
        Call(() => kiln.Run(() => Stations.Select(station, 0x5678, allowLocked: true)));
        var r = _fake.Submitted.Select(x => x.Request).ToArray();
        Assert.Equal(5, r.Length);
        Assert.Equal((StationsNative.Stop, 0u), (r[0].Action, r[0].Flags));  // the button's meaning: now
        Assert.Equal((StationsNative.Stop, StationsNative.AfterCraft), (r[1].Action, r[1].Flags));
        Assert.Equal((StationsNative.Resume, 0u), (r[2].Action, r[2].Flags));
        Assert.Equal((StationsNative.Select, StationsNative.KeepStopped, 0x1234u), (r[3].Action, r[3].Flags, r[3].RecipeId));
        Assert.Equal((77u, (byte)1), (r[3].C0.Category, r[3].C0.Index));
        Assert.Equal((StationsNative.Select, StationsNative.AllowLocked, 0x5678u), (r[4].Action, r[4].Flags, r[4].RecipeId));
        Assert.All(r, x => Assert.Equal(3300u, x.Station));
        Assert.Equal(_fake.Submitted.Count, _fake.Submitted.Select(x => x.Key).Distinct().Count());
    }

    [Fact]
    public void TheCallsCheckTheirArgumentsTheThreadAndTheCapability()
    {
        using (var off = Kiln(caps: false))
        {
            var e = off.Run(() => Assert.Throws<KilnUnavailableException>(() => Call(() => Stations.Fill(new EntityId(3300)))));
            Assert.Equal("stations.fill", e.Capability);
            e = off.Run(() => Assert.Throws<KilnUnavailableException>(() => Call(() => Stations.Stop(new EntityId(3300)))));
            Assert.Equal("stations.control", e.Capability);
            e = off.Run(() => Assert.Throws<KilnUnavailableException>(() => _ = Bases.All));
            Assert.Equal("stations.read", e.Capability);
        }
        using var kiln = Kiln();
        kiln.Run(() => Assert.Throws<ArgumentException>(() => Call(() => Stations.Fill(EntityId.None))));
        kiln.Run(() => Assert.Throws<ArgumentException>(() => Call(() => Stations.Stop(EntityId.None))));
        kiln.Run(() => Assert.Throws<ArgumentException>(() => Call(() => Stations.Select(new EntityId(3300), 1, [new CategoryChoice(1, 300)]))));
        Assert.Throws<InvalidOperationException>(() => Call(() => Stations.Resume(new EntityId(3300))));  // not the tick thread
        Assert.Throws<InvalidOperationException>(() => _ = Bases.All);
        // kiln.stations says why it is off
        _fake.State = (true, false, false, true, "the program kiln_items_stations was refused");
        var why = kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => Call(() => Stations.Fill(new EntityId(3300)))));
        Assert.Contains("refused", why.Message);
        _fake.State = null;
        Assert.False(Stations.Available);
        Assert.False(Bases.Available);
        kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => _ = Bases.All));
    }

    [Fact]
    public void TheWatchFollowsTheSubscriptionsAndRecordsBecomeEvents()
    {
        using var kiln = Kiln();
        _fake.Push(StationsNative.EvState, 3300, 4, 3);  // before the watch: not this handler's
        var states = new List<StationStateChangedEvent>();
        var placed = new List<StationPlacedEvent>();
        var removed = new List<StationRemovedEvent>();
        var bases = new List<BasesChangedEvent>();
        IDisposable? sub = null;
        kiln.Run(() =>
        {
            sub = Events.On<StationStateChangedEvent>(states.Add);
            Events.On<StationPlacedEvent>(placed.Add);
            Events.On<StationRemovedEvent>(removed.Add);
            Events.On<BasesChangedEvent>(bases.Add);
        });
        kiln.Run(StationsCore.PumpOnce);
        Assert.Equal([true], _fake.Watches);
        Assert.Empty(states);
        _fake.Push(StationsNative.EvState, 3300, (byte)StationState.Running, (byte)StationState.Stopped);
        _fake.Push(StationsNative.EvPlaced, 3301, baseIndex: 0);
        _fake.Push(StationsNative.EvRemoved, 3302, baseIndex: -1);
        _fake.Push(StationsNative.EvBases, 0, baseIndex: 2);
        kiln.Run(StationsCore.PumpOnce);
        var s = Assert.Single(states);
        Assert.Equal((StationState.Running, StationState.Stopped, true), (s.Before, s.After, s.Stopped));
        Assert.Null(s.ByMod);
        Assert.True(s.Involves(new EntityId(3300)));
        Assert.Equal(new EntityId(3301), Assert.Single(placed).Station);
        Assert.Equal(-1, Assert.Single(removed).BaseIndex);
        Assert.Equal(2, Assert.Single(bases).Count);
        kiln.Run(StationsCore.PumpOnce);  // each record once
        Assert.Single(states);
    }

    [Fact]
    public void ManyRecordsAreDrainedInOnePump()
    {
        using var kiln = Kiln();
        var n = 0;
        kiln.Run(() => Events.On<StationStateChangedEvent>(_ => n++));
        kiln.Run(StationsCore.PumpOnce);
        for (var i = 0; i < 100; i++) _fake.Push(StationsNative.EvState, 3300, 4, 1);
        kiln.Run(StationsCore.PumpOnce);
        Assert.Equal(100, n);
    }

    [Fact]
    public void ASubscriptionIsRefusedByNameWhileTheEventsAreOff()
    {
        using var kiln = Kiln();
        _fake.State = (true, true, true, false, "kiln.world is too old for the watch");
        var off = kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => Events.On<StationPlacedEvent>(_ => { })));
        Assert.Equal("stations.events", off.Capability);
        Assert.Contains("too old", off.Message);
    }

    [Fact]
    public void OnlyAStationsOwnSuccessfulCycleIsAStationCraft()
    {
        var station = new EntityId(3300);
        var ev = StationsCore.CraftedOf(station, station, true, 0x1234, 0x51, [(0x20u, 2)]);
        Assert.NotNull(ev);
        Assert.Equal(new StationStack(new ItemId(0x20), 2), Assert.Single(ev!.Outputs));
        Assert.Null(StationsCore.CraftedOf(station, station, false, 0x1234, 0x51, []));  // a failed attempt
        Assert.Null(StationsCore.CraftedOf(new EntityId(1), station, true, 0x1234, 0x51, []));  // a player's craft at a bench
        Assert.Null(StationsCore.CraftedOf(EntityId.None, EntityId.None, true, 0x1234, 0x51, []));
    }

    [Fact]
    public void APlayersCraftingActionIsDecoded()
    {
        var p = new byte[68];
        BitConverter.TryWriteBytes(p.AsSpan(0), 3300u);
        BitConverter.TryWriteBytes(p.AsSpan(4), 0x1234u);
        BitConverter.TryWriteBytes(p.AsSpan(0x0c), (ushort)7);
        BitConverter.TryWriteBytes(p.AsSpan(0x18), 77u);
        p[0x18 + 4] = 1;
        p[0x38] = 1;
        BitConverter.TryWriteBytes(p.AsSpan(0x3c), 9u);
        p[0x40] = (byte)StationPlayerAction.Fill;
        var ev = StationsCore.Decode(new EntityId(1), p, canCancel: true)!;
        Assert.Equal((new EntityId(3300), StationPlayerAction.Fill, 0x1234u, 7), (ev.Station, ev.Action, ev.Recipe, ev.Amount));
        Assert.Equal([new CategoryChoice(77, 1)], ev.Choices);
        Assert.True(ev.UseAllOptions && ev.CanCancel);
        Assert.True(ev.Involves(new EntityId(1)) && ev.Involves(new EntityId(3300)));
        p[0x40] = 6;
        Assert.Null(StationsCore.Decode(new EntityId(1), p, true));  // not a type the game has
        Assert.Null(StationsCore.Decode(new EntityId(1), p.AsSpan(0, 0x40), true));
    }

    [Fact]
    public void AResultIsReadFromTheNativeRecord()
    {
        var r = NewResult(StationActionStatus.Partial, "the stock covered 4 of 10 crafts");
        r.ChangeCount = 1;
        var ch = (StationsNative.MoveChange*)r.Changes;
        ch[0] = new StationsNative.MoveChange { FromSlot = SlotId.Of(new EntityId(700), 3).Value, ToSlot = 0, Item = 0x10, Amount = 8 };
        var x = StationsCore.ResultOf(r, StationAction.Fill);
        Assert.True(x.Ok && x.Changed);
        Assert.Null(x.Reason);
        Assert.Equal((10, 3, 7, 4), (x.CraftsRequested, x.CraftsBefore, x.CraftsAfter, x.CraftsAdded));
        var c = Assert.Single(x.Changes);
        Assert.Equal((new ItemId(0x10), 8), (c.Item, c.Amount));
        Assert.Equal("the stock covered 4 of 10 crafts", x.Message);
        var locked = StationsCore.ResultOf(NewResult(StationActionStatus.Locked, "no one on this server has unlocked recipe 0x00005678"), StationAction.Select);
        Assert.False(locked.Ok);
        Assert.Contains("unlocked", locked.Reason);
        Assert.True(StationsCore.ResultOf(NewResult(StationActionStatus.Unchanged, ""), StationAction.Stop) is { Ok: true, Changed: false });
    }

    internal static StationsNative.Result NewResult(StationActionStatus status, string message, uint flags = 0)
    {
        var r = new StationsNative.Result
        {
            Size = (uint)sizeof(StationsNative.Result), Status = (int)status, Action = StationsNative.Fill, Flags = flags, Station = 3300,
            RecipeId = 0x1234, CraftsRequested = 10, CraftsBefore = 3, CraftsAfter = 7, CraftsAdded = 4,
            StateBefore = (byte)StationState.MissingMaterials, StateAfter = (byte)StationState.Running,
        };
        var b = System.Text.Encoding.UTF8.GetBytes(message);
        for (var i = 0; i < b.Length && i < 127; i++) r.Message[i] = b[i];
        return r;
    }

    internal Fake Backend => _fake;
}

// Submissions complete their tasks (await needs a safe context: this class is not unsafe).
public sealed class StationsSubmitTests : IDisposable
{
    private readonly StationsTests _fixture = new();
    private StationsTests.Fake Fake => _fixture.Backend;

    public void Dispose() => _fixture.Dispose();

    private static KilnHarness Kiln()
    {
        var k = new KilnHarness();
        foreach (var c in new[] { Stations.ReadCapability, Stations.FillCapability, Stations.ControlCapability, Stations.EventsCapability })
            k.Capabilities.Override(c, true);
        return k;
    }

    [Fact]
    public async Task AFillCompletesWithItsResultAndPostsTheChange()
    {
        using var kiln = Kiln();
        var changed = new List<StationChangedEvent>();
        kiln.Run(() => Events.On<StationChangedEvent>(changed.Add));
        var task = kiln.Run(() => Stations.Fill(new EntityId(3300), new FillOptions { Crafts = 10 }));
        var (r, _, _, key) = Assert.Single(Fake.Submitted);
        Assert.Equal((StationsNative.Fill, 10u), (r.Action, r.Crafts));
        Assert.False(task.IsCompleted);
        kiln.Run(() => StationsCore.Complete(key, StationsTests.NewResult(StationActionStatus.Partial, "4 of 10")));
        var result = await task.WaitAsync(TimeSpan.FromSeconds(5), TestContext.Current.CancellationToken);
        Assert.Equal(StationActionStatus.Partial, result.Status);
        Assert.Equal(StationAction.Fill, result.Action);
        kiln.Run(() => { });  // the notices go out at the step's end
        Assert.Equal(StationAction.Fill, Assert.Single(changed).Action);
        // a second completion of the same key does nothing
        kiln.Run(() => StationsCore.Complete(key, StationsTests.NewResult(StationActionStatus.Done, "")));
    }

    [Fact]
    public async Task ADryRunOrAnUnchangedResultPostsNoChange()
    {
        using var kiln = Kiln();
        var changed = new List<StationChangedEvent>();
        kiln.Run(() => Events.On<StationChangedEvent>(changed.Add));
        var dry = kiln.Run(() => Stations.Fill(new EntityId(3300), new FillOptions { DryRun = true }));
        var stop = kiln.Run(() => Stations.Stop(new EntityId(3300)));
        kiln.Run(() => StationsCore.Complete(Fake.Submitted[0].Key, StationsTests.NewResult(StationActionStatus.Done, "", StationsNative.DryRun)));
        kiln.Run(() => StationsCore.Complete(Fake.Submitted[1].Key, StationsTests.NewResult(StationActionStatus.Unchanged, "already stopped")));
        Assert.True((await dry).DryRun);
        Assert.Equal(StationActionStatus.Unchanged, (await stop).Status);
        kiln.Run(() => { });
        Assert.Empty(changed);
    }

    [Fact]
    public async Task AFullQueueIsARefusalAndAnOffServiceThrows()
    {
        using var kiln = Kiln();
        Fake.SubmitResult = StationsNative.BufferTooSmall;
        var full = await kiln.Run(() => Stations.Resume(new EntityId(3300)));
        Assert.Equal(StationActionStatus.Refused, full.Status);
        Assert.Contains("1024", full.Message);
        Fake.SubmitResult = StationsNative.Unavailable;
        var off = kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => { _ = Stations.Resume(new EntityId(3300)); }));
        Assert.Equal("stations.control", off.Capability);
        Fake.SubmitResult = StationsNative.InvalidArgument;
        kiln.Run(() => Assert.Throws<ArgumentException>(() => { _ = Stations.Fill(new EntityId(3300)); }));
    }

    [Fact]
    public async Task CollectWithNothingToCollectIsUnchanged()
    {
        using var kiln = Kiln();
        var r = await kiln.Run(() => Stations.Collect(new EntityId(3300), new EntityId(700)));
        Assert.Equal(StationActionStatus.Unchanged, r.Status);
        Assert.True(r.Ok);
        var none = await kiln.Run(() => Stations.Collect(new EntityId(5), new EntityId(700)));
        Assert.Equal(StationActionStatus.NotAStation, none.Status);
    }
}
