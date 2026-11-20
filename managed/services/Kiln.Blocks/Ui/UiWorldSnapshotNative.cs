// SPDX-License-Identifier: MIT
// The native table kiln.ui.snapshot v1 (native/core/abi/services/kiln_ui_snapshot.h): live game values for UI blocks,
// collected by kiln-core on the right world's own thread (05a's pre-provider stage and the client world's Post; the
// hosted server's Post). Records and slots as laid out in C, and what Kiln.Blocks does with them: one read is
// acquire, copy (again with the same token when the inventory needs more room) and release, and gives one deep-immutable
// UiWorldSnapshot. Also here: the source seam's defaults (UiWorldSource in UiWorldSnapshot.cs points here), the rows
// ui.blocks.world and world.* (UiWorldCaps), and the client-main-thread feed of UiWorldSnapshotChangedEvent.
// Nothing here reads the world: the table only hands out what the collector published.
using System.Collections.Immutable;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class UiWorldSnapshotNative
{
    public const string Service = "kiln.ui.snapshot";
    public const uint Version = 1;
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5,
        Revoked = 6, Internal = 7;

    /// <summary>KilnUiWorldMask: every known bit (UiWorldFields).</summary>
    public const uint KnownMask = 15;

    /// <summary>The most records a publication carries (more slots: inventory is unavailable with Budget).</summary>
    public const int MaxRows = 512;

    // KilnUiSnapshotReason
    public const uint ReasonNone = 0, ReasonNoWorld = 1, ReasonNoSnapshot = 2, ReasonBudget = 3, ReasonExpired = 4,
        ReasonBufferSmall = 5, ReasonDeferredSource = 6, ReasonMissingComponent = 7, ReasonStaleSource = 8, ReasonLayoutOff = 9;

    /// <summary>KilnUiSnapshotAcquire (40 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Acquire
    {
        public uint Size, Ok, Reason, Reserved;
        public ulong Token, Publication, CaptureMs;
    }

    /// <summary>KilnUiWorldValues (152 bytes; 4 bytes of padding after ManaMax, XFixed at 96).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Values
    {
        public uint Size, AvailableMask, RequestedMask, InventoryCount;
        public fixed uint UnavailableReason[4];
        public ulong Publication, LocalSession, LocalStep, LocalCaptureMs;
        public uint LocalPlayer, Health, HealthMax, Stamina, StaminaMax, Mana, ManaMax;
        public long XFixed, YFixed, ZFixed;
        public ulong ServerSession, ServerStep, ServerCaptureMs;
        public double ServerHours;
    }

    /// <summary>KilnUiInventoryRow (24 bytes, no size field: fixed for the life of the v1 table).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct InventoryRow
    {
        public uint Holder, InventoryPage, SlotIndex, Item, Count, Pide;
    }

    /// <summary>KilnUiDemandResult (24 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct DemandResult
    {
        public uint Size, Ok, Reason, Reserved;
        public ulong Handle;
    }

    /// <summary>KilnUiSnapshotApi v1 (48 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, uint, DemandResult*, int> DemandAdd;
        public delegate* unmanaged[Cdecl]<ulong, int> DemandRemove;
        public delegate* unmanaged[Cdecl]<ulong, Acquire*, int> AcquireSnapshot;
        public delegate* unmanaged[Cdecl]<ulong, Values*, InventoryRow*, uint, uint*, int> Copy;
        public delegate* unmanaged[Cdecl]<ulong, int> Release;
    }

    private static Table* _table;
    private static readonly object Lock = new();

    /// <summary>The table, or null (a dedicated server, an older kiln-core, no core). A miss is asked again at the next
    /// use, so a table published later (or a test's new backend) is found. Checks header.size and every slot.</summary>
    public static Table* T
    {
        get
        {
            if (_table != null) return _table;
            lock (Lock)
            {
                if (_table != null) return _table;
                var t = Services.Get<Table>(Service, Version);
                if (t == null || t->DemandAdd == null || t->DemandRemove == null || t->AcquireSnapshot == null ||
                    t->Copy == null || t->Release == null)
                    return null;
                return _table = t;
            }
        }
    }

    /// <summary>Forgets the table (ServiceCaps.WhenFound, tests).</summary>
    internal static void LookAgain()
    {
        lock (Lock) _table = null;
    }

    private static ulong _ownOwner;

    /// <summary>Kiln.Blocks' own native owner for reads (tokens live for one read, under ReadGate) and the event feed.</summary>
    internal static ulong OwnOwner
    {
        get
        {
            if (Volatile.Read(ref _ownOwner) == 0)
                lock (Lock)
                    if (_ownOwner == 0) _ownOwner = Backend.Current.OwnerCreate("kiln.blocks.ui-world");
            return _ownOwner;
        }
    }

    /// <summary>The owner a demand is made with: the mod's own (its demands go when it unloads), else Kiln.Blocks'.</summary>
    internal static ulong OwnerOf(ModContext? mod) => mod is { Owner: not 0 } ? mod.Owner : OwnOwner;

    /// <summary>The owner of a loaded mod by id (the block runtime knows its windows' mods by id).</summary>
    internal static ulong OwnerOf(string? mod)
    {
        if (string.IsNullOrEmpty(mod)) return OwnOwner;
        var current = ModContext.CurrentMod;
        var info = current?.Info.Id == mod ? current.Info : Mods.Get(mod)?.Info;
        return OwnerOf(info == null ? null : ModContext.FindLive(info));
    }

    /// <summary>The stable name of a KilnUiSnapshotReason (UiWorldSnapshot.UnavailableReasons, UiWorldReadResult.Reason,
    /// UiDemandRegistration.Reason).</summary>
    internal static string ReasonName(uint reason) => reason switch
    {
        ReasonNoWorld => "NoWorld",
        ReasonNoSnapshot => "NoSnapshot",
        ReasonBudget => "Budget",
        ReasonExpired => "Expired",
        ReasonBufferSmall => "BufferTooSmall",
        ReasonDeferredSource => "DeferredSource",
        ReasonMissingComponent => "MissingComponent",
        ReasonStaleSource => "StaleSource",
        ReasonLayoutOff => "LayoutOff",
        _ => "Unavailable",
    };

    /// <summary>Whether <paramref name="fields"/> names at least one group and only known ones.</summary>
    internal static bool ValidFields(UiWorldFields fields) => fields != UiWorldFields.None && ((uint)fields & ~KnownMask) == 0;

    // One read at a time: Kiln.Blocks holds at most one token (the native budget is 2 per owner).
    private static readonly object ReadGate = new();

    /// <summary>One read: acquire, copy (once more with the same token when the inventory needs more room than the first
    /// buffer), release. Null when the newest publication is <paramref name="unless"/> (the feed's "nothing new").
    /// Throws nothing; a missing table is Ok false with reason "Unavailable".</summary>
    internal static UiWorldReadResult? Read(ulong owner, ulong unless = 0)
    {
        var t = T;
        if (t == null) return new UiWorldReadResult(false, "Unavailable", null);
        lock (ReadGate)
        {
            Acquire a = default;
            a.Size = (uint)sizeof(Acquire);
            var rc = t->AcquireSnapshot(owner, &a);
            if (rc != Ok) return new UiWorldReadResult(false, rc == Revoked ? "Revoked" : "Unavailable", null);
            if (a.Ok == 0) return new UiWorldReadResult(false, ReasonName(a.Reason), null);
            try
            {
                if (unless != 0 && a.Publication == unless) return null;
                const int First = 32;
                var small = stackalloc InventoryRow[First];
                Values v = default;
                v.Size = (uint)sizeof(Values);
                uint required = 0;
                rc = t->Copy(a.Token, &v, small, First, &required);
                if (rc == BufferTooSmall)
                {
                    // nothing was written; the same token copies the same publication with room for all of it
                    if (required > MaxRows) return new UiWorldReadResult(false, "Budget", null);
                    var rows = new InventoryRow[required];
                    fixed (InventoryRow* p = rows) rc = t->Copy(a.Token, &v, p, required, &required);
                    if (rc != Ok) return Refused(rc);
                    if (!Consistent(in v, in a, required, (uint)rows.Length)) return Mismatch();
                    return new UiWorldReadResult(true, null, Build(in v, rows));
                }
                if (rc != Ok) return Refused(rc);
                if (!Consistent(in v, in a, required, First)) return Mismatch();
                return new UiWorldReadResult(true, null, Build(in v, new ReadOnlySpan<InventoryRow>(small, (int)required)));
            }
            finally
            {
                t->Release(a.Token);
            }
        }
    }

    // A token the world revoked (a world or session change) shows nothing; one whose 2 s ran out is an Expired refusal.
    private static UiWorldReadResult Refused(int rc) => new(false, rc switch
    {
        Revoked => "NoWorld",
        NotFound => "Expired",
        _ => "Unavailable",
    }, null);

    private static UiWorldReadResult Mismatch() => new(false, "Unavailable", null);

    // Every reader checks the record's size and that header and records are the token's publication.
    private static bool Consistent(in Values v, in Acquire a, uint required, uint capacity) =>
        v.Size >= (uint)sizeof(Values) && v.Publication == a.Publication && v.InventoryCount == required && required <= capacity &&
        required <= MaxRows;

    /// <summary>The deep-immutable managed value of one publication. Fields of a group not in AvailableMask are zero.</summary>
    internal static UiWorldSnapshot Build(in Values v, ReadOnlySpan<InventoryRow> rows)
    {
        var avail = v.AvailableMask & KnownMask;
        var reasons = new string?[4];
        for (var i = 0; i < 4; i++) reasons[i] = (avail & (1u << i)) != 0 ? null : ReasonName(v.UnavailableReason[i]);
        const uint HostBit = (uint)UiWorldFields.HostTime;
        if ((avail & HostBit) != 0 && !(double.IsFinite(v.ServerHours) && Math.Abs(v.ServerHours) < 1e9))
        {
            avail &= ~HostBit;  // never a fabricated hour
            reasons[3] = ReasonName(ReasonMissingComponent);
        }
        bool Has(UiWorldFields f) => (avail & (uint)f) != 0;
        var inventory = ImmutableArray<UiInventoryValue>.Empty;
        if (Has(UiWorldFields.LocalInventory))
        {
            var b = ImmutableArray.CreateBuilder<UiInventoryValue>(rows.Length);
            foreach (var r in rows)
            {
                if (r.SlotIndex > byte.MaxValue) continue;  // a u8 in the game; never truncated into another slot
                b.Add(new UiInventoryValue(KeyOf(r.InventoryPage, r.SlotIndex), r.Holder, r.InventoryPage, (byte)r.SlotIndex, r.Item,
                    r.Count, r.Pide));
            }
            inventory = b.ToImmutable();
        }
        var vitals = Has(UiWorldFields.LocalVitals);
        var position = Has(UiWorldFields.LocalPosition);
        var host = Has(UiWorldFields.HostTime);
        return new UiWorldSnapshot(v.Publication, avail,
            new UiSnapshotStamp(v.LocalSession, v.LocalStep, v.LocalCaptureMs),
            host ? new UiSnapshotStamp(v.ServerSession, v.ServerStep, v.ServerCaptureMs) : new UiSnapshotStamp(0, 0, 0),
            v.LocalPlayer,
            vitals ? v.Health : 0, vitals ? v.HealthMax : 0, vitals ? v.Stamina : 0, vitals ? v.StaminaMax : 0,
            vitals ? v.Mana : 0, vitals ? v.ManaMax : 0,
            position ? v.XFixed : 0, position ? v.YFixed : 0, position ? v.ZFixed : 0,
            host ? (decimal)v.ServerHours : 0m,
            inventory, ImmutableArray.Create(reasons));
    }

    /// <summary>A slot's key: <c>l:</c>, 8 hex digits of the page, <c>:</c>, the slot index.</summary>
    internal static string KeyOf(uint page, uint slot) => string.Create(CultureInfo.InvariantCulture, $"l:{page:x8}:{slot}");

    /// <summary>Adds a demand. <paramref name="strict"/>: the public API's error rule (ArgumentException, KilnUnavailableException,
    /// InvalidOperationException); else every failure is a refused registration (the block runtime).</summary>
    internal static UiDemandRegistration Demand(ulong owner, UiWorldFields fields, bool strict)
    {
        if (!ValidFields(fields))
        {
            if (strict) throw new ArgumentException($"no known group of live game values in {fields}", nameof(fields));
            return new UiDemandRegistration(false, "InvalidArgument", 0, null);
        }
        var t = T;
        if (t == null)
        {
            if (strict) throw ServiceCaps.Unavailable(UiWorldCaps.World, "kiln.ui.snapshot is not here");
            return new UiDemandRegistration(false, "Unavailable", 0, null);
        }
        DemandResult r = default;
        r.Size = (uint)sizeof(DemandResult);
        var rc = t->DemandAdd(owner, (uint)fields, &r);
        switch (rc)
        {
            case Ok:
                break;
            case InvalidArgument when strict:
                throw new ArgumentException($"kiln.ui.snapshot refused the groups {fields}", nameof(fields));
            case Revoked when strict:
                throw new InvalidOperationException("the mod is unloading");
            case Unavailable when strict:
                throw ServiceCaps.Unavailable(UiWorldCaps.World, "kiln.ui.snapshot is not available here");
            case Revoked:
                return new UiDemandRegistration(false, "Revoked", 0, null);
            default:
                if (strict) throw new InvalidOperationException($"kiln.ui.snapshot: demand_add failed ({rc})");
                return new UiDemandRegistration(false, "Unavailable", 0, null);
        }
        if (r.Ok == 0) return new UiDemandRegistration(false, ReasonName(r.Reason), 0, null);
        var handle = r.Handle;
        return new UiDemandRegistration(true, null, handle, () =>
        {
            var table = T;
            if (table != null) table->DemandRemove(handle);  // NOT_FOUND after the owner was revoked: fine
        });
    }

    // ---- UiWorldSource's defaults (the block runtime): never throw ----
    internal static UiWorldReadResult RuntimeRead()
    {
        UiWorldCaps.Rows.Ensure();
        return Read(OwnOwner) ?? new UiWorldReadResult(false, "Unavailable", null);
    }

    internal static UiDemandRegistration RuntimeDemand(string mod, UiWorldFields fields)
    {
        UiWorldCaps.Rows.Ensure();
        return Demand(OwnerOf(mod), fields, strict: false);
    }

    /// <summary>This game hosts the world it plays (single player, a co-op host): world.server.* can be there.</summary>
    internal static bool Hosting() => Client.IsHosting;

    internal static void ResetForTests()
    {
        lock (Lock)
        {
            _table = null;
            _ownOwner = 0;
        }
    }
}

