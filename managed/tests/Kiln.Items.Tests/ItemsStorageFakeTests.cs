// SPDX-License-Identifier: MIT
// The shared fake kiln.items.storage (managed/Kiln.TestKit/Shared/ItemsStorageFake.cs, linked into this project as Kiln.Saves.Tests
// links it) row by row through its unmanaged table, as Kiln.Saves calls the persist rows: the claim, the join's witness, a
// restore and its checks, captures into batches and the captures-done stage, parking and unparking, an owner's revoke, a
// locked document, a leave. These pin the native implementation contract of kiln_items_storage.h.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.ItemsTests;

public sealed unsafe class ItemsStorageFakeTests : IDisposable
{
    private const ulong Saves = 0;      // Kiln.Saves holds the persist rows as owner 0 here
    private const ulong ModA = 101, ModB = 102;
    private const ulong Account = 0x1100001;
    private static readonly Guid Character = Guid.Parse("6f9619ff-8b86-d011-b42d-00c04fc964ff");

    private readonly ItemsStorageFake _fake;
    private readonly StorageNative.Table* _t;

    public ItemsStorageFakeTests()
    {
        _fake = new ItemsStorageFake().Install();
        _t = StorageNative.T;
        Stages.Clear();
    }

    public void Dispose() => _fake.Dispose();

    // ---- helpers: the rows as a native caller sees them ----

    private uint Register(ulong owner, string key, int slots, uint death = 0, uint categories = 0, uint flags = 0, uint[]? ids = null)
    {
        var bytes = Encoding.UTF8.GetBytes(key);
        ids ??= [];
        uint handle;
        int rc;
        fixed (byte* k = bytes)
        fixed (uint* pid = ids)
        {
            var def = new StorageNative.Def
            {
                Size = (uint)sizeof(StorageNative.Def), Slots = (uint)slots, Key = new NativeStr { Ptr = k, Len = (nuint)bytes.Length }, Death = death,
                Filter = new StorageNative.Filter { Size = (uint)sizeof(StorageNative.Filter), Categories = categories, Flags = flags, IdCount = (uint)ids.Length, Ids = ids.Length == 0 ? null : pid },
            };
            rc = _t->RegisterStorage(owner, &def, &handle);
        }
        Assert.Equal(StorageNative.Ok, rc);
        return handle;
    }

    private int RegisterRc(ulong owner, string key, int slots, uint[]? ids = null)
    {
        var bytes = Encoding.UTF8.GetBytes(key);
        ids ??= [];
        uint handle;
        fixed (byte* k = bytes)
        fixed (uint* pid = ids)
        {
            var def = new StorageNative.Def
            {
                Size = (uint)sizeof(StorageNative.Def), Slots = (uint)slots, Key = new NativeStr { Ptr = k, Len = (nuint)bytes.Length },
                Filter = new StorageNative.Filter { Size = (uint)sizeof(StorageNative.Filter), IdCount = (uint)ids.Length, Ids = ids.Length == 0 ? null : pid },
            };
            return _t->RegisterStorage(owner, &def, &handle);
        }
    }

    private static readonly List<(uint Kind, ulong Tag)> Stages = [];
    private static ItemsStorageFakeTests? _self;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnStage(uint kind, void* user)
    {
        // the stage reads the newest BATCH event and takes the batch, as Kiln.Saves does
        var self = _self!;
        StorageNative.Event e = default;
        ulong after = 0, tag = 0;
        while (true)
        {
            e.Size = (uint)sizeof(StorageNative.Event);
            if (self._t->NextEvent(after, &e) != StorageNative.Ok) break;
            after = e.Seq;
            if (e.Kind == StorageNative.EventBatch) tag = e.Tag;
        }
        Stages.Add((kind, tag));
    }

    private void Bind(ulong owner = Saves)
    {
        _self = this;
        ulong last;
        Assert.Equal(StorageNative.Ok, _t->PersistBind(owner, &OnStage, null, &last));
    }

    private StorageNative.Witness Witness(uint slot)
    {
        StorageNative.Witness w = default;
        w.Size = (uint)sizeof(StorageNative.Witness);
        Assert.Equal(StorageNative.Ok, _t->WitnessOf(Saves, slot, &w));
        return w;
    }

