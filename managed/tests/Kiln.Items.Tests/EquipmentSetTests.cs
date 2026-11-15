// SPDX-License-Identifier: MIT
// Equipment sets offline (plan 10-equipment-sets; chapter 10 §4.2, §5, §6): the argument rules (a bad argument throws,
// a stale scope is a result), the frozen copies, the session JSON (schema 1) and the result model. What the game does
// with a set is the lab test (mods/tests/equipment-sets-test); the native snapshot, planner and executor have their host
// tests (native/services/items/tests).
using System.Collections.ObjectModel;
using System.Runtime.InteropServices;
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.ItemsTests;

public sealed unsafe class EquipmentSetTests
{
    private static readonly Guid Session = Guid.Parse("01234567-89ab-4cde-8123-0123456789ab");
    private static readonly EventEntityKey Player = new(1, true, 123, 9, 4, 20);
    private static readonly EquipmentSetScope Scope = new(Session, 7, Player);

    internal static ItemInstanceRef Ref(uint entity, uint item, uint stamp) =>
        new(new EntityRef(new EntityId(entity), IdentitySources.Stamp(EntitySourceOrder.Items, stamp)), new ItemId(item));

    private static EquipmentSet Set(params EquipmentSetEntry[] entries) => new(Scope, entries);

    private static EquipmentSetEntry E(EquipmentSlot slot, EquipmentSetSelection selection) => new(slot, selection);

    private static void Bad(EquipmentSet? set, string contains)
    {
        var e = Assert.Throws<ArgumentException>(() => EquipmentSetRules.Check(set, "set"));
        Assert.Contains(contains, e.Message);
    }

    [Fact]
    public void AValidSetPassesAndComesBackAsACopy()
    {
        var list = new List<EquipmentSetEntry>
        {
            E(EquipmentSlot.Ring0, new InstanceEquipment(Ref(2552, 0x79442800, 5))),
            E(EquipmentSlot.Arrow, new StackEquipment(new ItemId(77), SlotId.Of(new EntityId(2544), 3), 40)),
            E(EquipmentSlot.Ring1, new EmptyEquipment()),
        };
        var set = new EquipmentSet(Scope, list);
        list.Clear();  // the set kept a copy
        Assert.Equal(3, set.Entries.Count);
        var copy = EquipmentSetRules.Check(set, "set");
        Assert.Equal(set.Entries, copy);
        Assert.NotSame(set.Entries, copy);
        // `with` puts a list in without the record's freeze: the entry points copy again
        var mutable = new List<EquipmentSetEntry>(set.Entries);
        var swapped = set with { Entries = mutable };
        var checkedCopy = EquipmentSetRules.Check(swapped, "set");
        mutable.Clear();
        Assert.Equal(3, checkedCopy.Length);
    }

    [Fact]
    public void AllTwentySlotsAndAnEmptyListAreValid()
    {
        var all = Enum.GetValues<EquipmentSlot>().Select(s => E(s, new EmptyEquipment())).ToArray();
        Assert.Equal(20, all.Length);
        Assert.Equal(20, EquipmentSetRules.Check(Set(all), "set").Length);
        Assert.Empty(EquipmentSetRules.Check(Set(), "set"));
    }