/// <summary>The rows of the live game values (docs/kiln/services/caps.md): ui.blocks.world (the table is here),
/// world.vitals and world.inventory (and ui.values.hud: 05a's update hook carries the stage), world.position (the
/// native service lowers it when GetEntityTransform did not resolve or faulted), world.host_time (and kiln.time).
/// Reported at first use, as Kiln.Blocks' other rows.</summary>
internal static class UiWorldCaps
{
    public const string World = "ui.blocks.world", Vitals = "world.vitals", Position = "world.position",
        Inventory = "world.inventory", HostTime = "world.host_time";

    private static readonly string[] All = [World, Vitals, Position, Inventory, HostTime];

    internal static readonly ServiceCapRows Rows = new(UiWorldSnapshotNative.Service, Report, everySteps: 50, refreshOnlyWithService: true);

    static UiWorldCaps() => ServiceCaps.WhenFound(UiWorldSnapshotNative.Service, UiWorldSnapshotNative.LookAgain);

    private static void Report()
    {
        if (UiWorldSnapshotNative.T == null)
        {
            var (reason, text) = Client.Available && !Client.IsClient
                ? (CapabilityReason.WrongSide, "live game values are client only (a dedicated server has no local player)")
                : ServiceCaps.Cause(CapabilityReason.ServiceMissing, "kiln.ui.snapshot is not here (an older kiln-core, or no Kiln core)",
                    UiWorldSnapshotNative.Service);
            foreach (var row in All) ServiceCaps.Report(row, CapabilityState.Unavailable, reason, text);
            ServiceCaps.NoteMissing(UiWorldSnapshotNative.Service);
            return;
        }
        ServiceCaps.Report(World, CapabilityState.Available);
        var stage = Capabilities.Is("ui.values.hud");
        const string NoStage = "the player-UI update's pre-provider stage (ui.values.hud) is off";
        ServiceCaps.Static(Vitals, stage, CapabilityReason.ServiceMissing, NoStage, "ui.values.hud");
        ServiceCaps.Static(Inventory, stage, CapabilityReason.ServiceMissing, NoStage, "ui.values.hud");
        ServiceCaps.Report(Position, CapabilityState.Available);  // kiln.ui.snapshot itself lowers it (GetEntityTransform)
        ServiceCaps.Static(HostTime, ServiceCaps.Probe("kiln.time"), CapabilityReason.ServiceMissing, "kiln.time is not here", "kiln.time");
    }

