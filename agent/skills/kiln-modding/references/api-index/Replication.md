# Replication (Kiln.Replication)

## `ComponentFilter<T>` `0.2.17` — Edits `value`: one peer's copy of the component as the server sends it.
- `Invoke(ref T value, in FilterContext context) -> void`

## `ExtensionProvider<T>` `0.2.17` — Computes an extension's value for one entity on the server (tick thread).
- `Invoke(EntityId entity, ref T value) -> bool`

## FilterContext `0.2.17` — What a `ComponentFilter` knows about the snapshot it edits.
- `Channel { get; } -> ReplChannel` — The channel the snapshot goes on.
- `Entity { get; } -> EntityId` — The entity.
- `Peer { get; } -> NetPeer?` — The peer the snapshot is for; null for a hosted world's own client and a lab's unbound fake.
- `PeerId { get; } -> uint` — The peer's id as kiln.repl names it (see `ReplPeerStats.PeerId`).
- `Tick { get; } -> uint` — The server tick of the snapshot.
- `TryRead<TC>(EntityId entity, out TC value) -> bool` — Reads another component of any entity as the world holds it now (read only; the world does not change during the...

## FilterOptions `0.2.17` — How a `Replication.Filter` runs.
- `AllowPrediction { get; init; } -> bool` — Required to include `ReplChannels.Prediction`.
- `Channels { get; init; } -> ReplChannels` — The channels it edits.
- `Priority { get; init; } -> int` — Higher runs first; equal priorities run in registration order.
- `Sources { get; init; } -> IReadOnlyList<string>` — Components of the same entity the filter's output depends on (short names, such as `"Fuel"`): a change of their...

## GameChannel `0.2.17` — The game's network channels, by their wire number.
enum: Login, ServerInfo, VoxelData, FogVoxelData, EcsClient, EcsPrediction, EcsSnapping, EntityProxies, Input, SaveGame, Knowledge, LogicGraph, Debug, EcsReset, VoiceChat, CustomPlayerStrings, Water, TwitchDrop, Weather, ChatMessages, BaseSharing, AdminInfo, Commerce, VoxelDamageSystem, FactoryInfo

## IGameMessage `0.2.17` — A message type of the game's codec, as a C# struct of the same layout (its schema's size).
- `SchemaName { get; } -> string` — The game's schema name (`ServerInfoMessage`, `ChatMessageServerMessage`).

## InjectResult `0.2.17` — What `Replication.Inject` did.
- `InjectResult(bool Ok, string? Reason)`
- `Channel { get; init; } -> GameChannel` — The channel it went to.
- `Ok { get; init; } -> bool` — The game's session took the message for sending.
- `Peer { get; init; } -> NetPeer?` — The peer it went to.
- `Reason { get; init; } -> string?` — Why not.

## MessageAction `0.2.17` — What a `MessageHandler` does with a message.
enum: Pass, Drop, Rewrite

## MessageDirection `0.2.17` — Which way a hooked message goes.
enum: Send, Receive

## MessageField `0.2.17` — One field of a game message schema (`Replication.Schema`).
- `MessageField(string Name, int Type, int Element, int Shape, int Offset, int CountOffset, int FixedCount)`
- `CountOffset { get; init; } -> int` — An optional field's present flag, or a list's u32 count.
- `Element { get; init; } -> int` — How one element is stored in the struct (the game's element code).
- `FixedCount { get; init; } -> int` — An inline array's length, or a list's capacity.
- `Name { get; init; } -> string` — The field's name.
- `Offset { get; init; } -> int` — Its byte offset in the struct.
- `Shape { get; init; } -> int` — 0 optional, 1 inline, 2 a list with a count.
- `Type { get; init; } -> int` — The wire type code (0 bool, 1 float, 3..10 integers, 11 string, 12 struct, 13 enum, 14 GUID, 15 bits, 16 varint).

## MessageHandler `0.2.17` — Looks at one message of a hooked channel and says what happens to it.
- `Invoke(NetPeer? peer, ReadOnlySpan<byte> data, ref MessageRewrite rewrite) -> MessageAction`

## MessageRewrite `0.2.17` — The rewrite buffer and peer identity of a hooked message.
- `Buffer { get; } -> Span<byte>` — The bytes to change (a copy of the message; as long as the message).
- `Length { get; set; } -> int` — The new length (at most `MessageRewrite.Buffer`'s length).
- `PeerId { get; } -> uint` — The `NetPeer.Id` on the server side.
- `PeerUnknown { get; } -> bool` — Whether Kiln could not resolve the message's peer id on the server side.

## MessageSchema `0.2.17` — A game message schema: its struct's size and fields (`Replication.Schema`).
- `MessageSchema(string Name, int Size, IReadOnlyList<MessageField> Fields)`
- `Fields { get; init; } -> IReadOnlyList<MessageField>` — The struct's fields, in the order the game lists them.
- `Name { get; init; } -> string` — The schema's name, as passed to `Replication.Schema`.
- `Size { get; init; } -> int` — The size of the message's struct in bytes.

## Peers `0.2.17` — The selectors of Kiln's replication rules (`PeerSelector`).
- `static All { get; } -> PeerSelector` — Every peer.
- `static LackingContent { get; } -> PeerSelector` — Peers that may lack content only Kiln has (Kiln-only items, templates): every vanilla peer, and every Kiln client...
- `static Vanilla { get; } -> PeerSelector` — Peers without Kiln: clients whose Kiln handshake did not finish (a vanilla client, or one still connecting) and lab...
- `static With(ReplicatedExtension extension) -> PeerSelector` — Peers that have `extension`: Kiln clients whose handshake advertised the same name, version, size and layout...
- `static Without(ReplicatedExtension extension) -> PeerSelector` — Peers that do not have `extension` (the rest of `Peers.With`): vanilla peers, Kiln clients without it or with...

## PeerSelector `0.2.17` — The peers a replication rule applies to: `Peers.All`, `Peers.Vanilla`, `Peers.With`, `Peers.Without`, `Peers.LackingContent`.
- `Description { get; } -> string` — What the selector picks, in words ("vanilla peers", "peers with bigcan:water@1").
- `Matches(NetPeer peer) -> bool` — Whether `peer` is one of the selected peers now (a client connection of this server).

## `ReceiveFilter<T>` `0.2.17` — Edits `value` as this client received it, before the client keeps it (client channel only).
- `Invoke(ref T value, EntityId entity) -> void`

## ReplChannel `0.2.17` — A replication channel, by its wire number.
enum: Client, Prediction

## ReplChannels `0.2.17` — The replication channels a filter applies to.
enum: Client, Prediction

## ReplComponentInfo `0.2.17` — What the game's component registry says about one component (`Replication.Component`).
- `ReplComponentInfo(string Name, int Index, int Size, uint Hash, bool ReplicationBit, bool Versioned, bool CustomCodec, bool MayBeOnClientChannel, bool MayBeOnPredictionChannel)`
- `CustomCodec { get; init; } -> bool` — The game packs it with a codec of its own, which loses precision: a client sees it rounded.
- `Hash { get; init; } -> uint` — Its hash (what Kiln's world reads and writes it by).
- `Index { get; init; } -> int` — Its index in the registry (what replication messages name it by).
- `MayBeOnClientChannel { get; init; } -> bool` — It may travel on the client channel.
- `MayBeOnPredictionChannel { get; init; } -> bool` — It may travel on the prediction channel.
- `Name { get; init; } -> string` — The component's name as asked.
- `ReplicationBit { get; init; } -> bool` — A tracked write of it queues the entity for the client channel by itself.
- `Size { get; init; } -> int` — Bytes of one component.
- `Versioned { get; init; } -> bool` — A version byte travels with it.

## `ReplExtensionChanged<T>` `0.2.17` — An extension's value changed: on a server, a value was sent to `ReplExtensionChanged.Peer` (or cleared there); on a client, a value arrived from the server or went...
- `ReplExtensionChanged(EntityId Entity, uint CreateTick, T? Value, NetPeer? Peer)`
- `CreateTick { get; init; } -> uint` — The entity's generation on that peer (the server tick of its create; 0 when the server's tracking began after it).
- `Entity { get; init; } -> EntityId` — The entity (the server's id).
- `Peer { get; init; } -> NetPeer?` — Server: the peer it was sent to (null for a hosted world's own client).
- `Value { get; init; } -> T?` — The new value; null when it went (cleared, the entity was destroyed or left the peer's view).

## ReplicatedExtension `0.2.17` — A replicated extension: a mod's own value on entities (a struct of numbers), sent by the server to every Kiln client that has the same extension, beside the game's...
- `IsAvailableFor(NetPeer peer) -> bool` — Whether `peer` has this extension: a Kiln client whose handshake advertised the same name, version, size and layout...
- `IsRegistered { get; } -> bool` — The extension is registered: not disposed, and its mod is loaded.
- `LayoutHash { get; } -> uint` — The value type's layout hash: FNV-1a-32 over every leaf field's path, type and offset...
- `Name { get; } -> string` — The extension's name, unique in the process (by convention "\<mod id>:\<name>").
- `Size { get; } -> int` — The value's size in bytes on the wire (at most 1,024).
- `Version { get; } -> int` — Its version (1 or more): a value type that changes gets a new version.

## `ReplicatedExtension<T>` `0.2.17` — A replicated extension with values of `T` (`Replication.Extension`).
- `Changed -> Action<ReplExtensionChanged<T>>?` — A value changed.
- `Dispose() -> void` — Removes the extension: the server stops sending it (clients drop its values at the next DEFS) and its name is free...
- `On<TComponent>(ExtensionProvider<T> provider) -> IDisposable` — Computes the value on entities that have component `TComponent`: Kiln calls `provider` on the server's tick thread...
- `TryGet(EntityId entity, out T value) -> bool` — The value of `entity` as this process knows it: on a Kiln client, the last value the server sent for the generation...

## Replication `0.2.17` — The network inspector: what the server sends each player, read only.
- `static Component(string name) -> ReplComponentInfo?` — What the game's component registry says about the component called `name` (its short name, as Kiln's world reads...
- `static Decode<T>(ReadOnlySpan<byte> bytes) -> T` — Decodes a game message with the game's own codec.
- `static Encode<T>(in T message) -> byte[]` — Encodes a game message with the game's own codec.
- `static Extension<T>(string name, int version) -> ReplicatedExtension<T>` — Registers a replicated extension: a value of `T` on entities, which the server computes (`ReplicatedExtension.On`)...
- `static Filter<TComponent>(PeerSelector peers, ComponentFilter<TComponent> filter, FilterOptions? options = null) -> IDisposable` — Edits component `TComponent` as the server sends it to the peers `peers` selects: `filter` gets that peer's copy of...
- `static Inject(NetPeer peer, GameChannel channel, ReadOnlySpan<byte> bytes) -> InjectResult` — Sends `bytes` to `peer` as a game message on `channel`: the schema channels the peer's machine socket carries...
- `static Injected -> Action<InjectResult>?` — Raised on the tick thread for every `Replication.Inject`, with its result.
- `static OnMessage(GameChannel channel, MessageDirection direction, MessageHandler handler) -> IDisposable` — Hooks the game's messages on `channel`: `handler` sees every message this process sends and/or receives there and...
- `static ReceivedStats() -> IReadOnlyList<ReplPeerStats>` — What this game client receives from its server, per channel: messages, bytes, creates, updates, destroy lists, and...
- `static RemapItems(PeerSelector peers) -> IDisposable` — Hides Kiln-only items from the peers `peers` selects, besides the ones Kiln always hides them from (vanilla clients...
- `static RemapStats() -> ReplRemapStats` — What the item remap did since the server started.
- `static Schema(string name) -> MessageSchema?` — A game message schema by name: its struct's size and fields, to write a struct for `Replication.Decode`.
- `static Stats() -> IReadOnlyList<ReplPeerStats>` — Counters per peer and channel: messages, bytes, creates, updates, destroys, unchanged and oversized entities...
- `static Touch(EntityId entity) -> TouchResult` — Re-sends `entity` on the client channel without changing a byte: the next sim step marks it, and that frame's send...
- `static Touched -> Action<TouchResult>?` — Raised on the tick thread for every `Replication.Touch`, with its result.
- `static Watch(EntityId entity, NetPeer? peer = null) -> ReplWatch` — Watches what the server sends of `entity`: one `ReplWatch.Sent` event per message to `peer` (every peer when null)...
- `static WatchReceived(EntityId entity) -> ReplWatch` — Watches what this client receives of `entity`: one `ReplWatch.Received` event per message with the values the game...

## ReplKind `0.2.17` — What a replication message did to an entity.
enum: Create, Update, Destroyed

## ReplPeerStats `0.2.17` — Counters of one peer and channel since the stats were turned on (`Replication.Stats`).
- `ReplPeerStats(NetPeer? Peer, ReplChannel Channel, long Frames, long Messages, long Bytes, long Refused, long Creates, long Updates, long DestroyLists, long DestroyedIds, long Unchanged, long TooBig, long Acks, long Resets, long Dropped)`
- `Acks { get; init; } -> long` — Acknowledgement messages the peer sent back.
- `Bytes { get; init; } -> long` — Their bytes.
- `Channel { get; init; } -> ReplChannel` — Which of the peer's two channels.
- `Creates { get; init; } -> long` — Entity create messages sent.
- `DestroyedIds { get; init; } -> long` — Ids those lists named (destroyed and out of view).
- `DestroyLists { get; init; } -> long` — Destroy-list messages sent.
- `Dropped { get; init; } -> long` — Watch records Kiln dropped for this peer and channel because its buffer was full (the game's traffic is never changed).
- `Fake { get; init; } -> bool` — A lab's fake player (`ReplicationLab`).
- `Frames { get; init; } -> long` — Frames that sent this peer and channel anything (one batch each).
- `Local { get; init; } -> bool` — A hosted world's own client: what the host's game sends itself.
- `Messages { get; init; } -> long` — Messages the session took (every message type).
- `Peer { get; init; } -> NetPeer?` — The peer; null for a hosted world's own client (`ReplPeerStats.Local`) and for a lab's fake player with no lab peer...
- `PeerId { get; init; } -> uint` — The peer's id as kiln.net and the server name it (with the session's generation in its high bits).
- `Received { get; init; } -> bool` — A game client's own row (`Replication.ReceivedStats`): what this client received from its server on...
- `Refused { get; init; } -> long` — Messages the session refused when it could not allocate them; they were not sent.
- `Resets { get; init; } -> long` — Times the peer asked for full resends (it lost its baselines).
- `TooBig { get; init; } -> long` — Entities the game skipped because their message was over the channel's size limit (the game logs "too big to fit in...
- `Unchanged { get; init; } -> long` — Entities the game looked at for this peer and found unchanged: no message.
- `Updates { get; init; } -> long` — Entity update messages sent.

## ReplReceived `0.2.17` — One message this client received about a watched entity (`ReplWatch.Received`).
- `ReplReceived(ReplKind Kind, EntityId Entity, uint Tick, bool Gap, IReadOnlyDictionary<string, ReadOnlyMemory<byte>> Components)`
- `Components { get; init; } -> IReadOnlyDictionary<string, ReadOnlyMemory<byte>>` — The values the game decoded, by component name.
- `Entity { get; init; } -> EntityId` — The entity (the server's id, which the client uses too).
- `Gap { get; init; } -> bool` — Kiln dropped records before this one.
- `Kind { get; init; } -> ReplKind` — Create, update, or destroyed.
- `Tick { get; init; } -> uint` — The server tick of the message.

## ReplRemapStats `0.2.17` — What the item remap did since the server started (`Replication.RemapStats`).
- `ReplRemapStats(bool Active, int Sites, int KilnOnlyItems, int WithoutLookAlike, long Snapshots, long Remapped, long Zeroed, long Faults)`
- `Active { get; init; } -> bool` — The remap table is installed: Kiln-only items are hidden from the peers that lack them.
- `Faults { get; init; } -> long` — Times the remap faulted and every site of the component was zeroed.
- `KilnOnlyItems { get; init; } -> int` — Kiln-only items known now.
- `Remapped { get; init; } -> long` — Ids replaced by their look-alike.
- `Sites { get; init; } -> int` — Item-id sites (component fields) the remap edits.
- `Snapshots { get; init; } -> long` — Snapshots of a site component sent to a remapped peer.
- `WithoutLookAlike { get; init; } -> int` — Of those, items without a look-alike: peers that lack them see an empty slot.
- `Zeroed { get; init; } -> long` — Ids replaced by 0 (no look-alike, or neither a game item nor known).

## ReplSent `0.2.17` — One message a peer was sent about a watched entity (`ReplWatch.Sent`).
- `ReplSent(NetPeer? Peer, ReplChannel Channel, ReplKind Kind, EntityId Entity, uint Tick, uint Seq, uint BaselineTick, int PayloadBytes, bool Gap, IReadOnlyDictionary<string, ReadOnlyMemory<byte>> Components, IReadOnlySet<string> PackedByGame)`
- `BaselineTick { get; init; } -> uint` — An update's baseline: the tick of the state it was coded against; 0 for a create or an update against the entity's...
- `Channel { get; init; } -> ReplChannel` — The channel it went on.
- `Components { get; init; } -> IReadOnlyDictionary<string, ReadOnlyMemory<byte>>` — The full values of every component the message carried, by the game's component name (its qualified name when two...
- `Entity { get; init; } -> EntityId` — The entity.
- `Fake { get; init; } -> bool` — A lab's fake player.
- `Gap { get; init; } -> bool` — Kiln dropped records before this one (its buffer was full): an earlier message is missing.
- `Kind { get; init; } -> ReplKind` — Create, update, or destroyed (a destroy list named it; no components then).
- `Local { get; init; } -> bool` — A hosted world's own client.
- `PackedByGame { get; init; } -> IReadOnlySet<string>` — The components among them that the game packs with a codec of its own: a client sees them rounded, so a difference...
- `PayloadBytes { get; init; } -> int` — The message's payload bytes as sent (the delta, after its header).
- `Peer { get; init; } -> NetPeer?` — The peer; null for a hosted world's own client or a lab's unbound fake (see `ReplSent.PeerId`).
- `PeerId { get; init; } -> uint` — The peer's id (see `ReplPeerStats.PeerId`).
- `Seq { get; init; } -> uint` — The message's number on that peer's channel (what the peer acknowledges).
- `Tick { get; init; } -> uint` — The server tick of the message.

## ReplWatch `0.2.17` — A watch on what one entity's replication sends (`Replication.Watch`) or receives (`Replication.WatchReceived`).
- `Dispose() -> void` — Stops the watch: no event comes after the start of the next step.
- `Ended -> Action<ReplWatchEnded>?` — The watch is over (destroyed, the id reused for another template, the peer left, the world stopped).
- `Entity { get; } -> EntityId` — The watched entity.
- `HasEnded { get; } -> bool` — The watch has ended (its `ReplWatch.Ended` event came).
- `IsActive { get; } -> bool` — The watch still delivers events: not disposed, not ended.
- `Peer { get; } -> NetPeer?` — The one peer the watch is for; null for every peer (and on a client).
- `Received -> Action<ReplReceived>?` — A message this client received about the entity (a client's watch).
- `Sent -> Action<ReplSent>?` — A message a peer was sent about the entity (a server's watch).

## ReplWatchEnded `0.2.17` — A watch is over (`ReplWatch.Ended`); it was its last event.
- `ReplWatchEnded(EntityId Entity, string Reason)`
- `Entity { get; init; } -> EntityId` — The entity.
- `Reason { get; init; } -> string` — "destroyed", "id reused" (the id now belongs to an entity of another template), "peer left", or "world stopped".

## TouchResult `0.2.17` — What `Replication.Touch` did.
- `TouchResult(bool Ok, string? Reason)`
- `Entity { get; init; } -> EntityId` — The entity asked for.
- `Ok { get; init; } -> bool` — The entity is queued: the next sim step marks it and that frame's send re-sends it to every peer.
- `Reason { get; init; } -> string?` — Why not: "entity gone", or "no carrier component" (it has none of CurrentTransform, ItemState, Inventory or...
