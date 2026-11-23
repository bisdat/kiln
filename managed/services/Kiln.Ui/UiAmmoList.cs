// SPDX-License-Identifier: MIT
// Ui.AmmoList (0.2.0-dev.30.6, GitHub issue 127): the HUD's ammunition (arrows) and spell lists as lists a mod adds rows to
// or replaces. Native: kiln.ui.ammo (native/core/abi/services/kiln_ui_ammo.h, served by kiln-tools), which never calls
// managed code from a game hook. Kiln.Ui asks the providers on the game's UI thread when a list's context changes (the
// held weapon, the game's own rows and counts, the session; native's game_serial), composes every mod's rows into one
// snapshot per list and publishes it under Kiln.Ui's own owner; the hook reads that copy. A pick of a mod row the game
// cannot equip itself comes back through a queue Kiln.Ui drains each UI frame. docs/kiln/ui.md "The HUD's ammunition and
// spell lists".
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>Which of the HUD's lists (<see cref="Ui.AmmoList"/>). <see cref="Ui.AmmoList.Provide"/> takes several at once
/// (<c>AmmoListKind.Arrow | AmmoListKind.Spell</c>); an <see cref="AmmoListContext"/> and an <see cref="AmmoListPick"/>
/// always name one. Since 0.2.0-dev.30.6 (mod API 0.2.15).</summary>
[Flags]
public enum AmmoListKind
{
    /// <summary>A bow's ammunition list (the arrows the player can switch to).</summary>
    Arrow = 1,
    /// <summary>A staff's or wand's spell list.</summary>
    Spell = 2,
}

/// <summary>How a provider's rows go into the game's list. Since 0.2.0-dev.30.6.</summary>
public enum AmmoListMode
{
    /// <summary>The mod's rows after the game's own rows. A row for an item the game already lists is left out (the
    /// game's row, with its real count, stays).</summary>
    Append = 1,
    /// <summary>Only the mods' rows: the game's own rows are not shown. When one provider of a list asks for Replace, the
    /// whole list is replaced (the rows of every provider of it, in registration order).</summary>
    Replace = 2,
}

/// <summary>A row of an ammunition or spell list: the item and the count shown next to it (a picture: the count is not
/// inventory). Since 0.2.0-dev.30.6.</summary>
public readonly record struct AmmoListRow(ItemId Item, int Count);

/// <summary>How <see cref="Ui.AmmoList.Provide"/> composes a provider's rows. Since 0.2.0-dev.30.6.</summary>
public sealed record AmmoListOptions
{
    /// <summary>After the game's rows (default) or instead of them.</summary>
    public AmmoListMode Mode { get; init; } = AmmoListMode.Append;
}

/// <summary>What a list is built for now: handed to a provider of <see cref="Ui.AmmoList.Provide"/>, which is asked again
/// whenever any of it changes (<see cref="Revision"/>). Since 0.2.0-dev.30.6.</summary>
public sealed class AmmoListContext
{
    internal AmmoListContext(AmmoListKind kind, uint session, EntityId player, ItemId held, IReadOnlyList<AmmoListRow> gameRows,
        ulong revision)
    {
        Kind = kind;
        Session = session;
        Player = player;
        Held = held;
        GameRows = gameRows;
        Revision = revision;
    }

    /// <summary>The list (one kind).</summary>
    public AmmoListKind Kind { get; }
    /// <summary>The client world session the list belongs to (<c>Client.ClientWorldSession</c>).</summary>
    public uint Session { get; }
    /// <summary>The local player.</summary>
    public EntityId Player { get; }
    /// <summary>The weapon the list is built for (the bow, the staff); default when the game did not say.</summary>
    public ItemId Held { get; }
    /// <summary>The game's own rows, in its order: the items it found in the backpack, with their counts.</summary>
    public IReadOnlyList<AmmoListRow> GameRows { get; }
    /// <summary>Changes whenever the held weapon, the game's own rows or counts, or the session change.</summary>
    public ulong Revision { get; }

