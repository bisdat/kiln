// SPDX-License-Identifier: MIT
// The blittable mirror of kiln.items.storage v1 (native/core/abi/services/kiln_items_storage.h, served by kiln-items.dll):
// every struct, the table and the constants, for Kiln.Items' player storage (PlayerStorage.cs) and for Kiln.Saves, which
// drives the persist rows (InternalsVisibleTo). Sizes are checked against the header in Kiln.Items.Tests
// (StorageNativeLayoutTests) and the header's own layout test (native/core/tests/abi_headers/items_storage.c).
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

internal static unsafe class StorageNative
{
    public const string Service = "kiln.items.storage";
    public const uint Version = 1;

    // KilnResult
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5,
        Revoked = 6, Internal = 7;

    // limits
    public const int MaxSlots = 64, MaxPerMod = 8, MaxSlotsPlayer = 256, MaxFilterIds = 1024, MaxStorages = 32,
        MaxEntries = 128, MaxRecords = 2048, KeyMax = 128, RecordMax = 64;

    // KIS_DEATH_*
    public const uint DeathKeep = 0, DeathGrave = 1, DeathFollowGame = 2;
    // KIS_FILTER_*
    public const uint FilterAmmoArrow = 1, FilterAmmoSpell = 2, FilterNoPide = 4;
    // KIS_STATE_*
    public const uint StateNone = 0, StateWaiting = 1, StateReady = 2, StateParked = 3, StateLocked = 4;
    // KIS_SOURCE_*
    public const uint SourceNone = 0, SourceNew = 1, SourceServer = 2, SourceClient = 3;
    // KIS_ENTRY_*
    public const uint EntryLive = 1, EntryParked = 2, EntryQuarantine = 3;
    // KIS_PARK_*
    public const uint ParkUnregistered = 1, ParkOverflow = 2, ParkFilter = 3, ParkHeld = 16, ParkRequested = 16, ParkStale = 17,
        ParkLocked = 18, ParkReturned = 19;
    // KIS_QUARANTINE_*
    public const byte QuarantineUnknownItem = 1, QuarantineCount = 2, QuarantineSlot = 3, QuarantineDecode = 4,
        QuarantineRebuild = 5;
    // KIS_REF_*
    public const int RefRanged = 0, RefArrow = 1, RefSpell = 2, RefBait = 3, RefCount = 4;
    public const ushort RefNone = 0xFFFF;
    public const uint RefClear = 0xFFFFFFFF;
    // KIS_RECORD_*
    public const byte RecordLevel = 0x01, RecordDurability = 0x02, RecordLiquid = 0x04, RecordDye = 0x08,
        RecordUpgrades = 0x10, RecordGem = 0x20;
    // KIS_DOC_*
    public const uint DocPaired = 1, DocLocked = 2;
    // KIS_CAPTURE_*
    public const uint CaptureUnrestored = 1, CaptureLocked = 2, CaptureRefused = 4, CaptureLeft = 8;
    // KIS_BATCH_*
    public const uint BatchComplete = 1, BatchRefused = 2;
    // KIS_WITNESS_*
    public const uint WitnessRead = 1, WitnessApplied = 2, WitnessRestored = 4, WitnessLeft = 8, WitnessExtraMainPages = 0x10;
    // KIS_EVENT_*
    public const uint EventJoined = 1, EventCaptured = 2, EventRestored = 3, EventParked = 4, EventQuarantined = 5,
        EventDeath = 6, EventLeft = 7, EventReturned = 8, EventBatch = 9;
    public const uint EventPaired = 1, EventLive = 2;
    // the two kiln.items values the header defines
    public const uint FindPlayerStorage = 0x10;
    public const uint ItemsFiltered = 10;

    /// <summary>KilnStorageFilter (24 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Filter
    {
        public uint Size, Categories, Flags, IdCount;
        public uint* Ids;
    }

    /// <summary>KilnStorageDef (56 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Def
    {
        public uint Size, Slots;
        public NativeStr Key;
        public uint Death, Flags;
        public Filter Filter;
    }

    /// <summary>KilnStorageState (192 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct State
    {
        public uint Size, Status;
        public fixed byte Key[KeyMax];
        public uint Handle, Slots, Pages, Holder, Stacks, Parked, Quarantined, Source, Paired, Death;
        public fixed ushort Refs[RefCount];
        public ulong Revision;
    }

    /// <summary>KilnStorageRecord (80 bytes, align 4).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Record
    {
        public ushort Slot;
        public byte Length, Flags;
        public uint Item, Count;
        public byte Level, Reason;
        public ushort Reserved;
        public fixed byte Data[RecordMax];
    }

    /// <summary>KilnStorageEntry (160 bytes, align 4).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Entry
    {
        public uint Size, Kind;
        public fixed byte Key[KeyMax];
        public uint Slots, First, Count, Reason;
        public fixed ushort Refs[RefCount];
    }

    /// <summary>KilnStorageDoc (80 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Doc
    {
        public uint Size, Flags, Source, EntryCount, RecordCount, Reserved;
        public ulong Revision, Witness, Account;
        public fixed byte Character[16];
        public Entry* Entries;
        public Record* Records;
    }

    /// <summary>KilnStorageCapture (104 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Capture
    {
        public uint Size, Flags;
        public ulong Seq, Join, Step, Witness, Revision, Changes, Account;
        public fixed byte Character[16];
        public uint Player, Slot, FirstEntry, EntryCount, Reason, Reserved;
    }