    [Fact]
    public void TheArgumentMatrixThrows()
    {
        Assert.Throws<ArgumentNullException>(() => EquipmentSetRules.Check(null, "set"));
        Bad(new EquipmentSet(null!, []), "no scope");
        Bad(new EquipmentSet(Scope with { ProcessSession = Guid.Empty }, []), "process session");
        Bad(new EquipmentSet(Scope with { WorldEpoch = 0 }, []), "epoch 0");
        Bad(new EquipmentSet(Scope with { Player = default }, []), "no player");
        Bad(Set([null!]), "entry 0 is null");
        Bad(Set(E(EquipmentSlot.Ring0, null!)), "no selection");
        Bad(Set(E((EquipmentSlot)20, new EmptyEquipment())), "not an equipment slot");   // past Backpack: materials, main hand
        Bad(Set(E((EquipmentSlot)(-1), new EmptyEquipment())), "not an equipment slot");
        Bad(Set(E(EquipmentSlot.Ring0, new EmptyEquipment()), E(EquipmentSlot.Ring0, new EmptyEquipment())), "named twice");
        var ring = Ref(2552, 0x79442800, 5);
        Bad(Set(E(EquipmentSlot.Ring0, new InstanceEquipment(ring)), E(EquipmentSlot.Ring1, new InstanceEquipment(ring))), "selected twice");
        var src = SlotId.Of(new EntityId(2544), 3);
        Bad(Set(E(EquipmentSlot.Arrow, new StackEquipment(new ItemId(1), src, 4)), E(EquipmentSlot.Spell, new StackEquipment(new ItemId(2), src, 1))), "selected twice");
        Bad(Set(E(EquipmentSlot.Ring0, new InstanceEquipment(ItemInstanceRef.None))), "is none");
        Bad(Set(E(EquipmentSlot.Ring0, new InstanceEquipment(Ref(2552, 0, 5)))), "no item type");
        Bad(Set(E(EquipmentSlot.Ring0, new InstanceEquipment(Ref(7, 0x79442800, 5)))), "not an item entity id");   // a player id
        Bad(Set(E(EquipmentSlot.Ring0, new InstanceEquipment(new ItemInstanceRef(new EntityRef(new EntityId(2552), 0), new ItemId(5))))), "no kiln.items stamp");
        var spawnSerial = IdentitySources.Stamp(EntitySourceOrder.Spawn, 5);  // kiln.spawn's place: not a stamp the wire carries
        Bad(Set(E(EquipmentSlot.Ring0, new InstanceEquipment(new ItemInstanceRef(new EntityRef(new EntityId(2552), spawnSerial), new ItemId(5))))), "no kiln.items stamp");
        Bad(Set(E(EquipmentSlot.Arrow, new StackEquipment(ItemId.None, src, 4))), "no item type");
        Bad(Set(E(EquipmentSlot.Arrow, new StackEquipment(new ItemId(1), default, 4))), "no source slot");
        Bad(Set(E(EquipmentSlot.Arrow, new StackEquipment(new ItemId(1), new SlotId(1UL << 40 | 2544), 4))), "not an inventory slot");
        Bad(Set(E(EquipmentSlot.Arrow, new StackEquipment(new ItemId(1), new SlotId(3UL << 32), 4))), "not an inventory slot");
        Bad(Set(E(EquipmentSlot.Arrow, new StackEquipment(new ItemId(1), src, 0))), "must be positive");
        Bad(Set(E(EquipmentSlot.Arrow, new StackEquipment(new ItemId(1), src, -3))), "must be positive");
    }

    private sealed record OtherSelection : EquipmentSetSelection;

    [Fact]
    public void AnUnknownSelectionTypeThrows() => Bad(Set(E(EquipmentSlot.Ring0, new OtherSelection())), "not a selection Kiln knows");

    [Fact]
    public void AStaleScopeIsAReasonNotAnException()
    {
        Assert.Null(EquipmentSetRules.Stale(Scope, Session, 7, Player));
        Assert.Contains("another process", EquipmentSetRules.Stale(Scope, Guid.NewGuid(), 7, Player));
        Assert.Contains("another server world", EquipmentSetRules.Stale(Scope, Session, 8, Player));
        Assert.Contains("not 2's", EquipmentSetRules.Stale(Scope, Session, 7, Player with { Entity = 2 }));
        // the same slot id, another session of the player (a rejoin): never re-resolved
        Assert.Contains("left or rejoined", EquipmentSetRules.Stale(Scope, Session, 7, Player with { PlayerHandle = 68, JoinedStep = 900 }));
        Assert.Contains("left or rejoined", EquipmentSetRules.Stale(Scope, Session, 7, Player with { LifeSerial = 10 }));
    }

    [Fact]
    public void StampOfReadsOnlyKilnItemsStamps()
    {
        Assert.Equal(5u, EquipmentSetRules.StampOf(Ref(2552, 1, 5)));
        Assert.Equal(0u, EquipmentSetRules.StampOf(ItemInstanceRef.None));
        Assert.Equal(0u, EquipmentSetRules.StampOf(new ItemInstanceRef(new EntityRef(new EntityId(2552), IdentitySources.Stamp(EntitySourceOrder.Spawn, 5)), new ItemId(1))));
        // a stamp value above 32 bits is no native stamp
        Assert.Equal(0u, EquipmentSetRules.StampOf(new ItemInstanceRef(new EntityRef(new EntityId(2552), IdentitySources.Stamp(EntitySourceOrder.Items, 1UL << 33)), new ItemId(1))));
    }

    // ---- the session JSON ----

    private static EquipmentSet Sample() => Set(
        E(EquipmentSlot.Ring0, new InstanceEquipment(Ref(2552, 0x79442800, 5))),
        E(EquipmentSlot.Arrow, new StackEquipment(new ItemId(0xdeadbeef), SlotId.Of(new EntityId(2544), 3), 40)),
        E(EquipmentSlot.Ring1, new EmptyEquipment()),
        E(EquipmentSlot.ArmourUpperBody, new InstanceEquipment(Ref(0x7FFFFFFF, 1, uint.MaxValue))));

