// SPDX-License-Identifier: MIT
// The owner's read-only copy of their player storages on their own game (0.2.0-dev.30.7, GitHub issue 128):
// PlayerStorage.Client.Rows, PlayerStorageClientChangedEvent and the two halves of PlayerStorage.Client.ProvideAmmo.
//
// Kiln's channel "kiln:storage" v1, opened by Kiln itself on both sides (not by a mod), not required: a game without it,
// or without Kiln, joins as before and gets nothing. The server is the authority: no message from a game ever changes a
// storage. About four times a second the server builds each player's rows (every ready storage of that player, any
// mod's) and, when they differ from what it sent, sends that player the whole set; never another player's.
//
//   server -> client  Update  v1: [view, seq] [storages: key, slots, rows: slot, item, count, level, flags]
//   client -> server  Ack     v1: [view, seq] the update the game applied
//   client -> server  Pick    v1: [key, kind, item] a pick of a row ProvideAmmo showed: a wish the server checks
//
// Delivery: kiln.net can accept an update and lose it later, so the server sends the latest set again when the game has
// not acknowledged it after 5 s, each wait twice the one before up to 60 s (a game that never answers costs one update
// a minute). A view is one player's session on the server (a random nonzero id): the game drops an update of its view
// older than the one it applied, and starts over when the view changes.
//
// Limits (the game drops an update that breaks them, whole): 32 storages, 64 slots each, 256 rows in all (Kiln's
// page budget per player), keys as Items.RegisterPlayerStorage makes them (128 bytes at most), item 0 or a count of 0
// never. A pick is read only for a storage whose own mod called ProvideAmmo on the server, at most 4 a second per
// player (8 at once), and checked like any request from a game: the storage is the sender's, the item is in it, it
// is ammunition of the kind picked.
//
// A hosting game is the server and the client in one process: its own player's rows go into the copy directly, and its
// picks are handed to the server tick through a queue, never over the network. Docs: docs/kiln/services/player-storage.md
// "The client copy and the HUD's ammunition lists".
using System.Collections.Concurrent;
using System.Text;
using System.Text.RegularExpressions;

namespace Kiln;

// ---- the wire ----

/// <summary>One slot of a storage on the wire.</summary>
internal readonly record struct StorageMirrorRow(byte Slot, uint Item, uint Count, ushort Level, uint Flags);

/// <summary>One storage on the wire: its key, its slots and its non-empty slots.</summary>
internal sealed record StorageMirrorEntry(string Key, byte Slots, StorageMirrorRow[] Rows)
{
    public bool SameAs(StorageMirrorEntry o) => Key == o.Key && Slots == o.Slots && Rows.AsSpan().SequenceEqual(o.Rows);
}

/// <summary>Server to client: every ready storage of the player, whole.</summary>
internal sealed record StorageMirrorUpdate(uint View, uint Seq, StorageMirrorEntry[] Storages)
{
    public const ushort Id = 1;
    public const int MaxStorages = StorageNative.MaxStorages, MaxSlots = StorageNative.MaxSlots, MaxRows = StorageNative.MaxSlotsPlayer,
        MaxKeyBytes = StorageNative.KeyMax;

    /// <summary>The largest update in bytes: what the channel is opened for.</summary>
    public const int MaxBytes = 4 + 4 + 1 + MaxStorages * (5 + MaxKeyBytes + 1 + 1) + MaxRows * (1 + 4 + 4 + 2 + 4) + 16;

    public static void Encode(StorageMirrorUpdate m, NetWriter w)
    {
        w.UInt32(m.View).UInt32(m.Seq).Byte((byte)m.Storages.Length);
        foreach (var s in m.Storages)
        {
            w.String(s.Key).Byte(s.Slots).Byte((byte)s.Rows.Length);
            foreach (var r in s.Rows) w.Byte(r.Slot).UInt32(r.Item).UInt32(r.Count).UInt16(r.Level).UInt32(r.Flags);
        }
    }

