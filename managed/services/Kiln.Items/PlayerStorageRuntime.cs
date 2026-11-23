// SPDX-License-Identifier: MIT
// The runtime behind PlayerStorage (PlayerStorage.cs): the mods' registrations, kiln.items.storage's public rows
// (StorageNative), its events on the bus, and Kiln.Items' capability rows for player storage. The persist rows are
// Kiln.Saves'. A registration whose filter needs the item catalog (StorageFilter.Where) waits until the server world has
// loaded its items, then compiles once and goes to kiln.items.storage.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

internal static unsafe class PlayerStorageRuntime
{
    private const string PersistCapability = "items.storage.persist";

    private static readonly object Gate = new();
    private static readonly List<PlayerStorage> Registered = [];  // every live registration of a mod, taken or waiting
    private static readonly List<PlayerStorage> Pending = [];     // waiting for the catalog (a Where filter)
    private static readonly HashSet<ModContext> Watched = [];
    private static ulong _after;                                  // the newest kiln.items.storage event handled
    private static IDisposable? _tick;

    /// <summary>The service tick: deferred registrations and the event drain. Started with Kiln.Items (ItemsStartup) and at
    /// the first registration.</summary>
    internal static void Start()
    {
        lock (Gate) _tick ??= ServiceTickAccess.Add("kiln.items player storage", Tick);
    }

    // ---- registration ----

    internal static PlayerStorage Register(string key, int slots, StorageFilter filter, StorageDeath death)
    {
        ArgumentNullException.ThrowIfNull(key);
        if (!PlayerStorage.NameRule.IsMatch(key))
            throw new ArgumentException($"'{key}' is no storage key: 1 to 32 of a-z, 0-9, '.', '_', '-', starting with a letter or digit", nameof(key));
        ArgumentOutOfRangeException.ThrowIfLessThan(slots, 1);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(slots, StorageNative.MaxSlots);
        if (!Enum.IsDefined(death)) throw new ArgumentException($"{(int)death} is no StorageDeath", nameof(death));
        Items.Rows.Ensure();
        var t = StorageNative.T;
        if (t == null) throw ServiceCaps.Unavailable(PlayerStorage.Capability, "kiln.items.storage is not loaded");
        if (!Capabilities.Is(PlayerStorage.Capability)) throw ServiceCaps.Unavailable(PlayerStorage.Capability);
        var mod = ModContext.CurrentMod ??
                  throw new InvalidOperationException("Items.RegisterPlayerStorage must be called from mod code: the storage's key belongs to the mod");
        if (!mod.Alive) throw new InvalidOperationException($"{mod.Info.Id} is unloading");
        var storage = new PlayerStorage(key, mod.Info.Id + ":" + key, slots, filter, death, mod.Owner, mod);
        bool watch;
        lock (Gate)
        {
            if (Registered.Any(s => s.FullKey == storage.FullKey))
                throw new InvalidOperationException($"{storage.FullKey} is registered already (once per key per mod load)");
            if (Registered.Count(s => s.Mod == mod) >= StorageNative.MaxPerMod)
                throw new InvalidOperationException($"{mod.Info.Id} has {StorageNative.MaxPerMod} player storages already, the most a mod may have");
            // a Where filter is evaluated once over the catalog: now when the world has loaded its items and this is the
            // tick thread (the catalog is read there), else at the service tick once it has
            if (filter.NeedsCatalog && (!Game.IsTickThread || Items.All.Count == 0)) Pending.Add(storage);
            else TakeNative(t, storage, throwing: true);
            Registered.Add(storage);
            watch = Watched.Add(mod);
        }
        if (watch) mod.Retiring += () => Retire(mod);
        Start();
        return storage;
    }