    private static void SameSet(EquipmentSet a, EquipmentSet b)
    {
        Assert.Equal(a.Scope, b.Scope);
        Assert.Equal(a.Entries, b.Entries);
    }

    [Fact]
    public void TheJsonRoundTripsEveryKind()
    {
        var set = Sample();
        var json = EquipmentSetJson.Serialize(set);
        SameSet(set, EquipmentSetJson.Deserialize(json));
        SameSet(set, EquipmentSetJson.Deserialize(EquipmentSetJson.Serialize(set, indented: true)));
        Assert.Contains("\"schema\":1", json);
        Assert.Contains("\"processSession\":\"01234567-89ab-4cde-8123-0123456789ab\"", json);
        Assert.Contains("\"slot\":\"ring0\"", json);
        Assert.Contains("\"slot\":\"armourUpperBody\"", json);
        Assert.Contains("{\"kind\":\"empty\"}", json);
        Assert.Contains("\"kind\":\"stack\",\"item\":3735928559,\"source\":{\"inventory\":2544,\"index\":3},\"expectedCount\":40", json);
        // the serial is EntityRef.Serial as it is, an exact 64-bit integer
        Assert.Contains($"\"serial\":{Ref(2552, 1, 5).Entity.Serial}", json);
    }

    [Fact]
    public void TheChaptersExampleReads()
    {
        var serial = IdentitySources.Stamp(EntitySourceOrder.Items, 3);
        var json = "{ \"schema\":1, \"scope\":{\"processSession\":\"01234567-89ab-4cde-8123-0123456789ab\",\"worldEpoch\":7,\"player\":" +
                   "{\"entity\":1,\"hasSpawnTime\":true,\"spawnTime\":123,\"lifeSerial\":9,\"playerHandle\":4,\"joinedStep\":20}}, " +
                   $"\"entries\":[{{\"slot\":\"RING0\",\"selection\":{{\"kind\":\"instance\",\"item\":{{\"entity\":4000,\"serial\":{serial},\"item\":12}}}}}}] }}";
        var set = EquipmentSetJson.Deserialize(json);
        Assert.Equal(Scope, set.Scope);
        var e = Assert.Single(set.Entries);
        Assert.Equal(EquipmentSlot.Ring0, e.Slot);
        Assert.Equal(new InstanceEquipment(new ItemInstanceRef(new EntityRef(new EntityId(4000), serial), new ItemId(12))), e.Selection);
    }

    [Theory]
    [InlineData("not json", "not JSON")]
    [InlineData("[]", "must be an object")]
    [InlineData("{\"schema\":2}", "schema 2")]
    [InlineData("{\"schema\":1}", "$.scope is missing")]
    public void BrokenJsonThrows(string json, string contains) =>
        Assert.Contains(contains, Assert.Throws<ArgumentException>(() => EquipmentSetJson.Deserialize(json)).Message);

    private static string With(string from, string to)
    {
        var json = EquipmentSetJson.Serialize(Sample());
        Assert.Contains(from, json);
        return json.Replace(from, to);
    }

    [Fact]
    public void MalformedFieldsThrow()
    {
        void Throws(string json, string contains) =>
            Assert.Contains(contains, Assert.Throws<ArgumentException>(() => EquipmentSetJson.Deserialize(json)).Message);
        Throws(With("\"kind\":\"empty\"", "\"kind\":\"hotbar\""), "is not empty, instance or stack");   // no implicit hotbar or saved discriminator
        Throws(With("\"slot\":\"ring1\"", "\"slot\":\"mainHand\""), "is not an equipment slot");
        Throws(With("\"slot\":\"ring1\"", "\"slot\":\"11\""), "is not an equipment slot");
        Throws(With("\"slot\":\"ring1\"", "\"slot\":11"), "must be a string");
        Throws(With("\"index\":3", "\"index\":8"), "0 to 7");
        Throws(With("\"expectedCount\":40", "\"expectedCount\":0"), "must be positive");
        Throws(With("\"worldEpoch\":7", "\"worldEpoch\":0"), "epoch 0");
        Throws(With("\"worldEpoch\":7", "\"worldEpoch\":-7"), "0 to 2^64-1");
        Throws(With("\"worldEpoch\":7", "\"worldEpoch\":7.5"), "0 to 2^64-1");
        Throws(With("\"processSession\":\"01234567-89ab-4cde-8123-0123456789ab\"", "\"processSession\":\"x\""), "a GUID");
        Throws(With("\"processSession\":\"01234567-89ab-4cde-8123-0123456789ab\"", "\"processSession\":\"00000000-0000-0000-0000-000000000000\""), "no process session");
        Throws(With("\"hasSpawnTime\":true", "\"hasSpawnTime\":1"), "true or false");
        Throws(With("{\"kind\":\"empty\"}", "{\"kind\":\"instance\"}"), "item is missing");
        Throws(With("\"slot\":\"ring1\"", "\"slot\":\"ring0\""), "named twice");
    }