    /// <summary>Reads an update and checks every limit; a broken one throws <see cref="NetFormatException"/> (the channel
    /// drops it, whole).</summary>
    public static StorageMirrorUpdate Decode(ref NetReader r)
    {
        var view = r.UInt32();
        var seq = r.UInt32();
        if (view == 0 || seq == 0) throw new NetFormatException("kiln:storage update without a view or number");
        int n = r.Byte(), rows = 0;
        if (n > MaxStorages) throw new NetFormatException($"kiln:storage update with {n} storages (limit {MaxStorages})");
        var list = new StorageMirrorEntry[n];
        var keys = new HashSet<string>(StringComparer.Ordinal);
        for (var i = 0; i < n; i++)
        {
            var key = r.String(MaxKeyBytes);
            if (!PlayerStorage.FullKeyRule.IsMatch(key) || !keys.Add(key)) throw new NetFormatException("kiln:storage update with a bad or repeated key");
            int slots = r.Byte(), k = r.Byte();
            if (slots is < 1 or > MaxSlots || k > slots) throw new NetFormatException($"kiln:storage update: {key} has {k} rows in {slots} slots");
            rows += k;
            if (rows > MaxRows) throw new NetFormatException($"kiln:storage update with more than {MaxRows} rows");
            var items = new StorageMirrorRow[k];
            var last = -1;
            for (var j = 0; j < k; j++)
            {
                var row = new StorageMirrorRow(r.Byte(), r.UInt32(), r.UInt32(), r.UInt16(), r.UInt32());
                if (row.Slot >= slots || row.Slot <= last || row.Item == 0 || row.Count == 0 || row.Count > int.MaxValue)
                    throw new NetFormatException($"kiln:storage update: a bad row in {key}");
                last = row.Slot;
                items[j] = row;
            }
            list[i] = new StorageMirrorEntry(key, (byte)slots, items);
        }
        Padding(ref r);
        return new StorageMirrorUpdate(view, seq, list);
    }

    // kiln.net may pad a frame by up to 3 zero bytes; anything else after the message is a broken message
    internal static void Padding(ref NetReader r)
    {
        if (r.Remaining > 3) throw new NetFormatException("kiln:storage message with trailing bytes");
        foreach (var b in r.Rest())
            if (b != 0) throw new NetFormatException("kiln:storage message with trailing bytes");
    }
}

/// <summary>Client to server: the game applied this update.</summary>
internal readonly record struct StorageMirrorAck(uint View, uint Seq)
{
    public const ushort Id = 2;
    public static void Encode(StorageMirrorAck m, NetWriter w) => w.UInt32(m.View).UInt32(m.Seq);

    public static StorageMirrorAck Decode(ref NetReader r)
    {
        var m = new StorageMirrorAck(r.UInt32(), r.UInt32());
        StorageMirrorUpdate.Padding(ref r);
        return m;
    }
}

/// <summary>Client to server: the player picked a row ProvideAmmo showed (kind 1 arrow, 2 spell). A wish.</summary>
internal sealed record StorageAmmoPick(string Key, byte Kind, uint Item)
{
    public const ushort Id = 3;
    public static void Encode(StorageAmmoPick m, NetWriter w) => w.String(m.Key).Byte(m.Kind).UInt32(m.Item);

    public static StorageAmmoPick Decode(ref NetReader r)
    {
        var key = r.String(StorageMirrorUpdate.MaxKeyBytes);
        var m = new StorageAmmoPick(key, r.Byte(), r.UInt32());
        if (!PlayerStorage.FullKeyRule.IsMatch(key) || m.Kind is not ((byte)AmmoKinds.Arrow or (byte)AmmoKinds.Spell) || m.Item == 0)
            throw new NetFormatException("kiln:storage pick with a bad key, kind or item");
        StorageMirrorUpdate.Padding(ref r);
        return m;
    }
}

// ---- the copy on the player's game ----

/// <summary>What a game holds of its player's storages: the rows of the last update it applied, by key. Any thread.</summary>
internal sealed class StorageMirrorStore
{
    private readonly object _gate = new();
    private uint _view, _seq;
    private Dictionary<string, PlayerStorageRow[]> _rows = new(StringComparer.Ordinal);

    /// <summary>The rows of <paramref name="key"/> by slot; empty when the copy has none.</summary>
    public IReadOnlyList<PlayerStorageRow> Rows(string key)
    {
        lock (_gate) return _rows.TryGetValue(key, out var rows) ? rows : [];
    }