    /// <summary>Whether the game lists <paramref name="item"/> itself (the player carries it, so a pick of it is the
    /// game's own and raises no <see cref="Ui.AmmoList.Picked"/>).</summary>
    public bool GameLists(ItemId item)
    {
        foreach (var r in GameRows)
            if (r.Item == item) return true;
        return false;
    }
}

/// <summary>The player picked a mod's row the game cannot equip itself (the item is not in the backpack): the game did
/// nothing, so the mod's server half can do the swap. A wish, never authority: the server checks it. Since
/// 0.2.0-dev.30.6.</summary>
public sealed record AmmoListPick(AmmoListKind Kind, ItemId Item, int Count);

/// <summary>A provider of <see cref="Ui.AmmoList.Provide"/>: dispose it to take its rows out (the game's own list shows
/// again once no provider is left). Ends by itself when its mod unloads. Since 0.2.0-dev.30.6.</summary>
public sealed class AmmoListProvider : IDisposable
{
    private int _disposed;
    internal readonly long[] Ids;

    internal AmmoListProvider(AmmoListKind kinds, AmmoListMode mode, long[] ids)
    {
        Kinds = kinds;
        Mode = mode;
        Ids = ids;
    }

    /// <summary>The lists it provides rows for.</summary>
    public AmmoListKind Kinds { get; }
    /// <summary>Its mode.</summary>
    public AmmoListMode Mode { get; }
    /// <summary>False once disposed or its mod unloaded.</summary>
    public bool Active => Volatile.Read(ref _disposed) == 0 && UiAmmoListBridge.Has(Ids[0]);

    /// <summary>Asks for the rows again at the next UI frame: for rows that depend on the mod's own data (what its server
    /// sent), which Kiln cannot see change. Any thread.</summary>
    [AnyThread]
    public void Refresh() => UiAmmoListBridge.Refresh(this);

    /// <summary>Takes the rows out at the next UI frame. Any thread.</summary>
    [AnyThread]
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        UiAmmoListBridge.Remove(this);
    }
}

public static partial class Ui
{
    /// <summary>The HUD's ammunition (arrows) and spell lists: rows a mod adds or puts in place of the game's, and picks
    /// of rows the game cannot equip itself (<see cref="Picked"/>). Client only. Since 0.2.0-dev.30.6 (mod API 0.2.15),
    /// GitHub issue 127.</summary>
    [ClientOnly]
    public static class AmmoList
    {
        /// <summary>The capability row of <see cref="Provide"/>: the game build's list builder is measured.</summary>
        public const string ListCapability = "ui.ammo.list";
        /// <summary>The capability row of <see cref="Picked"/>: the build's pick of a list row is measured, so Kiln can keep
        /// the game from acting on a mod's row and hand the pick to the mod.</summary>
        public const string PickCapability = "ui.ammo.pick";

        /// <summary>Whether <see cref="Provide"/> works on this game build (capability <c>ui.ammo.list</c>);
        /// <c>Capabilities.Why(Ui.AmmoList.ListCapability)</c> says why not.</summary>
        [AnyThread]
        public static bool Available => Capabilities.Is(ListCapability);

        /// <summary>Whether <see cref="Picked"/> works on this game build (capability <c>ui.ammo.pick</c>).</summary>
        [AnyThread]
        public static bool PicksAvailable => Capabilities.Is(PickCapability);

