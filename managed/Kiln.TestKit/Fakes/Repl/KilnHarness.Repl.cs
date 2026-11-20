// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessRepl? _repl;

    /// <summary>The fake kiln.repl of this harness (the network inspector, Kiln.Replication): the peers on the server's
    /// machine slots, what the server sends them, what a client receives, the component registry, and what mods asked.</summary>
    public HarnessRepl Repl => _repl ??= new HarnessRepl(this);

    /// <summary>Whether <paramref name="owner"/> was revoked in this harness (an unloaded mod's owner).</summary>
    internal bool ReplOwnerRevoked(ulong owner)
    {
        lock (_host.Revoked) return _host.Revoked.Contains(owner);
    }
}

/// <summary>What a replication message did to an entity, as kiln.repl numbers it.</summary>
public enum FakeReplKind
{
    /// <summary>A create message.</summary>
    Create = 2,

    /// <summary>An update message (the event carries the full values, never the delta).</summary>
    Update = 3,
}

/// <summary>A component of the fake world's registry (<see cref="HarnessRepl.AddComponent"/>): what
/// <c>Replication.Component</c> reads, and how a message's component is named and flagged.</summary>
/// <param name="Name">The short name (hashed as kiln.world's component_hash).</param>
/// <param name="Index">Its index in the registry.</param>
/// <param name="Size">Bytes of one component.</param>
/// <param name="Replicated">A tracked write queues the entity for the client channel.</param>
/// <param name="Versioned">A version byte travels with it.</param>
/// <param name="CustomCodec">The game packs it with a codec of its own (ReplSent.PackedByGame).</param>
/// <param name="ClientChannel">It may travel on the client channel.</param>
/// <param name="PredictionChannel">It may travel on the prediction channel.</param>
public sealed record FakeReplComponent(string Name, ushort Index, ushort Size, bool Replicated = true, bool Versioned = false,
    bool CustomCodec = false, bool ClientChannel = true, bool PredictionChannel = false);

/// <summary>One create or update message about an entity (<see cref="HarnessRepl.Send"/>, <see cref="HarnessRepl.Receive"/>).</summary>
/// <param name="Entity">The entity.</param>
/// <param name="Kind">Create or update.</param>
/// <param name="Tick">The server tick of the message.</param>
public sealed record FakeReplMessage(EntityId Entity, FakeReplKind Kind, uint Tick)
{
    /// <summary>The channel: 4 (client, the default) or 5 (prediction).</summary>
    public byte Channel { get; init; } = 4;

    /// <summary>The message's number on its peer's channel.</summary>
    public uint Seq { get; init; }

    /// <summary>An update's baseline tick (0: the template).</summary>
    public uint BaselineTick { get; init; }

    /// <summary>The payload bytes as sent (also what the counters add).</summary>
    public uint PayloadBytes { get; init; } = 32;

    /// <summary>Records were dropped before this one (also set by <see cref="HarnessRepl.Drop"/> for the next record).</summary>
    public bool Gap { get; init; }

    /// <summary>The full values the message carried, by registry component name (<see cref="HarnessRepl.AddComponent"/>).</summary>
    public IReadOnlyList<(string Component, byte[] Bytes)> Components { get; init; } = [];
}

/// <summary>
/// The fake kiln.repl of a harness (<see cref="KilnHarness.Repl"/>): Kiln.Replication's own code runs on it.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> The machine slots and their peers (a player, a lab fake with or without a bound lab peer, a
/// hosted world's own client); the counters per peer and channel, counted only while some owner holds them (the first
/// hold of all starts them from 0; a revoked owner lets go); what a watch gets: a record is written for a message only
/// while a watch of its entity and peer is on, delivered at the start of the next step (kiln.repl's Pre) with the full
/// component values, the gap after dropped records, a destroy list's entry, then ENDED as the last event (destroyed, id
/// reused, peer left, world stopped; the harness's end stops the world). A removed watch gets nothing more; an ended one
/// keeps its slot (64) until it is removed, as in kiln.repl. The receive side's watches work on a client-side harness
/// (<see cref="KilnHarnessOptions.Side"/>).</para>
/// <para><b>Not modelled.</b> The game's send path (what is relevant to whom, sizes, codecs), the capture file and the
/// loopback harness: those answer unavailable.</para>
/// </remarks>
public sealed class HarnessRepl
{
    private readonly KilnHarness _harness;