    private sealed record Entry(uint Kind, string Key, int Slots, uint Reason, (int Slot, uint Item, int Count)[] Records);

    private int Restore(uint slot, ulong join, uint flags, uint source, long revision, ulong account, Guid character, params Entry[] entries)
    {
        var e = new StorageNative.Entry[entries.Length];
        var recs = new List<StorageNative.Record>();
        for (var i = 0; i < entries.Length; i++)
        {
            e[i].Size = (uint)sizeof(StorageNative.Entry);
            e[i].Kind = entries[i].Kind;
            e[i].Slots = (uint)entries[i].Slots;
            e[i].Reason = entries[i].Reason;
            e[i].First = (uint)recs.Count;
            e[i].Count = (uint)entries[i].Records.Length;
            fixed (byte* k = e[i].Key) StorageNative.SetText(k, StorageNative.KeyMax, entries[i].Key);
            fixed (ushort* r = e[i].Refs) for (var j = 0; j < 4; j++) r[j] = StorageNative.RefNone;
            foreach (var (s, item, count) in entries[i].Records)
            {
                var bytes = StorageFakeRecord.Encode(s, item, count, 0, 0);
                var n = new StorageNative.Record { Slot = (ushort)s, Length = (byte)bytes.Length, Flags = bytes[7], Item = item, Count = (uint)count };
                for (var b = 0; b < bytes.Length; b++) n.Data[b] = bytes[b];
                recs.Add(n);
            }
        }
        var ra = recs.ToArray();
        fixed (StorageNative.Entry* pe = e)
        fixed (StorageNative.Record* pr = ra)
        {
            var doc = new StorageNative.Doc
            {
                Size = (uint)sizeof(StorageNative.Doc), Flags = flags, Source = source, EntryCount = (uint)e.Length, RecordCount = (uint)ra.Length,
                Revision = (ulong)revision, Account = account, Entries = e.Length == 0 ? null : pe, Records = ra.Length == 0 ? null : pr,
            };
            StorageNative.SetCharacter(doc.Character, character);
            return _t->Restore(Saves, slot, join, &doc);
        }
    }

    private sealed record Taken(StorageNative.Batch Batch, StorageNative.Capture[] Captures, (StorageNative.Entry Entry, string Key, StorageNative.Record[] Records)[] Entries);

    private Taken Take(ulong tag)
    {
        StorageNative.Batch b = default;
        b.Size = (uint)sizeof(StorageNative.Batch);
        Assert.Equal(StorageNative.BufferTooSmall, _t->TakeFrozen(Saves, tag, &b, null, 0, null, 0, null, 0));  // the first call sizes
        var caps = new StorageNative.Capture[b.CaptureCount];
        var ents = new StorageNative.Entry[b.EntryCount];
        var recs = new StorageNative.Record[b.RecordCount];
        fixed (StorageNative.Capture* pc = caps)
        fixed (StorageNative.Entry* pe = ents)
        fixed (StorageNative.Record* pr = recs)
            Assert.Equal(StorageNative.Ok, _t->TakeFrozen(Saves, tag, &b, pc, (uint)caps.Length, pe, (uint)ents.Length, pr, (uint)recs.Length));
        var list = new List<(StorageNative.Entry, string, StorageNative.Record[])>();
        foreach (var e in ents)
        {
            var copy = e;
            list.Add((e, StorageNative.Text(copy.Key, StorageNative.KeyMax), recs.Skip((int)e.First).Take((int)e.Count).ToArray()));
        }
        return new Taken(b, caps, [.. list]);
    }

    private static Entry Live(string key, int slots, params (int, uint, int)[] records) => new(StorageNative.EntryLive, key, slots, 0, records);
    private static Entry Parked(string key, int slots, uint reason, params (int, uint, int)[] records) => new(StorageNative.EntryParked, key, slots, reason, records);

    private void Park(uint player, string key, uint reason, int expect = StorageNative.Ok)
    {
        var bytes = Encoding.UTF8.GetBytes(key);
        fixed (byte* p = bytes) Assert.Equal(expect, _t->Park(Saves, player, new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, reason));
    }