    /// <summary>Throws KilnUnavailableException naming <paramref name="row"/> unless it is available. A row lowered for a
    /// missing table is decided again first (the table may be here now).</summary>
    internal static void Require(string row = World)
    {
        Rows.Ensure();
        if (!Capabilities.Is(row))
        {
            ServiceCaps.LookAgain();
            Rows.Refresh();
        }
        if (!Capabilities.Is(row)) throw ServiceCaps.Unavailable(row);
    }
}

/// <summary>UiWorldSnapshotChangedEvent's producer: started by the event's first subscriber (its static constructor), a
/// client-world Post subscription of Kiln.Blocks' own owner that, while anything listens, looks at the newest publication
/// (at most every 50 ms) and queues one event per new one for the client world's next Pre on the main thread.</summary>
internal static unsafe class UiWorldFeed
{
    private static readonly object Gate = new();
    private static int _started;  // 0 not yet, 1 subscribed, -1 no kiln.client here
    private static ulong _last;
    private static long _lastPollMs = long.MinValue;

    /// <summary>Where a new event goes: the client world's next Pre (tests capture it).</summary>
    internal static Action<KilnEvent> Raise = static e => ClientEvents.Enqueue(e);

    internal static void Ensure()
    {
        if (Volatile.Read(ref _started) != 0) return;
        lock (Gate)
        {
            if (_started != 0) return;
            var c = Services.Get<ClientEvents.Table>("kiln.client", 1);
            ulong handle = 0;
            _started = c != null && c->SubscribeTick(UiWorldSnapshotNative.OwnOwner, 1 /* KILN_TICK_POST */, &OnClientPost, null, &handle) == 0
                ? 1
                : -1;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnClientPost(void* tick, void* user)
    {
        try { Poll(); }
        catch { /* never into the game's step */ }
    }

    /// <summary>One look (the client world's Post, main thread). <paramref name="force"/>: without the 50 ms spacing (tests).</summary>
    internal static void Poll(bool force = false)
    {
        if (!Events.HasSubscribers<UiWorldSnapshotChangedEvent>()) return;
        var now = Environment.TickCount64;
        if (!force && now - Volatile.Read(ref _lastPollMs) < 50) return;
        Volatile.Write(ref _lastPollMs, now);
        var last = Volatile.Read(ref _last);
        if (UiWorldSnapshotNative.Read(UiWorldSnapshotNative.OwnOwner, last) is not { Ok: true, Snapshot: { } snapshot }) return;
        Volatile.Write(ref _last, snapshot.Publication);
        Raise(new UiWorldSnapshotChangedEvent(last, snapshot));
    }

    internal static void ResetForTests()
    {
        lock (Gate)
        {
            _started = 0;
            _last = 0;
            _lastPollMs = long.MinValue;
            Raise = static e => ClientEvents.Enqueue(e);
        }
    }
}
