// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>The Saves-only persist rows of kiln.items.storage. All item bytes come from capture batches; this adapter
/// never reads a live storage to save it. The captures-done pump stages before the Save2 worker reaches its barrier.</summary>
internal sealed unsafe class PlayerStorageNative : IPlayerStoragePersistence
{
    private static PlayerStorageNative? _bound;
    private readonly StorageNative.Table* _table;
    private ulong _seq;
    private readonly Dictionary<(CharacterKey Character, uint Generation), StorageFreeze> _frozen = [];
    private readonly Dictionary<uint, (JsonObject Active, bool Paired, string Source, long Revision)> _restore = [];
    private readonly Dictionary<uint, ulong> _restored = [];
    private uint _listingPlayer;
    internal PlayerStorageNative(StorageNative.Table* table) => _table = table;
    public bool AutomaticRegistration => true;

    internal static bool Attach()
    {
        var table = StorageNative.T;
        if (table == null) return false;
        // A remote client carries its sidecar even though only the server can bind persistence.
        CharacterSync.EnsureStarted();
        if (_bound is { } existing && existing._table == table)
        {
            SavesRuntime.Store.Storage.Native = existing;
            return true;
        }
        var adapter = new PlayerStorageNative(table);
        ulong seq = 0;
        if (table->PersistBind(0, &OnStage, null, &seq) != StorageNative.Ok) return false;
        adapter._seq = seq;
        _bound = adapter;
        SavesRuntime.Store.Storage.Native = adapter;
        CharacterSync.EnsureStarted();
        return true;
    }

    internal static void ResetForTests() { _bound = null; SavesRuntime.Store.Storage.Native = null; }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnStage(uint kind, void* user)
    {
        try { _bound?.Drain(kind); }
        catch (Exception e)
        {
            try { SavesRuntime.Store.Storage.Failed("storage stage failed: " + e.Message); }
            catch { /* No exception may cross the unmanaged callback. */ }
        }
    }

    private void Drain(uint kind)
    {
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;
        try
        {
            for (var n = 0; n < 256; n++)
            {
                var ev = new StorageNative.Event { Size = (uint)sizeof(StorageNative.Event) };
                if (_table->NextEvent(_seq, &ev) != StorageNative.Ok) break;
                _seq = ev.Seq;
                if (kind == StorageNative.EventBatch && ev.Kind == StorageNative.EventBatch) StageBatch(ev.Tag);
                else if (ev.Target != 0 && ((kind == StorageNative.StageParked && ev.Kind == StorageNative.EventParked)
                    || (kind == StorageNative.StageRestored && ev.Kind == StorageNative.EventRestored)))
                    SavesRuntime.Store.Storage.Housekeeping(new(ev.Account, StorageNative.CharacterOf(ev.Character)), new(ev.Player),
                        StorageNative.Text(ev.Key, StorageNative.KeyMax), new(ev.Target), kind == StorageNative.StageParked);
            }
        }
        finally { ModContext.CurrentMod = outer; }
    }