    internal HarnessRepl(KilnHarness harness) => _harness = harness;

    private ReplFakeState State => ReplFake.StateOf(_harness);

    // ---- the machine slots ----

    /// <summary>A player connects on machine slot <paramref name="machine"/> (1 to 16) with peer handle
    /// <paramref name="peer"/> (kiln.net's peer id).</summary>
    /// <exception cref="ArgumentException">A bad machine or peer, or the peer is connected already.</exception>
    /// <exception cref="InvalidOperationException">The slot is taken.</exception>
    public void Connect(uint machine, uint peer)
    {
        if (machine is < 1 or > 16) throw new ArgumentException("machine is 1 to 16 (0 is the server, or a hosted world's own client)", nameof(machine));
        if (peer == 0 || peer == ReplAbi.AllMachines) throw new ArgumentException("not a peer handle", nameof(peer));
        Put(machine, peer, 0);
    }

    /// <summary>A lab's loopback fake on machine slot <paramref name="machine"/>: its peer is
    /// <c>0x80000000 | machine</c>, or <paramref name="labPeer"/> when a kiln.net lab peer is bound to it.</summary>
    /// <returns>The peer handle events and rows carry.</returns>
    public uint ConnectFake(uint machine, uint? labPeer = null)
    {
        if (machine is < 1 or > 16) throw new ArgumentException("machine is 1 to 16", nameof(machine));
        var peer = labPeer ?? (ReplAbi.FakePeer | machine);
        if (peer == 0 || peer == ReplAbi.AllMachines) throw new ArgumentException("not a peer handle", nameof(labPeer));
        Put(machine, peer, (byte)(ReplAbi.PeerFake | (labPeer is null ? 0 : ReplAbi.PeerBound)));
        return peer;
    }

    /// <summary>A hosted world's own client on machine 0 (rows and events say Local).</summary>
    public void ConnectLocal(uint peer)
    {
        if (peer == 0 || peer == ReplAbi.AllMachines) throw new ArgumentException("not a peer handle", nameof(peer));
        Put(0, peer, ReplAbi.PeerLocal);
    }

    private void Put(uint machine, uint peer, byte flags)
    {
        var st = State;
        lock (st.Lock)
        {
            if (st.MachineOf(peer) >= 0) throw new ArgumentException($"peer 0x{peer:x} is connected already", nameof(peer));
            if (st.Machines[machine] is not null) throw new InvalidOperationException($"machine {machine} has a peer (Leave it first)");
            st.Machines[machine] = new ReplFakeState.Machine { Peer = peer, Flags = flags };
            st.ClearRows((int)machine);
        }
    }

    /// <summary>The peer leaves (its machine slot changes): its rows start over, and every watch for that one peer gets
    /// ENDED "peer left" at the start of the next step, after the records it was sent before.</summary>
    /// <exception cref="ArgumentException">The peer is not connected.</exception>
    public void Leave(uint peer)
    {
        var st = State;
        lock (st.Lock)
        {
            var m = MachineOf(st, peer);
            st.Machines[m] = null;
            st.ClearRows(m);
            st.GapNext[m, 0] = st.GapNext[m, 1] = false;
            st.PeersLeft.Add(peer);
        }
    }

    /// <summary>The server world stops: every watch still on gets ENDED "world stopped" at the start of the next step;
    /// the peers and rows go.</summary>
    public void StopWorld()
    {
        var st = State;
        lock (st.Lock) st.WorldStopping = true;
    }

    // ---- the registry ----

    /// <summary>Adds a component to the world's registry (what Replication.Component reads from the first step, and what
    /// a message's components are named by).</summary>
    /// <exception cref="ArgumentException">An empty name, or a component of that hash is there already.</exception>
    public void AddComponent(FakeReplComponent component)
    {
        ArgumentNullException.ThrowIfNull(component);
        ArgumentException.ThrowIfNullOrEmpty(component.Name);
        var st = State;
        lock (st.Lock)
            if (!st.Registry.TryAdd(ReplAbi.ComponentHash(component.Name), component))
                throw new ArgumentException($"the registry has {component.Name} already", nameof(component));
    }