        /// <summary>
        /// Adds rows to the lists in <paramref name="kinds"/> (or puts them in place of the game's rows, <see
        /// cref="AmmoListOptions.Mode"/>) until the returned provider is disposed or the mod unloads.
        /// </summary>
        /// <remarks>
        /// <para><paramref name="rows"/> runs as the mod on the game's UI thread: once per list when it is first shown and
        /// again whenever its <see cref="AmmoListContext"/> changes (the held weapon, the game's rows or counts, the session)
        /// or <see cref="AmmoListProvider.Refresh"/> is called; never inside the game's own code. Kiln copies what it
        /// returns, so the HUD may show the rows of the previous context for one frame. Keep it a lookup.</para>
        /// <para>Rows show in the order returned. Every provider of a list is composed in registration order; the first row
        /// of an item wins (a later row of the same item is left out), rows of item 0 are left out, a negative count shows
        /// as 0, and a list holds at most 64 rows (the rest is left out, and logged). A provider that throws adds no rows
        /// that time (logged in the mod's log). When the game refuses a provider's rows (an item that is not an arrow, or not
        /// a spell, for that list), the other providers' rows still show and the refusal is logged.</para>
        /// <para>The game keeps its selection on the same item when the rows change. A pick of a row whose item the player
        /// carries is the game's own (it equips it); a pick of any other mod row raises <see cref="Picked"/> for the mod that
        /// provided it.</para>
        /// </remarks>
        /// <exception cref="KilnUnavailableException">The row <c>ui.ammo.list</c> is off (an unmeasured game build, a
        /// server, an older kiln-tools).</exception>
        [ClientOnly, AnyThread, RequiresCapability(ListCapability)]
        public static AmmoListProvider Provide(AmmoListKind kinds, Func<AmmoListContext, IReadOnlyList<AmmoListRow>> rows,
            AmmoListOptions? options = null) => UiAmmoListBridge.Provide(kinds, rows, options ?? new AmmoListOptions());

        /// <summary>
        /// One of this mod's rows that the game cannot equip itself (the item is not in the backpack) was selected: the
        /// game did nothing, so the mod's server half can do the swap (send it the pick; the server checks it). Raised on
        /// the game's UI thread at the next UI frame, as the mod, only for rows this mod's providers supplied; picks of an
        /// earlier session are dropped. Subscriptions end when the mod unloads.
        /// </summary>
        /// <remarks>A selection wish, not proof of a click: the player's pick (mouse, keys, the controller's cycling) and
        /// the game's own automatic selection both raise it, e.g. when the weapon's current ammunition is not in a list a
        /// mod replaced and the game selects the first or the kept row. The server validates it (the player owns the item,
        /// the weapon takes it) and ignores repeats.</remarks>
        /// <exception cref="KilnUnavailableException">On subscribe, when the row <c>ui.ammo.pick</c> is off: Kiln cannot
        /// keep the game from acting on the pick on this build (<c>Capabilities.Why(Ui.AmmoList.PickCapability)</c>).</exception>
        [ClientOnly, RequiresCapability(PickCapability)]
        public static event Action<AmmoListPick>? Picked
        {
            add => UiAmmoListBridge.AddPicked(value);
            remove => UiAmmoListBridge.RemovePicked(value);
        }
    }
}

internal static unsafe class UiAmmoNative
{
    public const string Service = "kiln.ui.ammo";
    public const uint KindArrow = 1, KindSpell = 2;
    public const uint ModeGame = 0, ModeAppend = 1, ModeReplace = 2;
    public const int Capacity = 64;
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, Revoked = 6, Busy = 200;
    public const uint ReasonNone = 0, ReasonNoSession = 1, ReasonNoContext = 2, ReasonLimit = 3, ReasonStale = 4, ReasonFault = 5,
        ReasonCatalog = 6;

    /// <summary>KilnUiAmmoRow (16 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Row
    {
        public uint Item, Count, Flags, Key;
    }

    /// <summary>KilnUiAmmoPublish (40 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Publish
    {
        public uint Size, Kind, Mode, Count;
        public uint Session, Reserved;
        public ulong Serial;
        public Row* Rows;
    }

    [InlineArray(Capacity)]
    public struct Rows64
    {
        private Row _first;
    }

    /// <summary>KilnUiAmmoContext (1064 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Context
    {
        public uint Size, Ok, Reason, Kind;
        public uint Session, LocalPlayer, HeldItem, GameCount;
        public ulong GameSerial;
        public Rows64 GameRows;
    }

    /// <summary>KilnUiAmmoPick (32 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Pick
    {
        public uint Size, Kind, Item, Count;
        public uint Key, Session;
        public ulong Step;
    }