    private void Unpark(uint player, string key, int expect = StorageNative.Ok)
    {
        var bytes = Encoding.UTF8.GetBytes(key);
        fixed (byte* p = bytes) Assert.Equal(expect, _t->Unpark(Saves, player, new NativeStr { Ptr = p, Len = (nuint)bytes.Length }));
    }

    private List<StorageNative.Event> Events()
    {
        var list = new List<StorageNative.Event>();
        StorageNative.Event e = default;
        ulong after = 0;
        while (true)
        {
            e.Size = (uint)sizeof(StorageNative.Event);
            if (_t->NextEvent(after, &e) != StorageNative.Ok) return list;
            after = e.Seq;
            list.Add(e);
        }
    }

    // ---- tests ----

    [Fact]
    public void ThePersistRowsHaveOneOwnerAndNeedTheCapturesDoneHook()
    {
        ulong last;
        StorageNative.Witness w = default;
        w.Size = (uint)sizeof(StorageNative.Witness);
        Assert.Equal(StorageNative.Unavailable, _t->WitnessOf(Saves, 0, &w));  // nobody bound
        _fake.StagePoint = false;
        Assert.Equal(StorageNative.Unavailable, _t->PersistBind(Saves, null, null, &last));  // no captures-done hook on this build
        _fake.StagePoint = true;
        Assert.Equal(StorageNative.Ok, _t->PersistBind(Saves, null, null, &last));
        Assert.Equal(StorageNative.Unavailable, _t->PersistBind(ModA, null, null, &last));  // another owner
        Assert.Equal(StorageNative.Unavailable, _t->ReleaseFrozen(ModA, 1));
        Assert.Equal(StorageNative.Ok, _t->PersistBind(Saves, null, null, &last));  // again: keeps the claim
        _fake.Revoke(Saves);
        Assert.Equal(StorageNative.Ok, _t->PersistBind(ModA, null, null, &last));  // a revoke releases it
    }

    [Fact]
    public void RegistrationsFollowTheHeadersRules()
    {
        Assert.Equal(StorageNative.InvalidArgument, RegisterRc(ModA, "nocolon", 8));
        Assert.Equal(StorageNative.InvalidArgument, RegisterRc(ModA, "Mod:Upper", 8));
        Assert.Equal(StorageNative.InvalidArgument, RegisterRc(ModA, "a:b", 65));
        Assert.Equal(StorageNative.InvalidArgument, RegisterRc(ModA, "a:b", 8, [5, 3]));  // ids must be sorted
        Register(ModA, "a:b", 8);
        Assert.Equal(StorageNative.InvalidArgument, RegisterRc(ModB, "a:b", 8));  // one live registration per key
        for (var i = 0; i < 7; i++) Register(ModA, $"a:x{i}", 8);
        Assert.Equal(StorageNative.Unavailable, RegisterRc(ModA, "a:x7", 8));  // 8 per owner
        for (var i = 0; i < 3; i++) Register(ModB, $"b:big{i}", 64);  // 64 + 192 = 256 slots
        Assert.Equal(StorageNative.Unavailable, RegisterRc(ModB, "b:one", 1));  // over 256 slots in all
    }

    [Fact]
    public void AJoinGivesTheWitnessAndTheIdentity()
    {
        Bind();
        var join = _fake.JoinRead(3, Account, Character, 0xABCDEF);
        var w = Witness(3);
        Assert.Equal((join, 0xABCDEFUL, Account, StorageNative.WitnessRead, 0u, 3u), (w.Join, w.Value, w.Account, w.Flags, w.Player, w.Slot));
        Assert.Equal(Character, StorageNative.CharacterOf(w.Character));
        Assert.Contains(Events(), e => e.Kind == StorageNative.EventJoined && e.Join == join);
        _fake.JoinApply(3, 4);
        Assert.Equal((StorageNative.WitnessRead | StorageNative.WitnessApplied, 4u), (Witness(3).Flags, Witness(3).Player));
        StorageNative.Witness none = default;
        none.Size = (uint)sizeof(StorageNative.Witness);
        Assert.Equal(StorageNative.NotFound, _t->WitnessOf(Saves, 9, &none));
    }

