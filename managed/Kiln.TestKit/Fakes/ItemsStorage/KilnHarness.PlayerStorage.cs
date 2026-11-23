// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessPlayerStorage? _playerStorage;

    /// <summary>The fake kiln.items.storage of this harness: the game's side of player storage (joins, restores, saves,
    /// leaves, deaths). Since 0.2.0-dev.30.7 (mod API 0.2.16).</summary>
    public HarnessPlayerStorage PlayerStorage => _playerStorage ??= new HarnessPlayerStorage(this);
}

/// <summary>One record of a player storage in a test: a stack in a slot. Since 0.2.0-dev.30.7.</summary>
/// <param name="Slot">The slot in the storage (0 to slots - 1).</param>
/// <param name="Item">The item.</param>
/// <param name="Count">How many.</param>
/// <param name="Level">Gear's level (0 for a plain stack).</param>
public readonly record struct HarnessStorageRecord(int Slot, ItemId Item, int Count, int Level = 0);

/// <summary>One storage of a restored copy in a test (<see cref="HarnessPlayerStorage.Restore"/>). Since 0.2.0-dev.30.7.</summary>
/// <param name="FullKey">"&lt;mod id&gt;:&lt;name&gt;".</param>
/// <param name="Slots">The slots it had when it was saved.</param>
/// <param name="Records">Its stacks.</param>
/// <param name="Parked">Saved as parked (no mod had it registered then) rather than live.</param>
public sealed record HarnessStorageCopy(string FullKey, int Slots, IReadOnlyList<HarnessStorageRecord> Records, bool Parked = false);

/// <summary>The game's tombstone setting at a death (<see cref="HarnessPlayerStorage.Die"/>). Since 0.2.0-dev.30.7.</summary>
public enum HarnessTombstoneMode
{
    /// <summary>The game's default: materials and collectibles go into the tombstone.</summary>
    AddBackpackMaterials = 0,
    /// <summary>Everything goes.</summary>
    Everything = 1,
    /// <summary>No tombstone.</summary>
    NoTombstone = 2,
}

/// <summary>
/// The fake kiln.items.storage of a harness (<see cref="KilnHarness.PlayerStorage"/>): Kiln.Items' player storage runs its own
/// code on it. Since 0.2.0-dev.30.7 (mod API 0.2.16).
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> kiln.items.storage's rules for registrations (keys, 8 per mod, 256 slots in all, filters) and the
/// game's side, which the test scripts: <see cref="Join"/> (the game read the character), <see cref="Restore"/> (the copy
/// Kiln.Saves chose), <see cref="Apply"/> (the join made the player's pages: holders are built), <see cref="JoinAndRestore"/>
/// for all three, <see cref="Capture"/> and <see cref="CapturesDone"/> (a world save), <see cref="Leave"/> and
/// <see cref="Die"/>. A holder is a fake container of the kit's kiln.items: <c>Items.Read</c>, <c>Give</c> and <c>Take</c>
/// work on it, a give the storage's filter refuses is <c>Filtered</c>, and <c>Items.FindInventories</c> lists it only with
/// <c>InventoryKinds.PlayerStorage</c>. A restore validates records (unknown item, bad count or slot: quarantined), parks
/// keys no mod registered and what does not fit, and a registration while the player is on restores the key's parked
/// records. A mod that unloads parks its storages.</para>
/// <para><b>Not modelled.</b> The game's record codec (records carry slot, item, count and level), item entities in
/// storage, moves (<c>PlayerStorage.Store</c> and <c>Retrieve</c> need <c>items.move</c>), captures at the kit's own
/// saves, the client copy.</para>
/// </remarks>
public sealed class HarnessPlayerStorage
{
    private readonly KilnHarness _harness;

    internal HarnessPlayerStorage(KilnHarness harness) => _harness = harness;

    private ItemsStorageFake Fake =>
        KitFakes.State<ItemsStorageKitState>(_harness).Fake ?? throw new InvalidOperationException("the harness has no player storage yet");