    /// <summary>KilnUiAmmoApi v1 (32 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, Publish*, int> Publish;
        public delegate* unmanaged[Cdecl]<ulong, uint, Context*, int> GetContext;
        public delegate* unmanaged[Cdecl]<ulong, Pick*, uint, uint*, int> TakePicks;
    }

    private static Table* _table;
    private static readonly object Lock = new();

    public static Table* T
    {
        get
        {
            if (_table != null) return _table;
            lock (Lock)
            {
                if (_table != null) return _table;
                var t = Services.Get<Table>(Service, 1);
                return _table = t != null && t->Header.Size >= (uint)sizeof(Table) && t->Publish != null && t->GetContext != null &&
                    t->TakePicks != null ? t : null;
            }
        }
    }

    internal static string ReasonText(uint reason) => reason switch
    {
        ReasonNoSession => "no client session",
        ReasonNoContext => "the list is not built (no bow or staff held, or the game did not build its list yet)",
        ReasonLimit => "only the first 64 rows fit",
        ReasonStale => "the list changed since it was read",
        ReasonFault => "the game's list could not be read (see kiln.log)",
        ReasonCatalog => "the client's item catalog is not read yet",
        _ => $"refused ({reason})",
    };

    internal static void ResetForTests()
    {
        lock (Lock) _table = null;
    }
}

internal static unsafe class UiAmmoListBridge
{
    internal sealed class Provider
    {
        public required long Id;
        public required AmmoListKind Kind;  // one kind
        public required AmmoListMode Mode;
        public required Func<AmmoListContext, IReadOnlyList<AmmoListRow>> Fn;
        public ModContext? Mod;
        public bool Warned;
    }

    /// <summary>What Kiln.Ui knows of one list: the snapshot it published and the context it was built for.</summary>
    internal sealed class ListState(AmmoListKind kind)
    {
        public readonly AmmoListKind Kind = kind;
        public bool Dirty;
        public bool Live;               // a mod snapshot is published (not the game's list as is)
        public bool Tried;              // a build was tried for (TriedSerial, TriedSession)
        public ulong TriedSerial;
        public uint TriedSession;
        public uint Session;            // the session of the latest context read (0: none)
        public uint Mode = UiAmmoNative.ModeGame;
        public AmmoListRow[] Rows = [];
        public Dictionary<uint, ModContext?> Owners = new();
        public string? Refusal;         // why the latest build did not publish (null: it did, or none was tried)
    }

    private sealed class PickHandler
    {
        public required Action<AmmoListPick> Fn;
        public ModContext? Mod;
    }

    private static readonly object Lock = new();
    private static readonly object TickLock = new();
    private static Provider[] _providers = [];
    private static PickHandler[] _picked = [];
    private static readonly ListState[] Lists = [new(AmmoListKind.Arrow), new(AmmoListKind.Spell)];
    private static readonly HashSet<ModContext> Watched = new(ReferenceEqualityComparer.Instance);
    private static long _next;
    private static ulong _owner;
    private static ulong _tickHandle;

    /// <summary>The one owner every kiln.ui.ammo call is made with: Kiln.Ui's own (native allows one owner per list).</summary>
    internal static ulong Owner
    {
        get
        {
            if (_owner == 0) _owner = Backend.Current.OwnerCreate("kiln.ui.ammo");
            return _owner;
        }
    }

    internal static ListState StateOf(AmmoListKind kind) => kind == AmmoListKind.Arrow ? Lists[0] : Lists[1];

    private static ListState? StateOf(uint nativeKind) => nativeKind switch
    {
        UiAmmoNative.KindArrow => Lists[0],
        UiAmmoNative.KindSpell => Lists[1],
        _ => null,
    };

    internal static bool Has(long id)
    {
        foreach (var p in Volatile.Read(ref _providers))
            if (p.Id == id) return true;
        return false;
    }

