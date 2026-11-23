// SPDX-License-Identifier: MIT
// A fake kiln.items.storage v1 (native/core/abi/services/kiln_items_storage.h): every public and persist row behind the real
// unmanaged table, over a deterministic model of the native rules, so Kiln.Items' player storage and Kiln.Saves' persistence
// run their own code against it. Self-contained: Kiln.TestKit wraps it (Fakes/ItemsStorage, holders as fake containers),
// and Kiln.Saves.Tests links this file alone (<Compile Include=".../Shared/ItemsStorageFake.cs" Link=... />) and installs it
// with StorageNative.UseForTests (Install). A test plays the game: Join (the struct reader at a join: W_g), JoinApply,
// Capture (the game's capture of a character in a save: Kiln freezes its storages), CapturesDone (the per-player save step
// ends: the batch completes and the persistence owner's stage function runs), Step (the next sim step: queued restores,
// registrations, parks, returns), Leave, Die, Revoke (an owner goes away).
// Not modelled: the game's record converter and codec (records are written in the game's layout from the fields the fake
// knows: slot, item, count, flags, level; other flagged fields are kept as given), threads (every call is the test's
// thread), the game's tombstone (Die names it), item entities, or native parked-return temporary holders and tracker
// notices (Return delivers records directly; list has only ordinary rows).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace Kiln.TestKit;

/// <summary>One item record as the fake keeps it: the mirror fields and the game-format bytes.</summary>
internal sealed record StorageFakeRecord(int Slot, uint Item, int Count, int Level = 0, byte Flags = 0, byte[]? Data = null, byte Reason = 0)
{
    /// <summary>The record's bytes: as given, else the game's layout from the fields (u8 slot, u32 item, u16 count, u8 flags,
    /// u8 level with flag 0x01).</summary>
    public byte[] Bytes => Data ?? Encode(Slot, Item, Count, Level, Flags);

    public static byte[] Encode(int slot, uint item, int count, int level, byte flags)
    {
        var withLevel = (flags & StorageNative.RecordLevel) != 0 || level > 0;
        var b = new byte[withLevel ? 9 : 8];
        b[0] = (byte)slot;
        BitConverter.TryWriteBytes(b.AsSpan(1, 4), item);
        BitConverter.TryWriteBytes(b.AsSpan(5, 2), (ushort)count);
        b[7] = (byte)(flags | (withLevel ? StorageNative.RecordLevel : 0));
        if (withLevel) b[8] = (byte)level;
        return b;
    }

    public StorageFakeRecord At(int slot) => this with { Slot = slot, Data = Data is null ? null : Restamp(Data, slot) };

    private static byte[] Restamp(byte[] data, int slot)
    {
        var copy = (byte[])data.Clone();
        if (copy.Length > 0) copy[0] = (byte)slot;
        return copy;
    }
}

/// <summary>What the fake knows of an item (null from <see cref="IStorageFakeWorld.Item"/>: the game has no such item).</summary>
internal sealed record StorageFakeItem(int MaxStack, ItemCategory Category, bool Gear = false, uint Ammo = 0);

/// <summary>Where the fake's holders live and what it knows of the game's items: memory (<see cref="StorageFakeMemory"/>),
/// or the kit's fake containers.</summary>
internal interface IStorageFakeWorld
{
    /// <summary>A holder of <paramref name="pages"/> 8-slot pages; <paramref name="accepts"/> is its storage's filter (for a
    /// give into it).</summary>
    uint CreateHolder(int pages, Func<uint, bool> accepts);
    void DestroyHolder(uint holder);
    void WriteHolder(uint holder, int slots, IReadOnlyList<StorageFakeRecord> records);
    IReadOnlyList<StorageFakeRecord> ReadHolder(uint holder, int slots);
    StorageFakeItem? Item(uint id);
    /// <summary>Puts a record into an inventory owner (a return's target, a tombstone); false: no room.</summary>
    bool Deliver(uint target, StorageFakeRecord record);
}

/// <summary>Holders in memory; any item is known (max stack 9999, Materials) unless <see cref="Items"/> names some.</summary>
internal sealed class StorageFakeMemory : IStorageFakeWorld
{
    private readonly Dictionary<uint, StorageFakeRecord?[]> _holders = [];
    private uint _next = 0x10000;

    /// <summary>The game's items: empty = every item is known.</summary>
    public Dictionary<uint, StorageFakeItem> Items { get; } = [];

    /// <summary>What reached each target (returns, tombstones), in order.</summary>
    public Dictionary<uint, List<StorageFakeRecord>> Delivered { get; } = [];

    /// <summary>How many records a target takes (absent: any number).</summary>
    public Dictionary<uint, int> Room { get; } = [];

    public uint CreateHolder(int pages, Func<uint, bool> accepts)
    {
        var h = _next++;
        _holders[h] = new StorageFakeRecord?[pages * 8];
        return h;
    }

    public void DestroyHolder(uint holder) => _holders.Remove(holder);

    public bool Exists(uint holder) => _holders.ContainsKey(holder);

    public void WriteHolder(uint holder, int slots, IReadOnlyList<StorageFakeRecord> records)
    {
        var cells = _holders[holder];
        Array.Clear(cells);
        foreach (var r in records) cells[r.Slot] = r;
    }

    public IReadOnlyList<StorageFakeRecord> ReadHolder(uint holder, int slots) =>
        _holders.TryGetValue(holder, out var cells) ? [.. cells.Take(slots).Where(c => c is not null)!] : [];

    public StorageFakeItem? Item(uint id) =>
        Items.Count == 0 ? new StorageFakeItem(9999, ItemCategory.Materials) : Items.GetValueOrDefault(id);

    public bool Deliver(uint target, StorageFakeRecord record)
    {
        var list = Delivered.TryGetValue(target, out var l) ? l : Delivered[target] = [];
        if (Room.TryGetValue(target, out var room) && list.Count >= room) return false;
        list.Add(record);
        return true;
    }
}

/// <summary>The fake service. One instance is active at a time (<see cref="Current"/>): the unmanaged table answers for it.</summary>
internal sealed unsafe class ItemsStorageFake : IDisposable
{
    // ---- the model ----

    internal sealed class Registration
    {
        public required uint Handle;
        public required ulong Owner;
        public required string Key;
        public required int Slots;
        public required uint Death;
        public uint Categories, FilterFlags;
        public uint[] Ids = [];
    }

    internal sealed class Live
    {
        public required string Key;
        public required uint Handle;
        public required uint Holder;
        public required int Slots;
        public ushort[] Refs = [StorageNative.RefNone, StorageNative.RefNone, StorageNative.RefNone, StorageNative.RefNone];
    }

    internal sealed class Parked
    {
        public required string Key;
        public required uint Reason;
        public required int Slots;
        public List<StorageFakeRecord> Records = [];
    }

    internal sealed class Join
    {
        public required uint Slot;
        public required ulong Serial;
        public required ulong WitnessG;
        public required ulong Account;
        public required Guid Character;
        public required ulong Step;
        public uint Player;
        public bool Applied, Restored, Built, Left, Locked, Paired;
        public uint Source;
        public ulong Revision, DocWitness, Changes, LastHash;
        public bool HashSeen;
        public ulong Captured, CaptureSeq;
        public readonly Dictionary<string, Live> Live = new(StringComparer.Ordinal);
        public readonly List<Parked> Parked = [];
        public readonly List<(string Key, StorageFakeRecord Record)> Quarantine = [];
        // a restore accepted and not built: the given entries, as a capture reports them until then
        public List<(StorageNative.Entry Entry, List<StorageFakeRecord> Records)>? Given;
    }