    private void StageBatch(ulong tag)
    {
        var batch = new StorageNative.Batch { Size = (uint)sizeof(StorageNative.Batch) };
        var result = _table->TakeFrozen(0, tag, &batch, null, 0, null, 0, null, 0);
        if (result is not (StorageNative.BufferTooSmall or StorageNative.Ok)) throw new InvalidDataException($"take_frozen {tag}: {result}");
        if ((batch.Flags & StorageNative.BatchComplete) == 0 || batch.Tag != tag || batch.Revision == 0)
            throw new InvalidDataException("frozen batch incomplete or untagged");
        if (batch.CaptureCount > 16 || batch.EntryCount > 16 * StorageNative.MaxEntries || batch.RecordCount > 16 * StorageNative.MaxRecords)
            throw new InvalidDataException("frozen batch exceeds ABI limits");
        var captures = new StorageNative.Capture[batch.CaptureCount];
        var entries = new StorageNative.Entry[batch.EntryCount];
        var records = new StorageNative.Record[batch.RecordCount];
        fixed (StorageNative.Capture* c = captures)
        fixed (StorageNative.Entry* e = entries)
        fixed (StorageNative.Record* r = records)
        {
            if (c != null) c->Size = (uint)sizeof(StorageNative.Capture);
            if (e != null) e->Size = (uint)sizeof(StorageNative.Entry);
            if (_table->TakeFrozen(0, tag, &batch, c, (uint)captures.Length, e, (uint)entries.Length, r, (uint)records.Length) != StorageNative.Ok)
                throw new InvalidDataException("frozen batch changed during copy");
            _frozen.Clear();
            for (var i = 0; i < captures.Length; i++)
            {
                var capture = c + i;
                if ((capture->Flags & (StorageNative.CaptureUnrestored | StorageNative.CaptureLocked | StorageNative.CaptureRefused)) != 0) continue;
                if ((ulong)capture->FirstEntry + capture->EntryCount > (ulong)entries.Length) throw new InvalidDataException("capture entry range invalid");
                var character = new CharacterKey(capture->Account, StorageNative.CharacterOf(capture->Character));
                _frozen[(character, batch.Revision)] = Decode(capture, e, r, records.Length);
            }
        }
        var world = StorageNative.CharacterOf(batch.WorldGuid);
        if (world != Guid.Empty) SavesRuntime.Store.AdoptWorld(world.ToString());
        SavesRuntime.Store.Stage(batch.Revision, SaveReason.Unknown, late: false, snapshot: true);
        // Taking the batch transfers retention to Saves. The queued job and the in-memory document retain the pair on a
        // disk failure. The existing native stage barrier holds the world write/publication until it finishes or times out.
        if (_table->ReleaseFrozen(0, tag) != StorageNative.Ok) SavesRuntime.Log(LogLevel.Warn, $"player-storage: batch {tag} was staged but not released");
    }

    private static readonly string[] RefNames = ["rangedWeapon", "arrow", "spell", "fishingBait"];
    private static StorageFreeze Decode(StorageNative.Capture* capture, StorageNative.Entry* entries, StorageNative.Record* records, int recordCount)
    {
        var live = new JsonObject();
        var parked = new JsonObject();
        var quarantine = new JsonArray();
        for (var i = 0u; i < capture->EntryCount; i++)
        {
            var entry = entries + capture->FirstEntry + i;
            var key = StorageNative.Text(entry->Key, StorageNative.KeyMax);
            if ((ulong)entry->First + entry->Count > (ulong)recordCount) throw new InvalidDataException("record range invalid");
            var rows = new JsonArray();
            for (var j = 0u; j < entry->Count; j++)
            {
                var record = records + entry->First + j;
                if (record->Length > StorageNative.RecordMax) throw new InvalidDataException("record byte length invalid");
                var encoded = Convert.ToBase64String(new ReadOnlySpan<byte>(record->Data, record->Length));
                if (entry->Kind == StorageNative.EntryQuarantine)
                {
                    quarantine.Add(new JsonObject { ["storage"] = key, ["record"] = encoded, ["reason"] = QuarantineReason(record->Reason) });
                    continue;
                }
                var bytes = new ReadOnlySpan<byte>(record->Data, record->Length).ToArray();
                try
                {
                    PlayerStorageDocument.ValidateRecord(bytes, record->Slot);
                    rows.Add(PlayerStorageDocument.Mirror(new JsonObject { ["s"] = (int)record->Slot, ["b64"] = encoded }, bytes));
                }
                catch (InvalidDataException ex)
                {
                    quarantine.Add(new JsonObject { ["storage"] = key, ["record"] = encoded, ["reason"] = ex.Message });
                }
            }
            if (entry->Kind == StorageNative.EntryQuarantine) continue;
            var target = entry->Kind == StorageNative.EntryLive ? live : parked;
            var bag = new JsonObject { ["slots"] = entry->Slots, ["records"] = rows, ["refs"] = new JsonObject() };
            for (var j = 0; j < StorageNative.RefCount; j++)
                if (entry->Refs[j] != StorageNative.RefNone) bag["refs"]![RefNames[j]] = entry->Refs[j];
            if (entry->Kind == StorageNative.EntryParked)
            {
                bag["nativeParkReason"] = entry->Reason;
                if (entry->Reason >= StorageNative.ParkHeld)
                    bag["parkReason"] = entry->Reason switch
                    {
                        StorageNative.ParkRequested => "requested", StorageNative.ParkStale => "changed elsewhere since this server's copy",
                        StorageNative.ParkLocked => "locked", StorageNative.ParkReturned => "returned outside Kiln", _ => "held",
                    };
                target[key] = target[key] is JsonObject previous
                    ? PlayerStorageDocument.MergeParked(key, quarantine, capture->Player, previous, bag)
                    : PlayerStorageDocument.MergeParked(key, quarantine, capture->Player, bag);
            }
            else target[key] = bag;
        }
        return new("xxh3:" + capture->Witness.ToString("x16"), live, parked, quarantine);
    }