    [Fact]
    public void SerializeChecksTheSet()
    {
        Assert.Throws<ArgumentNullException>(() => EquipmentSetJson.Serialize(null!));
        Assert.Throws<ArgumentException>(() => EquipmentSetJson.Serialize(new EquipmentSet(Scope with { WorldEpoch = 0 }, [])));
    }

    [Fact]
    public void SlotNamesRoundTrip()
    {
        foreach (var slot in Enum.GetValues<EquipmentSlot>())
        {
            Assert.True(EquipmentSetJson.TryParseSlot(EquipmentSetJson.SlotName(slot), out var back));
            Assert.Equal(slot, back);
        }
        Assert.False(EquipmentSetJson.TryParseSlot("20", out _));
        Assert.False(EquipmentSetJson.TryParseSlot("", out _));
    }

    // ---- results and plans ----

    [Fact]
    public void OnlyDoneNoChangeAndSimulatedAreOk()
    {
        foreach (var status in Enum.GetValues<EquipmentSetStatus>())
        {
            IKilnResult r = EquipmentSetResult.NotRun(status, "why", new EntityId(1), dryRun: true);
            Assert.Equal(status is EquipmentSetStatus.Done or EquipmentSetStatus.NoChange or EquipmentSetStatus.Simulated, r.Ok);
        }
        var r2 = EquipmentSetResult.NotRun(EquipmentSetStatus.Canceled, "canceled", new EntityId(1), dryRun: true, ticket: 9);
        Assert.True(r2.DryRun);  // kept on a refusal
        Assert.False(r2.Committed || r2.MayHaveMutated);
        Assert.Empty(r2.Changes);
        Assert.Null(r2.FailedOperation);
    }

    [Fact]
    public void ResultChangesAndPlanStepsAreCopies()
    {
        var v = new EquipmentSlotValue(new ItemId(1), ItemInstanceRef.None, SlotId.Of(new EntityId(2), 0), 1);
        var changes = new List<EquipmentSlotChange> { new(EquipmentSlot.Ring0, v, v) };
        var r = new EquipmentSetResult(EquipmentSetStatus.Done, null, new EntityId(1), 3, null, false, true, false, null, 0, changes);
        changes.Clear();
        Assert.Single(r.Changes);
        Assert.IsAssignableFrom<ReadOnlyCollection<EquipmentSlotChange>>(r.Changes);
        var steps = new List<EquipmentSetStep> { new(0, EquipmentSlot.Ring0, new EmptyEquipment()) };
        var plan = new EquipmentSetPlan(new EntityId(1), Sample(), new InventorySnapshotToken(4, 7, new EntityId(1)), 300, steps, 1);
        steps.Clear();
        Assert.Single(plan.Steps);
    }

    [Fact]
    public void APlansSnapshotIsReleasedOrUsedOnce()
    {
        var released = new List<(ulong, ulong)>();
        var lease = new EquipmentSetLease(41, 9, (o, t) => released.Add((o, t)));
        var plan = new EquipmentSetPlan(new EntityId(1), Sample(), new InventorySnapshotToken(41, 7, new EntityId(1)), 300, [], 0) { Lease = lease };
        var copy = plan with { RequiredFreeSlots = 2 };  // a copy shares the snapshot
        copy.Dispose();
        plan.Dispose();
        Assert.Equal([(9UL, 41UL)], released);
        Assert.False(lease.TryUse());  // released: an apply cannot take it any more

        var used = new EquipmentSetLease(42, 9, (o, t) => released.Add((o, t)));
        Assert.True(used.TryUse());
        Assert.False(used.TryUse());     // one apply
        used.Release();                  // Dispose after an apply does nothing
        Assert.Single(released);
        used.Unuse();                    // refused before kiln.items took it: held again
        Assert.True(used.Held);
        used.Release();
        Assert.Equal(2, released.Count);

        // a plan made by hand has no lease: Dispose does nothing
        new EquipmentSetPlan(new EntityId(1), Sample(), default, 0, [], 0).Dispose();
        // a release that throws is swallowed (the snapshot expires by itself)
        new EquipmentSetLease(1, 1, (_, _) => throw new InvalidOperationException()).Release();
    }

    // ---- the native mirrors (kiln_items.h v6; sets.cpp static_asserts the same) ----

