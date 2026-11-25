// SPDX-License-Identifier: MIT
// Ui.AmmoList (0.2.0-dev.30.6, GitHub issue 127) against a fake kiln.ui.ammo table (kiln_ui_ammo.h): providers composed
// into one snapshot per list (registration order, first row of an item wins, Replace wins, Append keeps a mod's row of a
// game item behind the game's row without room, 64 rows), published under one owner with the context's serial and session; nothing asked again until the
// context changes; dispose and a mod's retirement publish the game's list again; picks routed to the mod whose row it was
// and old-session picks dropped; the error rule (ui.ammo.list, ui.ammo.pick: display only). The hook, the selection
// by item and the pick seam are native's (native/services/tools/tests).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln;
using Kiln.FakeUi;
using Xunit;

namespace UiAmmoListTestSuite;

internal static unsafe class FakeAmmo
{
    public static UiAmmoNative.Table* Table;
    public static readonly List<(ulong Owner, UiAmmoNative.Publish P, UiAmmoNative.Row[] Rows)> Published = [];
    public static readonly List<uint> ContextReads = [];
    public static readonly Dictionary<uint, UiAmmoNative.Context> Contexts = [];
    public static readonly Queue<UiAmmoNative.Pick> Picks = new();
    public static int ContextRc;
    public static uint ContextReason;  // with ContextRc: the diagnostic native writes with ok 0
    public static HashSet<uint> Refused = [];  // items the game refuses (not of the list's kind)
    public static int PublishRc;
    public static int RefuseSetsFromCall;  // 0: off; else the n-th publish call on refuses every set

    public static void Install()
    {
        Free();
        Table = (UiAmmoNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(UiAmmoNative.Table));
        Table->Header = new ServiceHeader { Size = (uint)sizeof(UiAmmoNative.Table), Version = 1 };
        Table->Publish = &Publish;
        Table->GetContext = &GetContext;
        Table->TakePicks = &TakePicks;
        Published.Clear();
        ContextReads.Clear();
        Contexts.Clear();
        Picks.Clear();
        ContextRc = 0;
        ContextReason = 0;
        PublishRc = 0;
        RefuseSetsFromCall = 0;
        Refused = [];
    }

    public static void Free()
    {
        if (Table != null) NativeMemory.Free(Table);
        Table = null;
    }

    /// <summary>The game builds list <paramref name="kind"/> for a held weapon with these rows of its own.</summary>
    public static void Context(uint kind, ulong serial, uint session = 1, uint held = 0x50, params (uint Item, uint Count)[] game)
    {
        var c = new UiAmmoNative.Context
        {
            Size = (uint)sizeof(UiAmmoNative.Context), Ok = 1, Kind = kind, Session = session, LocalPlayer = 77, HeldItem = held,
            GameCount = (uint)game.Length, GameSerial = serial,
        };
        for (var i = 0; i < game.Length; i++) c.GameRows[i] = new UiAmmoNative.Row { Item = game[i].Item, Count = game[i].Count, Key = (uint)i };
        Contexts[kind] = c;
    }