    [Fact]
    public void ARestoreIsCheckedAndTakenOncePerJoin()
    {
        Bind();
        var join = _fake.JoinRead(0, Account, Character, 7);
        Assert.Equal(StorageNative.NotFound, Restore(0, join + 1, 0, StorageNative.SourceNew, 0, Account, Character));  // not the current join
        Assert.Equal(StorageNative.InvalidArgument, Restore(0, join, 0, StorageNative.SourceNew, 0, Account, Guid.NewGuid()));  // another character
        Assert.Equal(StorageNative.InvalidArgument, Restore(0, join, 0, 0, 0, Account, Character));  // no source
        Assert.Equal(StorageNative.InvalidArgument, Restore(0, join, 0, StorageNative.SourceServer, 0, Account, Character,
            new Entry(StorageNative.EntryQuarantine, "a:b", 8, 0, [])));
        Assert.Equal(StorageNative.InvalidArgument, Restore(0, join, 0, StorageNative.SourceServer, 0, Account, Character,
            Live("a:b", 8), Live("a:b", 8)));  // two LIVE entries of one key
        Assert.Equal(StorageNative.Ok, Restore(0, join, StorageNative.DocPaired, StorageNative.SourceNew, 3, Account, Character));
        Assert.Equal(StorageNative.InvalidArgument, Restore(0, join, 0, StorageNative.SourceNew, 3, Account, Character));  // once per join
        Assert.Equal(StorageNative.WitnessRead | StorageNative.WitnessRestored, Witness(0).Flags);
    }

    [Fact]
    public void ACaptureBeforeTheRestoreSaysNothingAboutTheStorage()
    {
        Bind();
        Register(ModA, "a:quiver", 8);
        _fake.JoinRead(0, Account, Character, 7);
        _fake.JoinApply(0, 1);
        _fake.SavesSeq = 12;
        _fake.SaveRevision = 301;
        _fake.CaptureOf(0, 8);
        var tag = _fake.CapturesDone();
        var batch = Take(tag);
        var c = Assert.Single(batch.Captures);
        Assert.Equal((StorageNative.CaptureUnrestored, 8UL, 0u), (c.Flags, c.Witness, c.EntryCount));  // absent, not empty
        Assert.Equal((12UL, 301u, StorageNative.BatchComplete), (batch.Batch.SavesSeq, batch.Batch.Revision, batch.Batch.Flags));
        Assert.Equal((Account, Character), (c.Account, StorageNative.CharacterOf(c.Character)));
        Assert.Equal(8UL, Witness(0).Captured);
    }

    [Fact]
    public void TheCapturesDoneStageRunsWithTheCompleteBatch()
    {
        Bind();
        Register(ModA, "a:quiver", 8);
        var join = _fake.JoinRead(0, Account, Character, 7);
        Assert.Equal(StorageNative.Ok, Restore(0, join, StorageNative.DocPaired, StorageNative.SourceServer, 41, Account, Character,
            Live("a:quiver", 8, (0, 0x77, 64), (3, 0x77, 12))));
        _fake.CaptureOf(0, 7);  // before the join apply: the given entries, unchanged
        _fake.JoinApply(0, 1);
        var other = Guid.NewGuid();
        var second = _fake.JoinRead(1, Account + 1, other, 9);
        Assert.Equal(StorageNative.Ok, Restore(1, second, 0, StorageNative.SourceNew, 0, Account + 1, other));
        _fake.JoinApply(1, 2);
        Assert.Empty(Stages);
        _fake.CaptureOf(1, 9);
        var tag = _fake.CapturesDone();
        Assert.Equal((StorageNative.EventBatch, tag), Assert.Single(Stages));
        var batch = Take(tag);
        Assert.Equal(2, batch.Captures.Length);
        Assert.Equal((41UL, 0UL), (batch.Captures[0].Revision, batch.Captures[0].Changes));
        var given = batch.Entries[(int)batch.Captures[0].FirstEntry];
        Assert.Equal(("a:quiver", StorageNative.EntryLive, 2), (given.Key, given.Entry.Kind, given.Records.Length));
        Assert.Equal((64u, (byte)0), (given.Records[0].Count, given.Records[0].Data[0]));  // the record's bytes start with its slot
        var newcomer = batch.Entries[(int)batch.Captures[1].FirstEntry];
        Assert.Equal(("a:quiver", 0), (newcomer.Key, newcomer.Records.Length));  // a registered key with no copy: an empty storage
        Assert.Equal(StorageNative.Ok, _t->ReleaseFrozen(Saves, tag));
        Assert.Equal(StorageNative.NotFound, _t->ReleaseFrozen(Saves, tag));
        Assert.Equal(0UL, _fake.CapturesDone());  // nothing captured since
    }