    // kiln.items.storage takes the registration; throwing: the caller's mistakes as exceptions, else noted on the storage
    private static bool TakeNative(StorageNative.Table* t, PlayerStorage storage, bool throwing)
    {
        uint categories, flags;
        uint[] ids;
        try
        {
            (categories, flags, ids) = storage.Filter.Compile(storage.Filter.NeedsCatalog ? Items.All : []);
        }
        catch (Exception e) when (!throwing)
        {
            return Refuse(storage, $"its filter: {e.Message}");
        }
        var key = Encoding.UTF8.GetBytes(storage.FullKey);
        int rc;
        uint handle = 0;
        fixed (byte* pk = key)
        fixed (uint* pid = ids)
        {
            var def = new StorageNative.Def
            {
                Size = (uint)sizeof(StorageNative.Def),
                Slots = (uint)storage.Slots,
                Key = new NativeStr { Ptr = pk, Len = (nuint)key.Length },
                Death = (uint)storage.Death,
                Filter = new StorageNative.Filter
                {
                    Size = (uint)sizeof(StorageNative.Filter), Categories = categories, Flags = flags, IdCount = (uint)ids.Length,
                    Ids = ids.Length == 0 ? null : pid,
                },
            };
            rc = t->RegisterStorage(storage.Owner, &def, &handle);
        }
        if (rc == StorageNative.Ok && handle != 0)
        {
            storage.Handle = handle;
            return true;
        }
        var why = rc switch
        {
            StorageNative.InvalidArgument => $"kiln.items.storage refused {storage.FullKey}: another mod or plugin has the key, or an argument is bad",
            StorageNative.Unavailable => $"kiln.items.storage refused {storage.FullKey}: a limit is reached ({StorageNative.MaxPerMod} storages per mod, " +
                                         $"{StorageNative.MaxSlotsPlayer} slots for all storages together, each storage counted in whole 8-slot pages: at most 32 pages)",
            StorageNative.Revoked => $"{storage.Mod?.Info.Id ?? "the mod"} is unloading",
            _ => $"kiln.items.storage refused {storage.FullKey} ({rc})",
        };
        if (!throwing) return Refuse(storage, why);
        throw rc == StorageNative.InvalidArgument ? new ArgumentException(why) : new InvalidOperationException(why);
    }

    private static bool Refuse(PlayerStorage storage, string why)
    {
        storage.Refusal = why;
        Backend.Current.Log(storage.Owner, LogLevel.Error, $"player storage {storage.FullKey} is not registered: {why}");
        return false;
    }

    // A mod unloads: its registrations end here; kiln.items.storage ends them with the mod's owner (its storages' items are
    // parked for players in game).
    private static void Retire(ModContext mod)
    {
        lock (Gate)
        {
            Watched.Remove(mod);
            Registered.RemoveAll(s => s.Mod == mod);
            Pending.RemoveAll(s => s.Mod == mod);
        }
        lock (ReturnGate)
            foreach (var (k, p) in Returns.Where(kv => kv.Value.Mod == mod).ToList())
                Returns.Remove(k);
    }

    // ---- reads ----

    internal static EntityId HolderOf(PlayerStorage storage, EntityId player, string api)
    {
        Services.RequireTickThread(api);
        var t = StorageNative.T;
        if (t == null || storage.Handle == 0 || player.IsNone) return EntityId.None;
        uint holder = 0;
        return t->Holder(storage.Owner, storage.Handle, player.Value, &holder) == StorageNative.Ok ? new EntityId(holder) : EntityId.None;
    }

    internal static PlayerStorageStatus StatusOf(PlayerStorage storage, EntityId player)
    {
        var t = StorageNative.T;
        if (t == null || storage.Refusal is not null) return PlayerStorageStatus.NoneYet;
        if (storage.Handle == 0) return PlayerStorageStatus.NoneYet with { State = PlayerStorageState.Waiting };  // its filter waits for the catalog
        StorageNative.State s = default;
        s.Size = (uint)sizeof(StorageNative.State);
        return t->GetState(storage.Owner, storage.Handle, player.Value, &s) == StorageNative.Ok ? StatusOf(&s) : PlayerStorageStatus.NoneYet;
    }

    private static PlayerStorageStatus StatusOf(StorageNative.State* s) =>
        new(s->Status <= StorageNative.StateLocked ? (PlayerStorageState)s->Status : PlayerStorageState.None, s->Paired != 0,
            s->Source <= StorageNative.SourceClient ? (PlayerStorageSource)s->Source : PlayerStorageSource.None, (int)s->Stacks, (int)s->Parked,
            (int)s->Quarantined, (long)s->Revision);

    internal static IReadOnlyList<PlayerStorageInfo> Of(EntityId player)
    {
        var t = Require("Items.PlayerStorages.Of");
        var cap = 16u;
        while (true)
        {
            var buf = new StorageNative.State[cap];
            uint n = 0;
            int rc;
            fixed (StorageNative.State* p = buf)
            {
                p[0].Size = (uint)sizeof(StorageNative.State);
                rc = t->List(player.Value, p, cap, &n);
                if (rc == StorageNative.Ok)
                {
                    var list = new List<PlayerStorageInfo>((int)n);
                    for (var i = 0; i < n; i++)
                    {
                        // A return's temporary holder is listed for Kiln.Saves' InstanceData tracking, not as another
                        // storage. The ordinary row of its original key remains (permanent PARKED rows have no holder).
                        if (p[i].Status == StorageNative.StateParked && p[i].Handle == 0 && p[i].Holder != 0) continue;
                        list.Add(new PlayerStorageInfo(StorageNative.Text(p[i].Key, StorageNative.KeyMax), p[i].Handle != 0, (int)p[i].Slots,
                            new EntityId(p[i].Holder), StatusOf(&p[i])));
                    }
                    return list;
                }
            }
            if (rc == StorageNative.BufferTooSmall && n > cap && cap < 4096) { cap = n; continue; }
            if (rc == StorageNative.NotFound) return [];
            throw new InvalidOperationException($"Items.PlayerStorages.Of failed ({rc})");
        }
    }