    private static string QuarantineReason(byte reason) => reason == StorageNative.QuarantineParkedFull ? "parked storage full" : "native: " + (reason switch
    {
        StorageNative.QuarantineUnknownItem => "unknown item", StorageNative.QuarantineCount => "invalid stack count",
        StorageNative.QuarantineSlot => "invalid slot", StorageNative.QuarantineDecode => "item record decode failed",
        StorageNative.QuarantineRebuild => "item rebuild failed", _ => "invalid item record",
    });

    public StorageFreeze? Freeze(CharacterKey character, EntityId player, uint generation) => _frozen.GetValueOrDefault((character, generation));

    private StorageNative.Witness? Witness(EntityId player)
    {
        for (var slot = 0u; slot < 16; slot++)
        {
            var witness = new StorageNative.Witness { Size = (uint)sizeof(StorageNative.Witness) };
            if (_table->WitnessOf(0, slot, &witness) == StorageNative.Ok && witness.Player == player.Value && !player.IsNone
                && (witness.Flags & StorageNative.WitnessRead) != 0 && (witness.Flags & StorageNative.WitnessLeft) == 0) return witness;
        }
        return null;
    }

    public string? WitnessOf(EntityId player)
    {
        _listingPlayer = player.Value;
        return Witness(player) is { } w ? "xxh3:" + w.Value.ToString("x16") : null;
    }

    public bool ExtraMainPages(EntityId player) => Witness(player) is { } w && (w.Flags & StorageNative.WitnessExtraMainPages) != 0;

    private IReadOnlyList<StorageNative.State> States(EntityId player)
    {
        // Temporary parked-return holders can add rows of an existing key; they are not extra storage keys.
        var rows = new StorageNative.State[StorageNative.MaxEntries];
        uint count = 0;
        fixed (StorageNative.State* p = rows)
        {
            p->Size = (uint)sizeof(StorageNative.State);
            if (_table->List(player.Value, p, (uint)rows.Length, &count) != StorageNative.Ok || count > rows.Length) return [];
        }
        return rows.Take((int)count).ToArray();
    }

    public IReadOnlyDictionary<string, int> Registrations
    {
        get
        {
            var result = new Dictionary<string, int>(StringComparer.Ordinal);
            foreach (var row in States(new EntityId(_listingPlayer)))
            {
                var state = row;
                if (state.Handle != 0 && state.Slots != 0) result[StorageNative.Text(state.Key, StorageNative.KeyMax)] = (int)state.Slots;
            }
            return result;
        }
    }

    public IReadOnlyDictionary<string, EntityId> Holders(EntityId player)
    {
        var result = new Dictionary<string, EntityId>(StringComparer.Ordinal);
        foreach (var row in States(player))
        {
            var state = row;
            if (state.Status == StorageNative.StateReady && state.Holder != 0)
                result[StorageNative.Text(state.Key, StorageNative.KeyMax)] = new(state.Holder);
        }
        return result;
    }

    public IReadOnlyList<StorageInstanceHolder> InstanceHolders(EntityId player)
    {
        var result = new List<StorageInstanceHolder>();
        foreach (var row in States(player))
        {
            var state = row;
            if (state.Holder != 0) result.Add(new(StorageNative.Text(state.Key, StorageNative.KeyMax), new(state.Holder), state.Status != StorageNative.StateReady));
        }
        return result;
    }