    // ---- what the server sends (one frame: counted now, watch events at the start of the next step) ----

    /// <summary>The server sends <paramref name="peer"/> a create or update message about an entity in this frame. The
    /// counters count it while some owner holds them; a watch of the entity on that peer (or every peer) gets it at the
    /// start of the next step.</summary>
    /// <exception cref="ArgumentException">The peer is not connected, a bad channel, or a component not in the registry.</exception>
    public void Send(uint peer, FakeReplMessage message)
    {
        ArgumentNullException.ThrowIfNull(message);
        var st = State;
        lock (st.Lock)
        {
            var m = MachineOf(st, peer);
            var ch = ChannelIndex(message.Channel);
            var comps = Resolve(st, message);
            var row = Frame(st, m, ch);
            if (row is not null)
            {
                row.Messages++;
                row.Bytes += message.PayloadBytes;
                if (message.Kind == FakeReplKind.Create) row.Creates++;
                else row.Updates++;
            }
            var mach = st.Machines[m]!;
            if (!Watched(st, message.Entity.Value, ReplAbi.Send, (uint)m)) return;  // the watch hooks copy only a watched entity
            var gap = message.Gap || st.GapNext[m, ch];
            st.GapNext[m, ch] = false;
            st.Pending.Add(new ReplFakeState.Record(ReplAbi.Send, (uint)m, mach.Peer, mach.Flags, message.Channel, (byte)message.Kind, message.Entity.Value,
                message.Tick, message.Seq, message.BaselineTick, message.PayloadBytes, gap, st.TemplateOf(message.Entity.Value), comps));
        }
    }

    /// <summary>A destroy list to <paramref name="peer"/> names <paramref name="entity"/> as destroyed: a watch gets a
    /// Destroyed event, then ENDED "destroyed" after the step's other records.</summary>
    public void Destroy(uint peer, EntityId entity, uint tick, uint seq = 0, byte channel = 4)
    {
        var st = State;
        lock (st.Lock)
        {
            var m = MachineOf(st, peer);
            var ch = ChannelIndex(channel);
            var row = Frame(st, m, ch);
            if (row is not null)
            {
                row.Messages++;
                row.Bytes += 8;
                row.DestroyLists++;
                row.DestroyedIds++;
            }
            var mach = st.Machines[m]!;
            if (!Watched(st, entity.Value, ReplAbi.Send, (uint)m)) return;
            var gap = st.GapNext[m, ch];
            st.GapNext[m, ch] = false;
            st.Pending.Add(new ReplFakeState.Record(ReplAbi.Send, (uint)m, mach.Peer, mach.Flags, channel, ReplAbi.KindDestroyed, entity.Value, tick, seq,
                0, 0, gap, st.TemplateOf(entity.Value), []));
        }
    }