    // ---- registration ----
    internal static AmmoListProvider Provide(AmmoListKind kinds, Func<AmmoListContext, IReadOnlyList<AmmoListRow>> fn, AmmoListOptions options)
    {
        ArgumentNullException.ThrowIfNull(fn);
        ArgumentNullException.ThrowIfNull(options);
        if (kinds == 0 || (kinds & ~(AmmoListKind.Arrow | AmmoListKind.Spell)) != 0)
            throw new ArgumentException("kinds: Arrow, Spell or both", nameof(kinds));
        if (options.Mode is not (AmmoListMode.Append or AmmoListMode.Replace))
            throw new ArgumentException("Mode: Append or Replace", nameof(options));
        UiValueBridge.RequireCapability(Ui.AmmoList.ListCapability);
        if (UiAmmoNative.T == null)
            throw new KilnUnavailableException(Ui.AmmoList.ListCapability, "kiln.ui.ammo is not there (an older kiln-tools)");
        var mod = UiModCalls.Caller ?? ModContext.OwnerOf(fn);
        UiModCalls.RequireAlive(mod);
        var made = new List<Provider>();
        foreach (var kind in (AmmoListKind[])[AmmoListKind.Arrow, AmmoListKind.Spell])
            if ((kinds & kind) != 0)
                made.Add(new Provider { Id = Interlocked.Increment(ref _next), Kind = kind, Mode = options.Mode, Fn = fn, Mod = mod });
        var pub = new AmmoListProvider(kinds, options.Mode, made.Select(p => p.Id).ToArray());
        lock (Lock)
        {
            UiModCalls.RequireAlive(mod);
            _providers = [.. _providers, .. made];
            foreach (var p in made) StateOf(p.Kind).Dirty = true;
        }
        Watch(mod);
        EnsureTick();
        return pub;
    }

    internal static void Remove(AmmoListProvider pub)
    {
        lock (Lock)
        {
            foreach (var p in _providers)
                if (pub.Ids.Contains(p.Id)) StateOf(p.Kind).Dirty = true;
            _providers = _providers.Where(p => !pub.Ids.Contains(p.Id)).ToArray();
        }
    }

    internal static void Refresh(AmmoListProvider pub)
    {
        lock (Lock)
            foreach (var p in _providers)
                if (pub.Ids.Contains(p.Id)) StateOf(p.Kind).Dirty = true;
    }

    internal static void AddPicked(Action<AmmoListPick>? fn)
    {
        if (fn is null) return;
        UiValueBridge.RequireCapability(Ui.AmmoList.PickCapability);
        if (UiAmmoNative.T == null)
            throw new KilnUnavailableException(Ui.AmmoList.PickCapability, "kiln.ui.ammo is not there (an older kiln-tools)");
        var mod = UiModCalls.Caller ?? ModContext.OwnerOf(fn);
        UiModCalls.RequireAlive(mod);
        lock (Lock)
        {
            UiModCalls.RequireAlive(mod);
            _picked = [.. _picked, new PickHandler { Fn = fn, Mod = mod }];
        }
        Watch(mod);
        EnsureTick();
    }

    internal static void RemovePicked(Action<AmmoListPick>? fn)
    {
        if (fn is null) return;
        lock (Lock)
        {
            var i = Array.FindLastIndex(_picked, h => h.Fn == fn);
            if (i >= 0) _picked = _picked.Where((_, k) => k != i).ToArray();
        }
    }

    private static void Watch(ModContext? mod)
    {
        if (mod is null) return;
        lock (Watched)
            if (!Watched.Add(mod)) return;
        mod.Retiring += () => Retire(mod);
        if (!mod.Alive) Retire(mod);
    }

    internal static void Retire(ModContext mod)
    {
        lock (Lock)
        {
            foreach (var p in _providers)
                if (ReferenceEquals(p.Mod, mod)) StateOf(p.Kind).Dirty = true;
            _providers = _providers.Where(p => !ReferenceEquals(p.Mod, mod)).ToArray();
            _picked = _picked.Where(h => !ReferenceEquals(h.Mod, mod)).ToArray();
        }
        lock (Watched) Watched.Remove(mod);
    }

    internal static int Providers => Volatile.Read(ref _providers).Length;
    internal static int PickedHandlers => Volatile.Read(ref _picked).Length;