    internal sealed class Capture
    {
        public required StorageNative.Capture Head;
        public required List<(StorageNative.Entry Entry, List<StorageFakeRecord> Records)> Entries;
    }

    internal sealed class Batch
    {
        public required ulong Tag;
        public ulong SavesSeq;
        public uint Revision, Flags;
        public ulong FirstStep, LastStep;
        public long UnixMs;
        public Guid World;
        public readonly List<Capture> Captures = [];
    }

    private sealed record Op(string Kind, Action Run);

    private static readonly Regex KeyRule = new("^[a-z0-9][a-z0-9._-]{0,63}:[a-z0-9][a-z0-9._-]{0,31}$", RegexOptions.CultureInvariant);

    private readonly object _lock = new();
    private readonly List<Registration> _registrations = [];
    private readonly Dictionary<uint, Join> _joins = [];          // by the game's player slot: the current join
    private readonly List<Batch> _batches = [];
    private readonly List<StorageNative.Event> _events = [];
    private readonly List<Op> _queue = [];
    private uint _nextHandle = 1;
    private ulong _nextJoin = 1, _nextSeq = 1, _nextTag = 1, _nextEvent = 1, _nextTicket = 1;
    private Batch? _open;
    private bool _persistBound;
    private ulong _persistOwner;
    private delegate* unmanaged[Cdecl]<uint, void*, void> _stage;
    private void* _stageUser;
    private ulong _captures, _restores;

    public ItemsStorageFake(IStorageFakeWorld? world = null) => World = world ?? new StorageFakeMemory();

    /// <summary>Holders and items.</summary>
    public IStorageFakeWorld World { get; }

    /// <summary>Feature items.storage (status.storage); off: no holder is built and persist_bind refuses.</summary>
    public bool StorageOn { get; set; } = true;

    /// <summary>The captures-done hook resolved (status.stage_point); off: items.storage.persist is unavailable.</summary>
    public bool StagePoint { get; set; } = true;

    /// <summary>A server world runs (status.session).</summary>
    public bool Session { get; set; } = true;

    /// <summary>The server-world step the fake stamps (the kit advances it).</summary>
    public ulong StepNow { get; set; }

    /// <summary>What a batch carries as kiln.saves' SAVING seq and target revision (the test sets them before Capture).</summary>
    public ulong SavesSeq { get; set; }
    public uint SaveRevision { get; set; }
    public Guid WorldGuid { get; set; }

    /// <summary>A capture's freeze is refused (KIS_CAPTURE_REFUSED) while set: the reason.</summary>
    public uint? RefuseFreeze { get; set; }

    // ---- the game's side ----

    /// <summary>The game read a joining character for <paramref name="slot"/>: a new join with W_g. Returns its serial.</summary>
    public ulong JoinRead(uint slot, ulong account, Guid character, ulong witness)
    {
        lock (_lock)
        {
            if (_joins.TryGetValue(slot, out var old) && !old.Left) DropHolders(old);
            var j = new Join { Slot = slot, Serial = _nextJoin++, WitnessG = witness, Account = account, Character = character, Step = StepNow };
            _joins[slot] = j;
            Post(StorageNative.EventJoined, j, 0, "", (ref StorageNative.Event e) => e.Join = j.Serial);
            return j.Serial;
        }
    }

    /// <summary>The game's join apply made the player's pages: holders are built now when a restore was accepted.</summary>
    public void JoinApply(uint slot, uint player)
    {
        lock (_lock)
        {
            var j = Need(slot);
            j.Player = player;
            j.Applied = true;
            if (j.Restored && !j.Built) Build(j);
        }
    }

    /// <summary>The game captured the character of <paramref name="slot"/> in a save: Kiln freezes its storages into the
    /// open batch. Returns the capture's seq.</summary>
    public ulong CaptureOf(uint slot, ulong witness)
    {
        lock (_lock)
        {
            var j = Need(slot);
            var b = _open ??= NewBatch();
            var head = NewHead(j);
            head.Seq = _nextSeq++;
            head.Witness = witness;
            List<(StorageNative.Entry, List<StorageFakeRecord>)> entries;
            if (RefuseFreeze is { } reason)
            {
                head.Flags |= StorageNative.CaptureRefused;
                head.Reason = reason;
                entries = [];
                b.Flags |= StorageNative.BatchRefused;
            }
            else entries = Freeze(j, ref head);
            j.Captured = witness;
            j.CaptureSeq = head.Seq;
            b.Captures.Add(new Capture { Head = head, Entries = entries });
            b.LastStep = StepNow;
            if (b.Captures.Count == 1) b.FirstStep = StepNow;
            _captures++;
            Post(StorageNative.EventCaptured, j, 0, "", (ref StorageNative.Event e) => { e.Join = j.Serial; e.Tag = b.Tag; });
            return head.Seq;
        }
    }

    /// <summary>The per-player save step ended: the open batch completes, KIS_EVENT_BATCH is posted and the persistence
    /// owner's stage function runs (where the build has the hook). Returns the batch's tag (0: no capture since the last).</summary>
    public ulong CapturesDone()
    {
        delegate* unmanaged[Cdecl]<uint, void*, void> stage;
        void* user;
        ulong tag;
        lock (_lock)
        {
            var b = _open;
            if (b is null) return 0;
            _open = null;
            b.Flags |= StorageNative.BatchComplete;
            tag = b.Tag;
            Post(StorageNative.EventBatch, null, 0, "", (ref StorageNative.Event e) => e.Tag = b.Tag);
            stage = StagePoint && _persistBound ? _stage : null;
            user = _stageUser;
        }
        if (stage != null) stage(StorageNative.EventBatch, user);  // outside the lock: the stage reads events and takes the batch
        return tag;
    }

    /// <summary>The player of <paramref name="slot"/> left: holders go; its queued captures say so.</summary>
    public void Leave(uint slot)
    {
        lock (_lock)
        {
            if (!_joins.TryGetValue(slot, out var j) || j.Left) return;
            foreach (var b in _batches)
                foreach (var c in b.Captures)
                    if (c.Head.Join == j.Serial) c.Head.Flags |= StorageNative.CaptureLeft;
            DropHolders(j);
            j.Left = true;
            Post(StorageNative.EventLeft, j, 0, "", (ref StorageNative.Event e) => e.Join = j.Serial);
        }
    }