    [Fact]
    public void TheV6MirrorsMatchTheHeader()
    {
        Assert.Equal(256, sizeof(ItemsNative.SetSnapshot));
        Assert.Equal(48, (int)Marshal.OffsetOf<ItemsNative.SetSnapshot>(nameof(ItemsNative.SetSnapshot.Identity)));
        Assert.Equal(88, (int)Marshal.OffsetOf<ItemsNative.SetSnapshot>(nameof(ItemsNative.SetSnapshot.Refusal)));
        Assert.Equal(208, sizeof(ItemsNative.SetStatus));
        Assert.Equal(16, (int)Marshal.OffsetOf<ItemsNative.SetStatus>(nameof(ItemsNative.SetStatus.Captured)));
        // v5 ends at 240 (equip_status at 232); v6 appends after it, so a v5 table is never read as v6
        Assert.Equal(240, sizeof(ItemsNative.TableV5));
        Assert.Equal(240, (int)Marshal.OffsetOf<ItemsNative.TableV6>(nameof(ItemsNative.TableV6.SetCapture)));
        Assert.Equal(248, (int)Marshal.OffsetOf<ItemsNative.TableV6>(nameof(ItemsNative.TableV6.SetRelease)));
        Assert.Equal(256, (int)Marshal.OffsetOf<ItemsNative.TableV6>(nameof(ItemsNative.TableV6.GetSetStatus)));
    }

    [Fact]
    public void TheSetsCapabilityVerdictFollowsTheTable()
    {
        Assert.Equal(Items.EquipSetsVerdict.NoTable, Items.DecideEquipSets(null, false, false, "").Verdict);
        var older = Items.DecideEquipSets(5, false, false, "");
        Assert.Equal(Items.EquipSetsVerdict.NoV6, older.Verdict);
        Assert.Contains("v5 has no equipment sets", older.Text);
        var off = Items.DecideEquipSets(6, true, false, "kiln.decisions is not here, or too old for a native set decision (enqueue_native)");
        Assert.Equal(Items.EquipSetsVerdict.Off, off.Verdict);
        Assert.Contains("kiln.decisions", off.Text);
        Assert.Contains("see kiln.log", Items.DecideEquipSets(6, true, false, "").Text);
        Assert.Equal(Items.EquipSetsVerdict.On, Items.DecideEquipSets(6, true, true, "").Verdict);
    }

    private static InventorySlot Slot(SlotId at, uint item, int count, uint pide = 0) =>
        new(at, new ItemId(item), count, new EntityId(pide), 0, null, InventoryCategory.Equipment, 2, pide != 0, true, false, 0);

    [Fact]
    public void ACaptureNamesWhatEachSlotHolds()
    {
        var ring = SlotId.Of(new EntityId(2003), 0);
        var arrows = SlotId.Of(new EntityId(2003), 4);
        var empty = SlotId.Of(new EntityId(2003), 5);
        var bare = SlotId.Of(new EntityId(2003), 6);
        var r = Ref(5001, 0x79442800, 11);
        var slots = new List<InventorySlot>
        {
            Slot(ring, 0x79442800, 1, 5001) with { Ref = r },
            Slot(arrows, 77, 40),
            Slot(empty, 0, 0),
            Slot(bare, 0x4623bcb0, 1, 5002),  // an item entity without a stamp (items.instances gave none)
        };
        Assert.IsType<EmptyEquipment>(Items.SelectionAt(default, slots).Selection);
        Assert.Equal(new InstanceEquipment(r), Items.SelectionAt(ring, slots).Selection);
        Assert.Equal(new StackEquipment(new ItemId(77), arrows, 40), Items.SelectionAt(arrows, slots).Selection);
        Assert.IsType<EmptyEquipment>(Items.SelectionAt(empty, slots).Selection);
        var (none, why) = Items.SelectionAt(bare, slots);
        Assert.Null(none);
        Assert.Contains("no stamp", why);
        var (outside, why2) = Items.SelectionAt(SlotId.Of(new EntityId(9999), 0), slots);
        Assert.Null(outside);
        Assert.Contains("not one of the player's inventories", why2);
    }

    // ---- the plan's wire (S3) ----

    [Fact]
    public void ThePlanMirrorsMatchTheHeader()
    {
        Assert.Equal(40, sizeof(ItemsNative.SetStep));
        Assert.Equal(16, (int)Marshal.OffsetOf<ItemsNative.SetStep>(nameof(ItemsNative.SetStep.InstanceStamp)));
        Assert.Equal(5408, sizeof(ItemsNative.SetPlan));
        Assert.Equal(24, (int)Marshal.OffsetOf<ItemsNative.SetPlan>(nameof(ItemsNative.SetPlan.ValidThroughStep)));
        Assert.Equal(288, (int)Marshal.OffsetOf<ItemsNative.SetPlan>(nameof(ItemsNative.SetPlan.Steps)));
        Assert.Equal(264, (int)Marshal.OffsetOf<ItemsNative.TableV6>(nameof(ItemsNative.TableV6.SetPlanOf)));
        Assert.Equal(40, sizeof(EquipDecisions.SetOp));
    }