    public void Restore(EntityId player, JsonObject storages, bool paired, string source, long revision) =>
        _restore[player.Value] = (storages, paired, source, revision);

    public void Lock(EntityId player)
    {
        Restore(player, new JsonObject(), false, "locked", 0);
        Park(player, new JsonObject());
    }

    public void Park(EntityId player, JsonObject parked)
    {
        if (!_restore.Remove(player.Value, out var restore) || Witness(player) is not { } witness) return;
        if (_restored.GetValueOrDefault(witness.Slot) == witness.Join) return;
        _restored[witness.Slot] = witness.Join;
        var entries = new List<StorageNative.Entry>();
        var records = new List<StorageNative.Record>();
        Encode(restore.Active, StorageNative.EntryLive, entries, records);
        Encode(parked, StorageNative.EntryParked, entries, records);
        fixed (StorageNative.Entry* e = entries.ToArray())
        fixed (StorageNative.Record* r = records.ToArray())
        {
            var doc = new StorageNative.Doc
            {
                Size = (uint)sizeof(StorageNative.Doc), Flags = restore.Source == "locked" ? StorageNative.DocLocked : restore.Paired ? StorageNative.DocPaired : 0,
                Source = restore.Source == "client" ? StorageNative.SourceClient : restore.Source == "new" ? StorageNative.SourceNew : StorageNative.SourceServer,
                Revision = (ulong)restore.Revision, Witness = witness.Value, Account = witness.Account,
                EntryCount = (uint)entries.Count, RecordCount = (uint)records.Count, Entries = e, Records = r,
            };
            StorageNative.SetCharacter(doc.Character, StorageNative.CharacterOf(witness.Character));
            var code = _table->Restore(0, witness.Slot, witness.Join, &doc);
            if (code != StorageNative.Ok)
            {
                _restored.Remove(witness.Slot);
                throw new InvalidDataException($"storage restore refused: {code}");
            }
        }
    }

    private static uint U(JsonNode? value) => value is null ? 0 : JsonSerializer.Deserialize<uint>(value.ToJsonString());
    private static void Encode(JsonObject bags, uint kind, List<StorageNative.Entry> entries, List<StorageNative.Record> records)
    {
        foreach (var (key, value) in bags)
        {
            var bag = value!.AsObject();
            var reason = U(bag["nativeParkReason"]);
            if (bag["parkReason"] is { } held)
                reason = held.GetValue<string>() switch
                {
                    "requested" => StorageNative.ParkRequested, "changed elsewhere since this server's copy" => StorageNative.ParkStale,
                    "locked" => StorageNative.ParkLocked, "returned outside Kiln" => StorageNative.ParkReturned,
                    _ => reason >= StorageNative.ParkHeld ? reason : StorageNative.ParkHeld,
                };
            if (reason == 0) reason = StorageNative.ParkUnregistered;
            var entry = new StorageNative.Entry { Size = (uint)sizeof(StorageNative.Entry), Kind = kind, Slots = U(bag["slots"]), First = (uint)records.Count,
                Reason = kind == StorageNative.EntryParked ? reason : 0 };
            if (!StorageNative.SetText(entry.Key, StorageNative.KeyMax, key)) throw new InvalidDataException("storage key exceeds ABI limit");
            for (var j = 0; j < StorageNative.RefCount; j++) entry.Refs[j] = bag["refs"]?[RefNames[j]] is { } index ? checked((ushort)U(index)) : StorageNative.RefNone;
            foreach (var row in bag["records"]!.AsArray().OfType<JsonObject>())
            {
                var bytes = Convert.FromBase64String(row["b64"]!.GetValue<string>());
                if (bytes.Length > StorageNative.RecordMax) throw new InvalidDataException("record exceeds ABI limit");
                var record = new StorageNative.Record { Slot = checked((ushort)U(row["s"])), Length = (byte)bytes.Length, Item = U(row["item"]), Count = U(row["n"]), Level = checked((byte)U(row["level"])), Flags = bytes[7] };
                bytes.CopyTo(new Span<byte>(record.Data, StorageNative.RecordMax));
                records.Add(record);
            }
            entry.Count = (uint)records.Count - entry.First;
            entries.Add(entry);
        }
    }
}
