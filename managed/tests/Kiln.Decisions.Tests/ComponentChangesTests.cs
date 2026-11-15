// SPDX-License-Identifier: MIT
// Named component watches (Kiln.World's ComponentChanges, plan 07 S7, chapter 07 §4.6) on the test kit's
// kiln.component_changes. The service's rules are native (native/services/decisions/watch_core.h) and have their host
// test (tests/watch_test.cpp); the kit's fake follows the same rules, and these cases are the host test's where a
// managed caller can see them: the budgets refuse exactly at the same counts with the reason and no watch, a watch is
// suspended whole and says why, the origin of a change follows the tokens (Kiln with the writer's id, Mixed, Unknown,
// never Game), the typed attribute event, the world's end, and the refusal by name while events.component_net is off.
using System.Runtime.InteropServices;
using Kiln;
using Kiln.TestKit;
using Xunit;
using N = Kiln.ComponentChangesNative;

namespace Kiln.DecisionsTests;

public sealed unsafe class ComponentChangesTests : IDisposable
{
    private readonly List<IDisposable> _subs = [];
    private readonly List<ComponentValueChangedEvent> _changes = [];
    private readonly List<AttributeValueChangedEvent> _attributes = [];
    private readonly List<ComponentWatchStateChangedEvent> _states = [];

    public void Dispose()
    {
        foreach (var s in _subs) s.Dispose();
        ComponentChanges.Reset();
        Capabilities.Reset();
    }

    private static byte[] Bytes(byte fill, int n = 40) => Enumerable.Repeat(fill, n).ToArray();

    private KilnHarness Started()
    {
        var kiln = new KilnHarness();
        kiln.Step(2);
        _subs.Add(Events.On<ComponentValueChangedEvent>(e => _changes.Add(e)));
        _subs.Add(Events.On<AttributeValueChangedEvent>(e => _attributes.Add(e)));
        _subs.Add(Events.On<ComponentWatchStateChangedEvent>(e => _states.Add(e)));
        return kiln;
    }

    private static EntityId With(KilnHarness kiln, params (string Component, byte[] Bytes)[] components)
    {
        var e = kiln.Entities.Create(new WorldPosition(1, 2, 3));
        foreach (var (c, b) in components) kiln.Entities.SetBytes(e, c, b);
        return e;
    }

    private static ComponentWatchResult Watch(KilnHarness kiln, ComponentWatchRequest request)
    {
        ComponentWatchResult? r = null;
        kiln.Call(() => r = ComponentChanges.Watch(request));
        return r!;
    }

    // A Kiln writer's token, as kiln.combat or kiln.world give it to the native service (the kit's table, same rules).
    private static void Token(KilnHarness kiln, EntityId e, uint component, byte[] before, byte[] after, string source)
    {
        var id = System.Text.Encoding.UTF8.GetBytes(source);
        kiln.Call(() =>
        {
            fixed (byte* b = before, a = after, s = id)
            {
                var w = new N.KnownWrite { Size = (uint)sizeof(N.KnownWrite), Component = component, Before = b, After = a, Bytes = (uint)before.Length };
                w.Entity.Entity = e.Value;
                w.SourceMod = new NativeStr { Ptr = s, Len = (nuint)id.Length };
                Assert.Equal(N.Ok, ComponentChanges.Table()->RecordWrite(9, &w));
            }
        });
    }

    [Fact]
    public void The_native_mirrors_have_the_header_s_sizes()
    {
        // native/services/decisions/tests/contract_test.cpp pins the same numbers
        Assert.Equal(32, sizeof(N.WatchRequest));
        Assert.Equal(224, sizeof(N.WatchStatus));
        Assert.Equal(1808, sizeof(N.Change));
        Assert.Equal(1768, (int)Marshal.OffsetOf<N.Change>(nameof(N.Change.WorldKind)));
        Assert.Equal(1776, (int)Marshal.OffsetOf<N.Change>(nameof(N.Change.ValueBefore)));
        Assert.Equal(1792, (int)Marshal.OffsetOf<N.Change>(nameof(N.Change.SinceStep)));
        Assert.Equal(1640, (int)Marshal.OffsetOf<N.Change>("SourceMod"));
        Assert.Equal(112, sizeof(N.KnownWrite));
        Assert.Equal(96, (int)Marshal.OffsetOf<N.KnownWrite>(nameof(N.KnownWrite.SourceMod)));
        Assert.Equal(184, sizeof(N.Counters));
        Assert.Equal(8 + 16 * 8, sizeof(N.Table));
        Assert.Equal(8 + 10 * 8, (int)Marshal.OffsetOf<N.Table>(nameof(N.Table.RecordWrite)));
        Assert.Equal(8 + 15 * 8, (int)Marshal.OffsetOf<N.Table>(nameof(N.Table.CountersFn)));
        Assert.Contains("kiln.component_changes", ServiceNames.All);
    }