    [Fact]
    public void TheEntriesGoOnTheWireAsTheyAre()
    {
        var ring = Ref(5002, 0x4623bcb0, 1002);
        var src = SlotId.Of(new EntityId(2001), 5);
        var ops = Items.OpsOf([E(EquipmentSlot.Ring1, new InstanceEquipment(ring)), E(EquipmentSlot.Arrow, new StackEquipment(new ItemId(88), src, 30)),
            E(EquipmentSlot.Backpack, new EmptyEquipment())]);
        Assert.Equal((40u, (byte)11, (byte)1, 5002u, 0x4623bcb0u, 1002UL, 0UL), (ops[0].Size, ops[0].EquipmentSlot, ops[0].Kind, ops[0].ItemEntity, ops[0].ItemId, ops[0].InstanceStamp, ops[0].SourceSlot));
        Assert.Equal(((byte)14, (byte)2, 88u, 30u, src.Value, 0u), (ops[1].EquipmentSlot, ops[1].Kind, ops[1].ItemId, ops[1].ExpectedCount, ops[1].SourceSlot, ops[1].ItemEntity));
        Assert.Equal(((byte)23, (byte)0, 0u, 0u), (ops[2].EquipmentSlot, ops[2].Kind, ops[2].ItemId, ops[2].ExpectedCount));
    }

    [Fact]
    public void AStepComesBackAsTheSelectionItPuts()
    {
        var equip = new ItemsNative.SetStep { Index = 1, Op = ItemsNative.SetStepEquip, EquipmentSlot = 10, Entry = 0, Kind = 1, ItemId = 0x4623bcb0, ItemEntity = 5002, InstanceStamp = 1002, SourceSlot = SlotId.Of(new EntityId(2001), 2).Value, Count = 1 };
        Assert.Equal(new EquipmentSetStep(1, EquipmentSlot.Ring0, new InstanceEquipment(Ref(5002, 0x4623bcb0, 1002))), Items.StepOf(equip));
        var stack = equip with { Kind = 2, ItemId = 88, ItemEntity = 0, InstanceStamp = 0, EquipmentSlot = 14, Count = 30 };
        Assert.Equal(new EquipmentSetStep(1, EquipmentSlot.Arrow, new StackEquipment(new ItemId(88), SlotId.Of(new EntityId(2001), 2), 30)), Items.StepOf(stack));
        var off = new ItemsNative.SetStep { Index = 0, Op = ItemsNative.SetStepUnequip, EquipmentSlot = 23, Entry = 0xFF, ItemId = 5, ItemEntity = 5009 };
        Assert.Equal(new EquipmentSetStep(0, EquipmentSlot.Backpack, new EmptyEquipment()), Items.StepOf(off));
        Assert.Throws<InvalidOperationException>(() => Items.StepOf(off with { EquipmentSlot = 17 }));
    }

    [Theory]
    [InlineData(8u, "NoRoom: no free slot")]
    [InlineData(9u, "UnsupportedCycle: no free slot")]
    [InlineData(4u, "SnapshotBudgetExceeded: no free slot")]
    [InlineData(5u, "StaleSnapshot: no free slot")]
    [InlineData(10u, "UnsupportedBackpack: no free slot")]
    [InlineData(99u, "Refused: no free slot")]
    public void ARefusalStartsWithItsCode(uint code, string text) => Assert.Equal(text, Items.ReasonOf(code, "no free slot"));

    // ---- applying (S5): the result, the completion and its events, the one decision, the heartbeat ----

    [Fact]
    public void TheApplyMirrorsMatchTheHeader()
    {
        Assert.Equal(56, sizeof(ItemsNative.SetRequest));
        Assert.Equal(40, (int)Marshal.OffsetOf<ItemsNative.SetRequest>("ProcessSession"));
        Assert.Equal(32, sizeof(ItemsNative.SetSlotValue));
        Assert.Equal(72, sizeof(ItemsNative.SetChange));
        Assert.Equal(1760, sizeof(ItemsNative.SetResult));
        Assert.Equal(64, (int)Marshal.OffsetOf<ItemsNative.SetResult>("Reason"));
        Assert.Equal(320, (int)Marshal.OffsetOf<ItemsNative.SetResult>("Changes"));
        Assert.Equal(824, sizeof(ItemsNative.SetDecisionPayload));
        Assert.Equal(272, (int)Marshal.OffsetOf<ItemsNative.TableV6>(nameof(ItemsNative.TableV6.EquipSet)));
        Assert.Equal(280, (int)Marshal.OffsetOf<ItemsNative.TableV6>(nameof(ItemsNative.TableV6.SetCancel)));
        Assert.Equal(288, sizeof(ItemsNative.TableV6));
    }

