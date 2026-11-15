// SPDX-License-Identifier: MIT
// Equipment sets offline (plan 10-equipment-sets; chapter 10 §4.2, §5, §6): the argument rules (a bad argument throws,
// a stale scope is a result), the frozen copies, the session JSON (schema 1) and the result model. What the game does
// with a set is the lab test (mods/tests/equipment-sets-test); the native snapshot, planner and executor have their host
// tests (native/services/items/tests).
using System.Collections.ObjectModel;
using Kiln;
using Xunit;

namespace Kiln.ItemsTests;

public sealed class EquipmentSetTests
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
}