    [Fact]
    public void A_malformed_request_is_an_argument_error()
    {
        using var kiln = Started();
        var e = With(kiln, ("Health", Bytes(1)));
        Assert.Throws<ArgumentException>(() => Watch(kiln, new(ComponentWatchScope.Entities, [], ObservedComponents.Health)));
        Assert.Throws<ArgumentException>(() => Watch(kiln, new(ComponentWatchScope.Entities, [e, e], ObservedComponents.Health)));
        Assert.Throws<ArgumentException>(() => Watch(kiln, new(ComponentWatchScope.Entities, [EntityId.None], ObservedComponents.Health)));
        Assert.Throws<ArgumentException>(() => Watch(kiln, new(ComponentWatchScope.Entities, [e], ObservedComponents.None)));
        Assert.Throws<ArgumentException>(() => Watch(kiln, new(ComponentWatchScope.Entities, [e], (ObservedComponents)64)));
        Assert.Throws<ArgumentException>(() => Watch(kiln, new(ComponentWatchScope.Players, [e], ObservedComponents.Health)));
        Assert.Throws<ArgumentException>(() => Watch(kiln, new(ComponentWatchScope.Entities, [e], ObservedComponents.Health, (ComponentWatchExpansion)2)));
        var many = Enumerable.Range(0, 129).Select(i => kiln.Entities.Create()).ToArray();
        Assert.Throws<ArgumentException>(() => Watch(kiln, new(ComponentWatchScope.Entities, many, ObservedComponents.Health)));
        Assert.Throws<InvalidOperationException>(() => ComponentChanges.Watch(new(ComponentWatchScope.Entities, [e], ObservedComponents.Health)));  // not the tick thread
    }

    [Fact]
    public void The_budgets_refuse_at_the_service_s_counts_with_the_reason_and_no_watch()
    {
        using var kiln = Started();
        var e = With(kiln, ("Health", Bytes(1)));
        for (var i = 0; i < 32; i++) Assert.True(Watch(kiln, new(ComponentWatchScope.Entities, [e], ObservedComponents.Health)).Ok);
        var r = Watch(kiln, new(ComponentWatchScope.Entities, [e], ObservedComponents.Health));
        Assert.False(r.Ok);
        Assert.Null(r.Watch);
        Assert.Contains("32 watches", r.Reason);
        Assert.Equal(32, kiln.ComponentChanges.Watches);
    }

    [Fact]
    public void Exactly_4096_present_pairs_fit_and_one_more_is_refused()
    {
        using var kiln = Started();
        var roots = new List<EntityId>();
        for (var i = 0; i < 1024; i++)
            roots.Add(With(kiln, ("Health", Bytes(1)), ("Stamina", Bytes(1)), ("Mana", Bytes(1)), ("Fuel", Bytes(1))));
        var total = 0;
        for (var at = 0; at < 1024; at += 128)
        {
            var w = Watch(kiln, new(ComponentWatchScope.Entities, roots.GetRange(at, 128), ObservedComponents.All));  // Inventory, SlotSelection absent: free
            Assert.True(w.Ok, w.Reason);
            total += w.Pairs;
        }
        Assert.Equal(4096, total);
        Assert.True(Watch(kiln, new(ComponentWatchScope.Entities, [roots[0]], ObservedComponents.Health)).Ok);  // overlap is free
        var one = With(kiln, ("Health", Bytes(1)));
        var r = Watch(kiln, new(ComponentWatchScope.Entities, [one], ObservedComponents.Health));
        Assert.False(r.Ok);
        Assert.Null(r.Watch);
        Assert.Equal(1, r.Pairs);
        Assert.Contains("4096", r.Reason);
        Assert.True(Watch(kiln, new(ComponentWatchScope.Entities, [one], ObservedComponents.Mana)).Ok);  // absent: costs nothing
        Assert.Equal(4096, kiln.ComponentChanges.PresentPairs);
    }