    /// <summary>The player died and the game made <paramref name="tombstone"/> (0: none) under the game's
    /// <paramref name="tombstoneMode"/> (0 AddBackpackMaterials, 1 Everything, 2 NoTombstone).</summary>
    public void Die(uint slot, uint tombstone, int tombstoneMode)
    {
        lock (_lock)
        {
            var j = Need(slot);
            foreach (var live in j.Live.Values.OrderBy(l => l.Key, StringComparer.Ordinal))
            {
                var reg = _registrations.First(r => r.Handle == live.Handle);
                var records = World.ReadHolder(live.Holder, live.Slots).ToList();
                var take = reg.Death switch
                {
                    StorageNative.DeathGrave => tombstone != 0 ? records : [],
                    StorageNative.DeathFollowGame => tombstone == 0 || tombstoneMode == 2 ? []
                        : tombstoneMode == 1 ? records
                        : records.Where(r => World.Item(r.Item)?.Category is ItemCategory.Materials or ItemCategory.Collectibles).ToList(),
                    _ => [],
                };
                var moved = take.Where(r => World.Deliver(tombstone, r)).ToList();
                if (moved.Count > 0)
                {
                    World.WriteHolder(live.Holder, live.Slots, [.. records.Except(moved)]);
                    j.Changes++;
                }
                Post(StorageNative.EventDeath, j, live.Handle, live.Key, (ref StorageNative.Event e) =>
                {
                    e.Stacks = (uint)moved.Count;
                    e.Items = (uint)moved.Sum(r => r.Count);
                    e.Target = moved.Count > 0 ? tombstone : 0;
                    e.Reason = reg.Death;
                });
            }
        }
    }

    /// <summary>An owner went away: its registrations end (their storages are parked for online players) and a persist claim
    /// it held is released.</summary>
    public void Revoke(ulong owner)
    {
        lock (_lock)
        {
            foreach (var reg in _registrations.Where(r => r.Owner == owner).ToList())
            {
                _registrations.Remove(reg);
                foreach (var j in _joins.Values.Where(j => !j.Left))
                    if (j.Live.Remove(reg.Key, out var live)) ParkLive(j, live, StorageNative.ParkUnregistered);
            }
            if (_persistBound && _persistOwner == owner)
            {
                _persistBound = false;
                _stage = null;
            }
        }
    }

    /// <summary>The next sim step: queued restores, registrations, set_ref, parks, unparks and returns run.</summary>
    public void Step()
    {
        List<Op> ops;
        lock (_lock)
        {
            StepNow++;
            ops = [.. _queue];
            _queue.Clear();
        }
        foreach (var op in ops) op.Run();
    }

    /// <summary>Puts a record into a player's storage as kiln.items would after a move (the test's hand).</summary>
    public void Put(uint player, string key, StorageFakeRecord record)
    {
        lock (_lock)
        {
            var j = ByPlayer(player) ?? throw new ArgumentException($"no player {player}");
            var live = j.Live[key];
            var cells = World.ReadHolder(live.Holder, live.Slots).Where(r => r.Slot != record.Slot).ToList();
            cells.Add(record);
            World.WriteHolder(live.Holder, live.Slots, cells);
        }
    }

    /// <summary>A player's records of a key as Kiln holds them now, by kind.</summary>
    public (IReadOnlyList<StorageFakeRecord> Live, IReadOnlyList<(uint Reason, StorageFakeRecord Record)> Parked,
        IReadOnlyList<StorageFakeRecord> Quarantined) Contents(uint slot, string key)
    {
        lock (_lock)
        {
            var j = Need(slot);
            var live = j.Live.TryGetValue(key, out var l) ? World.ReadHolder(l.Holder, l.Slots) : [];
            return (live, [.. j.Parked.Where(p => p.Key == key).SelectMany(p => p.Records.Select(r => (p.Reason, r)))],
                [.. j.Quarantine.Where(q => q.Key == key).Select(q => q.Record)]);
        }
    }

    public IReadOnlyList<ulong> BatchTags
    {
        get { lock (_lock) return [.. _batches.Select(b => b.Tag)]; }
    }

    public ulong PersistOwner
    {
        get { lock (_lock) return _persistBound ? _persistOwner : ulong.MaxValue; }
    }

    /// <summary>Restores as the persistence owner would, bypassing the owner check (the test's hand).</summary>
    public int RestoreAsTest(uint slot, StorageNative.Doc* doc)
    {
        lock (_lock)
        {
            if (!_joins.TryGetValue(slot, out var j) || j.Left) return StorageNative.NotFound;
            return RestoreLocked(j, j.Serial, doc);
        }
    }

    // ---- the rows ----

    private int RegisterRow(ulong owner, StorageNative.Def* def, uint* outHandle)
    {
        if (outHandle != null) *outHandle = 0;
        if (def == null || outHandle == null || def->Size < (uint)sizeof(StorageNative.Def) || def->Filter.Size < (uint)sizeof(StorageNative.Filter))
            return StorageNative.InvalidArgument;
        var key = def->Key.ToString();
        if (!KeyRule.IsMatch(key) || def->Slots is < 1 or > StorageNative.MaxSlots || def->Death > StorageNative.DeathFollowGame ||
            def->Flags != 0 || def->Filter.Flags > 7 || def->Filter.IdCount > StorageNative.MaxFilterIds ||
            (def->Filter.IdCount > 0 && def->Filter.Ids == null))
            return StorageNative.InvalidArgument;
        var ids = new uint[def->Filter.IdCount];
        for (var i = 0; i < ids.Length; i++)
        {
            ids[i] = def->Filter.Ids[i];
            if (i > 0 && ids[i] <= ids[i - 1]) return StorageNative.InvalidArgument;  // sorted, no duplicates
        }
        lock (_lock)
        {
            if (_registrations.Any(r => r.Key == key)) return StorageNative.InvalidArgument;
            if (_registrations.Count(r => r.Owner == owner) >= StorageNative.MaxPerMod) return StorageNative.Unavailable;
            if (_registrations.Sum(r => r.Slots) + (int)def->Slots > StorageNative.MaxSlotsPlayer) return StorageNative.Unavailable;
            var reg = new Registration
            {
                Handle = _nextHandle++, Owner = owner, Key = key, Slots = (int)def->Slots, Death = def->Death,
                Categories = def->Filter.Categories, FilterFlags = def->Filter.Flags, Ids = ids,
            };
            _registrations.Add(reg);
            *outHandle = reg.Handle;
            _queue.Add(new Op("register", () =>
            {
                lock (_lock)
                    foreach (var j in _joins.Values.Where(j => j.Built && !j.Left && !j.Locked).OrderBy(j => j.Slot))
                        if (_registrations.Contains(reg) && !j.Live.ContainsKey(reg.Key)) BuildLive(j, reg, live: true);
            }));
            return StorageNative.Ok;
        }
    }

    private int HolderRow(ulong owner, uint handle, uint player, uint* out_)
    {
        if (out_ == null) return StorageNative.InvalidArgument;
        *out_ = 0;
        lock (_lock)
        {
            var reg = _registrations.FirstOrDefault(r => r.Handle == handle && r.Owner == owner);
            if (reg is null || ByPlayer(player) is not { } j || !j.Live.TryGetValue(reg.Key, out var live)) return StorageNative.NotFound;
            *out_ = live.Holder;
            return StorageNative.Ok;
        }
    }

    private int StateRow(ulong owner, uint handle, uint player, StorageNative.State* out_)
    {
        if (out_ == null || out_->Size < (uint)sizeof(StorageNative.State)) return StorageNative.InvalidArgument;
        lock (_lock)
        {
            var reg = _registrations.FirstOrDefault(r => r.Handle == handle && r.Owner == owner);
            if (reg is null) return StorageNative.NotFound;
            FillState(ByPlayer(player), reg.Key, out_);
            return StorageNative.Ok;
        }
    }