    [Fact]
    public void ARestoreParksWhatItCannotUseAndChangesShowInTheNextCapture()
    {
        Bind();
        var quiver = Register(ModA, "a:quiver", 8);
        Register(ModA, "a:held", 8);
        var join = _fake.JoinRead(0, Account, Character, 7);
        Assert.Equal(StorageNative.Ok, Restore(0, join, 0, StorageNative.SourceClient, 5, Account, Character,
            Live("a:quiver", 18, (0, 0x77, 10), (12, 0x77, 3)),
            Parked("a:held", 8, StorageNative.ParkStale, (0, 0x78, 1)),
            Live("gone:bag", 8, (1, 0x79, 2))));
        _fake.JoinApply(0, 1);
        _fake.CaptureOf(0, 7);
        var first = Take(_fake.CapturesDone());
        var kinds = first.Entries.Select(e => (e.Key, e.Entry.Kind, e.Entry.Reason, e.Records.Length)).ToList();
        Assert.Equal(new[]
        {
            ("a:held", StorageNative.EntryLive, 0u, 0), ("a:quiver", StorageNative.EntryLive, 0u, 1),
            ("a:held", StorageNative.EntryParked, StorageNative.ParkStale, 1), ("a:quiver", StorageNative.EntryParked, StorageNative.ParkOverflow, 1),
            ("gone:bag", StorageNative.EntryParked, StorageNative.ParkUnregistered, 1),
        }, kinds);
        Assert.Equal(0UL, first.Captures[0].Changes);
        _fake.Put(1, "a:quiver", new StorageFakeRecord(5, 0x77, 2));
        _fake.CaptureOf(0, 7);  // the witness did not change: the storage did
        var second = Take(_fake.CapturesDone());
        Assert.Equal(1UL, second.Captures[0].Changes);
        StorageNative.State s = default;
        s.Size = (uint)sizeof(StorageNative.State);
        Assert.Equal(StorageNative.Ok, _t->GetState(ModA, quiver, 1, &s));
        Assert.Equal((StorageNative.StateReady, 2u, 1u, StorageNative.SourceClient, 5UL), (s.Status, s.Stacks, s.Parked, s.Source, s.Revision));
        Assert.Equal(StorageNative.NotFound, _t->GetState(ModB, quiver, 1, &s));  // not ModB's handle
    }

    [Fact]
    public void ParkAndUnparkMoveAStorageOutOfPlayAndBack()
    {
        Bind();
        var quiver = Register(ModA, "a:quiver", 8);
        var join = _fake.JoinRead(0, Account, Character, 7);
        Assert.Equal(StorageNative.Ok, Restore(0, join, 0, StorageNative.SourceServer, 1, Account, Character, Live("a:quiver", 8, (2, 0x77, 10))));
        _fake.JoinApply(0, 1);
        Park(1, "a:quiver", StorageNative.ParkOverflow, StorageNative.InvalidArgument);  // only held reasons
        Park(1, "a:quiver", StorageNative.ParkRequested);
        _fake.Step();
        uint holder;
        Assert.Equal(StorageNative.NotFound, _t->Holder(ModA, quiver, 1, &holder));
        Assert.Equal(StorageNative.ParkRequested, Assert.Single(_fake.Contents(0, "a:quiver").Parked).Reason);
        StorageNative.State state = default;
        state.Size = (uint)sizeof(StorageNative.State);
        Assert.Equal(StorageNative.Ok, _t->GetState(ModA, quiver, 1, &state));
        Assert.Equal(StorageNative.StateParked, state.Status);
        Unpark(1, "a:quiver");
        _fake.Step();
        Assert.Equal(StorageNative.Ok, _t->Holder(ModA, quiver, 1, &holder));
        Assert.Equal(2, Assert.Single(_fake.Contents(0, "a:quiver").Live).Slot);
        Unpark(1, "a:quiver", StorageNative.NotFound);
    }