    /// <summary>The keys the copy holds rows of.</summary>
    public IReadOnlyList<string> Keys()
    {
        lock (_gate) return [.. _rows.Keys.Order(StringComparer.Ordinal)];
    }

    /// <summary>Applies an update. Null when it is older than the one applied in its view (dropped, nothing changes);
    /// otherwise the keys whose rows changed, with their rows now (empty: gone).</summary>
    public List<(string Key, PlayerStorageRow[] Rows)>? Apply(StorageMirrorUpdate u)
    {
        var next = new Dictionary<string, PlayerStorageRow[]>(StringComparer.Ordinal);
        foreach (var s in u.Storages)
            next[s.Key] = [.. s.Rows.Select(r => new PlayerStorageRow(s.Key, r.Slot, new ItemId(r.Item), (int)r.Count, r.Level, r.Flags))];
        lock (_gate)
        {
            if (u.View == _view && u.Seq <= _seq) return null;
            _view = u.View;
            _seq = u.Seq;
            return Swap(next);
        }
    }

    /// <summary>Forgets everything (the game left the world); the keys that had rows, now empty.</summary>
    public List<(string Key, PlayerStorageRow[] Rows)> Clear()
    {
        lock (_gate)
        {
            _view = 0;
            _seq = 0;
            return Swap(new Dictionary<string, PlayerStorageRow[]>(StringComparer.Ordinal));
        }
    }

    private List<(string Key, PlayerStorageRow[] Rows)> Swap(Dictionary<string, PlayerStorageRow[]> next)
    {
        var changed = new List<(string, PlayerStorageRow[])>();
        foreach (var (key, rows) in next)
            if (!_rows.TryGetValue(key, out var before) || !before.AsSpan().SequenceEqual(rows)) changed.Add((key, rows));
        foreach (var key in _rows.Keys)
            if (!next.ContainsKey(key)) changed.Add((key, []));
        _rows = next;
        changed.Sort((a, b) => string.CompareOrdinal(a.Item1, b.Item1));
        return changed;
    }
}

// ---- the seam to kiln.net, kiln.players and kiln.client (tests put a loopback in its place) ----

/// <summary>A player the server sends rows to: <paramref name="Local"/> is this machine's own player of a hosted
/// game (rows go into this process's copy).</summary>
internal readonly record struct StorageMirrorViewer(EntityId Player, ulong Account, bool Local);

internal interface IStorageMirrorLink
{
    /// <summary>Server: the players in game.</summary>
    IReadOnlyList<StorageMirrorViewer> Viewers();
    /// <summary>Server: the player's game has Kiln with this channel (a vanilla game is never sent rows).</summary>
    bool CanSend(EntityId player);
    /// <summary>Server: sends an update to a player's game; false when it has no Kiln with this channel.</summary>
    bool Send(EntityId player, StorageMirrorUpdate update);
    /// <summary>Client: sends a pick to the server; false without a Kiln server that has this channel.</summary>
    bool SendPick(StorageAmmoPick pick);
    /// <summary>Client: this game hosts the world it plays in.</summary>
    bool Hosting { get; }
    /// <summary>Client: the local player (a hosted game's own player).</summary>
    EntityId LocalPlayer { get; }
}

/// <summary>The real link: kiln.net's channel, kiln.players' list, kiln.client's session.</summary>
internal sealed class NetStorageMirrorLink(NetChannel channel) : IStorageMirrorLink
{
    public IReadOnlyList<StorageMirrorViewer> Viewers()
    {
        var list = new List<StorageMirrorViewer>();
        try
        {
            if (!Players.Available) return list;
            var hosting = Hosting;
            var local = hosting ? LocalPlayer : EntityId.None;
            foreach (var p in Players.All)
                if (p.InGame && !p.Entity.IsNone)
                    list.Add(new StorageMirrorViewer(p.Entity, p.AccountId, hosting && (p.Entity == local || (local.IsNone && p.Slot == 0))));
        }
        catch (KilnUnavailableException)
        {
            // no server world (any more): nobody is in game, and a hosting game's own copy empties
        }
        return list;
    }

