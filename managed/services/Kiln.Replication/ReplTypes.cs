// SPDX-License-Identifier: MIT
// Kiln.Replication's records (the dossier's chapter 11 §5.2): what the server sends each peer, read only.
namespace Kiln;

/// <summary>A replication channel, by its wire number.</summary>
public enum ReplChannel
{
    /// <summary>The client channel (4): what every client keeps of the entities it can see.</summary>
    Client = 4,

    /// <summary>The prediction channel (5): the few entities a client predicts (its own player, what it rides or holds),
    /// with server-side components a client channel does not carry.</summary>
    Prediction = 5,
}

/// <summary>What a replication message did to an entity.</summary>
public enum ReplKind
{
    /// <summary>The peer creates the entity from these values.</summary>
    Create = 2,

    /// <summary>The peer updates the entity (the event carries the full values, never the delta that was sent).</summary>
    Update = 3,

    /// <summary>A destroy list named the entity as destroyed (not one that only left the peer's view).</summary>
    Destroyed = 4,
}

/// <summary>Counters of one peer and channel since the stats were turned on (<see cref="Replication.Stats"/>).</summary>
/// <param name="Peer">The peer; null for a hosted world's own client (<see cref="Local"/>) and for a lab's fake player
/// with no lab peer bound to it (<see cref="Fake"/>).</param>
/// <param name="Channel">Which of the peer's two channels.</param>
/// <param name="Frames">Frames that sent this peer and channel anything (one batch each).</param>
/// <param name="Messages">Messages the session took (every message type).</param>
/// <param name="Bytes">Their bytes.</param>
/// <param name="Refused">Messages the session refused when it could not allocate them; they were not sent.</param>
/// <param name="Creates">Entity create messages sent.</param>
/// <param name="Updates">Entity update messages sent.</param>
/// <param name="DestroyLists">Destroy-list messages sent.</param>
/// <param name="DestroyedIds">Ids those lists named (destroyed and out of view).</param>
/// <param name="Unchanged">Entities the game looked at for this peer and found unchanged: no message.</param>
/// <param name="TooBig">Entities the game skipped because their message was over the channel's size limit (the game
/// logs "too big to fit in one message").</param>
/// <param name="Acks">Acknowledgement messages the peer sent back.</param>
/// <param name="Resets">Times the peer asked for full resends (it lost its baselines).</param>
/// <param name="Dropped">Watch records Kiln dropped for this peer and channel because its buffer was full (the game's
/// traffic is never changed).</param>
public sealed record ReplPeerStats(NetPeer? Peer, ReplChannel Channel, long Frames, long Messages, long Bytes, long Refused,
    long Creates, long Updates, long DestroyLists, long DestroyedIds, long Unchanged, long TooBig, long Acks, long Resets, long Dropped)
{
    /// <summary>The peer's id as kiln.net and the server name it (with the session's generation in its high bits).</summary>
    public uint PeerId { get; init; }

    /// <summary>A hosted world's own client: what the host's game sends itself.</summary>
    public bool Local { get; init; }

    /// <summary>A lab's fake player (<see cref="ReplicationLab"/>).</summary>
    public bool Fake { get; init; }
}

/// <summary>What the game's component registry says about one component (<see cref="Replication.Component"/>).</summary>
/// <param name="Name">The component's name as asked.</param>
/// <param name="Index">Its index in the registry (what replication messages name it by).</param>
/// <param name="Size">Bytes of one component.</param>
/// <param name="Hash">Its hash (what Kiln's world reads and writes it by).</param>
/// <param name="ReplicationBit">A tracked write of it queues the entity for the client channel by itself.</param>
/// <param name="Versioned">A version byte travels with it.</param>
/// <param name="CustomCodec">The game packs it with a codec of its own, which loses precision: a client sees it rounded.</param>
/// <param name="MayBeOnClientChannel">It may travel on the client channel.</param>
/// <param name="MayBeOnPredictionChannel">It may travel on the prediction channel.</param>
public sealed record ReplComponentInfo(string Name, int Index, int Size, uint Hash, bool ReplicationBit, bool Versioned,
    bool CustomCodec, bool MayBeOnClientChannel, bool MayBeOnPredictionChannel);

/// <summary>One message a peer was sent about a watched entity (<see cref="ReplWatch.Sent"/>).</summary>
/// <param name="Peer">The peer; null for a hosted world's own client or a lab's unbound fake (see <see cref="PeerId"/>).</param>
/// <param name="Channel">The channel it went on.</param>
/// <param name="Kind">Create, update, or destroyed (a destroy list named it; no components then).</param>
/// <param name="Entity">The entity.</param>
/// <param name="Tick">The server tick of the message.</param>
/// <param name="Seq">The message's number on that peer's channel (what the peer acknowledges).</param>
/// <param name="BaselineTick">An update's baseline: the tick of the state it was coded against; 0 for a create or an
/// update against the entity's template.</param>
/// <param name="PayloadBytes">The message's payload bytes as sent (the delta, after its header).</param>
/// <param name="Gap">Kiln dropped records before this one (its buffer was full): an earlier message is missing.</param>
/// <param name="Components">The full values of every component the message carried, by the game's component name (its
/// qualified name when two component types share the short name): the state the game coded the delta from, so a watch
/// started mid-session shows full values at once.</param>
/// <param name="PackedByGame">The components among them that the game packs with a codec of its own: a client sees them
/// rounded, so a difference from these bytes on the client is expected.</param>
public sealed record ReplSent(NetPeer? Peer, ReplChannel Channel, ReplKind Kind, EntityId Entity, uint Tick, uint Seq,
    uint BaselineTick, int PayloadBytes, bool Gap, IReadOnlyDictionary<string, ReadOnlyMemory<byte>> Components,
    IReadOnlySet<string> PackedByGame)
{
    /// <summary>The peer's id (see <see cref="ReplPeerStats.PeerId"/>).</summary>
    public uint PeerId { get; init; }

    /// <summary>A hosted world's own client.</summary>
    public bool Local { get; init; }

    /// <summary>A lab's fake player.</summary>
    public bool Fake { get; init; }
}

/// <summary>One message this client received about a watched entity (<see cref="ReplWatch.Received"/>).</summary>
/// <param name="Kind">Create, update, or destroyed.</param>
/// <param name="Entity">The entity (the server's id, which the client uses too).</param>
/// <param name="Tick">The server tick of the message.</param>
/// <param name="Gap">Kiln dropped records before this one.</param>
/// <param name="Components">The values the game decoded, by component name.</param>
public sealed record ReplReceived(ReplKind Kind, EntityId Entity, uint Tick, bool Gap,
    IReadOnlyDictionary<string, ReadOnlyMemory<byte>> Components);

/// <summary>A watch is over (<see cref="ReplWatch.Ended"/>); it was its last event.</summary>
/// <param name="Entity">The entity.</param>
/// <param name="Reason">"destroyed", "id reused" (the id now belongs to an entity of another template), "peer left",
/// or "world stopped".</param>
public sealed record ReplWatchEnded(EntityId Entity, string Reason);