    /// <summary>A player's session ended (left, or the world stopped; null: every player's): Activate forgets the stacks it
    /// moved into their backpack, so the next player in that slot (the same entity id) gets none of them back. Tick
    /// thread.</summary>
    internal static void ForgetActivations(uint? player)
    {
        lock (Gate)
            foreach (var s in Registered)
            {
                if (player is null) s.Activated.Clear();
                else
                    foreach (var key in s.Activated.Keys.Where(k => k.Player == player).ToList()) s.Activated.Remove(key);
            }
    }

    /// <summary>The live registration of <paramref name="fullKey"/> that kiln.items.storage took; null when there is none.</summary>
    internal static PlayerStorage? Find(string fullKey)
    {
        lock (Gate) return Registered.FirstOrDefault(s => s.FullKey == fullKey && s.Handle != 0 && s.Mod is not { Alive: false });
    }

    private static StorageNative.Table* Require(string api)
    {
        Services.RequireTickThread(api);
        Items.Rows.Ensure();
        var t = StorageNative.T;
        if (t == null) throw ServiceCaps.Unavailable(PlayerStorage.Capability, "kiln.items.storage is not loaded");
        if (!Capabilities.Is(PlayerStorage.Capability)) throw ServiceCaps.Unavailable(PlayerStorage.Capability);
        return t;
    }

    // ---- returning parked items ----

    private sealed class PendingReturn(TaskCompletionSource<MoveResult> tcs, ModContext? mod)
    {
        public readonly TaskCompletionSource<MoveResult> Tcs = tcs;
        public readonly ModContext? Mod = mod;
    }

    private static readonly object ReturnGate = new();
    private static readonly Dictionary<long, PendingReturn> Returns = [];
    private static long _nextReturn;