    public bool CanSend(EntityId player) =>
        Network.PeerOf(player) is { State: NetPeerState.Ready } peer && peer.HasChannel(PlayerStorageMirror.ChannelName);

    public bool Send(EntityId player, StorageMirrorUpdate update) =>
        Network.PeerOf(player) is { State: NetPeerState.Ready } peer && peer.HasChannel(PlayerStorageMirror.ChannelName) &&
        channel.SendTo(peer, update);

    public bool SendPick(StorageAmmoPick pick) => channel.SendToServer(pick);

    public bool Hosting
    {
        get
        {
            try { return Client.Available && Client.IsHosting; }
            catch (Exception) { return false; }  // no kiln.client
        }
    }

    public EntityId LocalPlayer
    {
        get
        {
            try { return Client.Available ? Client.LocalPlayer : EntityId.None; }
            catch (Exception) { return EntityId.None; }
        }
    }
}

// ---- the server half of ProvideAmmo ----

/// <summary>A storage whose mod takes its players' picks of the rows <see cref="PlayerStorage.Client.ProvideAmmo"/>
/// showed (one per key, on the server).</summary>
internal sealed class StorageAmmoAcceptor(string key, AmmoKinds kinds, bool activate, ModContext? mod)
{
    public readonly string Key = key;
    public readonly AmmoKinds Kinds = kinds;
    public readonly bool Activate = activate;
    public readonly ModContext? Mod = mod;
}

/// <summary>Both halves of the client copy: the server's sender and pick check, the game's copy and its picks.</summary>
internal static class PlayerStorageMirror
{
    public const string ChannelName = "kiln:storage";
    public const string ChannelVersion = "1";
    internal const long PassMs = 250, FirstWaitMs = 5000, MaxWaitMs = 60000;
    internal const double PicksPerSecond = 4, PickBurst = 8;

    private static readonly object Gate = new();
    private static NetChannel? _channel;
    private static IStorageMirrorLink? _link;
    private static bool _started;
    private static IDisposable? _tick;
    private static long _lastPass = long.MinValue;
    private static long _picksRefused, _sent;

    /// <summary>This process's copy (a game client's, or a hosting game's own player's).</summary>
    internal static readonly StorageMirrorStore Store = new();

    /// <summary>Real time in ms; tests set their own.</summary>
    internal static Func<long> Clock = static () => Environment.TickCount64;

    /// <summary>Test hook: the equip a pick runs (player, storage slot, equipment slot, item entity); null: Items.Equip.</summary>
    internal static Func<EntityId, SlotId, EquipmentSlot, EntityId, Task<EquipResult>>? EquipHook;

    private sealed class View
    {
        public uint Id, Seq, SentSeq;
        public ulong Account;
        public bool Local, Acked = true;
        public long SentMs, WaitMs = FirstWaitMs;
        public StorageMirrorEntry[] Built = [];
        public double Tokens = PickBurst;
        public long TokensMs;
        public readonly HashSet<(string Key, uint Item)> Pending = [];
    }

    private static readonly Dictionary<uint, View> Views = new();
    private static readonly Dictionary<string, StorageAmmoAcceptor> Acceptors = new(StringComparer.Ordinal);
    private static readonly ConcurrentQueue<(EntityId Player, StorageAmmoPick Pick)> LocalPicks = new();

    /// <summary>The channel is open here (or a test's link stands in): <c>items.storage.client</c> is available.</summary>
    internal static bool Open
    {
        get { lock (Gate) return _link is not null; }
    }

    /// <summary>Kiln.Items' start (both sides), or the first use of the client copy (a mod's OnLoad runs before Kiln.Items'
    /// start): the channel, the service tick, the capability row.</summary>
    internal static void Start()
    {
        lock (Gate)
        {
            if (_started) return;
            _started = true;
        }
        try
        {
            if (Network.Available) OpenChannel();
        }
        catch (Exception e) when (e is FileNotFoundException or FileLoadException or TypeLoadException or TypeInitializationException)
        {
            Backend.Current.Log(0, LogLevel.Warn, $"kiln.items: the channel {ChannelName} needs Kiln.Network: {e.Message}");
        }
        lock (Gate) _tick ??= ServiceTickAccess.Add("kiln.items storage copy", static _ => Pass());
        ReportCapability();
    }