    private int SetRefRow(ulong owner, uint handle, uint player, uint equipSlot, uint index)
    {
        lock (_lock)
        {
            var reg = _registrations.FirstOrDefault(r => r.Handle == handle && r.Owner == owner);
            if (reg is null || ByPlayer(player) is not { } j || !j.Live.TryGetValue(reg.Key, out var live)) return StorageNative.NotFound;
            var which = equipSlot switch { 12 => StorageNative.RefRanged, 14 => StorageNative.RefArrow, 15 => StorageNative.RefSpell, 16 => StorageNative.RefBait, _ => -1 };
            if (which < 0 || (index != StorageNative.RefClear && index >= (uint)live.Slots)) return StorageNative.InvalidArgument;
            _queue.Add(new Op("set_ref", () => { lock (_lock) live.Refs[which] = index == StorageNative.RefClear ? StorageNative.RefNone : (ushort)index; }));
            return StorageNative.Ok;
        }
    }

    private int StatusRow(StorageNative.Status* out_)
    {
        if (out_ == null || out_->Size < (uint)sizeof(StorageNative.Status)) return StorageNative.InvalidArgument;
        lock (_lock)
        {
            var size = out_->Size;
            *out_ = default;
            out_->Size = size;
            out_->Storage = StorageOn ? (byte)1 : (byte)0;
            out_->Persist = _persistBound ? (byte)1 : (byte)0;
            out_->Session = Session ? (byte)1 : (byte)0;
            out_->StagePoint = StagePoint ? (byte)1 : (byte)0;
            out_->Reason = StorageOn ? 0u : 1u;
            out_->Registrations = (uint)_registrations.Count;
            var on = _joins.Values.Where(j => !j.Left).ToList();
            out_->Holders = (uint)on.Sum(j => j.Live.Count);
            out_->Parked = (uint)on.Sum(j => j.Parked.Sum(p => p.Records.Count));
            out_->Quarantined = (uint)on.Sum(j => j.Quarantine.Count);
            out_->Batches = (uint)_batches.Count;
            out_->Captures = _captures;
            out_->Restores = _restores;
            StorageNative.SetText(out_->Detail, 160, StorageOn ? "" : "items.storage is off (test)");
            return StorageNative.Ok;
        }
    }

    private int ListRow(uint player, StorageNative.State* out_, uint capacity, uint* outCount)
    {
        if (outCount == null) return StorageNative.InvalidArgument;
        *outCount = 0;
        lock (_lock)
        {
            if (ByPlayer(player) is not { } j) return StorageNative.NotFound;
            var keys = _registrations.Select(r => r.Key).Concat(j.Parked.Select(p => p.Key)).Concat(j.Quarantine.Select(q => q.Key))
                .Distinct(StringComparer.Ordinal).OrderBy(k => k, StringComparer.Ordinal).ToList();
            *outCount = (uint)keys.Count;
            if (keys.Count > capacity) return StorageNative.BufferTooSmall;
            if (keys.Count > 0 && (out_ == null || out_[0].Size < (uint)sizeof(StorageNative.State))) return StorageNative.InvalidArgument;
            for (var i = 0; i < keys.Count; i++)
            {
                out_[i].Size = (uint)sizeof(StorageNative.State);
                FillState(j, keys[i], &out_[i]);
            }
            return StorageNative.Ok;
        }
    }

    private int NextEventRow(ulong after, StorageNative.Event* out_)
    {
        if (out_ == null || out_->Size < (uint)sizeof(StorageNative.Event)) return StorageNative.InvalidArgument;
        lock (_lock)
        {
            foreach (var e in _events)
                if (e.Seq > after)
                {
                    *out_ = e;
                    return StorageNative.Ok;
                }
            return StorageNative.NotFound;
        }
    }

    private int ReturnRow(ulong owner, uint player, string key, uint target, delegate* unmanaged[Cdecl]<StorageNative.ReturnResult*, void*, void> done,
        void* user, ulong* outTicket)
    {
        if (outTicket != null) *outTicket = 0;
        if (!KeyRule.IsMatch(key) || target == 0) return StorageNative.InvalidArgument;
        lock (_lock)
        {
            if (ByPlayer(player) is not { } j || !j.Parked.Any(p => p.Key == key && p.Records.Count > 0)) return StorageNative.NotFound;
            var ticket = _nextTicket++;
            if (outTicket != null) *outTicket = ticket;
            var start = StepNow;
            _queue.Add(new Op("return", () =>
            {
                StorageNative.ReturnResult r = default;
                lock (_lock)
                {
                    r.Size = (uint)sizeof(StorageNative.ReturnResult);
                    r.Ticket = ticket;
                    r.Player = player;
                    r.Target = target;
                    r.Steps = (uint)(StepNow - start);
                    var parked = j.Parked.Where(p => p.Key == key).ToList();
                    var all = parked.SelectMany(p => p.Records).ToList();
                    r.Requested = (uint)all.Sum(x => x.Count);
                    var moved = new List<StorageFakeRecord>();
                    foreach (var p in parked)
                        foreach (var rec in p.Records.ToList())
                            if (World.Deliver(target, rec))
                            {
                                moved.Add(rec);
                                p.Records.Remove(rec);
                            }
                    j.Parked.RemoveAll(p => p.Records.Count == 0);
                    r.Done = (uint)moved.Sum(x => x.Count);
                    r.Stacks = (uint)moved.Count;
                    r.Kept = (uint)(all.Count - moved.Count);
                    r.Status = moved.Count == all.Count ? 0u : moved.Count > 0 ? 1u : 2u;  // DONE, PARTIAL, NO_ROOM
                    if (moved.Count > 0) j.Changes++;
                    uint stacks = r.Stacks, items = r.Done;
                    Post(StorageNative.EventReturned, j, 0, key, (ref StorageNative.Event e) => { e.Stacks = stacks; e.Items = items; e.Target = target; });
                }
                if (done != null) done(&r, user);
            }));
            return StorageNative.Ok;
        }
    }

    private int PersistBindRow(ulong owner, delegate* unmanaged[Cdecl]<uint, void*, void> fn, void* user, ulong* lastSeq)
    {
        lock (_lock)
        {
            if (lastSeq != null) *lastSeq = _nextEvent - 1;
            if (!StorageOn || !StagePoint) return StorageNative.Unavailable;
            if (_persistBound && _persistOwner != owner) return StorageNative.Unavailable;
            _persistBound = true;
            _persistOwner = owner;
            _stage = fn;
            _stageUser = user;
            return StorageNative.Ok;
        }
    }

    private bool Persisting(ulong owner) => _persistBound && _persistOwner == owner;