    /// <summary>KilnStorageFrozenBatch (80 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Batch
    {
        public uint Size, Flags;
        public ulong Tag, SavesSeq;
        public uint Revision, CaptureCount, EntryCount, RecordCount;
        public ulong FirstStep, LastStep;
        public long UnixMs;
        public fixed byte WorldGuid[16];
    }

    /// <summary>KilnStorageWitness (80 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Witness
    {
        public uint Size, Flags;
        public ulong Join, Value, Step, Captured, CaptureSeq, Account;
        public fixed byte Character[16];
        public uint Player, Slot;
    }

    /// <summary>KilnStorageEvent (240 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Event
    {
        public uint Size, Kind;
        public ulong Seq, Step;
        public uint Player, Handle, Flags, Reason, Stacks, Items, Target, Source, Slot, Reserved;
        public ulong Revision, Join, Tag, Account;
        public fixed byte Character[16];
        public fixed byte Key[KeyMax];
    }

    /// <summary>KilnItemsStorageStatus (208 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Status
    {
        public uint Size;
        public byte Storage, Persist, Session, StagePoint;
        public uint Reason, Registrations, Holders, Parked, Quarantined, Batches;
        public ulong Captures, Restores;
        public fixed byte Detail[160];
    }

    /// <summary>KilnStorageReturnResult (208 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ReturnResult
    {
        public uint Size, Status;
        public ulong Ticket;
        public uint Player, Target, Requested, Done, Stacks, Kept, Steps, Reserved;
        public fixed byte Message[160];
    }

    /// <summary>KilnItemsStorageApi v1 (128 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        // public rows
        public delegate* unmanaged[Cdecl]<ulong, Def*, uint*, int> RegisterStorage;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint, uint*, int> Holder;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint, State*, int> GetState;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint, uint, uint, int> SetRef;
        public delegate* unmanaged[Cdecl]<Status*, int> GetStatus;
        public delegate* unmanaged[Cdecl]<uint, State*, uint, uint*, int> List;
        public delegate* unmanaged[Cdecl]<ulong, Event*, int> NextEvent;
        public delegate* unmanaged[Cdecl]<ulong, uint, NativeStr, uint, delegate* unmanaged[Cdecl]<ReturnResult*, void*, void>, void*, ulong*, int> ReturnParked;
        // persist rows (Kiln.Saves only)
        public delegate* unmanaged[Cdecl]<ulong, delegate* unmanaged[Cdecl]<uint, void*, void>, void*, ulong*, int> PersistBind;
        public delegate* unmanaged[Cdecl]<ulong, ulong, Batch*, Capture*, uint, Entry*, uint, Record*, uint, int> TakeFrozen;
        public delegate* unmanaged[Cdecl]<ulong, ulong, int> ReleaseFrozen;
        public delegate* unmanaged[Cdecl]<ulong, uint, Witness*, int> WitnessOf;
        public delegate* unmanaged[Cdecl]<ulong, uint, ulong, Doc*, int> Restore;
        public delegate* unmanaged[Cdecl]<ulong, uint, NativeStr, uint, int> Park;
        public delegate* unmanaged[Cdecl]<ulong, uint, NativeStr, int> Unpark;
    }

    private static Table* _table;
    private static Table* _forTests;
    private static bool _testSeam;
    private static readonly object Lock = new();

    /// <summary>The table, or null when kiln-items.dll does not serve it (an older one, the test kit without its fake).
    /// Looked up until found; a table smaller than v1's is no table.</summary>
    public static Table* T
    {
        get
        {
            if (_testSeam) return _forTests;
            if (_table != null) return _table;
            lock (Lock)
            {
                if (_table != null) return _table;
                var t = Services.Get<Table>(Service, Version);
                return _table = t != null && t->Header.Size >= (uint)sizeof(Table) ? t : null;
            }
        }
    }

    /// <summary>Tests: answer <paramref name="table"/> instead of the backend's (null: none); <see cref="ResetForTests"/>
    /// goes back to the backend.</summary>
    internal static void UseForTests(Table* table)
    {
        lock (Lock)
        {
            _forTests = table;
            _testSeam = true;
            _table = null;
        }
    }

    internal static void ResetForTests()
    {
        lock (Lock)
        {
            _forTests = null;
            _testSeam = false;
            _table = null;
        }
    }

    /// <summary>A NUL-terminated UTF-8 buffer as a string.</summary>
    public static string Text(byte* p, int capacity)
    {
        var n = 0;
        while (n < capacity && p[n] != 0) n++;
        return Encoding.UTF8.GetString(p, n);
    }

    /// <summary>Writes <paramref name="text"/> into a NUL-terminated buffer; false when it does not fit (nothing is
    /// written then).</summary>
    public static bool SetText(byte* p, int capacity, string text)
    {
        var bytes = Encoding.UTF8.GetBytes(text);
        if (bytes.Length >= capacity) return false;
        new Span<byte>(p, capacity).Clear();
        bytes.CopyTo(new Span<byte>(p, capacity));
        return true;
    }

    /// <summary>The game's CharacterId bytes (Windows byte order, as .NET Guid(byte[])).</summary>
    public static Guid CharacterOf(byte* p) => new(new ReadOnlySpan<byte>(p, 16));

    public static void SetCharacter(byte* p, Guid id) => id.TryWriteBytes(new Span<byte>(p, 16));
}
