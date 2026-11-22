// SPDX-License-Identifier: MIT
using System.Buffers.Binary;
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed class ReplNetFakeTests
{
    private struct ValueComponent : IGameComponent<ValueComponent>
    {
        public static string Name => "TestValue";
        public static string QualifiedName => Name;
        public static uint Hash => 1;
        public static int Size => 4;
        public static ulong Layout => 1;
        public uint Value;
    }

    [Fact]
    public void Registers_a_filter_for_a_scripted_component()
    {
        using var kiln = new KilnHarness();
        kiln.Repl.AddComponent(new FakeReplComponent("TestValue", 1, 4));
        kiln.Boot();
        using var filter = kiln.Run(() => Replication.Filter<ValueComponent>(Peers.Vanilla,
            static (ref ValueComponent value, in FilterContext _) => value.Value++));
        var error = Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => Replication.Filter<ValueComponent>(
            Peers.All, static (ref ValueComponent _, in FilterContext _) => { },
            new FilterOptions { Sources = [ValueComponent.Name] })));
        Assert.Contains("does not schedule sends", error.Message);
    }

    [Fact]
    public void Queues_a_touch_for_an_entity_with_a_carrier()
    {
        using var kiln = new KilnHarness();
        var entity = kiln.Entities.Create(new WorldPosition(1, 2, 3));
        Assert.True(kiln.Run(() => Replication.Touch(entity)).Ok);
    }

    [Fact]
    public void Registers_an_item_remap_selector()
    {
        using var kiln = new KilnHarness();
        using var remap = kiln.Run(() => Replication.RemapItems(Peers.Vanilla));
    }

    [Fact]
    public void Registers_a_game_message_hook()
    {
        using var kiln = new KilnHarness();
        using var hook = kiln.Run(() => Replication.OnMessage(GameChannel.ChatMessages, MessageDirection.Send,
            static (NetPeer? _, ReadOnlySpan<byte> _, ref MessageRewrite _) => MessageAction.Pass));
    }

    [Fact]
    public void Filters_run_on_per_peer_copies_in_priority_order_and_only_on_selected_channels()
    {
        using var kiln = new KilnHarness();
        kiln.Repl.AddComponent(new FakeReplComponent(ValueComponent.Name, 1, 4, PredictionChannel: true));
        var vanilla = kiln.Repl.ConnectFake(1);
        var ready = kiln.ReplNet.Connect(2, 12, vanilla: false);
        var entity = kiln.Entities.Create();
        var source = kiln.Entities.Create();
        kiln.Entities.Set(source, new Ecs.ItemState { itemLevelUi = 3 });
        kiln.Boot();
        var observed = new List<ReplSent>();
        using var watch = Watch(kiln, entity, observed);

        var contexts = new List<(uint Peer, EntityId Entity, uint Tick, ReplChannel Channel)>();
        using var low = kiln.Run(() => Replication.Filter<ValueComponent>(Peers.All,
            static (ref ValueComponent v, in FilterContext _) => v.Value *= 2));
        using var high = kiln.Run(() => Replication.Filter<ValueComponent>(Peers.Vanilla,
            (ref ValueComponent v, in FilterContext ctx) =>
            {
                contexts.Add((ctx.PeerId, ctx.Entity, ctx.Tick, ctx.Channel));
                if (ctx.TryRead<Ecs.ItemState>(source, out var s)) v.Value += s.itemLevelUi;
            }, new FilterOptions { Priority = 5 }));
        using var samePriority = kiln.Run(() => Replication.Filter<ValueComponent>(Peers.Vanilla,
            static (ref ValueComponent v, in FilterContext _) => v.Value *= 10, new FilterOptions { Priority = 5 }));

        var input = Snapshot(entity, ValueComponent.Name, Words(5));
        kiln.Repl.Send(vanilla, input);
        kiln.Repl.Send(ready.Id, input);
        kiln.Repl.Send(vanilla, input with { Channel = 5 });
        kiln.Step();
        Assert.Equal(new uint[] { 160, 10, 5 }, observed.Select(e => Read(e)[0]));
        Assert.Equal(new[] { (vanilla, entity, 41u, ReplChannel.Client) }, contexts);
        Assert.Equal(Words(5), input.Components[0].Bytes);
        Assert.Equal(3, kiln.Entities.Get<Ecs.ItemState>(source).itemLevelUi);

        kiln.Repl.Leave(ready.Id);
        var replacement = kiln.ReplNet.Connect(2, 23);
        kiln.Step();
        kiln.Repl.Send(replacement.Id, input);
        kiln.Step();
        Assert.Equal(160u, Read(observed[^1])[0]);
        Assert.Equal(replacement.Id, contexts[^1].Peer);

        high.Dispose();
        samePriority.Dispose();
        kiln.Step();
        kiln.Repl.Send(vanilla, input);
        kiln.Step();
        Assert.Equal(10u, Read(observed[^1])[0]);
    }

    [Fact]
    public void A_fault_rolls_back_the_component_and_retires_only_the_failed_filter()
    {
        using var kiln = new KilnHarness();
        kiln.Repl.AddComponent(new FakeReplComponent(ValueComponent.Name, 1, 4));
        var peer = kiln.Repl.ConnectFake(1);
        var entity = kiln.Entities.Create();
        kiln.Boot();
        var observed = new List<ReplSent>();
        using var watch = Watch(kiln, entity, observed);
        using var good = kiln.Run(() => Replication.Filter<ValueComponent>(Peers.All,
            static (ref ValueComponent v, in FilterContext _) => v.Value++, new FilterOptions { Priority = 1 }));
        int faults = 0;
        using var bad = kiln.Run(() => Replication.Filter<ValueComponent>(Peers.All,
            (ref ValueComponent v, in FilterContext _) =>
            {
                faults++;
                v.Value = 99;
                throw new InvalidOperationException("scripted filter fault");
            }));
        var input = Snapshot(entity, ValueComponent.Name, Words(5));
        kiln.Repl.Send(peer, input);
        kiln.Repl.Send(peer, input);
        kiln.Step();
        Assert.Equal(1, faults);
        Assert.Equal(new uint[] { 5, 6 }, observed.Select(e => Read(e)[0]));
    }

    [Fact]
    public void Touch_records_requests_and_returns_carrier_and_entity_status()
    {
        using var kiln = new KilnHarness();
        var entity = kiln.Entities.Create(new WorldPosition(1, 2, 3));
        var withoutCarrier = kiln.Entities.Create();
        var missing = new EntityId(uint.MaxValue);
        Assert.True(kiln.Run(() => Replication.Touch(entity)).Ok);
        Assert.Equal("no carrier component", kiln.Run(() => Replication.Touch(withoutCarrier)).Reason);
        Assert.Equal("entity gone", kiln.Run(() => Replication.Touch(missing)).Reason);
        Assert.Equal(new[] { entity, withoutCarrier, missing }, kiln.ReplNet.Touches);
        kiln.Capabilities.Override("repl.touch", false, "no touch");
        Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => Replication.Touch(entity)));
        Assert.Equal(3, kiln.ReplNet.Touches.Count);
    }

    [Fact]
    public void Item_remap_runs_after_filters_and_counts_only_selected_snapshots()
    {
        using var kiln = new KilnHarness();
        var vanilla = kiln.Repl.ConnectFake(1);
        var ready = kiln.ReplNet.Connect(2, 22, vanilla: false);
        kiln.Repl.ConnectLocal(33);
        kiln.Repl.AddComponent(new FakeReplComponent(ValueComponent.Name, 1, 4));
        kiln.Repl.AddComponent(new FakeReplComponent("Items", 2, 16));
        kiln.ReplNet.ConfigureItemRemap([new FakeReplItemSite("Items", 0, 3, 4), new FakeReplItemSite(ValueComponent.Name, 0)],
            [new ItemId(10)], new Dictionary<ItemId, ItemId> { [new(20)] = new(10), [new(30)] = default });
        var entity = kiln.Entities.Create();
        kiln.Boot();
        var observed = new List<ReplSent>();
        using var watch = Watch(kiln, entity, observed);
        using var filter = kiln.Run(() => Replication.Filter<ValueComponent>(Peers.All,
            static (ref ValueComponent v, in FilterContext _) => v.Value = 20));

        var input = Snapshot(entity, "Items", Words(20, 30, 10, 42));
        kiln.Repl.Send(vanilla, input);
        kiln.Repl.Send(ready.Id, input);
        kiln.Repl.Send(33, input);
        kiln.Repl.Send(vanilla, Snapshot(entity, ValueComponent.Name, Words(10)));
        using var select = kiln.Run(() => Replication.RemapItems(Peers.All));
        kiln.Step();
        kiln.Repl.Send(ready.Id, input);
        kiln.Repl.Send(33, input);
        select.Dispose();
        kiln.Step();
        kiln.Repl.Send(ready.Id, input);
        kiln.Repl.Send(vanilla, Snapshot(entity, "Items", Words(77, 10, 0, 42)));
        kiln.Repl.Send(vanilla, Snapshot(entity, "Items", Words(20))); // malformed snapshot: in-bounds IDs get cleared
        kiln.Step();

        Assert.Equal(new uint[] { 10, 0, 10, 42 }, Read(observed[0]));
        Assert.Equal(new uint[] { 20, 30, 10, 42 }, Read(observed[1]));
        Assert.Equal(new uint[] { 20, 30, 10, 42 }, Read(observed[2]));
        Assert.Equal(new uint[] { 10 }, Read(observed[3]));
        Assert.Equal(new uint[] { 10, 0, 10, 42 }, Read(observed[4]));
        Assert.Equal(new uint[] { 20, 30, 10, 42 }, Read(observed[5]));
        Assert.Equal(new uint[] { 20, 30, 10, 42 }, Read(observed[6]));
        Assert.Equal(new uint[] { 0, 10, 0, 42 }, Read(observed[7]));
        Assert.Equal(new uint[] { 0 }, Read(observed[8]));
        Assert.Equal(new ReplRemapStats(true, 2, 2, 1, 5, 3, 3, 1), Replication.RemapStats());
    }

    [Fact]
    public void Message_hooks_match_channel_and_direction_and_chain_rewrite_then_drop()
    {
        using var kiln = new KilnHarness();
        var peer = kiln.ReplNet.Connect(1, 12);
        var calls = new List<(NetPeer? Peer, byte[] Bytes)>();
        using var send = kiln.Run(() => Replication.OnMessage(GameChannel.ChatMessages, MessageDirection.Send,
            (NetPeer? from, ReadOnlySpan<byte> bytes, ref MessageRewrite rewrite) =>
            {
                calls.Add((from, bytes.ToArray()));
                rewrite.Buffer[0] = 9;
                rewrite.Length = 1;
                return MessageAction.Rewrite;
            }));
        using var receive = kiln.Run(() => Replication.OnMessage(GameChannel.ChatMessages, MessageDirection.Receive,
            static (NetPeer? _, ReadOnlySpan<byte> _, ref MessageRewrite _) => MessageAction.Drop));
        using var inspect = kiln.Run(() => Replication.OnMessage(GameChannel.ChatMessages, MessageDirection.Send,
            (NetPeer? from, ReadOnlySpan<byte> bytes, ref MessageRewrite _) =>
            {
                calls.Add((from, bytes.ToArray()));
                return MessageAction.Pass;
            }));
        var channel = GameChannel.ChatMessages;
        var input = new byte[] { 1, 2 };
        Assert.Equal(input, kiln.ReplNet.DriveMessage(peer.Id, channel, MessageDirection.Send, input));
        Assert.Equal(new byte[] { 9 }, calls[1].Bytes);
        kiln.ReplNet.ValidateSend = (kind, bytes) => kind == channel && bytes.Span.SequenceEqual(new byte[] { 9 });
        Assert.Equal(new byte[] { 9 }, kiln.ReplNet.DriveMessage(peer.Id, channel, MessageDirection.Send, input));
        Assert.All(calls, call => Assert.Same(peer, call.Peer));
        Assert.Null(kiln.ReplNet.DriveMessage(peer.Id, channel, MessageDirection.Receive, input));
        Assert.Equal(input, kiln.ReplNet.DriveMessage(peer.Id, GameChannel.ServerInfo, MessageDirection.Send, input));
        Assert.Equal(4, calls.Count);
        send.Dispose();
        inspect.Dispose();
        Assert.Equal(input, kiln.ReplNet.DriveMessage(peer.Id, channel, MessageDirection.Send, input));
        Assert.Equal(4, calls.Count);
    }

    [Fact]
    public void Inject_copies_attempts_and_only_accepts_a_connected_peer_with_a_scripted_verdict()
    {
        using var kiln = new KilnHarness();
        var peer = kiln.ReplNet.Connect(1, 12);
        var channel = GameChannel.ChatMessages;
        byte[] bytes = [4, 5];
        Assert.False(kiln.Run(() => Replication.Inject(peer, channel, bytes)).Ok);
        kiln.ReplNet.ValidateSend = (_, _) => false;
        Assert.Contains("do not decode", kiln.Run(() => Replication.Inject(peer, channel, bytes)).Reason);
        kiln.ReplNet.ValidateSend = (kind, data) => kind == channel && data.Span.SequenceEqual(bytes);
        Assert.True(kiln.Run(() => Replication.Inject(peer, channel, bytes)).Ok);
        bytes[0] = 0;
        kiln.Repl.Leave(peer.Id);
        Assert.Contains("peer gone", kiln.Run(() => Replication.Inject(peer, channel, bytes)).Reason);
        var requests = kiln.ReplNet.Injections;
        Assert.Equal(4, requests.Count);
        Assert.All(requests, attempt => Assert.Equal((peer.Id, channel), (attempt.Peer, attempt.Channel)));
        Assert.Equal(new byte[] { 4, 5 }, requests[2].Bytes);
        Assert.Equal(new byte[] { 0, 5 }, requests[3].Bytes);
        requests[2].Bytes[0] = 6;
        Assert.Equal((byte)4, kiln.ReplNet.Injections[2].Bytes[0]);
    }

    [Fact]
    public void Joined_client_hooks_can_be_driven_and_game_only_services_stay_offline()
    {
        using var kiln = new KilnHarness(new KilnHarnessOptions { Side = HarnessSide.JoinedClient });
        NetPeer? sender = null;
        var received = false;
        using var hook = kiln.Run(() => Replication.OnMessage(GameChannel.ChatMessages, MessageDirection.Receive,
            (NetPeer? from, ReadOnlySpan<byte> _, ref MessageRewrite rewrite) =>
            {
                sender = from;
                received = true;
                rewrite.Buffer[0] = 9;
                return MessageAction.Rewrite;
            }));
        Assert.Equal(new byte[] { 9 }, kiln.ReplNet.DriveMessage(0, GameChannel.ChatMessages, MessageDirection.Receive, [4]));
        Assert.True(received);
        Assert.Null(sender);
        Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => Replication.Touch(new EntityId(1))));
        Assert.True(kiln.Capabilities.Is("repl.messages.client"));
        Assert.False(kiln.Capabilities.Is("repl.messages.server"));
        Assert.False(kiln.Capabilities.Is("repl.schema"));
        Assert.False(kiln.Capabilities.Is("repl.sidecar"));
        Assert.Contains("codec", kiln.Capabilities.Why("repl.schema"));
        Assert.Null(Replication.Schema("ChatMessageServerMessage"));
    }

    [Fact]
    public void Registrations_and_records_end_with_the_harness()
    {
        IDisposable abandoned;
        using (var kiln = new KilnHarness())
        {
            var peer = kiln.ReplNet.Connect(1, 12);
            abandoned = kiln.Run(() => Replication.OnMessage(GameChannel.ChatMessages, MessageDirection.Receive,
                static (NetPeer? _, ReadOnlySpan<byte> _, ref MessageRewrite _) => MessageAction.Drop));
            Assert.Null(kiln.ReplNet.DriveMessage(peer.Id, GameChannel.ChatMessages, MessageDirection.Receive, [4]));
            kiln.Run(() => Replication.Touch(new EntityId(1)));
            kiln.Run(() => Replication.Inject(peer, GameChannel.ChatMessages, [4]));
        }
        using var next = new KilnHarness();
        var nextPeer = next.ReplNet.Connect(1, 12);
        using var nextHook = next.Run(() => Replication.OnMessage(GameChannel.ChatMessages, MessageDirection.Receive,
            static (NetPeer? _, ReadOnlySpan<byte> _, ref MessageRewrite _) => MessageAction.Pass));
        abandoned.Dispose();
        Assert.Equal(new byte[] { 4 }, next.ReplNet.DriveMessage(nextPeer.Id, GameChannel.ChatMessages, MessageDirection.Receive, [4]));
        Assert.Empty(next.ReplNet.Touches);
        Assert.Empty(next.ReplNet.Injections);
        Assert.False(Replication.RemapStats().Active);
    }

    private static FakeReplMessage Snapshot(EntityId entity, string component, byte[] bytes) =>
        new(entity, FakeReplKind.Update, 41) { Components = [(component, bytes)] };

    private static ReplWatch Watch(KilnHarness kiln, EntityId entity, List<ReplSent> observed)
    {
        var watch = kiln.Run(() => Replication.Watch(entity));
        watch.Sent += observed.Add;
        return watch;
    }

    private static byte[] Words(params uint[] values)
    {
        var bytes = new byte[values.Length * 4];
        for (var i = 0; i < values.Length; i++) BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(i * 4), values[i]);
        return bytes;
    }

    private static uint[] Read(ReplSent sent)
    {
        var bytes = sent.Components.Values.Single().Span;
        var words = new uint[bytes.Length / 4];
        for (var i = 0; i < words.Length; i++) words[i] = BinaryPrimitives.ReadUInt32LittleEndian(bytes[(i * 4)..]);
        return words;
    }
}