    // ---- the UI frame ----
    private static void EnsureTick()
    {
        if (Volatile.Read(ref _tickHandle) != 0 || !UiNative.HasGameUi) return;
        lock (Lock)
        {
            if (_tickHandle != 0) return;
            ulong h = 0;
            if (UiNative.T->SubscribeUiTick(0, &OnUiTick, null, &h) == UiNative.Ok) _tickHandle = h;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnUiTick(UiNative.Screen* now, void* user)
    {
        try
        {
            Tick();
        }
        catch (Exception e)
        {
            UiEngine.Log(LogLevel.Error, $"ui.ammo: {e}");
        }
    }

    /// <summary>One UI frame (the game's UI thread): each list whose context changed (or whose providers did) is built
    /// again and published; then the picks are drained and handed to the mods whose rows they were.</summary>
    internal static void Tick()
    {
        var t = UiAmmoNative.T;
        if (t == null) return;
        lock (TickLock)
        {
            foreach (var st in Lists) TickList(t, st);
            DrainPicks(t);
        }
    }

    private static Provider[] ProvidersOf(AmmoListKind kind) => Volatile.Read(ref _providers).Where(p => p.Kind == kind).ToArray();

    private static void TickList(UiAmmoNative.Table* t, ListState st)
    {
        var providers = ProvidersOf(st.Kind);
        bool dirty;
        lock (Lock)
        {
            dirty = st.Dirty;
            st.Dirty = false;
        }
        if (providers.Length == 0)
        {
            st.Session = 0;
            if (st.Live || dirty) Clear(t, st);
            return;
        }
        UiAmmoNative.Context c = default;
        c.Size = (uint)sizeof(UiAmmoNative.Context);
        var rc = t->GetContext(Owner, (uint)st.Kind, &c);
        if (rc != UiAmmoNative.Ok || c.Ok == 0)
        {
            st.Session = 0;
            st.Refusal = rc == UiAmmoNative.Ok ? UiAmmoNative.ReasonText(c.Reason)
                : rc == UiAmmoNative.NotFound ? "the game has not built the list yet" : $"kiln.ui.ammo context refused ({rc})";
            if (dirty) lock (Lock) st.Dirty = true;  // build when there is a context
            return;
        }
        st.Session = c.Session;
        if (!dirty && st.Tried && st.TriedSerial == c.GameSerial && st.TriedSession == c.Session) return;
        st.Tried = true;
        st.TriedSerial = c.GameSerial;
        st.TriedSession = c.Session;
        Build(t, st, providers, &c);
    }

    internal static AmmoListContext ContextOf(AmmoListKind kind, UiAmmoNative.Context* c)
    {
        var n = (int)Math.Min(c->GameCount, (uint)UiAmmoNative.Capacity);
        var rows = new AmmoListRow[n];
        for (var i = 0; i < n; i++)
        {
            var r = c->GameRows[i];
            rows[i] = new AmmoListRow(new ItemId(r.Item), (int)Math.Min(r.Count, int.MaxValue));
        }
        return new AmmoListContext(kind, c->Session, new EntityId(c->LocalPlayer), new ItemId(c->HeldItem), rows, c->GameSerial);
    }

    /// <summary>A provider's rows for one context, as Kiln keeps them: in order, item 0 left out, counts at least 0.</summary>
    private static List<AmmoListRow> Ask(Provider p, AmmoListContext ctx)
    {
        IReadOnlyList<AmmoListRow>? got = null;
        if (!UiModCalls.Invoke(p.Mod, x => got = p.Fn(x), ctx, $"Ui.AmmoList provider ({p.Kind})") || got is null) return [];
        var rows = new List<AmmoListRow>(got.Count);
        foreach (var r in got)
            if (r.Item.Value != 0) rows.Add(r with { Count = Math.Max(0, r.Count) });
        return rows;
    }

    /// <summary>The list a set of providers' rows makes: their rows in registration order, the first row of an item wins,
    /// in Append the game's own items left out, at most 64.</summary>
    internal static (uint Mode, AmmoListRow[] Rows, (uint Item, ModContext? Mod)[] Owners, bool Cut) Compose(
        IReadOnlyList<(Provider P, List<AmmoListRow> Rows)> asked, AmmoListContext ctx)
    {
        var mode = asked.Any(a => a.P.Mode == AmmoListMode.Replace) ? UiAmmoNative.ModeReplace : UiAmmoNative.ModeAppend;
        var seen = new HashSet<uint>();
        if (mode == UiAmmoNative.ModeAppend)
            foreach (var g in ctx.GameRows) seen.Add(g.Item.Value);
        var rows = new List<AmmoListRow>();
        var owners = new List<(uint, ModContext?)>();
        var cut = false;
        foreach (var (p, list) in asked)
            foreach (var r in list)
            {
                if (!seen.Add(r.Item.Value)) continue;
                if (rows.Count == UiAmmoNative.Capacity)
                {
                    cut = true;
                    continue;
                }
                rows.Add(r);
                owners.Add((r.Item.Value, p.Mod));
            }
        return (mode, rows.ToArray(), owners.ToArray(), cut);
    }

    private static void Build(UiAmmoNative.Table* t, ListState st, Provider[] providers, UiAmmoNative.Context* c)
    {
        var ctx = ContextOf(st.Kind, c);
        var asked = providers.Select(p => (P: p, Rows: Ask(p, ctx))).ToList();
        var composed = Compose(asked, ctx);
        if (composed.Cut) UiEngine.Log(LogLevel.Warn, $"ui.ammo: the {st.Kind} list holds 64 rows; the rest are left out");
        var rc = Publish(t, st, c, composed);
        if (rc == UiAmmoNative.InvalidArgument || rc == UiAmmoNative.NotFound)
        {
            if (!Current(t, st, c))
            {
                lock (Lock) st.Dirty = true;  // the list changed meanwhile: build again next frame
                return;
            }
            if (rc == UiAmmoNative.InvalidArgument)
            {
                // the game refused an item (not of this list's kind, or unknown): keep the providers whose rows it takes
                var kept = new List<(Provider P, List<AmmoListRow> Rows)>();
                foreach (var a in asked)
                {
                    kept.Add(a);
                    if (a.Rows.Count == 0 || Publish(t, st, c, Compose(kept, ctx)) == UiAmmoNative.Ok) continue;
                    kept.RemoveAt(kept.Count - 1);
                    var kind = st.Kind.ToString().ToLowerInvariant();
                    UiEngine.Log(LogLevel.Warn,
                        $"ui.ammo: the game refused the {kind} rows of {a.P.Mod?.Info.Id ?? "kiln"} (an item that is not a {kind} item of this game?)");
                    a.P.Mod?.Log.Warn($"Ui.AmmoList: the game refused this provider's {kind} rows (an item that is not a {kind} item of this game?)");
                }
                rc = Publish(t, st, c, Compose(kept, ctx));
            }
        }
        // anything else waits for the next change of the list's context (no retry each frame)
        if (rc != UiAmmoNative.Ok) UiEngine.Log(LogLevel.Warn, $"ui.ammo: the {st.Kind} list was not published: {st.Refusal}");
    }

    // whether the context the rows were built for is still the game's (a refusal then is about the rows)
    private static bool Current(UiAmmoNative.Table* t, ListState st, UiAmmoNative.Context* c)
    {
        UiAmmoNative.Context now = default;
        now.Size = (uint)sizeof(UiAmmoNative.Context);
        return t->GetContext(Owner, (uint)st.Kind, &now) == UiAmmoNative.Ok && now.Ok != 0 && now.GameSerial == c->GameSerial &&
            now.Session == c->Session;
    }

    private static int Publish(UiAmmoNative.Table* t, ListState st, UiAmmoNative.Context* c,
        (uint Mode, AmmoListRow[] Rows, (uint Item, ModContext? Mod)[] Owners, bool Cut) composed)
    {
        var rows = stackalloc UiAmmoNative.Row[UiAmmoNative.Capacity];
        for (var i = 0; i < composed.Rows.Length; i++)
            rows[i] = new UiAmmoNative.Row { Item = composed.Rows[i].Item.Value, Count = (uint)composed.Rows[i].Count, Key = (uint)i };
        UiAmmoNative.Publish p = new()
        {
            Size = (uint)sizeof(UiAmmoNative.Publish), Kind = (uint)st.Kind, Mode = composed.Mode, Count = (uint)composed.Rows.Length,
            Session = c->Session, Serial = c->GameSerial, Rows = rows,
        };
        var rc = t->Publish(Owner, &p);
        if (rc == UiAmmoNative.Ok)
        {
            st.Live = true;
            st.Mode = composed.Mode;
            st.Rows = composed.Rows;
            var owners = new Dictionary<uint, ModContext?>();
            foreach (var (item, mod) in composed.Owners) owners.TryAdd(item, mod);
            st.Owners = owners;
            st.Refusal = null;
        }
        else
            st.Refusal = RefusalText(rc);
        return rc;
    }

    private static string RefusalText(int rc) => rc switch
    {
        UiAmmoNative.InvalidArgument => "the game refused the rows (an item of another kind, or the list changed meanwhile)",
        UiAmmoNative.NotFound => "the game has not built the list yet",
        UiAmmoNative.Unavailable => $"the list is off: {Capabilities.Why(Ui.AmmoList.ListCapability)}",
        UiAmmoNative.Busy => "another owner holds the list",
        UiAmmoNative.Revoked => "Kiln.Ui's owner was retired",
        _ => $"kiln.ui.ammo refused ({rc})",
    };

    // no provider left: the game's own list again
    private static void Clear(UiAmmoNative.Table* t, ListState st)
    {
        UiAmmoNative.Publish p = new() { Size = (uint)sizeof(UiAmmoNative.Publish), Kind = (uint)st.Kind, Mode = UiAmmoNative.ModeGame };
        var rc = t->Publish(Owner, &p);
        if (rc != UiAmmoNative.Ok && st.Live) UiEngine.Log(LogLevel.Warn, $"ui.ammo: clearing the {st.Kind} list failed ({rc})");
        st.Live = false;
        st.Tried = false;
        st.Mode = UiAmmoNative.ModeGame;
        st.Rows = [];
        st.Owners = new();
        st.Refusal = null;
    }

    private static void DrainPicks(UiAmmoNative.Table* t)
    {
        if (!Lists.Any(l => l.Live)) return;  // picks come only from published rows
        var buf = stackalloc UiAmmoNative.Pick[UiAmmoNative.Capacity];
        for (var i = 0; i < UiAmmoNative.Capacity; i++) buf[i] = new UiAmmoNative.Pick { Size = (uint)sizeof(UiAmmoNative.Pick) };
        uint n = 0;
        if (t->TakePicks(Owner, buf, UiAmmoNative.Capacity, &n) != UiAmmoNative.Ok) return;
        var handlers = Volatile.Read(ref _picked);
        for (var i = 0; i < (int)Math.Min(n, (uint)UiAmmoNative.Capacity); i++)
        {
            var p = buf[i];
            if (StateOf(p.Kind) is not { } st || p.Session == 0 || p.Session != st.Session) continue;  // an earlier session's
            if (!st.Owners.TryGetValue(p.Item, out var mod)) continue;  // no provider shows that item now
            var pick = new AmmoListPick(st.Kind, new ItemId(p.Item), (int)Math.Min(p.Count, int.MaxValue));
            foreach (var h in handlers)
                if (ReferenceEquals(h.Mod, mod)) UiModCalls.Invoke(h.Mod, h.Fn, pick, "Ui.AmmoList.Picked");
        }
    }

    internal static void ResetForTests()
    {
        lock (Lock)
        {
            _providers = [];
            _picked = [];
            for (var i = 0; i < Lists.Length; i++) Lists[i] = new ListState(Lists[i].Kind);
            _owner = 0;
            _tickHandle = 0;
        }
        lock (Watched) Watched.Clear();
        UiAmmoNative.ResetForTests();
    }
}