    private static ItemsNative.SetResult NativeResult(uint status, uint flags, string reason = "", uint refusal = 0, uint failed = uint.MaxValue)
    {
        var n = new ItemsNative.SetResult
        {
            Size = (uint)sizeof(ItemsNative.SetResult), Status = status, Ticket = 12, DecisionId = 5, WorldEpoch = 7, AppliedStep = 900,
            Player = 1, Flags = flags, Refusal = refusal, FailedOperation = failed, GameError = failed == uint.MaxValue ? 0u : 0x11u,
        };
        var bytes = System.Text.Encoding.UTF8.GetBytes(reason);
        for (var i = 0; i < bytes.Length && i < 255; i++) n.Reason[i] = bytes[i];
        return n;
    }

    [Fact]
    public void ANativeResultBecomesTheOneErrorRulesResult()
    {
        var n = NativeResult(0, ItemsNative.SetResultCommitted);
        n.ChangeCount = 1;
        var c = (ItemsNative.SetChange*)n.Changes;
        c[0] = new ItemsNative.SetChange
        {
            Size = 72, EquipmentSlot = 10,
            Before = new ItemsNative.SetSlotValue { ItemId = 0x79442800, ItemEntity = 5001, Count = 1, InstanceStamp = 1001, InventorySlot = SlotId.Of(new EntityId(2003), 0).Value },
            After = new ItemsNative.SetSlotValue { ItemId = 0x4623bcb0, ItemEntity = 5002, Count = 1, InstanceStamp = 1002, InventorySlot = SlotId.Of(new EntityId(2003), 0).Value },
        };
        var done = Items.ToSetResult(n);
        Assert.True(done.Ok);
        Assert.Null(done.Reason);
        Assert.True(done.Committed && !done.MayHaveMutated && !done.DryRun);
        Assert.Equal(new DecisionId(7, 5), done.Decision);
        Assert.Null(done.FailedOperation);
        var change = Assert.Single(done.Changes);
        Assert.Equal(EquipmentSlot.Ring0, change.Slot);
        Assert.Equal(Ref(5001, 0x79442800, 1001), change.Before.Instance);
        Assert.Equal(Ref(5002, 0x4623bcb0, 1002), change.After.Instance);

        var refused = Items.ToSetResult(NativeResult(5, ItemsNative.SetResultDryRun, "step 1 (equip into equipment slot 11): the game refused it (error 0x11)", failed: 1));
        Assert.False(refused.Ok);
        Assert.True(refused.DryRun);
        Assert.Equal(1, refused.FailedOperation);
        Assert.Equal(0x11, refused.GameError);
        Assert.StartsWith("Refused: step 1", refused.Reason);
        Assert.StartsWith("NoRoom: ", Items.ToSetResult(NativeResult(5, 0, "no free slot", refusal: 8)).Reason);
        Assert.StartsWith("StaleSnapshot: ", Items.ToSetResult(NativeResult(4, 0, "the inventory changed")).Reason);
        var fault = Items.ToSetResult(NativeResult(8, ItemsNative.SetResultMayHaveMutated | ItemsNative.SetResultCommitted, "after the commit ..."));
        Assert.True(fault is { Ok: false, MayHaveMutated: true, Committed: true, Status: EquipmentSetStatus.InvariantFailure });
        Assert.StartsWith("InvariantFailure: after the commit", fault.Reason);
        Assert.Null(Items.ToSetResult(NativeResult(1, 0) with { DecisionId = 0 }).Decision);  // no decision filed (NoChange)
        Assert.Equal(EquipmentSetStatus.InvariantFailure, Items.ToSetResult(NativeResult(42, 0)).Status);  // a status of a later service: not Ok
    }

    private static EquipmentSetEntry[] OneRing() => [E(EquipmentSlot.Ring0, new InstanceEquipment(Ref(5002, 0x4623bcb0, 1002)))];