    [Fact]
    public void ARevokeParksAndANewRegistrationRestoresLive()
    {
        Bind();
        Register(ModA, "a:quiver", 8);
        var join = _fake.JoinRead(0, Account, Character, 7);
        Assert.Equal(StorageNative.Ok, Restore(0, join, 0, StorageNative.SourceServer, 1, Account, Character, Live("a:quiver", 8, (0, 0x77, 10))));
        _fake.JoinApply(0, 1);
        _fake.Revoke(ModA);
        Assert.Equal(StorageNative.ParkUnregistered, Assert.Single(_fake.Contents(0, "a:quiver").Parked).Reason);
        var again = Register(ModB, "a:quiver", 8);  // the mod came back (another load, another owner)
        _fake.Step();
        uint holder;
        Assert.Equal(StorageNative.Ok, _t->Holder(ModB, again, 1, &holder));
        Assert.Empty(_fake.Contents(0, "a:quiver").Parked);
        Assert.Contains(Events(), e => e.Kind == StorageNative.EventRestored && (e.Flags & StorageNative.EventLive) != 0);
    }

    [Fact]
    public void ALockedDocumentBuildsNothingAndSaysSo()
    {
        Bind();
        Register(ModA, "a:quiver", 8);
        var join = _fake.JoinRead(0, Account, Character, 7);
        Assert.Equal(StorageNative.Ok, Restore(0, join, StorageNative.DocLocked, StorageNative.SourceServer, 9, Account, Character,
            Live("a:quiver", 8, (0, 0x77, 10))));
        _fake.JoinApply(0, 1);
        _fake.Step();
        _fake.CaptureOf(0, 7);
        var c = Take(_fake.CapturesDone());
        Assert.Equal(StorageNative.CaptureLocked, c.Captures[0].Flags & StorageNative.CaptureLocked);
        Assert.Equal(10u, Assert.Single(c.Entries).Records[0].Count);  // kept as given
    }

    [Fact]
    public void ALeaveMarksItsCapturesAndBatchesStayApart()
    {
        Bind();
        var join = _fake.JoinRead(0, Account, Character, 7);
        Assert.Equal(StorageNative.Ok, Restore(0, join, 0, StorageNative.SourceNew, 0, Account, Character));
        _fake.JoinApply(0, 1);
        _fake.CaptureOf(0, 7);
        var failed = _fake.CapturesDone();
        _fake.CaptureOf(0, 7);
        var retry = _fake.CapturesDone();
        Assert.NotEqual(failed, retry);
        _fake.Leave(0);
        Assert.Equal(StorageNative.CaptureLeft, Take(retry).Captures[0].Flags & StorageNative.CaptureLeft);
        Assert.Equal(StorageNative.WitnessLeft, Witness(0).Flags & StorageNative.WitnessLeft);
        Assert.Contains(Events(), e => e.Kind == StorageNative.EventLeft);
        for (var i = 0; i < 20; i++)
        {
            _fake.JoinRead(1, Account, Character, 1);
            _fake.CaptureOf(1, 1);
            _fake.CapturesDone();
        }
        Assert.Equal(16, _fake.BatchTags.Count);  // the newest 16
    }

    [Fact]
    public void AStorageRefusalIsReported()
    {
        Bind();
        Register(ModA, "a:quiver", 8);
        var join = _fake.JoinRead(0, Account, Character, 7);
        Assert.Equal(StorageNative.Ok, Restore(0, join, 0, StorageNative.SourceNew, 0, Account, Character));
        _fake.JoinApply(0, 1);
        _fake.RefuseFreeze = 7;  // faulted
        _fake.CaptureOf(0, 7);
        var b = Take(_fake.CapturesDone());
        Assert.Equal((StorageNative.CaptureRefused, 7u, 0u), (b.Captures[0].Flags & StorageNative.CaptureRefused, b.Captures[0].Reason, b.Captures[0].EntryCount));
        Assert.Equal(StorageNative.BatchRefused, b.Batch.Flags & StorageNative.BatchRefused);
    }
}
