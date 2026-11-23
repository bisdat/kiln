# Network (Kiln.Network)

## ConfigSync — Kiln's config sync (see `SyncedAttribute`).
- `const ChannelName = "kiln:config" -> string` — The name of Kiln's own config sync channel (`kiln:config`).
- `const ChannelVersion = "1" -> string` — The version of the config sync channel.
- `static CheckRoundTrip(ConfigFile file) -> string` — A self-check of the client side on one config of this process (tick thread; a lab / diagnostic tool): takes the...
- `static LastReceived { get; } -> (uint StateId, int Configs)?` — Client: the last state the server sent (id, configs), or null.
- `static ServerConfigs { get; } -> int` — Client: the server's config state is in use (configs with server values right now).
- `static Serving { get; } -> (uint StateId, int Configs)?` — Server: configs with [Synced] entries being sent, and the state id.

## ConfigSyncedEvent — Client: a Kiln server's [Synced] config values were applied to `ConfigSyncedEvent.Config`, or (`ConfigSyncedEvent.Restored`) this game's own values came back after...
- `Changed { get; } -> IReadOnlyList<string>` — Key paths whose running value changed.
- `Config { get; } -> ConfigFile` — The config file whose values were applied or restored.
- `Errors { get; } -> IReadOnlyList<string>` — Server values this game's schema refused (another mod version): those entries keep the local value.
- `ModId { get; } -> string` — The id of the mod that owns `ConfigSyncedEvent.Config`.
- `Restored { get; } -> bool` — True when the local values came back (left the server, or the server stopped syncing this config).

## DirHash — kdh1 of a folder: sha256 over `path "\0" size "\0" sha256hex "\n"` for each regular file, paths relative with `/`, NFC, sorted by their UTF-8 bytes; entries whose...
- `static Compute(string root) -> string` — kdh1 of `root`.
- `const Empty = "kdh1:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" -> string` — The kdh1 of an empty folder.
- `static Files(string root) -> List<(byte[] Key, string Path)>` — The regular files of the tree, sorted by their kdh1 key (relative NFC path, UTF-8).
- `static ModIdentity(string root) -> string` `0.2.2` — A mod's identity: the kdh1 of the files that are the mod.
- `const Prefix = "kdh1:" -> string` — The prefix every kdh1 hash string starts with.
- `static Signature(string root) -> string` — A cheap fingerprint of the folder's tree (paths, sizes, write times) to know when to hash again.

## DirHashException — A folder cannot be hashed (symlink inside, bad name, two names equal after NFC, not a folder).
- `DirHashException(string message)`

## `INetMessage<TSelf>` — A message body type with its own codec (Forge: a message class with encode/decode).
- `Decode(ref NetReader r) -> TSelf` — Reads a message body and builds the message from it.
- `Encode(NetWriter w) -> void` — Writes this message's body.

## ModSyncPolicy — What the server does when a client's mods do not match (Forge's mod rejection).
enum: Off, Warn, Refuse

## ModVersionMatch — How a client's mod version must match the server's.
enum: Exact, Major, Any