    /// <summary>The account key and CharacterId the fake gives a player (the CharacterId made from the name).</summary>
    private static (ulong Account, Guid Character) IdOf(FakePlayer player)
    {
        var hash = System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes("kiln-testkit-character:" + player.Name));
        return (player.SteamId, new Guid(hash.AsSpan(0, 16)));
    }

    /// <summary>The game read <paramref name="player"/>'s character at the join, with this inventory witness.</summary>
    public void Join(FakePlayer player, ulong witness = 1)
    {
        ArgumentNullException.ThrowIfNull(player);
        var (account, character) = IdOf(player);
        _harness.Run(() => Fake.JoinRead((uint)player.Slot, account, character, witness));
    }

    /// <summary>The join made the player's own pages: a restored player's storages are built now.</summary>
    public void Apply(FakePlayer player)
    {
        ArgumentNullException.ThrowIfNull(player);
        _harness.Run(() => Fake.JoinApply((uint)player.Slot, player.Entity.Value));
    }

    /// <summary>Restores the copy Kiln.Saves chose for <paramref name="player"/>'s join (once per join): an empty one with
    /// <see cref="PlayerStorageSource.New"/>.</summary>
    /// <exception cref="InvalidOperationException">The player did not join, or kiln.items.storage refused the copy.</exception>
    public unsafe void Restore(FakePlayer player, PlayerStorageSource source = PlayerStorageSource.New, bool paired = false, long revision = 0,
        params HarnessStorageCopy[] storages)
    {
        ArgumentNullException.ThrowIfNull(player);
        ArgumentNullException.ThrowIfNull(storages);
        var (account, character) = IdOf(player);
        var records = new List<StorageNative.Record>();
        var entries = new StorageNative.Entry[storages.Length];
        for (var i = 0; i < storages.Length; i++)
        {
            var s = storages[i];
            ref var e = ref entries[i];
            e.Size = (uint)sizeof(StorageNative.Entry);
            e.Kind = s.Parked ? StorageNative.EntryParked : StorageNative.EntryLive;
            e.Reason = s.Parked ? StorageNative.ParkUnregistered : 0;
            e.Slots = (uint)s.Slots;
            e.First = (uint)records.Count;
            e.Count = (uint)s.Records.Count;
            fixed (byte* key = e.Key) StorageNative.SetText(key, StorageNative.KeyMax, s.FullKey);
            fixed (ushort* refs = e.Refs) for (var r = 0; r < StorageNative.RefCount; r++) refs[r] = StorageNative.RefNone;
            foreach (var rec in s.Records)
            {
                var bytes = StorageFakeRecord.Encode(rec.Slot, rec.Item.Value, rec.Count, rec.Level, 0);
                var n = new StorageNative.Record
                {
                    Slot = (ushort)rec.Slot, Length = (byte)bytes.Length, Flags = bytes[7], Item = rec.Item.Value, Count = (uint)rec.Count,
                    Level = (byte)rec.Level,
                };
                for (var b = 0; b < bytes.Length; b++) n.Data[b] = bytes[b];
                records.Add(n);
            }
        }
        var recs = records.ToArray();
        _harness.Run(() =>
        {
            fixed (StorageNative.Entry* pe = entries)
            fixed (StorageNative.Record* pr = recs)
            {
                var doc = new StorageNative.Doc
                {
                    Size = (uint)sizeof(StorageNative.Doc), Flags = paired ? StorageNative.DocPaired : 0, Source = (uint)source,
                    EntryCount = (uint)entries.Length, RecordCount = (uint)recs.Length, Revision = (ulong)revision, Account = account,
                    Entries = pe, Records = pr,
                };
                StorageNative.SetCharacter(doc.Character, character);
                var rc = Fake.RestoreAsTest((uint)player.Slot, &doc);
                if (rc != StorageNative.Ok) throw new InvalidOperationException($"kiln.items.storage refused the restore ({rc})");
            }
        });
    }

    /// <summary><see cref="Join"/>, <see cref="Restore"/> with <paramref name="storages"/> (none: a new character), and
    /// <see cref="Apply"/>: the player's storages are ready.</summary>
    public void JoinAndRestore(FakePlayer player, params HarnessStorageCopy[] storages)
    {
        Join(player);
        Restore(player, storages.Length == 0 ? PlayerStorageSource.New : PlayerStorageSource.Server, false, 0, storages);
        Apply(player);
    }

    /// <summary>The game captured <paramref name="player"/>'s character in a world save (with this witness): Kiln froze the
    /// storages into the save's batch. Returns the capture's sequence number.</summary>
    public ulong Capture(FakePlayer player, ulong witness)
    {
        ArgumentNullException.ThrowIfNull(player);
        return _harness.Run(() => Fake.CaptureOf((uint)player.Slot, witness));
    }

    /// <summary>The save's captures are done: the batch completes and the persistence owner's stage runs. Returns the
    /// batch's tag (0: nothing was captured).</summary>
    public ulong CapturesDone() => _harness.Run(() => Fake.CapturesDone());

    /// <summary>The player left: the storages' inventories go.</summary>
    public void Leave(FakePlayer player)
    {
        ArgumentNullException.ThrowIfNull(player);
        _harness.Run(() => Fake.Leave((uint)player.Slot));
    }

    /// <summary>The player died; the game made <paramref name="tombstone"/> (none: no tombstone) under
    /// <paramref name="mode"/>. Each storage's death rule runs.</summary>
    public void Die(FakePlayer player, EntityId tombstone, HarnessTombstoneMode mode = HarnessTombstoneMode.AddBackpackMaterials)
    {
        ArgumentNullException.ThrowIfNull(player);
        _harness.Run(() => Fake.Die((uint)player.Slot, tombstone.Value, (int)mode));
    }

    /// <summary>The player's parked records of a storage, as Kiln holds them now.</summary>
    public IReadOnlyList<HarnessStorageRecord> Parked(FakePlayer player, string fullKey) =>
        _harness.Run(() => (IReadOnlyList<HarnessStorageRecord>)[.. Fake.Contents((uint)player.Slot, fullKey).Parked
            .Select(p => new HarnessStorageRecord(p.Record.Slot, new ItemId(p.Record.Item), p.Record.Count, p.Record.Level))]);

    /// <summary>The player's quarantined records of a storage (Kiln could not use them; kept).</summary>
    public IReadOnlyList<HarnessStorageRecord> Quarantined(FakePlayer player, string fullKey) =>
        _harness.Run(() => (IReadOnlyList<HarnessStorageRecord>)[.. Fake.Contents((uint)player.Slot, fullKey).Quarantined
            .Select(r => new HarnessStorageRecord(r.Slot, new ItemId(r.Item), r.Count, r.Level))]);
}