    private int TakeFrozenRow(ulong owner, ulong tag, StorageNative.Batch* out_, StorageNative.Capture* captures, uint captureCap,
        StorageNative.Entry* entries, uint entryCap, StorageNative.Record* records, uint recordCap)
    {
        lock (_lock)
        {
            if (!Persisting(owner)) return StorageNative.Unavailable;
            if (out_ == null || out_->Size < (uint)sizeof(StorageNative.Batch)) return StorageNative.InvalidArgument;
            var b = _batches.FirstOrDefault(x => x.Tag == tag);
            if (b is null) return StorageNative.NotFound;
            var entryCount = b.Captures.Sum(c => c.Entries.Count);
            var recordCount = b.Captures.Sum(c => c.Entries.Sum(e => e.Records.Count));
            var size = out_->Size;
            *out_ = default;
            out_->Size = size;
            out_->Flags = b.Flags;
            out_->Tag = b.Tag;
            out_->SavesSeq = b.SavesSeq;
            out_->Revision = b.Revision;
            out_->CaptureCount = (uint)b.Captures.Count;
            out_->EntryCount = (uint)entryCount;
            out_->RecordCount = (uint)recordCount;
            out_->FirstStep = b.FirstStep;
            out_->LastStep = b.LastStep;
            out_->UnixMs = b.UnixMs;
            b.World.TryWriteBytes(new Span<byte>(out_->WorldGuid, 16));
            if (b.Captures.Count > captureCap || entryCount > entryCap || recordCount > recordCap) return StorageNative.BufferTooSmall;
            uint ei = 0, ri = 0;
            for (var ci = 0; ci < b.Captures.Count; ci++)
            {
                var c = b.Captures[ci];
                var head = c.Head;
                head.Size = (uint)sizeof(StorageNative.Capture);
                head.FirstEntry = ei;
                head.EntryCount = (uint)c.Entries.Count;
                captures[ci] = head;
                foreach (var (entry, recs) in c.Entries)
                {
                    var e = entry;
                    e.First = ri;
                    e.Count = (uint)recs.Count;
                    entries[ei++] = e;
                    foreach (var rec in recs) WriteRecord(&records[ri++], rec);
                }
            }
            return StorageNative.Ok;
        }
    }

    private int ReleaseFrozenRow(ulong owner, ulong tag)
    {
        lock (_lock)
        {
            if (!Persisting(owner)) return StorageNative.Unavailable;
            return _batches.RemoveAll(b => b.Tag == tag) > 0 ? StorageNative.Ok : StorageNative.NotFound;
        }
    }

    private int WitnessRow(ulong owner, uint slot, StorageNative.Witness* out_)
    {
        if (out_ == null || out_->Size < (uint)sizeof(StorageNative.Witness)) return StorageNative.InvalidArgument;
        lock (_lock)
        {
            if (!Persisting(owner)) return StorageNative.Unavailable;
            if (!_joins.TryGetValue(slot, out var j)) return StorageNative.NotFound;
            var size = out_->Size;
            *out_ = default;
            out_->Size = size;
            out_->Flags = StorageNative.WitnessRead | (j.Applied ? StorageNative.WitnessApplied : 0) |
                          (j.Restored ? StorageNative.WitnessRestored : 0) | (j.Left ? StorageNative.WitnessLeft : 0);
            out_->Join = j.Serial;
            out_->Value = j.WitnessG;
            out_->Step = j.Step;
            out_->Captured = j.Captured;
            out_->CaptureSeq = j.CaptureSeq;
            out_->Account = j.Account;
            StorageNative.SetCharacter(out_->Character, j.Character);
            out_->Player = j.Player;
            out_->Slot = j.Slot;
            return StorageNative.Ok;
        }
    }

    private int RestoreRow(ulong owner, uint slot, ulong join, StorageNative.Doc* doc)
    {
        lock (_lock)
        {
            if (!Persisting(owner)) return StorageNative.Unavailable;
            if (!_joins.TryGetValue(slot, out var j) || j.Left || j.Serial != join) return StorageNative.NotFound;
            return RestoreLocked(j, join, doc);
        }
    }

    private int RestoreLocked(Join j, ulong join, StorageNative.Doc* doc)
    {
        if (doc == null || doc->Size < (uint)sizeof(StorageNative.Doc) || (doc->Flags & ~3u) != 0 || doc->Source is < 1 or > 3 ||
            doc->EntryCount > StorageNative.MaxEntries || doc->RecordCount > StorageNative.MaxRecords || doc->Reserved != 0 ||
            (doc->EntryCount > 0 && doc->Entries == null) || (doc->RecordCount > 0 && doc->Records == null))
            return StorageNative.InvalidArgument;
        if (j.Restored) return StorageNative.InvalidArgument;
        if (doc->Account != j.Account || StorageNative.CharacterOf(doc->Character) != j.Character) return StorageNative.InvalidArgument;
        var given = new List<(StorageNative.Entry, List<StorageFakeRecord>)>();
        var liveKeys = new HashSet<string>(StringComparer.Ordinal);
        var keys = new HashSet<string>(StringComparer.Ordinal);
        for (var i = 0; i < doc->EntryCount; i++)
        {
            var e = doc->Entries[i];
            var key = StorageNative.Text(e.Key, StorageNative.KeyMax);
            if (e.Size < (uint)sizeof(StorageNative.Entry) || !KeyRule.IsMatch(key) || e.Kind is not (StorageNative.EntryLive or StorageNative.EntryParked) ||
                (ulong)e.First + e.Count > doc->RecordCount || e.Slots > StorageNative.MaxSlots ||
                (e.Kind == StorageNative.EntryLive && !liveKeys.Add(key)) ||
                (e.Kind == StorageNative.EntryParked && e.Reason is not (>= 1 and <= 3) and not (>= StorageNative.ParkHeld and <= StorageNative.ParkLocked)))
                return StorageNative.InvalidArgument;
            keys.Add(key);
            var recs = new List<StorageFakeRecord>();
            for (var r = 0u; r < e.Count; r++) recs.Add(ReadRecord(&doc->Records[e.First + r]));
            given.Add((e, recs));
        }
        if (keys.Count > StorageNative.MaxStorages) return StorageNative.InvalidArgument;
        j.Restored = true;
        j.Locked = (doc->Flags & StorageNative.DocLocked) != 0;
        j.Paired = (doc->Flags & StorageNative.DocPaired) != 0;
        j.Source = doc->Source;
        j.Revision = doc->Revision;
        j.DocWitness = doc->Witness;
        j.Given = given;
        _restores++;
        if (j.Locked) return StorageNative.Ok;  // nothing is built: the entries stay as given
        if (j.Applied) _queue.Add(new Op("restore", () => { lock (_lock) if (!j.Built && !j.Left) Build(j); }));
        return StorageNative.Ok;
    }

    private int ParkRow(ulong owner, uint player, string key, uint reason)
    {
        lock (_lock)
        {
            if (!Persisting(owner)) return StorageNative.Unavailable;
            if (reason < StorageNative.ParkHeld) return StorageNative.InvalidArgument;
            if (ByPlayer(player) is not { } j || (!j.Live.ContainsKey(key) && !j.Parked.Any(p => p.Key == key))) return StorageNative.NotFound;
            _queue.Add(new Op("park", () =>
            {
                lock (_lock)
                {
                    foreach (var p in j.Parked.Where(p => p.Key == key && p.Reason < StorageNative.ParkHeld)) p.Reason = reason;
                    if (j.Live.Remove(key, out var live)) ParkLive(j, live, reason);
                    MergeParked(j);
                }
            }));
            return StorageNative.Ok;
        }
    }