    public static (uint Mode, (uint Item, uint Count)[] Rows) Last(uint kind)
    {
        var (_, p, rows) = Published.Last(x => x.P.Kind == kind);
        return (p.Mode, rows.Select(r => (r.Item, r.Count)).ToArray());
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Publish(ulong owner, UiAmmoNative.Publish* p)
    {
        var rows = new UiAmmoNative.Row[p->Count];
        for (var i = 0; i < rows.Length; i++) rows[i] = p->Rows[i];
        Published.Add((owner, *p, rows));
        if (PublishRc != 0) return PublishRc;
        if (p->Mode == UiAmmoNative.ModeGame) return 0;
        if (RefuseSetsFromCall != 0 && Published.Count >= RefuseSetsFromCall) return UiAmmoNative.InvalidArgument;
        if (!Contexts.TryGetValue(p->Kind, out var c)) return UiAmmoNative.NotFound;
        if (c.GameSerial != p->Serial || c.Session != p->Session || rows.Any(r => Refused.Contains(r.Item))) return UiAmmoNative.InvalidArgument;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GetContext(ulong owner, uint kind, UiAmmoNative.Context* o)
    {
        ContextReads.Add(kind);
        if (ContextRc != 0)
        {
            *o = new UiAmmoNative.Context { Size = (uint)sizeof(UiAmmoNative.Context), Kind = kind, Reason = ContextReason };
            return ContextRc;
        }
        if (!Contexts.TryGetValue(kind, out var c)) return UiAmmoNative.NotFound;
        *o = c;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int TakePicks(ulong owner, UiAmmoNative.Pick* o, uint capacity, uint* written)
    {
        uint n = 0;
        while (n < capacity && Picks.Count > 0) o[n++] = Picks.Dequeue();
        *written = n;
        return 0;
    }

    public sealed class Backend(HashSet<string> features, bool withTable = true) : IKilnBackend
    {
        public string Version => "test";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => features.Contains(name);
        public void Log(ulong owner, LogLevel level, string text) { }
        public nint Service(string name, uint minVersion) => name switch
        {
            UiAmmoNative.Service => withTable ? (nint)Table : 0,
            _ => 0,
        };
        public ulong OwnerCreate(string id) => id == "kiln.ui.ammo" ? 4242UL : 99UL;
    }
}

[Collection(UiSerial.Name)]
public sealed unsafe class AmmoListTests : IDisposable
{
    private const uint Arrow = UiAmmoNative.KindArrow, Spell = UiAmmoNative.KindSpell;
    private readonly IKilnBackend _previous;
    private static readonly HashSet<string> Rows = [Ui.AmmoList.ListCapability, Ui.AmmoList.PickCapability];

    public AmmoListTests()
    {
        _previous = Backend.Current;
        FakeAmmo.Install();
        Use(new FakeAmmo.Backend(Rows));
    }

    private static void Use(IKilnBackend b)
    {
        Backend.Current = b;
        UiNative.ResetForTests();
        UiAmmoListBridge.ResetForTests();
    }

    public void Dispose()
    {
        UiAmmoListBridge.ResetForTests();
        UiNative.ResetForTests();
        Backend.Current = _previous;
        FakeAmmo.Free();
    }

    private static AmmoListRow R(uint item, int count) => new(new ItemId(item), count);

    private static ModContext Mod(string id, ulong owner) =>
        HostAccess.CreateContext(new ModInfo(id, id, "1.0.0", Path.GetTempPath()), owner, Path.GetTempPath());

    private static T As<T>(ModContext mod, Func<T> f)
    {
        var previous = HostAccess.Current;
        HostAccess.Current = mod;
        try { return f(); }
        finally { HostAccess.Current = previous; }
    }

    [Fact]
    public void The_table_mirrors_the_header()
    {
        Assert.Equal((16, 40, 1064, 32, 32), (sizeof(UiAmmoNative.Row), sizeof(UiAmmoNative.Publish), sizeof(UiAmmoNative.Context),
            sizeof(UiAmmoNative.Pick), sizeof(UiAmmoNative.Table)));
        Assert.Equal(1064 - 64 * 16, (int)Marshal.OffsetOf<UiAmmoNative.Context>(nameof(UiAmmoNative.Context.GameRows)));
        Assert.Equal(32, (int)Marshal.OffsetOf<UiAmmoNative.Publish>(nameof(UiAmmoNative.Publish.Rows)));
        Assert.Equal(24, (int)Marshal.OffsetOf<UiAmmoNative.Pick>(nameof(UiAmmoNative.Pick.Step)));
    }

    [Fact]
    public void Appended_rows_follow_in_the_providers_order_with_the_contexts_serial_and_session()
    {
        FakeAmmo.Context(Arrow, serial: 7, session: 3, game: (0x10, 20));
        AmmoListContext? seen = null;
        using var p = Ui.AmmoList.Provide(AmmoListKind.Arrow, c => { seen = c; return [R(0x30, 5), R(0x20, 9)]; });
        UiAmmoListBridge.Tick();
        var (owner, pub, _) = Assert.Single(FakeAmmo.Published);
        Assert.Equal(4242UL, owner);  // Kiln.Ui's own owner, never a mod's
        Assert.Equal((Arrow, UiAmmoNative.ModeAppend, 7UL, 3u), (pub.Kind, pub.Mode, pub.Serial, pub.Session));
        Assert.Equal([(0x30u, 5u), (0x20u, 9u)], FakeAmmo.Last(Arrow).Rows);
        Assert.NotNull(seen);
        Assert.Equal((AmmoListKind.Arrow, 3u, new EntityId(77), new ItemId(0x50), 7UL), (seen.Kind, seen.Session, seen.Player, seen.Held, seen.Revision));
        Assert.Equal([R(0x10, 20)], seen.GameRows);
        Assert.True(seen.GameLists(new ItemId(0x10)));
        Assert.True(p.Active);
    }

    [Fact]
    public void Append_keeps_a_mods_row_of_a_game_item_behind_the_games_row_and_the_first_row_of_an_item_wins()
    {
        // GitHub issue 145: the mod's row of an item the game lists with stock is published (native shows the game's row and
        // count in its place), so a pick of it after its stack left the backpack is known as the mod's row.
        FakeAmmo.Context(Arrow, 1, game: (0x10, 20));
        using var a = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x10, 99), R(0x30, 1), R(0, 4), R(0x31, -5)]);
        using var b = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x30, 2), R(0x32, 3), R(0x10, 1)]);
        UiAmmoListBridge.Tick();
        Assert.Equal([(0x10u, 99u), (0x30u, 1u), (0x31u, 0u), (0x32u, 3u)], FakeAmmo.Last(Arrow).Rows);
        Assert.Equal([0x10u, 0x30u, 0x31u, 0x32u], UiAmmoListBridge.StateOf(AmmoListKind.Arrow).Keys.Select(k => k.Item));
    }

    [Fact]
    public void In_Append_a_mods_row_behind_a_game_row_with_stock_takes_no_room()
    {
        // the game's 63 rows leave one: the mod's rows of its items take none of it, so the next new item still fits
        FakeAmmo.Context(Arrow, 1, game: Enumerable.Range(1, 63).Select(i => ((uint)i, 1u)).ToArray());
        using var p = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(5, 7), R(0x100, 1), R(63, 2), R(0x101, 1)]);
        UiAmmoListBridge.Tick();
        Assert.Equal([(5u, 7u), (0x100u, 1u), (63u, 2u)], FakeAmmo.Last(Arrow).Rows);
        // the game's 64 leave none: still its items' rows, no new one
        FakeAmmo.Context(Arrow, 2, game: Enumerable.Range(1, 64).Select(i => ((uint)i, 1u)).ToArray());
        UiAmmoListBridge.Tick();
        Assert.Equal([(5u, 7u), (63u, 2u)], FakeAmmo.Last(Arrow).Rows);
    }

    [Fact]
    public void In_Append_a_pick_of_a_mods_row_whose_stack_just_left_the_backpack_reaches_that_mod()
    {
        // GitHub issue 145 in Append: the book lists the equipped 0x41 (in the backpack, so the game lists it with stock)
        // and 0x40. Its server half swaps 0x40 in and 0x41 out; native finds no stack of 0x41 at the pick, before and after
        // the game's rebuild, and queues it under the key of the book's row. Kiln.Ui hands it to the book.
        FakeAmmo.Context(Spell, 1, game: (0x41, 1));
        var book = Mod("book", 75);
        var other = Mod("other", 76);
        var toBook = new List<AmmoListPick>();
        var toOther = new List<AmmoListPick>();
        using var p = As(book, () => Ui.AmmoList.Provide(AmmoListKind.Spell, c => c.Revision == 1 ? [R(0x41, 1), R(0x40, 1)] : [R(0x41, 1)]));
        using var q = As(other, () => Ui.AmmoList.Provide(AmmoListKind.Spell, _ => [R(0x41, 5)]));
        As(book, () => { Ui.AmmoList.Picked += toBook.Add; return 0; });
        As(other, () => { Ui.AmmoList.Picked += toOther.Add; return 0; });
        UiAmmoListBridge.Tick();
        var (mode, rows) = FakeAmmo.Last(Spell);
        Assert.Equal(UiAmmoNative.ModeAppend, mode);
        Assert.Equal([(0x41u, 1u), (0x40u, 1u)], rows);
        FakeAmmo.Picks.Enqueue(new UiAmmoNative.Pick { Size = 32, Kind = Spell, Item = 0x41, Count = 1, Key = 0, Session = 1 });
        UiAmmoListBridge.Tick();
        Assert.Equal([new AmmoListPick(AmmoListKind.Spell, new ItemId(0x41), 1)], toBook);
        // the game's rebuild keeps 0x41 at count 0 and lists 0x40 with stock: both rows go behind the game's
        FakeAmmo.Context(Spell, 2, game: [(0x41, 0), (0x40, 1)]);
        UiAmmoListBridge.Tick();
        Assert.Equal([(0x41u, 1u)], FakeAmmo.Last(Spell).Rows);
        FakeAmmo.Picks.Enqueue(new UiAmmoNative.Pick { Size = 32, Kind = Spell, Item = 0x41, Count = 1, Key = 0, Session = 1 });
        UiAmmoListBridge.Tick();
        Assert.Equal(2, toBook.Count);
        Assert.Empty(toOther);
        book.Retire();
        other.Retire();
    }

    [Fact]
    public void In_Append_a_game_row_at_count_0_leaves_the_mods_row_in_and_takes_no_room()
    {
        // GitHub issue 131: the game keeps the row of a used-up stack at count 0. The mod's row of its item is published
        // (since 0.2.0-dev.31.12 also of an item with stock, GitHub issue 145); native shows the game's row and sends its
        // pick to the mod when the backpack does not hold it.
        FakeAmmo.Context(Arrow, 1, game: [(0x10, 0), (0x11, 4)]);
        using var p = Ui.AmmoList.Provide(AmmoListKind.Arrow, c => c.Revision == 1
            ? [R(0x10, 7), R(0x11, 2), R(0x30, 1)]
            : [R(1, 7), R(0x100, 1), R(0x101, 1)]);
        UiAmmoListBridge.Tick();
        Assert.Equal([(0x10u, 7u), (0x11u, 2u), (0x30u, 1u)], FakeAmmo.Last(Arrow).Rows);
        Assert.Contains(0x10u, UiAmmoListBridge.StateOf(AmmoListKind.Arrow).Keys.Select(k => k.Item));
        // the game's 63 rows leave one: its count-0 item shows as the game's row, so the next item still fits
        FakeAmmo.Context(Arrow, 2, game: Enumerable.Range(1, 63).Select(i => ((uint)i, i == 1 ? 0u : 1u)).ToArray());
        UiAmmoListBridge.Tick();
        Assert.Equal([(1u, 7u), (0x100u, 1u)], FakeAmmo.Last(Arrow).Rows);
    }

    [Fact]
    public void Replace_wins_for_the_whole_list_and_keeps_registration_order()
    {
        FakeAmmo.Context(Spell, 1, game: (0x10, 1));
        using var a = Ui.AmmoList.Provide(AmmoListKind.Spell, _ => [R(0x10, 1), R(0x40, 1)]);
        using var b = Ui.AmmoList.Provide(AmmoListKind.Spell, _ => [R(0x41, 1)], new AmmoListOptions { Mode = AmmoListMode.Replace });
        UiAmmoListBridge.Tick();
        var (mode, rows) = FakeAmmo.Last(Spell);
        Assert.Equal(UiAmmoNative.ModeReplace, mode);
        Assert.Equal([(0x10u, 1u), (0x40u, 1u), (0x41u, 1u)], rows);  // in Replace the game's item is a mod row like any
    }

    [Fact]
    public void Both_kinds_are_one_provider_with_one_call_per_list()
    {
        FakeAmmo.Context(Arrow, 1);
        FakeAmmo.Context(Spell, 1);
        var kinds = new List<AmmoListKind>();
        using var p = Ui.AmmoList.Provide(AmmoListKind.Arrow | AmmoListKind.Spell, c => { kinds.Add(c.Kind); return [R(c.Kind == AmmoListKind.Arrow ? 0x30u : 0x40u, 1)]; });
        UiAmmoListBridge.Tick();
        Assert.Equal([AmmoListKind.Arrow, AmmoListKind.Spell], kinds);
        Assert.Equal([(0x30u, 1u)], FakeAmmo.Last(Arrow).Rows);
        Assert.Equal([(0x40u, 1u)], FakeAmmo.Last(Spell).Rows);
        Assert.Equal(AmmoListKind.Arrow | AmmoListKind.Spell, p.Kinds);
    }

    [Fact]
    public void Nothing_is_asked_again_until_the_context_changes_or_the_mod_refreshes()
    {
        FakeAmmo.Context(Arrow, 1);
        var calls = 0;
        using var p = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => { calls++; return [R(0x30, calls)]; });
        UiAmmoListBridge.Tick();
        UiAmmoListBridge.Tick();
        UiAmmoListBridge.Tick();
        Assert.Equal((1, 1), (calls, FakeAmmo.Published.Count));
        FakeAmmo.Context(Arrow, 2);                 // the inventory or the held weapon changed
        UiAmmoListBridge.Tick();
        Assert.Equal(2, calls);
        FakeAmmo.Context(Arrow, 2, session: 2);     // a new session, the same serial
        UiAmmoListBridge.Tick();
        Assert.Equal(3, calls);
        p.Refresh();
        UiAmmoListBridge.Tick();
        Assert.Equal(4, calls);
        Assert.Equal([(0x30u, 4u)], FakeAmmo.Last(Arrow).Rows);
    }

    [Fact]
    public void Without_a_context_no_provider_runs_and_the_list_is_built_once_there_is_one()
    {
        var calls = 0;
        using var p = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => { calls++; return [R(0x30, 1)]; });
        UiAmmoListBridge.Tick();
        Assert.Equal(0, calls);
        Assert.Empty(FakeAmmo.Published);
        Assert.Contains("not built the list", UiAmmoListBridge.StateOf(AmmoListKind.Arrow).Refusal);
        FakeAmmo.Context(Arrow, 5);
        UiAmmoListBridge.Tick();
        Assert.Equal(1, calls);
    }

    [Fact]
    public void A_missing_context_says_natives_reason_and_a_truncated_one_still_builds()
    {
        var calls = 0;
        AmmoListContext? seen = null;
        using var p = Ui.AmmoList.Provide(AmmoListKind.Arrow, x => { calls++; seen = x; return [R(0x30, 1)]; });
        FakeAmmo.ContextRc = UiAmmoNative.NotFound;
        FakeAmmo.ContextReason = UiAmmoNative.ReasonNoSession;  // NO_SESSION, NO_CONTEXT, STALE: NOT_FOUND with ok 0
        UiAmmoListBridge.Tick();
        Assert.Equal("no client session", UiAmmoListBridge.StateOf(AmmoListKind.Arrow).Refusal);
        FakeAmmo.ContextRc = 0;
        FakeAmmo.Context(Arrow, 3);
        var c = FakeAmmo.Contexts[Arrow];
        c.Reason = UiAmmoNative.ReasonLimit;  // ok 1 with LIMIT: the game's first 64 rows, a usable context
        FakeAmmo.Contexts[Arrow] = c;
        UiAmmoListBridge.Tick();
        Assert.Equal(1, calls);
        Assert.True(seen!.Truncated);  // the provider sees that rows were cut at 64 (LIMIT)
        // LIMIT alone (e.g. left from an earlier overfull list) takes no room: the room follows the game's rows seen
        Assert.Equal([(0x30u, 1u)], FakeAmmo.Last(Arrow).Rows);
    }

    [Fact]
    public void In_Append_the_mods_rows_fit_in_what_the_games_rows_leave_of_64()
    {
        FakeAmmo.Context(Arrow, 1, game: Enumerable.Range(1, 63).Select(i => ((uint)i, 1u)).ToArray());
        AmmoListContext? seen = null;
        using var p = Ui.AmmoList.Provide(AmmoListKind.Arrow, x => { seen = x; return [R(0x100, 1), R(0x101, 1)]; });
        UiAmmoListBridge.Tick();
        Assert.False(seen!.Truncated);
        Assert.Equal([(0x100u, 1u)], FakeAmmo.Last(Arrow).Rows);  // 63 + 1: what the HUD shows
        Assert.Equal([0x100u], UiAmmoListBridge.StateOf(AmmoListKind.Arrow).Keys.Select(k => k.Item));
        // the game's 64 (it had more: LIMIT) leave none; Replace still holds 64 of the mods'
        FakeAmmo.Context(Arrow, 2, game: Enumerable.Range(1, 64).Select(i => ((uint)i, 1u)).ToArray());
        var c = FakeAmmo.Contexts[Arrow];
        c.Reason = UiAmmoNative.ReasonLimit;
        FakeAmmo.Contexts[Arrow] = c;
        UiAmmoListBridge.Tick();
        Assert.True(seen!.Truncated);
        Assert.Empty(FakeAmmo.Last(Arrow).Rows);
        using var r = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => Enumerable.Range(0x200, 70).Select(i => R((uint)i, 1)).ToArray(),
            new AmmoListOptions { Mode = AmmoListMode.Replace });
        UiAmmoListBridge.Tick();
        Assert.Equal((UiAmmoNative.ModeReplace, 64), (FakeAmmo.Last(Arrow).Mode, FakeAmmo.Last(Arrow).Rows.Length));
    }

    [Fact]
    public void A_stale_publish_is_built_again_with_the_new_context()
    {
        FakeAmmo.Context(Arrow, 1);
        var calls = 0;
        using var p = Ui.AmmoList.Provide(AmmoListKind.Arrow, c =>
        {
            calls++;
            if (calls == 1) FakeAmmo.Context(Arrow, 2);  // the list changes while the rows are built
            return [R(0x30, (int)c.Revision)];
        });
        UiAmmoListBridge.Tick();
        UiAmmoListBridge.Tick();
        Assert.Equal(2, calls);
        Assert.Equal([(0x30u, 2u)], FakeAmmo.Last(Arrow).Rows);
        Assert.Null(UiAmmoListBridge.StateOf(AmmoListKind.Arrow).Refusal);
    }

    [Fact]
    public void Rows_the_game_refuses_drop_only_their_provider()
    {
        FakeAmmo.Context(Arrow, 1);
        FakeAmmo.Refused = [0x99];  // e.g. a spell item in the arrow list
        using var good = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x30, 1)]);
        using var bad = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x99, 1), R(0x31, 1)]);
        using var later = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x32, 1)]);
        UiAmmoListBridge.Tick();
        Assert.Equal([(0x30u, 1u), (0x32u, 1u)], FakeAmmo.Last(Arrow).Rows);
        Assert.Null(UiAmmoListBridge.StateOf(AmmoListKind.Arrow).Refusal);
        var n = FakeAmmo.Published.Count;
        UiAmmoListBridge.Tick();
        Assert.Equal(n, FakeAmmo.Published.Count);  // not tried again each frame
    }

    [Fact]
    public void A_part_taken_on_the_way_does_not_stay_up_when_the_whole_list_is_refused()
    {
        FakeAmmo.Context(Arrow, 1);
        FakeAmmo.Refused = [0x99];
        FakeAmmo.RefuseSetsFromCall = 4;  // 1 all: refused, 2 the first provider: taken, 3 with the second: refused, 4 final: refused
        using var good = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x30, 1)]);
        using var bad = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x99, 1)]);
        UiAmmoListBridge.Tick();
        Assert.Equal(5, FakeAmmo.Published.Count);
        Assert.Equal(UiAmmoNative.ModeGame, FakeAmmo.Published[^1].P.Mode);  // the part taken in call 2 is cleared
        Assert.False(UiAmmoListBridge.StateOf(AmmoListKind.Arrow).Live);
        Assert.Contains("refused", UiAmmoListBridge.StateOf(AmmoListKind.Arrow).Refusal);
        UiAmmoListBridge.Tick();
        Assert.Equal(5, FakeAmmo.Published.Count);  // not tried again until the context changes
    }

    [Fact]
    public void A_throwing_provider_adds_no_rows_and_the_others_still_show()
    {
        FakeAmmo.Context(Arrow, 1);
        using var a = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => throw new InvalidOperationException("boom"));
        using var b = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x30, 1)]);
        UiAmmoListBridge.Tick();
        Assert.Equal([(0x30u, 1u)], FakeAmmo.Last(Arrow).Rows);
    }

    [Fact]
    public void A_list_holds_64_rows()
    {
        FakeAmmo.Context(Arrow, 1);
        using var a = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => Enumerable.Range(1, 70).Select(i => R((uint)i, 1)).ToArray());
        UiAmmoListBridge.Tick();
        var rows = FakeAmmo.Last(Arrow).Rows;
        Assert.Equal(64, rows.Length);
        Assert.Equal(64u, rows[^1].Item);
    }

    [Fact]
    public void Disposing_the_last_provider_shows_the_games_list_again()
    {
        FakeAmmo.Context(Arrow, 1);
        var a = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x30, 1)]);
        var b = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x31, 1)]);
        UiAmmoListBridge.Tick();
        a.Dispose();
        Assert.False(a.Active);
        UiAmmoListBridge.Tick();
        Assert.Equal([(0x31u, 1u)], FakeAmmo.Last(Arrow).Rows);
        b.Dispose();
        b.Dispose();
        UiAmmoListBridge.Tick();
        Assert.Equal((UiAmmoNative.ModeGame, 0), (FakeAmmo.Last(Arrow).Mode, FakeAmmo.Last(Arrow).Rows.Length));
        var n = FakeAmmo.Published.Count;
        UiAmmoListBridge.Tick();
        Assert.Equal(n, FakeAmmo.Published.Count);
        Assert.Equal(0, UiAmmoListBridge.Providers);
    }

    [Fact]
    public void A_retiring_mod_takes_its_rows_and_pick_handlers_with_it()
    {
        FakeAmmo.Context(Arrow, 1);
        var mod = Mod("quiver", 72);
        var p = As(mod, () => Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x30, 1)]));
        As(mod, () => { Ui.AmmoList.Picked += _ => { }; return 0; });
        using var other = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x31, 1)]);
        UiAmmoListBridge.Tick();
        Assert.Equal((2, 1), (UiAmmoListBridge.Providers, UiAmmoListBridge.PickedHandlers));
        mod.Retire();
        Assert.False(p.Active);
        Assert.Equal((1, 0), (UiAmmoListBridge.Providers, UiAmmoListBridge.PickedHandlers));
        UiAmmoListBridge.Tick();
        Assert.Equal([(0x31u, 1u)], FakeAmmo.Last(Arrow).Rows);
    }

    [Fact]
    public void A_pick_goes_to_the_mod_whose_row_it_was_and_old_session_picks_are_dropped()
    {
        FakeAmmo.Context(Arrow, 1, session: 4);
        var quiver = Mod("quiver", 72);
        var bag = Mod("bag", 73);
        var toQuiver = new List<AmmoListPick>();
        var toBag = new List<AmmoListPick>();
        using var pq = As(quiver, () => Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x30, 12)]));
        using var pb = As(bag, () => Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x31, 3)]));
        As(quiver, () => { Ui.AmmoList.Picked += toQuiver.Add; return 0; });
        As(bag, () => { Ui.AmmoList.Picked += toBag.Add; return 0; });
        UiAmmoListBridge.Tick();
        FakeAmmo.Picks.Enqueue(new UiAmmoNative.Pick { Size = 32, Kind = Arrow, Item = 0x30, Count = 12, Key = 0, Session = 4 });
        FakeAmmo.Picks.Enqueue(new UiAmmoNative.Pick { Size = 32, Kind = Arrow, Item = 0x31, Count = 3, Key = 1, Session = 3 });  // earlier session
        FakeAmmo.Picks.Enqueue(new UiAmmoNative.Pick { Size = 32, Kind = Arrow, Item = 0x77, Count = 1, Key = 5, Session = 4 });  // no provider's
        FakeAmmo.Picks.Enqueue(new UiAmmoNative.Pick { Size = 32, Kind = Arrow, Item = 0x31, Count = 3, Key = 1, Session = 4 });
        UiAmmoListBridge.Tick();
        Assert.Equal([new AmmoListPick(AmmoListKind.Arrow, new ItemId(0x30), 12)], toQuiver);
        Assert.Equal([new AmmoListPick(AmmoListKind.Arrow, new ItemId(0x31), 3)], toBag);
        quiver.Retire();
        bag.Retire();
    }

    [Fact]
    public void A_pick_of_a_replaced_row_the_game_still_lists_with_stock_reaches_the_mod_that_provided_it()
    {
        // GitHub issue 145: the mod's server half swapped the equipped spell 0x41 into storage after the game built its rows,
        // which still list 0x41 with stock. Native finds no stack in the backpack and queues the pick; Kiln.Ui hands it to
        // the row's mod by its key, whatever the context's count said.
        FakeAmmo.Context(Spell, 1, game: (0x41, 1));
        var book = Mod("book", 74);
        var toBook = new List<AmmoListPick>();
        using var p = As(book, () => Ui.AmmoList.Provide(AmmoListKind.Spell, c => [R(0x40, 1), .. c.GameRows],
            new AmmoListOptions { Mode = AmmoListMode.Replace }));
        As(book, () => { Ui.AmmoList.Picked += toBook.Add; return 0; });
        UiAmmoListBridge.Tick();
        Assert.Equal(UiAmmoNative.ModeReplace, FakeAmmo.Last(Spell).Mode);
        Assert.Equal([(0x40u, 1u), (0x41u, 1u)], FakeAmmo.Last(Spell).Rows);
        FakeAmmo.Picks.Enqueue(new UiAmmoNative.Pick { Size = 32, Kind = Spell, Item = 0x41, Count = 1, Key = 1, Session = 1 });
        UiAmmoListBridge.Tick();
        Assert.Equal([new AmmoListPick(AmmoListKind.Spell, new ItemId(0x41), 1)], toBook);
        book.Retire();
    }

    [Fact]
    public void A_queued_pick_of_a_retired_providers_row_never_reaches_the_provider_that_shows_the_item_next()
    {
        FakeAmmo.Context(Arrow, 1);
        var quiver = Mod("quiver", 72);
        var bag = Mod("bag", 73);
        var toBag = new List<AmmoListPick>();
        As(quiver, () => Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x30, 12)]));
        As(quiver, () => { Ui.AmmoList.Picked += _ => { }; return 0; });
        UiAmmoListBridge.Tick();
        // the player picks the quiver's row; before the next UI frame the quiver unloads and the bag shows the same item
        FakeAmmo.Picks.Enqueue(new UiAmmoNative.Pick { Size = 32, Kind = Arrow, Item = 0x30, Count = 12, Key = 0, Session = 1 });
        quiver.Retire();
        using var pb = As(bag, () => Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x30, 3)]));
        As(bag, () => { Ui.AmmoList.Picked += toBag.Add; return 0; });
        UiAmmoListBridge.Tick();
        Assert.Empty(toBag);
        Assert.Equal([(0x30u, 3u)], FakeAmmo.Last(Arrow).Rows);
        // a key that does not name that item in the publication it came from is dropped too
        FakeAmmo.Picks.Enqueue(new UiAmmoNative.Pick { Size = 32, Kind = Arrow, Item = 0x31, Count = 1, Key = 0, Session = 1 });
        FakeAmmo.Picks.Enqueue(new UiAmmoNative.Pick { Size = 32, Kind = Arrow, Item = 0x30, Count = 3, Key = 0, Session = 1 });
        UiAmmoListBridge.Tick();
        Assert.Equal([new AmmoListPick(AmmoListKind.Arrow, new ItemId(0x30), 3)], toBag);
        bag.Retire();
    }

    [Fact]
    public void The_error_rule_and_a_display_only_build()
    {
        Use(new FakeAmmo.Backend([Ui.AmmoList.ListCapability]));  // the pick seam is not measured: display only
        Assert.True(Ui.AmmoList.Available);
        Assert.False(Ui.AmmoList.PicksAvailable);
        using var p = Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [R(0x30, 1)]);
        var e = Assert.Throws<KilnUnavailableException>(() => Ui.AmmoList.Picked += _ => { });
        Assert.Contains(Ui.AmmoList.PickCapability, e.Message);
        Assert.Equal(0, UiAmmoListBridge.PickedHandlers);
        Use(new FakeAmmo.Backend([]));
        Assert.Throws<KilnUnavailableException>(() => Ui.AmmoList.Provide(AmmoListKind.Spell, _ => []));
        Use(new FakeAmmo.Backend(Rows, withTable: false));
        Assert.Throws<KilnUnavailableException>(() => Ui.AmmoList.Provide(AmmoListKind.Spell, _ => []));
        Use(new FakeAmmo.Backend(Rows));
        Assert.Throws<ArgumentException>(() => Ui.AmmoList.Provide(0, _ => []));
        Assert.Throws<ArgumentException>(() => Ui.AmmoList.Provide((AmmoListKind)4, _ => []));
        Assert.Throws<ArgumentException>(() => Ui.AmmoList.Provide(AmmoListKind.Arrow, _ => [], new AmmoListOptions { Mode = 0 }));
        Assert.Throws<ArgumentNullException>(() => Ui.AmmoList.Provide(AmmoListKind.Arrow, null!));
    }
}