## NetChannel — A named, versioned message channel (Forge's SimpleChannel).
- `BytesIn -> long` — Traffic counters for this channel, summed over all peers.
- `BytesOut -> long` — Traffic counters for this channel, summed over all peers.
- `DecodeErrors -> long` — Traffic counters for this channel, summed over all peers.
- `Dispose() -> void` — Closes the channel (also done when the owning mod unloads).
- `MaxMessageBytes { get; } -> int` — Largest message on this channel (bytes, after encoding); never above the server's limit.
- `MessagesIn -> long` — Traffic counters for this channel, summed over all peers.
- `MessagesOut -> long` — Traffic counters for this channel, summed over all peers.
- `Name { get; } -> string` — The channel's name.
- `OnRaw(NetRawHandler handler) -> NetChannel` — Receives every message of this channel whose id has no registered type (raw bytes).
- `Register<T>(Action<T, NetContext>? handler = null, NetDirection direction = NetDirection.Both) -> NetChannel` — Registers `T` (its own Encode/Decode) under the next free id.
- `Register<T>(ushort id, Action<T, NetContext>? handler, NetDirection direction = NetDirection.Both) -> NetChannel` — Registers `T` (its own Encode/Decode) under `id`.
- `Register<T>(ushort id, Action<T, NetWriter> encode, NetDecoder<T> decode, Action<T, NetContext>? handler = null, NetDirection direction = NetDirection.Both) -> NetChannel` — Registers message type `T` under `id` with explicit codecs.
- `RegisterJson<T>(Action<T, NetContext>? handler = null, NetDirection direction = NetDirection.Both) -> NetChannel` — Registers `T` as JSON (System.Text.Json, reflection) under the next free id.
- `RegisterJson<T>(Json.Serialization.Metadata.JsonTypeInfo<T> typeInfo, Action<T, NetContext>? handler = null, NetDirection direction = NetDirection.Both) -> NetChannel` — Registers `T` as JSON with source-generated metadata (a `JsonSerializerContext`'s `JsonTypeInfo`) under the next...
- `Required { get; } -> bool` — Whether a client lacking this channel counts as lacking the mod in the mod sync check.
- `SendNear<T>(WorldPosition position, double radius, T message, NetPeer? except = null) -> int` — Server side: sends to Kiln clients whose player is within `radius` metres of `position`.
- `SendRaw(NetPeer peer, ushort messageId, ReadOnlySpan<byte> body) -> bool` — Sends raw bytes as message `messageId` (for peers using `NetChannel.OnRaw`).
- `SendTo<T>(EntityId player, T message) -> bool` — Server side: sends to the player `player`.
- `SendTo<T>(NetPeer peer, T message) -> bool` — Sends to one peer (server side: a client; client side: `Network.Server`).
- `SendToAll<T>(T message, NetPeer? except = null) -> int` — Server side: sends to every Kiln client that has this channel.
- `SendToServer<T>(T message) -> bool` — Client side: sends to the server.
- `Version { get; } -> string` — The channel's version.

## NetClientHandshakeEvent — Server: a Kiln client sent its handshake.
- `Extra { get; } -> IReadOnlyList<string>` — Client mods with `side: both` the server does not have.
- `Missing { get; } -> IReadOnlyList<string>` — Required server mods/channels the client lacks or has in another version ("id 1.2.0 (client 1.1.0)").
- `Peer { get; } -> NetPeer` — The client that sent the handshake.
- `Reason { get; set; } -> string` — Shown to a refused client.
- `Refuse { get; set; } -> bool` — Whether Kiln will refuse the client.

## NetContext — Who sent a message and how to answer.
- `Channel { get; } -> NetChannel` — The channel the message arrived on.
- `Direction { get; } -> NetDirection` — Which way the message travelled: `NetDirection.ToServer` when it is handled on the server, `NetDirection.ToClient`...
- `Player { get; } -> EntityId` — The sending player's entity (server side).
- `Reply<T>(T message) -> bool` — Sends `message` back to the sender on the same channel.
- `Sender { get; } -> NetPeer` — The client (server side) or the server (client side).

## `NetDecoder<T>` — Reads a message of type `T` from a message body.
- `Invoke(ref NetReader reader) -> T`

## NetDirection — Which way a message travels.
enum: ToServer, ToClient, Both

## NetFormatException — A message could not be decoded (truncated, oversized field, bad value).
- `NetFormatException(string message)`

## NetModpack — Where players get a server's mods (`network.json modpack`; schema common modpackRef).
- `NetModpack(string Url, string Sha256, string? Name = null, string? Version = null, long? Size = null)`
- `Name { get; init; } -> string?` — The modpack's display name, or null.
- `Sha256 { get; init; } -> string` — The SHA-256 of the download, as 64 lowercase hex digits.
- `Size { get; init; } -> long?` — The download size in bytes, or null when not stated.
- `Url { get; init; } -> string` — Where players download the modpack: an `http`, `https` or `file` URL of at most 2048 characters.
- `Version { get; init; } -> string?` — The modpack's version, or null.

## NetPeer — A connection: on the server, one client; on a client, the server (`NetPeer.IsServer`).
- `AccountId { get; } -> ulong` — The game's account id of the player (server side), 0 when unknown.
- `BytesIn -> long` — Traffic counters for this peer.
- `BytesOut -> long` — Traffic counters for this peer.
- `ConnectedAt { get; } -> DateTime` — When Kiln.Network first saw this connection (UTC).
- `Dropped -> long` — Traffic counters for this peer.
- `FramesIn { get; } -> long` `0.2.3` — Channel-12 frames this peer sent us (server side: one client; on a client: the server), every type, the HELLO and...
- `FramesOut { get; } -> long` `0.2.3` — Channel-12 frames we sent this peer that kiln.net took.
- `HasChannel(string name) -> bool` — Whether the peer has channel `name` (with a matching version).
- `Id { get; } -> uint` — kiln.net's peer id (server side: the game's machine handle).
- `IsLab { get; } -> bool` — A lab test peer (tools/nettest).
- `IsServer { get; } -> bool` — This is the server (on a client).
- `KilnVersion { get; } -> string?` — The peer's Kiln version (null until the handshake).
- `MessagesIn -> long` — Traffic counters for this peer.
- `MessagesOut -> long` — Traffic counters for this peer.
- `Mods { get; } -> IReadOnlyList<NetRemoteMod>` — The peer's mods with `side` both or client (client) / both (server), from the handshake.
- `Name { get; } -> string` — The player's platform name (server side; "server" on a client).
- `Player { get; } -> EntityId` — The player's entity (server side), `EntityId.None` before the game created it.
- `Properties { get; } -> IReadOnlyDictionary<string, string>` — Handshake properties the peer sent (see `Network.SetHandshakeProperty`).
- `RoundTrip { get; } -> TimeSpan?` — Last measured round trip (Kiln ping), null before the first.
- `State { get; } -> NetPeerState` — Where this peer stands: connecting, ready (handshake done), vanilla, rejected or closed.
- `Warnings { get; } -> IReadOnlyList<string>` — Mismatch notes from the handshake (missing / extra / different mods).