    [Fact]
    public void A_change_is_posted_once_with_its_bytes_and_an_unknown_origin_without_a_token()
    {
        using var kiln = Started();
        var e = With(kiln, ("Health", Bytes(1)));
        var a = Watch(kiln, new(ComponentWatchScope.Entities, [e], ObservedComponents.Health));
        var b = Watch(kiln, new(ComponentWatchScope.Entities, [e], ObservedComponents.Health | ObservedComponents.Mana));
        Assert.True(a.Ok && b.Ok);
        Assert.Equal(WorldKind.Server, a.Watch!.World);
        kiln.Step();
        Assert.Empty(_changes);
        kiln.Entities.SetBytes(e, "Health", Bytes(7));  // as a game program would: no token
        kiln.Step(2);
        var c = Assert.Single(_changes);
        Assert.Equal("Health", c.Component);
        Assert.Equal(e, c.Entity);
        Assert.Equal(ChangeOrigin.Unknown, c.Origin);  // never Game for a change without a token
        Assert.Null(c.SourceMod);
        Assert.Equal(Bytes(1), c.Before.ToArray());
        Assert.Equal(Bytes(7), c.After.ToArray());
        Assert.Equal([a.Watch.Id, b.Watch!.Id], c.WatchIds.Order().ToArray());  // two watches, one event
        Assert.True(c.Involves(e));
        Assert.Equal(WorldKind.Server, c.World);
        // A -> B -> A between two compares: nothing
        _changes.Clear();
        kiln.Entities.SetBytes(e, "Health", Bytes(8));
        kiln.Entities.SetBytes(e, "Health", Bytes(7));
        kiln.Step(2);
        Assert.Empty(_changes);
        // absent -> present
        kiln.Entities.SetBytes(e, "Mana", Bytes(3));
        kiln.Step(2);
        var m = Assert.Single(_changes);
        Assert.False(m.WasPresent);
        Assert.True(m.IsPresent);
    }

    [Fact]
    public void A_full_token_chain_is_Kiln_s_with_the_writer_two_writers_or_a_gap_are_Mixed()
    {
        using var kiln = Started();
        var e = With(kiln, ("Health", Bytes(1)));
        Assert.True(Watch(kiln, new(ComponentWatchScope.Entities, [e], ObservedComponents.Health)).Ok);
        kiln.Step();
        kiln.Entities.SetBytes(e, "Health", Bytes(2));
        Token(kiln, e, 1, Bytes(1), Bytes(2), "my-mod");
        kiln.Step(2);
        var k = Assert.Single(_changes);
        Assert.Equal(ChangeOrigin.Kiln, k.Origin);
        Assert.Equal("my-mod", k.SourceMod);
        Assert.Single(k.OperationTokens);
        _changes.Clear();
        kiln.Entities.SetBytes(e, "Health", Bytes(4));
        Token(kiln, e, 1, Bytes(2), Bytes(3), "mod-a");
        Token(kiln, e, 1, Bytes(3), Bytes(4), "mod-b");
        kiln.Step(2);
        var two = Assert.Single(_changes);
        Assert.Equal(ChangeOrigin.Mixed, two.Origin);
        Assert.Equal("mod-a, mod-b", two.SourceMod);
        _changes.Clear();
        kiln.Entities.SetBytes(e, "Health", Bytes(6));  // something without a token wrote after the mod
        Token(kiln, e, 1, Bytes(4), Bytes(5), "my-mod");
        kiln.Step(2);
        Assert.Equal(ChangeOrigin.Mixed, Assert.Single(_changes).Origin);
    }

    [Fact]
    public void A_kit_tracked_write_carries_the_mod_s_token()
    {
        using var kiln = Started();
        var e = With(kiln, ("Health", Bytes(1)));
        Assert.True(Watch(kiln, new(ComponentWatchScope.Entities, [e], ObservedComponents.Health)).Ok);
        kiln.Step();
        // what the kit's World.Write does when it writes (EntitiesFake: kiln.world's token, the mod's id)
        kiln.Call(() => ComponentChangesFake.NoteWrite(kiln, e.Value, "Health", Bytes(1), Bytes(2), "writes-mod"));
        kiln.Entities.SetBytes(e, "Health", Bytes(2));
        kiln.Step(2);
        var c = Assert.Single(_changes);
        Assert.Equal(ChangeOrigin.Kiln, c.Origin);
        Assert.Equal("writes-mod", c.SourceMod);
    }