    [Fact]
    public void ACompletionPostsTheEventsAndCompletesTheTaskOnce()
    {
        Items.ResetSetsForTests();
        try
        {
            using var kiln = new KilnHarness();
            kiln.Step(2);
            var changed = new List<EquipmentSetChangedEvent>();
            var faulted = new List<EquipmentSetFaultedEvent>();
            using var a = Events.On<EquipmentSetChangedEvent>(changed.Add);
            using var b = Events.On<EquipmentSetFaultedEvent>(faulted.Add);
            var v = new EquipmentSlotValue(new ItemId(1), ItemInstanceRef.None, SlotId.Of(new EntityId(2003), 0), 1);
            var tcs = new TaskCompletionSource<EquipmentSetResult>(TaskCreationOptions.RunContinuationsAsynchronously);
            var key = Items.TrackSet(tcs, null, new EntityId(1), Scope, OneRing(), false, 41, 7, 0, 12);
            var done = new EquipmentSetResult(EquipmentSetStatus.Done, null, new EntityId(1), 12, null, false, true, false, null, 0,
                [new EquipmentSlotChange(EquipmentSlot.Ring0, v, v with { Item = new ItemId(2) })]);
            kiln.Run(() => Items.CompleteSet(key, done));
            kiln.Run(() => Items.CompleteSet(key, done));  // a second report of the same key does nothing to the task
            Assert.Same(done, tcs.Task.Result);
            Assert.Equal(2, changed.Count);  // the events are the native facts: each report is posted, the task once
            Assert.Equal((12UL, EquipmentSetStatus.Done), (changed[0].Ticket, changed[0].Outcome));
            Assert.True(changed[0].Involves(new EntityId(1)) && !changed[0].Involves(EntityId.None) && !changed[0].Involves(new EntityId(2)));
            Assert.Empty(faulted);
            Assert.Equal(0, Items.SetsPending);

            // a dry run and a refusal post nothing
            var t2 = new TaskCompletionSource<EquipmentSetResult>();
            var k2 = Items.TrackSet(t2, null, new EntityId(1), Scope, OneRing(), true, 42, 7, 0, 13);
            kiln.Run(() => Items.CompleteSet(k2, EquipmentSetResult.NotRun(EquipmentSetStatus.Simulated, "", new EntityId(1), true, 13)));
            Assert.Equal(2, changed.Count);

            // a mod that unloaded: the events still come, its task does not complete
            var t3 = new TaskCompletionSource<EquipmentSetResult>();
            var k3 = Items.TrackSet(t3, null, new EntityId(1), Scope, OneRing(), false, 43, 7, 0, 14);
            Items.RetireSetForTests(k3);
            kiln.Run(() => Items.CompleteSet(k3, done with { Ticket = 14 }));
            Assert.Equal(3, changed.Count);
            Assert.False(t3.Task.IsCompleted);

            // an invariant failure: the faulted event, the changes that could be read, and the row goes off
            var t4 = new TaskCompletionSource<EquipmentSetResult>();
            var k4 = Items.TrackSet(t4, null, new EntityId(1), Scope, OneRing(), false, 44, 7, 0, 15);
            var bad = done with { Status = EquipmentSetStatus.InvariantFailure, Reason = "InvariantFailure: after the commit", MayHaveMutated = true, Ticket = 15 };
            kiln.Run(() => Items.CompleteSet(k4, bad));
            Assert.Same(bad, Assert.Single(faulted).Result);
            Assert.Equal(EquipmentSetStatus.InvariantFailure, changed[^1].Outcome);
            Assert.Equal(CapabilityState.Unavailable, Capabilities.Get("items.equip_sets").State);
        }
        finally
        {
            Items.ResetSetsForTests();
        }
    }

    [Fact]
    public void ALostResultEndsTheTaskAndSwitchesSetsOff()
    {
        Items.ResetSetsForTests();
        try
        {
            using var kiln = new KilnHarness();
            kiln.Step(2);
            var tcs = new TaskCompletionSource<EquipmentSetResult>();
            Items.TrackSet(tcs, null, new EntityId(1), Scope, OneRing(), false, 41, 7, 100, 12);
            var young = new TaskCompletionSource<EquipmentSetResult>();
            Items.TrackSet(young, null, new EntityId(1), Scope, OneRing(), false, 42, 7, 400, 13);
            kiln.Run(() => Items.SetHeartbeat(100 + Items.SetHeartbeatSteps + 1));
            Assert.True(tcs.Task.IsCompleted);
            Assert.Equal(EquipmentSetStatus.Dropped, tcs.Task.Result.Status);
            Assert.False(young.Task.IsCompleted);  // still within its time
            Assert.Equal(1, Items.SetsPending);
            Assert.Equal(CapabilityState.Unavailable, Capabilities.Get("items.equip_sets").State);
        }
        finally
        {
            Items.ResetSetsForTests();
        }
    }

}