## NetPeerLeftEvent — Server: a client's connection is gone (Kiln or vanilla).
- `NetPeerLeftEvent(NetPeer peer)`
- `Peer { get; } -> NetPeer` — The client whose connection is gone.

## NetPeerReadyEvent — Server: a client finished the Kiln handshake; channels it shares are usable.
- `NetPeerReadyEvent(NetPeer peer)`
- `Peer { get; } -> NetPeer` — The client that finished the handshake.

## NetPeerState — Where a peer stands.
enum: Connecting, Ready, Vanilla, Rejected, Closed

## NetRawHandler — Handles a message whose id has no registered type, with its body as raw bytes (see `NetChannel.OnRaw`).
- `Invoke(NetContext context, ushort messageId, ReadOnlySpan<byte> body) -> void`

## NetReader — Reads a message body written by `NetWriter`.
- `NetReader()` — Creates a reader over `data`, positioned at its start.
- `NetReader(ReadOnlySpan<byte> data)` — Creates a reader over `data`, positioned at its start.
- `Bool() -> bool` — Reads a boolean (one byte; any non-zero value is true).
- `Byte() -> byte` — Reads one byte.
- `Bytes(int maxBytes = 16777216) -> byte[]` — Reads a length-prefixed byte array, as written by `NetWriter.Bytes`.
- `Double() -> double` — Reads a 64-bit float (8 bytes, little-endian).
- `End { get; } -> bool` — True when every byte has been read.
- `Entity() -> EntityId` — Reads an entity id (a 32-bit value) written by `NetWriter.Entity`.
- `Guid() -> Guid` — Reads a GUID (16 bytes).
- `Int16() -> short` — Reads a signed 16-bit integer (2 bytes, little-endian).
- `Int32() -> int` — Reads a signed 32-bit integer (4 bytes, little-endian).
- `Int64() -> long` — Reads a signed 64-bit integer (8 bytes, little-endian).
- `const MaxString = 1048576 -> int` — Strings longer than this (bytes) are refused.
- `Position { get; } -> int` — How many bytes have been read so far (the offset of the next read).
- `Position3() -> WorldPosition` — Reads a world position written by `NetWriter.Position`: three 64-bit floats (X, Y, Z).
- `Raw(int n) -> ReadOnlySpan<byte>` — Reads `n` bytes with no length prefix (the counterpart of `NetWriter.Raw`).
- `Remaining { get; } -> int` — How many bytes are left to read.
- `Rest() -> ReadOnlySpan<byte>` — Reads everything that is left.
- `Single() -> float` — Reads a 32-bit float (4 bytes, little-endian).
- `String(int maxBytes = 1048576) -> string` — Reads a string written by `NetWriter.String`: a varint byte count, then UTF-8.
- `UInt16() -> ushort` — Reads an unsigned 16-bit integer (2 bytes, little-endian).
- `UInt32() -> uint` — Reads an unsigned 32-bit integer (4 bytes, little-endian).
- `UInt64() -> ulong` — Reads an unsigned 64-bit integer (8 bytes, little-endian).
- `VarInt() -> long` — Reads a zig-zag signed varint, as written by `NetWriter.VarInt`.
- `VarUInt() -> ulong` — Reads an unsigned LEB128 varint, as written by `NetWriter.VarUInt`.