    private int UnparkRow(ulong owner, uint player, string key)
    {
        lock (_lock)
        {
            if (!Persisting(owner)) return StorageNative.Unavailable;
            if (ByPlayer(player) is not { } j || !j.Parked.Any(p => p.Key == key && p.Records.Count > 0)) return StorageNative.NotFound;
            var reg = _registrations.FirstOrDefault(r => r.Key == key);
            if (reg is null || !j.Built) return StorageNative.Unavailable;
            _queue.Add(new Op("unpark", () =>
            {
                lock (_lock)
                {
                    if (!j.Live.TryGetValue(key, out var live)) live = BuildLive(j, reg, live: true, autoUnpark: false);
                    var records = j.Parked.Where(p => p.Key == key).SelectMany(p => p.Records).ToList();
                    j.Parked.RemoveAll(p => p.Key == key);
                    Place(j, live, reg, records, ownSlotOnly: false);
                }
            }));
            return StorageNative.Ok;
        }
    }

    // ---- the native rules ----

    private Join Need(uint slot) => _joins.TryGetValue(slot, out var j) ? j : throw new InvalidOperationException($"no join on slot {slot}");

    private Join? ByPlayer(uint player) => player == 0 ? null : _joins.Values.FirstOrDefault(j => j.Player == player && !j.Left);

    private Batch NewBatch()
    {
        var b = new Batch { Tag = _nextTag++, SavesSeq = SavesSeq, Revision = SaveRevision, UnixMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(), World = WorldGuid };
        _batches.Add(b);
        while (_batches.Count > 16) _batches.RemoveAt(0);
        return b;
    }

    private StorageNative.Capture NewHead(Join j)
    {
        var h = new StorageNative.Capture
        {
            Size = (uint)sizeof(StorageNative.Capture), Join = j.Serial, Step = StepNow, Revision = j.Revision, Account = j.Account,
            Player = j.Player, Slot = j.Slot,
        };
        StorageNative.SetCharacter(h.Character, j.Character);
        return h;
    }

    // Everything Kiln holds for the character now, as a capture reports it.
    private List<(StorageNative.Entry, List<StorageFakeRecord>)> Freeze(Join j, ref StorageNative.Capture head)
    {
        if (!j.Restored)
        {
            head.Flags |= StorageNative.CaptureUnrestored;
            return [];
        }
        if (j.Locked) head.Flags |= StorageNative.CaptureLocked;
        List<(StorageNative.Entry, List<StorageFakeRecord>)> list;
        if (!j.Built) list = [.. j.Given!.Select(g => (g.Entry, g.Records.ToList()))];
        else
        {
            list = [];
            foreach (var live in j.Live.Values.OrderBy(l => l.Key, StringComparer.Ordinal))
            {
                var e = NewEntry(StorageNative.EntryLive, live.Key, live.Slots, 0);
                for (var i = 0; i < StorageNative.RefCount; i++) e.Refs[i] = live.Refs[i];
                list.Add((e, [.. World.ReadHolder(live.Holder, live.Slots).OrderBy(r => r.Slot)]));
            }
            foreach (var p in j.Parked.OrderBy(p => p.Key, StringComparer.Ordinal).ThenBy(p => p.Reason))
                list.Add((NewEntry(StorageNative.EntryParked, p.Key, p.Slots, p.Reason), [.. p.Records]));
            foreach (var g in j.Quarantine.GroupBy(q => q.Key, StringComparer.Ordinal).OrderBy(g => g.Key, StringComparer.Ordinal))
                list.Add((NewEntry(StorageNative.EntryQuarantine, g.Key, 0, 0), [.. g.Select(q => q.Record)]));
        }
        var hash = Hash(list);
        if (!j.HashSeen) j.HashSeen = true;
        else if (hash != j.LastHash) j.Changes++;
        j.LastHash = hash;
        head.Changes = j.Changes;
        return list;
    }

    private static ulong Hash(List<(StorageNative.Entry Entry, List<StorageFakeRecord> Records)> list)
    {
        var h = 1469598103934665603UL;
        void Mix(ReadOnlySpan<byte> b)
        {
            foreach (var x in b) h = (h ^ x) * 1099511628211UL;
        }
        foreach (var (e, recs) in list)
        {
            var entry = e;
            Mix(new ReadOnlySpan<byte>(&entry, sizeof(StorageNative.Entry)));
            foreach (var r in recs) Mix(r.Bytes);
        }
        return h;
    }

    private static StorageNative.Entry NewEntry(uint kind, string key, int slots, uint reason)
    {
        var e = new StorageNative.Entry { Size = (uint)sizeof(StorageNative.Entry), Kind = kind, Slots = (uint)slots, Reason = reason };
        StorageNative.SetText(e.Key, StorageNative.KeyMax, key);
        for (var i = 0; i < StorageNative.RefCount; i++) e.Refs[i] = StorageNative.RefNone;
        return e;
    }

    // The restore's entries become holders (registered keys), parked records and quarantine.
    private void Build(Join j)
    {
        j.Built = true;
        var given = j.Given ?? [];
        j.Given = null;
        var byKey = new Dictionary<string, List<StorageFakeRecord>>(StringComparer.Ordinal);
        var liveRefs = new Dictionary<string, ushort[]>(StringComparer.Ordinal);
        foreach (var (entry, recs) in given)
        {
            var e = entry;
            var key = StorageNative.Text(e.Key, StorageNative.KeyMax);
            var valid = new List<StorageFakeRecord>();
            var seen = new HashSet<int>();
            foreach (var r in recs)
            {
                var why = Validate(r, seen);
                if (why != 0) j.Quarantine.Add((key, r with { Reason = why }));
                else valid.Add(r);
            }
            var held = e.Kind == StorageNative.EntryParked && e.Reason >= StorageNative.ParkHeld;
            var registered = _registrations.Any(x => x.Key == key);
            if (held || !registered)
            {
                if (valid.Count > 0 || !registered)
                    AddParked(j, key, held ? e.Reason : (e.Kind == StorageNative.EntryParked ? e.Reason : StorageNative.ParkUnregistered), (int)e.Slots, valid);
                continue;
            }
            (byKey.TryGetValue(key, out var list) ? list : byKey[key] = []).AddRange(valid);
            if (e.Kind == StorageNative.EntryLive)
            {
                var refs = new ushort[StorageNative.RefCount];
                for (var i = 0; i < refs.Length; i++) refs[i] = e.Refs[i];
                liveRefs[key] = refs;
            }
        }
        foreach (var g in j.Quarantine.GroupBy(q => q.Key, StringComparer.Ordinal))
            Post(StorageNative.EventQuarantined, j, 0, g.Key, (ref StorageNative.Event ev) => { ev.Stacks = (uint)g.Count(); ev.Reason = g.First().Record.Reason; });
        foreach (var reg in _registrations.OrderBy(r => r.Key, StringComparer.Ordinal))
        {
            var live = NewLive(j, reg);
            if (liveRefs.TryGetValue(reg.Key, out var refs)) live.Refs = refs;
            var placed = Place(j, live, reg, byKey.GetValueOrDefault(reg.Key) ?? [], ownSlotOnly: true, post: false);
            Post(StorageNative.EventRestored, j, reg.Handle, reg.Key, (ref StorageNative.Event e) =>
            {
                e.Stacks = (uint)placed;
                e.Source = j.Source;
                e.Revision = j.Revision;
                e.Flags = j.Paired ? StorageNative.EventPaired : 0;
            });
        }
        foreach (var p in j.Parked.GroupBy(p => (p.Key, p.Reason)))
            Post(StorageNative.EventParked, j, 0, p.Key.Key, (ref StorageNative.Event e) => { e.Stacks = (uint)p.Sum(x => x.Records.Count); e.Reason = p.Key.Reason; });
    }