    internal static Task<MoveResult> ReturnOrphans(EntityId player, string fullKey, OrphanTarget target)
    {
        var t = Require("Items.PlayerStorages.ReturnOrphans");
        ArgumentException.ThrowIfNullOrEmpty(fullKey);
        var to = target.IsBackpack ? player : target.Container;
        if (player.IsNone) return Task.FromResult(MoveResult.Fail(ItemsStatus.Invalid, MoveKind.All, player, to, "no player"));
        var mod = ModContext.CurrentMod;
        if (mod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
        var tcs = new TaskCompletionSource<MoveResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        long key;
        lock (ReturnGate)
        {
            key = ++_nextReturn;
            Returns[key] = new PendingReturn(tcs, mod);
        }
        if (mod != null)
        {
            bool watch;
            lock (Gate) watch = Watched.Add(mod);
            if (watch) mod.Retiring += () => Retire(mod);
        }
        var bytes = Encoding.UTF8.GetBytes(fullKey);
        int rc;
        ulong ticket = 0;
        fixed (byte* p = bytes)
            rc = t->ReturnParked(Services.CallerOwner, player.Value, new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, to.Value, &OnReturned,
                (void*)key, &ticket);
        if (rc == StorageNative.Ok) return tcs.Task;
        lock (ReturnGate) Returns.Remove(key);
        var (status, why) = rc switch
        {
            StorageNative.NotFound => (ItemsStatus.NotEnough, $"{player} has no parked items of {fullKey}"),
            StorageNative.InvalidArgument => (ItemsStatus.Invalid, $"refused: '{fullKey}' is no storage key, or {to} is no inventory owner"),
            _ => (ItemsStatus.Unavailable, "player storage cannot return items here: " + (Status()?.Detail is { Length: > 0 } d ? d : $"({rc})")),
        };
        return Task.FromResult(MoveResult.Fail(status, MoveKind.All, player, to, why));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnReturned(StorageNative.ReturnResult* r, void* user)
    {
        try
        {
            PendingReturn? p;
            lock (ReturnGate)
                if (!Returns.Remove((long)user, out p)) return;
            var status = Enum.IsDefined(typeof(ItemsStatus), (int)r->Status) ? (ItemsStatus)r->Status : ItemsStatus.Refused;
            var message = StorageNative.Text(r->Message, 160);
            if (message.Length == 0 && r->Kept > 0) message = $"{r->Kept} stack(s) stay parked (no room)";
            p.Tcs.TrySetResult(new MoveResult(status, MoveKind.All, new EntityId(r->Player), new EntityId(r->Target), (int)r->Requested,
                (int)r->Done, (int)r->Stacks, [], message, 0, false, (int)r->Steps, false));
        }
        catch
        {
            // never let an exception cross into native code
        }
    }

    private static (string Detail, bool Storage)? Status()
    {
        var t = StorageNative.T;
        if (t == null) return null;
        StorageNative.Status s = default;
        s.Size = (uint)sizeof(StorageNative.Status);
        return t->GetStatus(&s) == StorageNative.Ok ? (StorageNative.Text(s.Detail, 160), s.Storage != 0) : null;
    }

    // ---- the service tick ----

    private static void Tick(ulong step)
    {
        var t = StorageNative.T;
        if (t == null) return;
        TakePending(t);
        Drain(t);
    }

    private static void TakePending(StorageNative.Table* t)
    {
        List<PlayerStorage> ready;
        lock (Gate)
        {
            if (Pending.Count == 0) return;
            if (Items.All.Count == 0) return;  // the world has not loaded its items yet
            ready = [.. Pending];
            Pending.Clear();
        }
        foreach (var storage in ready)
            if (storage.Mod is not { Alive: false })
                TakeNative(t, storage, throwing: false);
    }

    // kiln.items.storage's events on the bus: restored, parked, death (the persistence kinds are Kiln.Saves')
    private static void Drain(StorageNative.Table* t)
    {
        StorageNative.Event e = default;
        for (var i = 0; i < 256; i++)
        {
            e.Size = (uint)sizeof(StorageNative.Event);
            if (t->NextEvent(_after, &e) != StorageNative.Ok) return;
            _after = e.Seq;
            try
            {
                Post(&e);
            }
            catch (Exception x)
            {
                Backend.Current.Log(0, LogLevel.Error, $"kiln.items: posting a player storage event failed: {x.Message}");
            }
        }
    }

    private static void Post(StorageNative.Event* e)
    {
        // Temporary or retired holders without a public registration post housekeeping for Kiln.Saves. Keep those
        // events in the raw queue, not on the public bus (ordinary orphan PARKED without a holder has target 0).
        if (e->Handle == 0 && e->Target != 0 && e->Kind is StorageNative.EventRestored or StorageNative.EventParked) return;
        var player = new EntityId(e->Player);
        switch (e->Kind)
        {
            case StorageNative.EventRestored when Events.HasSubscribers<PlayerStorageRestoredEvent>():
                Events.Post(new PlayerStorageRestoredEvent(player, StorageNative.CharacterOf(e->Character), StorageNative.Text(e->Key, StorageNative.KeyMax),
                    (int)e->Stacks, (e->Flags & StorageNative.EventPaired) != 0,
                    e->Source <= StorageNative.SourceClient ? (PlayerStorageSource)e->Source : PlayerStorageSource.None,
                    (e->Flags & StorageNative.EventLive) != 0, (long)e->Revision));
                break;
            case StorageNative.EventParked when Events.HasSubscribers<PlayerStorageParkedEvent>():
                Events.Post(new PlayerStorageParkedEvent(player, StorageNative.CharacterOf(e->Character), StorageNative.Text(e->Key, StorageNative.KeyMax),
                    (int)e->Stacks, (PlayerStorageParkReason)e->Reason));
                break;
            case StorageNative.EventDeath when Events.HasSubscribers<PlayerStorageDeathEvent>():
                Events.Post(new PlayerStorageDeathEvent(player, StorageNative.Text(e->Key, StorageNative.KeyMax), (int)e->Stacks, (int)e->Items,
                    new EntityId(e->Target), e->Reason <= StorageNative.DeathFollowGame ? (StorageDeath)e->Reason : StorageDeath.Keep));
                break;
        }
    }

    // ---- capability rows ----

    /// <summary>Kiln.Items' rows for player storage (ItemsCapabilities' start): the client copy follows the kiln:storage
    /// channel (PlayerStorageMirror); without the kiln.items.storage table the sigdb feature and the persist row are
    /// lowered.</summary>
    internal static void ReportCapabilities()
    {
        PlayerStorageMirror.ReportCapability();
        if (StorageNative.T != null) return;
        var (reason, text) = ServiceCaps.Cause(CapabilityReason.ServiceMissing, "kiln.items.storage is not loaded", StorageNative.Service);
        ServiceCaps.Lower(PlayerStorage.Capability, reason, text);
        ServiceCaps.Lower(PersistCapability, reason, text);
    }

    /// <summary>Tests: a new harness starts with no registrations and reads the event queue from its start.</summary>
    internal static void ResetForTests()
    {
        lock (Gate)
        {
            Registered.Clear();
            Pending.Clear();
            Watched.Clear();
            _after = 0;
        }
        lock (ReturnGate) Returns.Clear();
        PlayerStorageMirror.ResetForTests();
        PlayerStorageAmmo.ResetForTests();
    }
}