## NetRemoteMod — A mod as the other side reported it in the handshake.
- `NetRemoteMod(string Id, string Version, ModSide Side)`
- `Id { get; init; } -> string` — The mod's id.
- `Side { get; init; } -> ModSide` — Where the mod runs (`ModSide`).
- `Version { get; init; } -> string` — The mod's version string.

## NetRequiredMod — A mod players need: kind (`kiln` | `native`), id, version, kdh1 of its folder.
- `NetRequiredMod(string Id, string Version, string Hash, string Kind = "kiln", string? Name = null)`
- `Hash { get; init; } -> string` — The kdh1 hash of the mod's folder (see `DirHash`).
- `Id { get; init; } -> string` — The mod's id.
- `Kind { get; init; } -> string` — Either `kiln` (a Kiln mod) or `native` (a native mod).
- `Name { get; init; } -> string?` — The mod's display name, or null.
- `Version { get; init; } -> string` — The mod's version string.

## NetServerAdvert — What a server advertises in REJECT/WELCOME.
- `NetServerAdvert(string? ServerId, string? ServerName, string? Kiln, NetModpack? Modpack, IReadOnlyList<NetRequiredMod> Required, int RequiredCount, bool Truncated, bool Pending)`
- `Kiln { get; init; } -> string?` — The server's Kiln version, or null when it is not known.
- `Modpack { get; init; } -> NetModpack?` — Where players get the server's mods, or null when the server names no modpack.
- `Pending { get; init; } -> bool` — True while the server has not finished hashing the required mods, so their hashes are not complete yet.
- `Present { get; } -> bool` — The handshake carried Kiln's server properties (false: an old Kiln server, or advertise off).
- `Required { get; init; } -> IReadOnlyList<NetRequiredMod>` — The mods players need, with their hashes.
- `RequiredCount { get; init; } -> int` — How many mods players need in all, also when `Required` was cut short.
- `ServerId { get; init; } -> string?` — The server's stable id, or null when the handshake carried none.
- `ServerName { get; init; } -> string?` — The name the server sends to clients, or null.
- `Truncated { get; init; } -> bool` — True when `Required` holds fewer mods than `RequiredCount`.

## NetVanillaClientEvent — Server: a client finished loading and gave no proof of Kiln within `vanillaHoldSeconds` (on a build without the gate: it was in game for `handshakeTimeoutSeconds`...
- `Kick { get; set; } -> bool` — Whether Kiln will kick the client.
- `Peer { get; } -> NetPeer` — The client that was found to have no Kiln.