    /// <summary>kiln.repl's ring for the peer and channel was full: <paramref name="records"/> watch records were dropped
    /// (the row's Dropped), and the next record there carries Gap.</summary>
    public void Drop(uint peer, byte channel = 4, int records = 1)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(records, 1);
        var st = State;
        lock (st.Lock)
        {
            var m = MachineOf(st, peer);
            var ch = ChannelIndex(channel);
            st.Rows[m, ch].Dropped += (ulong)records;
            st.GapNext[m, ch] = true;
        }
    }

    /// <summary>Counts what the game's writer and session did for the peer and channel besides messages (only while some
    /// owner holds the counters).</summary>
    public void Count(uint peer, byte channel = 4, long unchanged = 0, long tooBig = 0, long refused = 0, long acks = 0, long resets = 0)
    {
        var st = State;
        lock (st.Lock)
        {
            var m = MachineOf(st, peer);
            if (Frame(st, m, ChannelIndex(channel)) is not { } row) return;
            row.Unchanged += (ulong)unchanged;
            row.TooBig += (ulong)tooBig;
            row.Refused += (ulong)refused;
            row.Acks += (ulong)acks;
            row.Resets += (ulong)resets;
        }
    }

    /// <summary>The entity's id now belongs to an entity of another template: the next message about it ends its
    /// watches with ENDED "id reused" (and is not delivered).</summary>
    public void ReuseId(EntityId entity)
    {
        var st = State;
        lock (st.Lock) st.Templates[entity.Value] = st.TemplateOf(entity.Value) + 1;
    }

    // ---- what a client receives ----

    /// <summary>This client receives a create or update message; a receive watch of the entity gets it at the start of
    /// the next step.</summary>
    public void Receive(FakeReplMessage message)
    {
        ArgumentNullException.ThrowIfNull(message);
        var st = State;
        lock (st.Lock)
        {
            var comps = Resolve(st, message);
            if (!Watched(st, message.Entity.Value, ReplAbi.Receive, 0)) return;
            st.Pending.Add(new ReplFakeState.Record(ReplAbi.Receive, 0, 0, 0, message.Channel, (byte)message.Kind, message.Entity.Value, message.Tick,
                message.Seq, message.BaselineTick, message.PayloadBytes, message.Gap, st.TemplateOf(message.Entity.Value), comps));
        }
    }

    /// <summary>This client is told the entity was destroyed: Destroyed, then ENDED "destroyed".</summary>
    public void ReceiveDestroy(EntityId entity, uint tick)
    {
        var st = State;
        lock (st.Lock)
        {
            if (!Watched(st, entity.Value, ReplAbi.Receive, 0)) return;
            st.Pending.Add(new ReplFakeState.Record(ReplAbi.Receive, 0, 0, 0, 4, ReplAbi.KindDestroyed, entity.Value, tick, 0, 0, 0, false,
                st.TemplateOf(entity.Value), []));
        }
    }

    // ---- what mods asked ----

    /// <summary>Every stats_enable call that changed or kept a hold, in order: the owner's name (<c>mod:&lt;id&gt;</c>-style,
    /// as the kit names owners) and on or off.</summary>
    public IReadOnlyList<(string Owner, bool On)> StatsCalls
    {
        get
        {
            var st = State;
            lock (st.Lock) return [.. st.StatsCalls.Select(c => (KitBackend.Instance.OwnerName(c.Owner), c.On))];
        }
    }

    /// <summary>Some owner holds the counters now (they count).</summary>
    public bool Counting => ReplFake.CountingIn(_harness);

    /// <summary>kiln.repl's watch slots in use: live watches, and ended ones not removed yet (a slot is free at the step
    /// after its removal). At most 64.</summary>
    public int WatchSlots
    {
        get
        {
            var st = State;
            lock (st.Lock) return st.Watches.Count;
        }
    }

    // ---- helpers (under the state's lock) ----

    private static int MachineOf(ReplFakeState st, uint peer) =>
        st.MachineOf(peer) is var m and >= 0 ? m : throw new ArgumentException($"peer 0x{peer:x} is not connected", nameof(peer));

    private static int ChannelIndex(byte channel) => channel switch
    {
        4 => 0,
        5 => 1,
        _ => throw new ArgumentException("channel is 4 (client) or 5 (prediction)", nameof(channel)),
    };

    private static IReadOnlyList<(FakeReplComponent, byte[])> Resolve(ReplFakeState st, FakeReplMessage message)
    {
        var list = new List<(FakeReplComponent, byte[])>(message.Components.Count);
        foreach (var (name, bytes) in message.Components)
        {
            ArgumentNullException.ThrowIfNull(bytes);
            if (!st.Registry.TryGetValue(ReplAbi.ComponentHash(name), out var c))
                throw new ArgumentException($"{name} is not in the registry (KilnHarness.Repl.AddComponent)", nameof(message));
            list.Add((c, [.. bytes]));
        }
        return list;
    }

    /// <summary>The row of this frame (one frame per step), or null while no owner holds the counters.</summary>
    private ReplFakeState.Counters? Frame(ReplFakeState st, int machine, int ch)
    {
        if (!ReplFake.CountingIn(_harness)) return null;
        var row = st.Rows[machine, ch];
        var step = _harness.CurrentStep;
        if (row.FrameStep != step)
        {
            row.FrameStep = step;
            row.Frames++;
        }
        return row;
    }

    private static bool Watched(ReplFakeState st, uint entity, uint direction, uint machine)
    {
        foreach (var w in st.Watches)
            if (!w.Removed && !w.Ended && w.Entity == entity && w.Direction == direction && ((w.Machines >> (int)machine) & 1) != 0) return true;
        return false;
    }
}