    private byte Validate(StorageFakeRecord r, HashSet<int> seen)
    {
        var data = r.Bytes;
        if (data.Length is < 8 or > StorageNative.RecordMax || data[0] != (byte)r.Slot || BitConverter.ToUInt32(data, 1) != r.Item ||
            BitConverter.ToUInt16(data, 5) != r.Count)
            return StorageNative.QuarantineDecode;
        if (r.Slot is < 0 or >= 128 || !seen.Add(r.Slot)) return StorageNative.QuarantineSlot;
        var item = World.Item(r.Item);
        if (item is null) return StorageNative.QuarantineUnknownItem;
        if (r.Count < 1 || r.Count > item.MaxStack) return StorageNative.QuarantineCount;
        return 0;
    }

    private Live NewLive(Join j, Registration reg)
    {
        var live = new Live { Key = reg.Key, Handle = reg.Handle, Holder = World.CreateHolder((reg.Slots + 7) / 8, id => Allows(reg, id)), Slots = reg.Slots };
        j.Live[reg.Key] = live;
        return live;
    }

    // A registration while the player is online: a holder, and the key's parked records below KIS_PARK_HELD go in.
    private Live BuildLive(Join j, Registration reg, bool live, bool autoUnpark = true)
    {
        var l = NewLive(j, reg);
        var records = new List<StorageFakeRecord>();
        if (autoUnpark)
        {
            foreach (var p in j.Parked.Where(p => p.Key == reg.Key && p.Reason < StorageNative.ParkHeld).ToList())
            {
                records.AddRange(p.Records);
                j.Parked.Remove(p);
            }
        }
        var placed = Place(j, l, reg, records, ownSlotOnly: false, post: true);
        if (live)
            Post(StorageNative.EventRestored, j, reg.Handle, reg.Key, (ref StorageNative.Event e) =>
            {
                e.Stacks = (uint)placed;
                e.Source = j.Source;
                e.Revision = j.Revision;
                e.Flags = StorageNative.EventLive | (j.Paired ? StorageNative.EventPaired : 0);
            });
        return l;
    }

    // Records into a holder: their own slot when free and in range (else, unless ownSlotOnly, the first free slot); the
    // filter decides first. What does not go in stays parked. Returns how many went in.
    private int Place(Join j, Live live, Registration reg, List<StorageFakeRecord> records, bool ownSlotOnly, bool post = true)
    {
        var cells = World.ReadHolder(live.Holder, live.Slots).ToDictionary(r => r.Slot);
        var overflow = new List<StorageFakeRecord>();
        var filtered = new List<StorageFakeRecord>();
        var placed = 0;
        foreach (var r in records.OrderBy(r => r.Slot))
        {
            if (!Allows(reg, r.Item))
            {
                filtered.Add(r);
                continue;
            }
            var slot = r.Slot < live.Slots && !cells.ContainsKey(r.Slot) ? r.Slot : -1;
            if (slot < 0 && !ownSlotOnly)
                for (var s = 0; s < live.Slots; s++)
                    if (!cells.ContainsKey(s)) { slot = s; break; }
            if (slot < 0)
            {
                overflow.Add(r);
                continue;
            }
            cells[slot] = r.At(slot);
            placed++;
        }
        World.WriteHolder(live.Holder, live.Slots, [.. cells.Values]);
        if (overflow.Count > 0) AddParked(j, reg.Key, StorageNative.ParkOverflow, reg.Slots, overflow);
        if (filtered.Count > 0) AddParked(j, reg.Key, StorageNative.ParkFilter, reg.Slots, filtered);
        if (post)
        {
            if (overflow.Count > 0) Post(StorageNative.EventParked, j, reg.Handle, reg.Key, (ref StorageNative.Event e) => { e.Stacks = (uint)overflow.Count; e.Reason = StorageNative.ParkOverflow; });
            if (filtered.Count > 0) Post(StorageNative.EventParked, j, reg.Handle, reg.Key, (ref StorageNative.Event e) => { e.Stacks = (uint)filtered.Count; e.Reason = StorageNative.ParkFilter; });
        }
        return placed;
    }

    private bool Allows(Registration reg, uint id)
    {
        var item = World.Item(id);
        if (item is null) return false;
        if (reg.Categories != 0 && (reg.Categories & (1u << (int)item.Category)) == 0) return false;
        var ammo = reg.FilterFlags & (StorageNative.FilterAmmoArrow | StorageNative.FilterAmmoSpell);
        if (ammo != 0 && (item.Ammo & ammo) == 0) return false;
        if ((reg.FilterFlags & StorageNative.FilterNoPide) != 0 && item.Gear) return false;
        if (reg.Ids.Length > 0 && Array.BinarySearch(reg.Ids, id) < 0) return false;
        return true;
    }

    private void AddParked(Join j, string key, uint reason, int slots, List<StorageFakeRecord> records)
    {
        var p = j.Parked.FirstOrDefault(x => x.Key == key && x.Reason == reason);
        if (p is null) j.Parked.Add(p = new Parked { Key = key, Reason = reason, Slots = slots });
        p.Records.AddRange(records);
    }

    private static void MergeParked(Join j)
    {
        foreach (var g in j.Parked.GroupBy(p => (p.Key, p.Reason)).Where(g => g.Count() > 1).ToList())
        {
            var first = g.First();
            foreach (var other in g.Skip(1))
            {
                first.Records.AddRange(other.Records);
                j.Parked.Remove(other);
            }
        }
    }

    private void ParkLive(Join j, Live live, uint reason)
    {
        var records = World.ReadHolder(live.Holder, live.Slots).ToList();
        World.DestroyHolder(live.Holder);
        AddParked(j, live.Key, reason, live.Slots, records);
        Post(StorageNative.EventParked, j, live.Handle, live.Key, (ref StorageNative.Event e) => { e.Stacks = (uint)records.Count; e.Reason = reason; });
    }

    private void DropHolders(Join j)
    {
        foreach (var live in j.Live.Values) World.DestroyHolder(live.Holder);
        j.Live.Clear();
    }

    private void FillState(Join? j, string key, StorageNative.State* out_)
    {
        var size = out_->Size;
        *out_ = default;
        out_->Size = size;
        StorageNative.SetText(out_->Key, StorageNative.KeyMax, key);
        for (var i = 0; i < StorageNative.RefCount; i++) out_->Refs[i] = StorageNative.RefNone;
        var reg = _registrations.FirstOrDefault(r => r.Key == key);
        if (reg is not null)
        {
            out_->Handle = reg.Handle;
            out_->Slots = (uint)reg.Slots;
            out_->Death = reg.Death;
        }
        if (j is null)
        {
            out_->Status = reg is null ? StorageNative.StateNone : StorageNative.StateWaiting;
            return;
        }
        out_->Parked = (uint)j.Parked.Where(p => p.Key == key).Sum(p => p.Records.Count);
        out_->Quarantined = (uint)j.Quarantine.Count(q => q.Key == key);
        out_->Source = j.Restored ? j.Source : 0;
        out_->Paired = j.Restored && j.Paired ? 1u : 0u;
        out_->Revision = j.Restored ? j.Revision : 0;
        if (j.Locked) out_->Status = StorageNative.StateLocked;
        else if (j.Live.TryGetValue(key, out var live))
        {
            out_->Status = StorageNative.StateReady;
            out_->Holder = live.Holder;
            out_->Pages = (uint)((live.Slots + 7) / 8);
            out_->Stacks = (uint)World.ReadHolder(live.Holder, live.Slots).Count;
            for (var i = 0; i < StorageNative.RefCount; i++) out_->Refs[i] = live.Refs[i];
        }
        else if (out_->Parked > 0) out_->Status = StorageNative.StateParked;
        else out_->Status = reg is not null ? StorageNative.StateWaiting : StorageNative.StateNone;
    }