    [Fact]
    public void The_typed_attribute_event_carries_current_and_maximum()
    {
        using var kiln = Started();
        var e = With(kiln, ("Health", Bytes(1)));
        kiln.ComponentChanges.SetAttribute(e, ObservedAttribute.Health, 100, 120);
        Assert.True(Watch(kiln, new(ComponentWatchScope.Entities, [e], ObservedComponents.Health)).Ok);
        kiln.Step();
        kiln.ComponentChanges.SetAttribute(e, ObservedAttribute.Health, 80, 120);
        kiln.Entities.SetBytes(e, "Health", Bytes(2));
        kiln.Step(2);
        var a = Assert.Single(_attributes);
        Assert.Equal(ObservedAttribute.Health, a.Attribute);
        Assert.Equal((100, 80, 120, 120), (a.Before, a.After, a.MaximumBefore, a.MaximumAfter));
        Assert.Same(Assert.Single(_changes), a.Change);
        Assert.True(a.Involves(e));
    }

    [Fact]
    public void A_root_that_goes_is_suspended_whole_and_says_why()
    {
        using var kiln = Started();
        var e = With(kiln, ("Health", Bytes(1)));
        var f = With(kiln, ("Health", Bytes(1)));
        var r = Watch(kiln, new(ComponentWatchScope.Entities, [e, f], ObservedComponents.Health));
        kiln.Step();
        kiln.Entities.Destroy(e);
        kiln.Step(3);
        var s = Assert.Single(_states);
        Assert.Equal(r.Watch!.Id, s.WatchId);
        Assert.Equal(ComponentWatchState.SuspendedMetadata, s.State);
        Assert.Contains("gone", s.Reason);
        Assert.Equal(ComponentWatchState.SuspendedMetadata, r.Watch.State);
        _changes.Clear();
        kiln.Entities.SetBytes(f, "Health", Bytes(9));  // the other root: not reported in part
        kiln.Step(2);
        Assert.Empty(_changes);
        ComponentWatchResult? again = null;
        kiln.Call(() => again = ComponentChanges.Resume(r.Watch));
        Assert.False(again!.Ok);
        Assert.Contains("gone", again.Reason);
    }

    [Fact]
    public void A_world_that_stops_ends_every_watch()
    {
        using var kiln = Started();
        var e = With(kiln, ("Health", Bytes(1)));
        var r = Watch(kiln, new(ComponentWatchScope.Entities, [e], ObservedComponents.Health));
        kiln.Step();
        kiln.Decisions.RestartWorld();
        kiln.Step(2);
        var s = Assert.Single(_states);
        Assert.Equal(ComponentWatchState.Disposed, s.State);
        Assert.Contains("stopped", s.Reason);
        Assert.Equal(ComponentWatchState.Disposed, r.Watch!.State);
        Assert.Equal(0, kiln.ComponentChanges.Watches);
        ComponentWatchResult? again = null;
        kiln.Call(() => again = ComponentChanges.Resume(r.Watch));
        Assert.False(again!.Ok);
    }

    [Fact]
    public void Dispose_ends_the_watch_without_a_state_event()
    {
        using var kiln = Started();
        var e = With(kiln, ("Health", Bytes(1)));
        var r = Watch(kiln, new(ComponentWatchScope.Entities, [e], ObservedComponents.Health));
        kiln.Step();
        r.Watch!.Dispose();
        r.Watch.Dispose();
        kiln.Entities.SetBytes(e, "Health", Bytes(5));
        kiln.Step(2);
        Assert.Empty(_changes);
        Assert.Empty(_states);
        Assert.Equal(0, kiln.ComponentChanges.Watches);
    }