## Network — Kiln's mod networking: channels, peers, the join handshake and mod sync.
- `static Advertised { get; } -> NetServerAdvert` — Server side: what REJECT/WELCOME advertise: id, name, modpack and the mods players need (manifest `clients: "mod"`...
- `static Available { get; } -> bool` — The transport (kiln.net) works on this build (capability `kiln.net`).
- `static Channel(string name, string version = "1", bool required = true, int maxMessageBytes = 0) -> NetChannel` — Creates (or returns the same mod's existing) channel `name`.
- `static Channels { get; } -> IReadOnlyList<NetChannel>` — The channels open in this process.
- `static Config { get; } -> NetworkConfig` — The settings in use (`kiln/config/network.json`).
- `static ConfigError { get; } -> string?` — The last network.json load error (null when it loaded or is absent).
- `static Disconnected -> Action<NetPeer>?` — Client: the connection to the server is gone.
- `static Kick(NetPeer peer, string reason) -> Task<KickResult>` `0.2.2` — Server side: refuses a connection, also one whose player the game has not made yet (from `NetClientHandshakeEvent`...
- `static PeerOf(EntityId player) -> NetPeer?` — Server side: the peer of a player (null when not connected).
- `static Peers { get; } -> IReadOnlyList<NetPeer>` — Server side: connected clients (Kiln or vanilla), a snapshot.
- `static RefreshRequired() -> void` — Server side: hash the required mods again now (they are refreshed when the mod list changes and every 60 s anyway).
- `static Rejected -> Action<string, IReadOnlyList<string>>?` — Client: the server refused this client (reason, details).
- `static ReloadConfig() -> void` — Re-reads `kiln/config/network.json`.
- `static Server { get; } -> NetPeer?` — Client side: the server connection, null when not in a game.
- `static ServerAdvert { get; } -> NetServerAdvert?` — Client side: what the server sent in its REJECT/WELCOME (null before an answer).
- `static ServerHasKiln { get; } -> bool` — Client side: connected to a server that speaks Kiln (handshake done).
- `static ServerId { get; } -> string` — Server side: this server's stable id (network.json `serverId`, else `kiln/data/kiln-network/server-id`, generated once).
- `static ServerName { get; } -> string?` — Server side: the name sent to clients (network.json `serverName`, else enshrouded_server.json).
- `static ServerReady -> Action<NetPeer>?` — Client: the server answered the handshake (Kiln channels usable).
- `static ServerVanilla -> Action<NetPeer>?` — Client: the server has no Kiln (no answer to the handshake).
- `static SetHandshakeProperty(string key, Func<string> provider) -> void` — A value exchanged in the handshake (e.g. a content hash); the other side sees it in `NetPeer.Properties`.
- `static Status { get; } -> NetworkStatus` — The current transport and traffic counters.

## NetworkConfig — `kiln/config/network.json`: Kiln.Network's server settings.
- `Advertise { get; init; } -> bool` — Send server id/name, Kiln version, the modpack and the required mods (kdh1) in REJECT and WELCOME...
- `BytesPerSecond { get; init; } -> double` — Bytes a client may send per second (burst = 2 s worth); excess is dropped.
- `DefinitionsWaitSeconds { get; init; } -> double` — How long Kiln-only entities wait for a Kiln client's content ack, so that definitions the sync brings are in place...
- `static FilePath { get; } -> string?` — `network.json` in the config folder (`kiln/config`, or the active profile's configDir).
- `FloodKickDrops { get; init; } -> int` — Kick a client whose messages were dropped this many times within 10 s (0 = never).
- `HandshakeTimeoutSeconds { get; init; } -> double` — A player who got past the gate (a build without it) and is in game this long without a Kiln hello is treated as a...
- `KickReason { get; init; } -> uint` — The game's login error shown to refused players (1 = Version, 5 = Generic).
- `LabPort { get; init; } -> int` — Lab only: UDP port for tools/nettest's fake clients (read by the native service).
- `static Load(out string? error) -> NetworkConfig` — Reads `network.json` from the config folder.
- `MaxMessageBytes { get; init; } -> int` — Largest message (after reassembly) in either direction.
- `MessagesPerSecond { get; init; } -> double` — Messages a client may send per second (burst = 2 s worth); excess is dropped.
- `Modpack { get; init; } -> NetModpack?` — Where players get the mods this server needs (`modpack {url, sha256, name, version, size?}`); written by the...
- `ModSync { get; init; } -> ModSyncPolicy` — What to do with clients that lack a required (`side: both`) mod.
- `ModSyncStated { get; init; } -> bool` — `network.json` sets `NetworkConfig.ModSync`.
- `OptionalMods { get; init; } -> IReadOnlyList<string>` — `side: both` mods that clients may lack.
- `RequireKiln { get; init; } -> bool` — Refuse every client without Kiln, whatever `NetworkConfig.ModSync` says.
- `ServerId { get; init; } -> string?` — The server's stable id (`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`) sent to clients in REJECT/WELCOME.
- `ServerName { get; init; } -> string?` — The server's name sent to clients.
- `VanillaHoldSeconds { get; init; } -> double` — How long a client that finished loading without proof of Kiln waits in the gate before it is treated as a vanilla...
- `VersionMatch { get; init; } -> ModVersionMatch` — Version rule for required mods.
- `VersionsMatch(string server, string client) -> bool` — Whether `client` satisfies `server` under `NetworkConfig.VersionMatch`.

## NetworkStatus — Counters of Kiln.Network and its transport.
- `NetworkStatus()`
- `NetworkStatus(bool Available, bool ServerRole, bool ClientRole, bool LabTransport, int Peers, int ReadyPeers, int Channels, uint MaxFrame, long FramesIn, long FramesOut, long BytesIn, long BytesOut, long Dropped, long SendFailures, string? ConfigError)`
- `Available { get; init; } -> bool` — The kiln.net transport works on this build.
- `BytesIn { get; init; } -> long` — Bytes the transport has received.
- `BytesOut { get; init; } -> long` — Bytes the transport has sent.
- `Channels { get; init; } -> int` — How many channels are open in this process.
- `ClientRole { get; init; } -> bool` — This process has the client half of the transport.
- `ConfigError { get; init; } -> string?` — The last `network.json` load error, or null when it loaded or is absent.
- `Dropped { get; init; } -> long` — Inbound frames the transport dropped (queue full, rate limit, oversize).
- `FramesIn { get; init; } -> long` — Frames Kiln.Network has received since start.
- `FramesOut { get; init; } -> long` — Frames Kiln.Network has sent since start.
- `LabTransport { get; init; } -> bool` — The lab loopback UDP transport is open (tests only).
- `MaxFrame { get; init; } -> uint` — The largest frame the transport carries, in bytes (0 when the transport is unavailable).
- `Peers { get; init; } -> int` — Connections now: the clients on a server, plus the server link on a client.
- `ReadyPeers { get; init; } -> int` — How many of `Peers` have finished the Kiln handshake.
- `SendFailures { get; init; } -> long` — Outbound frames the game refused (no socket, budget, too big) even after retries.
- `ServerRole { get; init; } -> bool` — This process has the server half of the transport.

## NetWriter — Writes a message body.
- `NetWriter(int capacity = 256)` — Creates a writer with room for `capacity` bytes (at least 16); it grows as needed.
- `Bool(bool v) -> NetWriter` — Writes a boolean as one byte: 1 for true, 0 for false.
- `Byte(byte v) -> NetWriter` — Writes one byte.
- `Bytes(ReadOnlySpan<byte> v) -> NetWriter` — Length-prefixed bytes.
- `Clear() -> void` — Forgets everything written so far, so the writer can be reused.
- `Double(double v) -> NetWriter` — Writes a 64-bit float (8 bytes, little-endian).
- `Entity(EntityId e) -> NetWriter` — Writes an entity id as its 32-bit value.
- `Guid(Guid v) -> NetWriter` — Writes a GUID as its 16 bytes.
- `Int16(short v) -> NetWriter` — Writes a signed 16-bit integer (2 bytes, little-endian).
- `Int32(int v) -> NetWriter` — Writes a signed 32-bit integer (4 bytes, little-endian).
- `Int64(long v) -> NetWriter` — Writes a signed 64-bit integer (8 bytes, little-endian).
- `Length { get; } -> int` — Bytes written so far.
- `Position(WorldPosition p) -> NetWriter` — Writes a world position as three 64-bit floats (X, Y, Z; 24 bytes).
- `Raw(ReadOnlySpan<byte> v) -> NetWriter` — Bytes without a length (the reader must know the size).
- `Single(float v) -> NetWriter` — Writes a 32-bit float (4 bytes, little-endian).
- `String(string? v) -> NetWriter` — Writes a string: a varint byte count followed by the UTF-8 bytes.
- `ToArray() -> byte[]` — Copies the bytes written so far into a new array.
- `UInt16(ushort v) -> NetWriter` — Writes an unsigned 16-bit integer (2 bytes, little-endian).
- `UInt32(uint v) -> NetWriter` — Writes an unsigned 32-bit integer (4 bytes, little-endian).
- `UInt64(ulong v) -> NetWriter` — Writes an unsigned 64-bit integer (8 bytes, little-endian).
- `VarInt(long v) -> NetWriter` — Zig-zag signed varint.
- `VarUInt(ulong v) -> NetWriter` — Unsigned LEB128 (1 byte below 128).
- `WrittenSpan { get; } -> ReadOnlySpan<byte>` — The bytes written so far, as a view of the writer's own buffer.

## SteamAcceptMode `0.2.13` — Whom a session-answering lease accepts (kiln_net.h's `KILN_STEAM_ACCEPT_*`).
enum: None, Any, Listed

## SteamAcceptResult `0.2.13` — The answer of `SteamMessageLease.Accept`.
- `SteamAcceptResult()`
- `SteamAcceptResult(bool Ok, string? Reason, SteamMessagesRefusal Refusal)`
- `Ok { get; init; } -> bool` — True when Steam accepted the session.
- `Reason { get; init; } -> string?` — Why not; null when accepted.
- `Refusal { get; init; } -> SteamMessagesRefusal` — Why not, as a value.

## SteamLeaseResult `0.2.13` — The answer of `SteamMessages.Open`.
- `SteamLeaseResult(bool Ok, string? Reason, SteamMessagesRefusal Refusal, SteamMessageLease? Lease)`
- `Lease { get; init; } -> SteamMessageLease?` — The lease; null when refused.
- `Ok { get; init; } -> bool` — True when the lease is open.
- `Reason { get; init; } -> string?` — Why not; null when open.
- `Refusal { get; init; } -> SteamMessagesRefusal` — Why not, as a value; `SteamMessagesRefusal.None` when open.

## SteamMessageInfo `0.2.13` — The header of one received message (`SteamReceiveResult.Message`); its bytes are the first `SteamMessageInfo.Length` bytes of the buffer given to...
- `SteamMessageInfo()`
- `SteamMessageInfo(ulong Sender, ushort Channel, int Length, bool Reliable)`
- `Channel { get; init; } -> ushort` — The Steam message channel it came on.
- `Length { get; init; } -> int` — Bytes copied into the buffer.
- `Reliable { get; init; } -> bool` — It came reliably.
- `Sender { get; init; } -> ulong` — The sender's Steam id.

## SteamMessageLease `0.2.13` — A lease on an inclusive range of Steam message channels (`SteamMessages.Open`).
- `Accept(ulong steamId) -> SteamAcceptResult` — Accepts a pending session request of `steamId` by hand (any lease may).
- `AnswersSessions { get; } -> bool` — This lease answers Steam's session requests through its accept policy.
- `Dispose() -> void` — Closes the lease.
- `FirstChannel { get; } -> ushort` — The lease's first channel.
- `IsOpen { get; } -> bool` — False once the lease was disposed or its mod unloaded.
- `LastChannel { get; } -> ushort` — The lease's last channel (inclusive).
- `Receive(ushort channel, Span<byte> buffer) -> SteamReceiveResult` — Takes at most one message off Steam's queue of `channel` and copies it into `buffer`.
- `Send(ulong steamId, ushort channel, ReadOnlySpan<byte> data, SteamSendFlags flags = SteamSendFlags.Reliable) -> SteamSendResult` — Sends one message to `steamId` on `channel`.
- `SetAcceptPolicy(SteamAcceptMode mode, IReadOnlyCollection<ulong>? steamIds = null) -> SteamPolicyResult` — Sets whom this lease accepts when a Steam user asks for a session.

## SteamMessages `0.2.13` — A mod's own Steam peer-to-peer messages (Steam's ISteamNetworkingMessages) between game processes, by Steam id: separate from the game connection and from...
- `static Available { get; } -> bool` — True when Steam messages work here now: this kiln.net has them, the capability row is not unavailable, and Steam is...
- `const Capability = "net.steam_messages" -> string` — The capability row: pending until a call finds Steam ready here, then available.
- `const MaxLeases = 16 -> int` — Open leases in the process, every owner's.
- `const MaxListedIds = 256 -> int` — Steam ids in a `SteamAcceptMode.Listed` policy.
- `const MaxMessageBytes = 524288 -> int` — The most bytes in one message (kiln_net.h's `KILN_STEAM_MESSAGE_MAX`), and the receive buffer that never drops a...
- `static Open(ushort firstChannel, ushort lastChannel, bool answerSessions = false) -> SteamLeaseResult` — Opens a lease on the Steam message channels `firstChannel` to `lastChannel` (inclusive) for the calling mod.
- `static Status { get; } -> SteamMessagesStatus` — kiln.net's Steam messages status, read now.

## SteamMessagesFlags `0.2.13` — What kiln.net's Steam messages status says (kiln_net.h's `KILN_STEAM_MSG_*`).
enum: None, SteamReady, GameServer, HasIdentity, SessionsAnswered, SessionLeaseOpen, ClosedForExit

## SteamMessagesRefusal `0.2.13` — Why a Steam messages call did not do what it was asked.
enum: None, NoMessage, SteamNotReady, SteamRefused, Dropped, LeaseClosed, ClosedForExit, NotPermitted, Taken, Fault

## SteamMessagesStatus `0.2.13` — One read of kiln.net's Steam messages status (`SteamMessages.Status`).
- `SteamMessagesStatus()`
- `SteamMessagesStatus(bool Answered, SteamMessagesFlags Flags, ulong LocalSteamId, long SessionsAccepted, long SessionsRefused, SteamAcceptMode AcceptMode, int ListedIds, string? Reason)`
- `AcceptMode { get; init; } -> SteamAcceptMode` — The accept policy in force.
- `Answered { get; init; } -> bool` — kiln.net filled the status.
- `ClosedForExit { get; } -> bool` — `SteamMessagesStatus.Flags` has `SteamMessagesFlags.ClosedForExit`.
- `Flags { get; init; } -> SteamMessagesFlags` — What there is.
- `ListedIds { get; init; } -> int` — Steam ids in a `SteamAcceptMode.Listed` policy, else 0.
- `LocalSteamId { get; init; } -> ulong` — This process's Steam id with `SteamMessagesFlags.HasIdentity`, else 0.
- `Reason { get; init; } -> string?` — Why the read is not a plain answer (not answered, the game is exiting, a Steam call faulted); null otherwise.
- `SessionsAccepted { get; init; } -> long` — Session requests the session callback accepted since the game started.
- `SessionsRefused { get; init; } -> long` — Session requests it left unanswered (the policy, a bad identity, the exit, Steam said no).
- `SteamReady { get; } -> bool` — `SteamMessagesStatus.Flags` has `SteamMessagesFlags.SteamReady`.

## SteamPolicyResult `0.2.13` — The answer of `SteamMessageLease.SetAcceptPolicy`.
- `SteamPolicyResult(bool Ok, string? Reason, SteamMessagesRefusal Refusal, SteamMessagesStatus Status)`
- `Ok { get; init; } -> bool` — True when the policy is in force.
- `Reason { get; init; } -> string?` — Why not; null when in force.
- `Refusal { get; init; } -> SteamMessagesRefusal` — Why not, as a value.
- `Status { get; init; } -> SteamMessagesStatus` — kiln.net's status right after the call.

## SteamReceiveResult `0.2.13` — The answer of `SteamMessageLease.Receive` (a struct: the per-message path allocates nothing).
- `SteamReceiveResult()`
- `SteamReceiveResult(bool Ok, string? Reason, SteamMessagesRefusal Refusal, SteamMessageInfo Message)`
- `Message { get; init; } -> SteamMessageInfo` — The message's header when `Ok`, else default.
- `Ok { get; init; } -> bool` — True when one message was taken and copied into the buffer.
- `Reason { get; init; } -> string?` — Why not; null when received.
- `Refusal { get; init; } -> SteamMessagesRefusal` — Why not, as a value: `SteamMessagesRefusal.NoMessage` when nothing waits.

## SteamSendFlags `0.2.13` — How a message is sent (Steam's `k_nSteamNetworkingSend_*`; kiln_net.h's `KILN_STEAM_SEND_*`).
enum: Unreliable, Reliable, RestartBrokenSession

## SteamSendResult `0.2.13` — The answer of `SteamMessageLease.Send` (a struct: the per-message path allocates nothing).
- `SteamSendResult()`
- `SteamSendResult(bool Ok, string? Reason, SteamMessagesRefusal Refusal, int SteamResult)`
- `Ok { get; init; } -> bool` — True when Steam took the message (its EResult was 1, OK).
- `Reason { get; init; } -> string?` — Why not; null when sent.
- `Refusal { get; init; } -> SteamMessagesRefusal` — Why not, as a value.
- `SteamResult { get; init; } -> int` — Steam's EResult when Steam was called (1 = OK), else 0.

## SyncSettings — `kiln/config/sync.json`: what Kiln's server-to-client sync (content and configs) does.
- `AcceptConfigs { get; set; } -> bool` — Client: whether to use a server's `[Synced]` config values while connected.
- `AcceptContent { get; set; } -> bool` — Client: whether to apply a server's content changes while connected.
- `static Current { get; } -> SyncSettings` — The settings in use (registered as Kiln's config "sync" at the first use on the tick thread; defaults before that).
- `MatchServer { get; set; } -> bool` — Client: whether to also put back, while connected, what this game changed on its own (local mods and data packs)...
- `MaxContentKiB { get; set; } -> int` — The largest content document a server sends or a client accepts, in KiB of uncompressed JSON.
- `SendConfigs { get; set; } -> bool` — Server: whether to send the `[Synced]` entries of server and common configs to Kiln clients.
- `SendContent { get; set; } -> bool` — Server: whether to send this server's content changes (items and recipes) to Kiln clients.