    private static void OpenChannel()
    {
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // Kiln's own channel, not a mod's
        try
        {
            var ch = Network.Channel(ChannelName, ChannelVersion, required: false, maxMessageBytes: StorageMirrorUpdate.MaxBytes)
                .Register<StorageMirrorUpdate>(StorageMirrorUpdate.Id, static (m, w) => StorageMirrorUpdate.Encode(m, w),
                    static (ref NetReader r) => StorageMirrorUpdate.Decode(ref r), static (m, _) => OnUpdate(m), NetDirection.ToClient)
                .Register<StorageMirrorAck>(StorageMirrorAck.Id, static (m, w) => StorageMirrorAck.Encode(m, w),
                    static (ref NetReader r) => StorageMirrorAck.Decode(ref r), static (m, c) => OnAck(c.Player, m), NetDirection.ToServer)
                .Register<StorageAmmoPick>(StorageAmmoPick.Id, static (m, w) => StorageAmmoPick.Encode(m, w),
                    static (ref NetReader r) => StorageAmmoPick.Decode(ref r), static (m, c) => OnPick(c.Player, m), NetDirection.ToServer);
            Network.Disconnected += static _ => ClientGone();
            lock (Gate)
            {
                _channel = ch;
                _link = new NetStorageMirrorLink(ch);
            }
        }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or NotSupportedException or KilnUnavailableException)
        {
            Backend.Current.Log(0, LogLevel.Warn, $"kiln.items: the channel {ChannelName} is unavailable: {e.Message}");
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    /// <summary>The row <c>items.storage.client</c>: available where the channel is open (both sides: the server sends,
    /// the game holds the copy); without kiln.net unavailable.</summary>
    internal static void ReportCapability()
    {
        if (Open)
            ServiceCaps.Report(PlayerStorage.Client.Capability, CapabilityState.Available, CapabilityReason.None, $"{ChannelName} v{ChannelVersion}");
        else
            ServiceCaps.Report(PlayerStorage.Client.Capability, CapabilityState.Unavailable, CapabilityReason.ServiceMissing,
                $"the {ChannelName} channel needs kiln.net, which is not here");
    }

    // ================================================================ the game ====

    private static void OnUpdate(StorageMirrorUpdate m)
    {
        try
        {
            if (_link is { Hosting: true }) return;  // a hosting game fills its copy itself
        }
        catch (Exception)
        {
        }
        var changed = Store.Apply(m);
        _channel?.SendToServer(new StorageMirrorAck(m.View, m.Seq));
        if (changed is not null) Changed(changed);
    }

    private static void ClientGone()
    {
        try
        {
            Changed(Store.Clear());
        }
        catch (Exception e)
        {
            Backend.Current.Log(0, LogLevel.Warn, $"kiln.items: clearing the storage copy failed: {e.Message}");
        }
    }

    private static void Changed(List<(string Key, PlayerStorageRow[] Rows)> changed)
    {
        foreach (var (key, rows) in changed)
        {
            PlayerStorageAmmo.Changed(key);
            if (Events.HasSubscribers<PlayerStorageClientChangedEvent>()) Events.Post(new PlayerStorageClientChangedEvent(key, rows));
        }
    }

    /// <summary>The game: the rows of <paramref name="fullKey"/> whose item is ammunition of <paramref name="kind"/>, one
    /// per item (counts added up), in slot order. For the HUD's list (any thread).</summary>
    internal static List<(ItemId Item, int Count)> AmmoRows(string fullKey, AmmoKinds kind)
    {
        var list = new List<(ItemId Item, int Count)>();
        foreach (var r in Store.Rows(fullKey))
        {
            if (KindOf(r.Item) != kind) continue;
            var i = list.FindIndex(x => x.Item == r.Item);
            if (i < 0) list.Add((r.Item, r.Count));
            else list[i] = (r.Item, (int)Math.Min(int.MaxValue, (long)list[i].Count + r.Count));
        }
        return list;
    }

    /// <summary>The game: the player picked a row of <paramref name="fullKey"/> (the UI thread): to the server, or on a
    /// hosting game to its own server tick. False when nothing could take it.</summary>
    internal static bool Pick(string fullKey, AmmoKinds kind, ItemId item)
    {
        var link = _link;
        if (link is null || item.IsNone) return false;
        var pick = new StorageAmmoPick(fullKey, (byte)kind, item.Value);
        if (link.Hosting)
        {
            var player = link.LocalPlayer;
            if (player.IsNone) return false;
            LocalPicks.Enqueue((player, pick));
            return true;
        }
        return link.SendPick(pick);
    }

    /// <summary>The game's equipment slot type of ammunition (the item's <see cref="ItemInfo.EquipSlot"/>): 6 arrows, 7
    /// spells (docs/re/items.md, 0x936c30).</summary>
    internal static AmmoKinds KindOf(ItemId item) => Items.Get(item)?.EquipSlot switch
    {
        6 => AmmoKinds.Arrow,
        7 => AmmoKinds.Spell,
        _ => 0,
    };

    // ================================================================ server ====

    /// <summary>The service tick (both sides; does work only where players are in game): local picks, then about four
    /// times a second every player's rows.</summary>
    internal static void Pass()
    {
        IStorageMirrorLink? link;
        lock (Gate) link = _link;
        if (link is null) return;
        while (LocalPicks.TryDequeue(out var p)) HandlePick(p.Player, p.Pick);
        var now = Clock();
        if (_lastPass != long.MinValue && now - _lastPass < PassMs) return;
        _lastPass = now;
        try
        {
            if (!Game.IsTickThread) return;
            if (!Capabilities.Is(PlayerStorage.Capability) && Views.Count == 0) return;  // no storage here and nobody was sent rows
            Send(link, now);
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException)
        {
            // no server world, or kiln.players is not here: nothing to send this pass
        }
    }

    private static void Send(IStorageMirrorLink link, long now)
    {
        var present = new HashSet<uint>();
        foreach (var v in link.Viewers())
        {
            present.Add(v.Player.Value);
            if (!Views.TryGetValue(v.Player.Value, out var view) || view.Account != v.Account || view.Local != v.Local)
            {
                if (view is { Local: true } && !v.Local) Changed(Store.Clear());
                Views[v.Player.Value] = view = new View { Id = NewViewId(), Account = v.Account, Local = v.Local, TokensMs = now };
            }
            if (!v.Local && !link.CanSend(v.Player))
            {
                view.Built = [];  // no Kiln game with the channel (yet): nothing is built for it; sent once it has one
                view.Acked = true;
                continue;
            }
            var built = Build(v.Player);
            var same = built.Length == view.Built.Length && built.Zip(view.Built).All(x => x.First.SameAs(x.Second));
            if (same && (view.Acked || now - view.SentMs < view.WaitMs)) continue;
            if (same) view.WaitMs = Math.Min(MaxWaitMs, view.WaitMs * 2);  // the game did not acknowledge: again, later each time
            else view.WaitMs = FirstWaitMs;
            view.Built = built;
            var update = new StorageMirrorUpdate(view.Id, ++view.Seq, built);
            if (view.Local)
            {
                if (Store.Apply(update) is { } changed) Changed(changed);
                view.Acked = true;
                continue;
            }
            if (!link.Send(v.Player, update))
            {
                view.Built = [];  // no Kiln game with the channel (yet): try again when it has one
                view.Acked = true;
                continue;
            }
            Interlocked.Increment(ref _sent);
            view.SentSeq = update.Seq;
            view.SentMs = now;
            view.Acked = false;
        }
        foreach (var gone in Views.Keys.Where(k => !present.Contains(k)).ToList())
        {
            if (Views[gone].Local) Changed(Store.Clear());
            Views.Remove(gone);
        }
    }

    private static uint NewViewId()
    {
        uint id;
        do id = (uint)Random.Shared.NextInt64(1, uint.MaxValue); while (id == 0);
        return id;
    }

    /// <summary>The rows of every ready storage of <paramref name="player"/> (tick thread), sorted by key.</summary>
    internal static StorageMirrorEntry[] Build(EntityId player)
    {
        IReadOnlyList<PlayerStorageInfo> infos;
        try
        {
            infos = PlayerStorageRuntime.Of(player);
        }
        catch (KilnUnavailableException)
        {
            return [];
        }
        var list = new List<StorageMirrorEntry>();
        var total = 0;
        foreach (var info in infos)
        {
            if (info.Status.State != PlayerStorageState.Ready || info.Holder.IsNone || info.Slots is < 1 or > StorageMirrorUpdate.MaxSlots) continue;
            if (list.Count == StorageMirrorUpdate.MaxStorages || Encoding.UTF8.GetByteCount(info.FullKey) > StorageMirrorUpdate.MaxKeyBytes) continue;
            if (Items.Read(info.Holder) is not { } contents) continue;
            var pages = new Dictionary<uint, int>();
            for (var i = 0; i < contents.Inventories.Count; i++) pages[contents.Inventories[i].Inventory.Value] = i;
            var rows = new List<StorageMirrorRow>();
            foreach (var s in contents.Slots)
            {
                if (s.IsEmpty || s.Count <= 0 || !pages.TryGetValue(s.Slot.Inventory.Value, out var page)) continue;
                var index = page * 8 + s.Slot.Index;
                if (index >= info.Slots) continue;
                rows.Add(new StorageMirrorRow((byte)index, s.Item.Value, (uint)s.Count, (ushort)Math.Clamp(s.Level, 0, ushort.MaxValue), 0));
            }
            rows.Sort((a, b) => a.Slot.CompareTo(b.Slot));
            if (total + rows.Count > StorageMirrorUpdate.MaxRows) break;
            total += rows.Count;
            list.Add(new StorageMirrorEntry(info.FullKey, (byte)info.Slots, [.. rows]));
        }
        list.Sort((a, b) => string.CompareOrdinal(a.Key, b.Key));
        return [.. list];
    }

    internal static void OnAck(EntityId player, StorageMirrorAck m)
    {
        if (!Views.TryGetValue(player.Value, out var view) || m.View != view.Id || m.Seq != view.SentSeq) return;
        view.Acked = true;
        view.WaitMs = FirstWaitMs;
    }

    private static void OnPick(EntityId player, StorageAmmoPick m) => HandlePick(player, m);

    /// <summary>The server: a player's pick of a row ProvideAmmo showed (tick thread). A wish: every part is checked, and
    /// a pick that does not check out is dropped (logged, at most once a minute per reason).</summary>
    internal static void HandlePick(EntityId player, StorageAmmoPick m)
    {
        if (player.IsNone) return;
        var now = Clock();
        if (!Views.TryGetValue(player.Value, out var view))
        {
            // a pick before the first pass that saw the player: a view of its own, so the rate limit holds from the start
            Views[player.Value] = view = new View { Id = NewViewId(), TokensMs = now, Account = ulong.MaxValue };
        }
        view.Tokens = Math.Min(PickBurst, view.Tokens + (now - view.TokensMs) * PicksPerSecond / 1000.0);
        view.TokensMs = now;
        if (view.Tokens < 1)
        {
            Refuse(player, m, "more than 4 picks a second");
            return;
        }
        view.Tokens -= 1;
        StorageAmmoAcceptor? acceptor;
        lock (Gate) Acceptors.TryGetValue(m.Key, out acceptor);
        var kind = (AmmoKinds)m.Kind;
        if (acceptor is null || acceptor.Mod is { Alive: false } || (acceptor.Kinds & kind) == 0)
        {
            Refuse(player, m, "no mod takes picks of this storage here");
            return;
        }
        var storage = PlayerStorageRuntime.Find(m.Key);
        if (storage is null || !storage.IsReady(player))
        {
            Refuse(player, m, "the storage is not ready for the player");
            return;
        }
        var item = new ItemId(m.Item);
        if (KindOf(item) != kind)
        {
            Refuse(player, m, $"item {item} is no {kind} ammunition");
            return;
        }
        var contents = storage.Read(player);
        var index = -1;
        InventorySlot? found = null;
        if (contents is not null)
        {
            var pages = new Dictionary<uint, int>();
            for (var i = 0; i < contents.Inventories.Count; i++) pages[contents.Inventories[i].Inventory.Value] = i;
            foreach (var s in contents.Slots)
            {
                if (s.IsEmpty || s.Item != item || !pages.TryGetValue(s.Slot.Inventory.Value, out var page)) continue;
                var at = page * 8 + s.Slot.Index;
                if (at >= storage.Slots || (index >= 0 && at > index)) continue;
                index = at;
                found = s;
            }
        }
        if (found is null)
        {
            Refuse(player, m, $"item {item} is not in the storage");
            return;
        }
        if (!view.Pending.Add((m.Key, m.Item))) return;  // the same pick is under way: a repeat
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = acceptor.Mod;  // the work is the storage's mod's (its unload drops the task)
        Task<EquipResult> task;
        try
        {
            var slot = kind == AmmoKinds.Arrow ? EquipmentSlot.Arrow : EquipmentSlot.Spell;
            task = acceptor.Activate ? storage.Activate(player, index)
                : EquipHook is { } hook ? hook(player, found.Slot, slot, found.ItemEntity)
                : Items.Equip(player, found.Slot, slot, EquipOptions.None, found.ItemEntity);
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or ArgumentException)
        {
            view.Pending.Remove((m.Key, m.Item));
            Refuse(player, m, e.Message);
            return;
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
        var pending = view.Pending;
        task.ContinueWith(t =>
        {
            pending.Remove((m.Key, m.Item));
            var line = t.IsCompletedSuccessfully ? t.Result.ToString() : t.Exception?.InnerException?.Message ?? "canceled";
            Backend.Current.Log(storage.Owner, LogLevel.Info, $"player-storage: {m.Key} pick by {player}: {item} from slot {index}" +
                (acceptor.Activate ? " (activate)" : "") + $": {line}");
        }, TaskContinuationOptions.ExecuteSynchronously);
    }

    private static readonly Dictionary<string, long> RefusalLogged = new(StringComparer.Ordinal);

    private static void Refuse(EntityId player, StorageAmmoPick m, string why)
    {
        Interlocked.Increment(ref _picksRefused);
        var now = Clock();
        if (RefusalLogged.TryGetValue(why, out var at) && now - at < 60_000) return;
        RefusalLogged[why] = now;
        Backend.Current.Log(0, LogLevel.Info, $"player-storage: a pick of {m.Key} by {player} was not taken: {why}");
    }

    // ---- acceptors (ProvideAmmo's server half) ----

    internal static void Accept(StorageAmmoAcceptor acceptor)
    {
        lock (Gate)
        {
            if (Acceptors.TryGetValue(acceptor.Key, out var had) && had.Mod is not { Alive: false })
                throw new InvalidOperationException($"{acceptor.Key} provides its ammunition already (dispose the first ProvideAmmo)");
            Acceptors[acceptor.Key] = acceptor;
        }
    }

    internal static void Withdraw(StorageAmmoAcceptor acceptor)
    {
        lock (Gate)
            if (Acceptors.TryGetValue(acceptor.Key, out var had) && ReferenceEquals(had, acceptor)) Acceptors.Remove(acceptor.Key);
    }

    /// <summary>What the copy did, for a status line.</summary>
    internal static (bool Open, long Sent, long PicksRefused) Counters()
    {
        lock (Gate) return (_link is not null, Interlocked.Read(ref _sent), Interlocked.Read(ref _picksRefused));
    }

    /// <summary>Tests: a link in place of kiln.net (null: none), and a fresh state.</summary>
    internal static void UseLinkForTests(IStorageMirrorLink? link)
    {
        lock (Gate)
        {
            _link = link;
            _started = true;
        }
        ReportCapability();
    }

    internal static void ResetForTests()
    {
        lock (Gate)
        {
            _link = _channel is null ? null : new NetStorageMirrorLink(_channel);
            Acceptors.Clear();
            Views.Clear();
            RefusalLogged.Clear();
            _lastPass = long.MinValue;
            _sent = _picksRefused = 0;
        }
        while (LocalPicks.TryDequeue(out _)) { }
        Store.Clear();
        EquipHook = null;
        Clock = static () => Environment.TickCount64;
    }
}