    [Fact]
    public void Linked_inventories_and_their_items_are_watched_through_the_root()
    {
        using var kiln = Started();
        var p = With(kiln, ("Health", Bytes(1)));
        var bag = kiln.Entities.Create();
        var torch = kiln.Entities.Create();
        var setup = new byte[84];
        BitConverter.TryWriteBytes(setup.AsSpan(0, 4), bag.Value);
        setup[0x40] = 4;  // Generic
        kiln.Entities.SetBytes(p, "InventorySetup", setup);
        var stacks = new byte[96];
        BitConverter.TryWriteBytes(stacks.AsSpan(0, 4), 11u);
        BitConverter.TryWriteBytes(stacks.AsSpan(4, 4), 1u);
        BitConverter.TryWriteBytes(stacks.AsSpan(8, 4), torch.Value);
        kiln.Entities.SetBytes(bag, "Inventory", stacks);
        kiln.Entities.SetBytes(torch, "Fuel", Bytes(64));
        var r = Watch(kiln, new(ComponentWatchScope.Entities, [p], ObservedComponents.All, ComponentWatchExpansion.PlayerInventoryAndItems));
        Assert.True(r.Ok, r.Reason);
        Assert.Equal(3, r.Pairs);  // Health on the root, Inventory on the bag, Fuel on the torch
        kiln.Step();
        kiln.Entities.SetBytes(torch, "Fuel", Bytes(40));
        kiln.Step(2);
        var c = Assert.Single(_changes);
        Assert.Equal(torch, c.Entity);
        Assert.Equal([p], c.RootEntities);
        Assert.True(c.Involves(p));  // a filter on the root sees its linked child
        Assert.False(c.Key.HasSpawnTime);
        // a cycle in the links: suspended whole
        var nested = new byte[84];
        BitConverter.TryWriteBytes(nested.AsSpan(0, 4), p.Value);
        nested[0x40] = 5;
        var v = kiln.Entities.Create();
        kiln.Entities.SetBytes(v, "InventorySetup", nested);
        BitConverter.TryWriteBytes(setup.AsSpan(4, 4), v.Value);
        setup[0x41] = 5;
        kiln.Entities.SetBytes(p, "InventorySetup", setup);
        kiln.Step(2);
        Assert.Equal(ComponentWatchState.SuspendedMetadata, r.Watch!.State);
        Assert.Contains("cycle", r.Watch.Reason);
    }

    [Fact]
    public void Subscribing_where_the_row_is_off_is_refused_by_name_and_watches_nothing()
    {
        using var kiln = new KilnHarness();
        kiln.Step(2);
        kiln.Capabilities.Override(ComponentChanges.Capability, false);
        var e = Assert.Throws<KilnUnavailableException>(() => Events.On<ComponentValueChangedEvent>(_ => { }));
        Assert.Contains(ComponentChanges.Capability, e.Message);
        var w = kiln.Entities.Create();
        Assert.Throws<KilnUnavailableException>(() => Watch(kiln, new(ComponentWatchScope.Entities, [w], ObservedComponents.Health)));
        kiln.Capabilities.ClearOverride(ComponentChanges.Capability);
        // a subscription alone watches nothing
        _subs.Add(Events.On<ComponentValueChangedEvent>(x => _changes.Add(x)));
        kiln.Entities.SetBytes(w, "Health", Bytes(1));
        kiln.Step(2);
        Assert.Empty(_changes);
        Assert.Equal(0, kiln.ComponentChanges.Watches);
    }

    [Fact]
    public void The_kit_s_queue_holds_two_frames_and_then_suspends_with_the_last_complete_step()
    {
        using var kiln = Started();
        var e = With(kiln, ("Health", Bytes(1)));
        var r = Watch(kiln, new(ComponentWatchScope.Entities, [e], ObservedComponents.Health));
        kiln.Step();
        var model = ComponentChangesFake.ModelOf(kiln);
        // three compares without a read in between (the managed consumer runs only in Pre: drive the fake directly)
        kiln.Call(() =>
        {
            for (byte i = 2; i <= 4; i++)
            {
                kiln.Entities.SetBytes(e, "Health", Bytes(i));
                model.BeforeStep(1000ul + i);
            }
        });
        Assert.Equal(1ul, kiln.ComponentChanges.QueueOverflows);
        kiln.Step();
        Assert.Equal(ComponentWatchState.SuspendedBudget, r.Watch!.State);
        Assert.Contains("queue", r.Watch.Reason);
        Assert.Equal(2, _changes.Count);  // the two frames that fit were delivered
    }
}