    private delegate void EventFill(ref StorageNative.Event e);

    private void Post(uint kind, Join? j, uint handle, string key, EventFill? fill = null)
    {
        var e = new StorageNative.Event { Size = (uint)sizeof(StorageNative.Event), Kind = kind, Seq = _nextEvent++, Step = StepNow, Handle = handle };
        if (j is not null)
        {
            e.Player = j.Player;
            e.Slot = j.Slot;
            e.Join = j.Serial;
            e.Account = j.Account;
            StorageNative.SetCharacter(e.Character, j.Character);
        }
        StorageNative.SetText(e.Key, StorageNative.KeyMax, key);
        fill?.Invoke(ref e);
        _events.Add(e);
        if (_events.Count > 256) _events.RemoveAt(0);
    }

    private static StorageFakeRecord ReadRecord(StorageNative.Record* r)
    {
        var data = new byte[Math.Min((int)r->Length, StorageNative.RecordMax)];
        for (var i = 0; i < data.Length; i++) data[i] = r->Data[i];
        return new StorageFakeRecord(r->Slot, r->Item, (int)r->Count, r->Level, r->Flags, data, r->Reason);
    }

    private static void WriteRecord(StorageNative.Record* out_, StorageFakeRecord rec)
    {
        *out_ = default;
        var bytes = rec.Bytes;
        out_->Slot = (ushort)rec.Slot;
        out_->Length = (byte)bytes.Length;
        out_->Flags = bytes.Length > 7 ? bytes[7] : rec.Flags;
        out_->Item = rec.Item;
        out_->Count = (uint)rec.Count;
        out_->Level = (byte)rec.Level;
        out_->Reason = rec.Reason;
        for (var i = 0; i < bytes.Length && i < StorageNative.RecordMax; i++) out_->Data[i] = bytes[i];
    }

    // ---- the unmanaged table ----

    private static StorageNative.Table* _table;
    private static volatile ItemsStorageFake? _current;

    /// <summary>The active fake (the table's calls go to it; with none they answer UNAVAILABLE).</summary>
    public static ItemsStorageFake? Current => _current;

    /// <summary>The process-lifetime unmanaged table.</summary>
    public static StorageNative.Table* Table
    {
        get
        {
            if (_table != null) return _table;
            var t = (StorageNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(StorageNative.Table));
            t->Header = new ServiceHeader { Size = (uint)sizeof(StorageNative.Table), Version = StorageNative.Version };
            t->RegisterStorage = &CRegister;
            t->Holder = &CHolder;
            t->GetState = &CState;
            t->SetRef = &CSetRef;
            t->GetStatus = &CStatus;
            t->List = &CList;
            t->NextEvent = &CNextEvent;
            t->ReturnParked = &CReturn;
            t->PersistBind = &CPersistBind;
            t->TakeFrozen = &CTakeFrozen;
            t->ReleaseFrozen = &CReleaseFrozen;
            t->WitnessOf = &CWitness;
            t->Restore = &CRestore;
            t->Park = &CPark;
            t->Unpark = &CUnpark;
            return _table = t;
        }
    }

    /// <summary>Makes this the fake the table answers for.</summary>
    public ItemsStorageFake Activate()
    {
        _ = Table;
        _current = this;
        return this;
    }

    /// <summary>Activates this fake and answers Kiln.Items' StorageNative.T with its table, without a backend (Kiln.Saves.Tests).
    /// Dispose undoes it.</summary>
    public ItemsStorageFake Install()
    {
        Activate();
        StorageNative.UseForTests(Table);
        _installed = true;
        return this;
    }

    private bool _installed;

    public void Dispose()
    {
        if (ReferenceEquals(_current, this)) _current = null;
        if (_installed) StorageNative.ResetForTests();
        _installed = false;
    }

    private static int With(Func<ItemsStorageFake, int> f)
    {
        try
        {
            return _current is { } fake ? f(fake) : StorageNative.Unavailable;
        }
        catch
        {
            return StorageNative.Internal;  // never let an exception cross into native code
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CRegister(ulong owner, StorageNative.Def* def, uint* outHandle) => With(f => f.RegisterRow(owner, def, outHandle));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CHolder(ulong owner, uint handle, uint player, uint* out_) => With(f => f.HolderRow(owner, handle, player, out_));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CState(ulong owner, uint handle, uint player, StorageNative.State* out_) => With(f => f.StateRow(owner, handle, player, out_));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CSetRef(ulong owner, uint handle, uint player, uint equipSlot, uint index) =>
        With(f => f.SetRefRow(owner, handle, player, equipSlot, index));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CStatus(StorageNative.Status* out_) => With(f => f.StatusRow(out_));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CList(uint player, StorageNative.State* out_, uint capacity, uint* outCount) => With(f => f.ListRow(player, out_, capacity, outCount));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CNextEvent(ulong after, StorageNative.Event* out_) => With(f => f.NextEventRow(after, out_));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CReturn(ulong owner, uint player, NativeStr key, uint target, delegate* unmanaged[Cdecl]<StorageNative.ReturnResult*, void*, void> done,
        void* user, ulong* outTicket)
    {
        var k = key.ToString();
        return With(f => f.ReturnRow(owner, player, k, target, done, user, outTicket));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CPersistBind(ulong owner, delegate* unmanaged[Cdecl]<uint, void*, void> fn, void* user, ulong* lastSeq) =>
        With(f => f.PersistBindRow(owner, fn, user, lastSeq));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CTakeFrozen(ulong owner, ulong tag, StorageNative.Batch* out_, StorageNative.Capture* captures, uint captureCap,
        StorageNative.Entry* entries, uint entryCap, StorageNative.Record* records, uint recordCap) =>
        With(f => f.TakeFrozenRow(owner, tag, out_, captures, captureCap, entries, entryCap, records, recordCap));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CReleaseFrozen(ulong owner, ulong tag) => With(f => f.ReleaseFrozenRow(owner, tag));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CWitness(ulong owner, uint slot, StorageNative.Witness* out_) => With(f => f.WitnessRow(owner, slot, out_));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CRestore(ulong owner, uint slot, ulong join, StorageNative.Doc* doc) => With(f => f.RestoreRow(owner, slot, join, doc));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CPark(ulong owner, uint player, NativeStr key, uint reason)
    {
        var k = key.ToString();
        return With(f => f.ParkRow(owner, player, k, reason));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CUnpark(ulong owner, uint player, NativeStr key)
    {
        var k = key.ToString();
        return With(f => f.UnparkRow(owner, player, k));
    }
}
