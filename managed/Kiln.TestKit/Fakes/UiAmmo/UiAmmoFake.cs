// SPDX-License-Identifier: MIT
// The fake kiln.ui.ammo (native/core/abi/services/kiln_ui_ammo.h, v1) under the real Ui.AmmoList of Kiln.Ui (0.2.0-dev.30.6,
// GitHub issue 127). A test plays the game's side (KilnHarness.AmmoList: Hold a bow or a staff with the game's own rows,
// change them, a new session, the client's item catalog per list) and the player's (Select, Pick). At the start of each
// step (the next UI frame) Kiln.Ui's own code asks the providers when a list's context changed and publishes its snapshot
// here; the native rules run: one owner per list, the context's serial and session, at most 64 rows, no item 0, no
// duplicate, flags 0, items of the list's kind when the test named a catalog. Shown composes the game's rows with the
// snapshot as the HUD would; the selection stays on its item; a pick of a mod row the game does not list is queued for
// Ui.AmmoList.Picked (64 at most, the oldest dropped). The game's hook, its drawing and the equip request are not
// modelled.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

internal sealed class UiAmmoFakeState
{
    public sealed class List
    {
        public bool Built;                      // the game built this list (a bow or a staff is held)
        public uint Held;
        public (uint Item, uint Count)[] Game = [];
        public ulong Serial;
        public ulong Owner;                     // the owner whose snapshot it shows (0: the game's list as is)
        public uint Mode;
        public UiAmmoNative.Row[] Rows = [];
        public HashSet<uint>? Known;            // the client's items of this kind (null: any item is taken)
        public uint? Selected;
    }

    public readonly object Lock = new();
    public readonly List Arrow = new(), Spell = new();
    public uint Session = 1;
    public uint Player;
    public ulong NextSerial = 1;
    public readonly Queue<(ulong Owner, UiAmmoNative.Pick Pick)> Picks = new();
    public int Publishes;
    public ulong Step;

    public List? Of(uint kind) => kind switch { UiAmmoNative.KindArrow => Arrow, UiAmmoNative.KindSpell => Spell, _ => null };
}

internal sealed unsafe class UiAmmoFake : IKitFake
{
    public const string Service = UiAmmoNative.Service;
    public const string ListRow = Ui.AmmoList.ListCapability;
    public const string PickRow = Ui.AmmoList.PickCapability;

    private static UiAmmoNative.Table* _table;

    string IKitFake.Service => Service;

    public void Register()
    {
        if (_table == null)
        {
            var t = (UiAmmoNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(UiAmmoNative.Table));
            t->Header = new ServiceHeader { Size = (uint)sizeof(UiAmmoNative.Table), Version = 1 };
            t->Publish = &Publish;
            t->GetContext = &GetContext;
            t->TakePicks = &TakePicks;
            _table = t;  // process lifetime
        }
        KitBackend.Instance.RegisterTable(Service, (nint)_table);
        FakeTables.Features([ListRow, PickRow]);
    }

    // Kiln.Ui's lists are process-wide: a harness starts with none (a mod of an earlier harness has retired)
    public void Start(KilnHarness harness) => UiAmmoListBridge.ResetForTests();

    // the next UI frame: Kiln.Ui builds the lists whose context changed and drains the picks
    public void BeforeStep(KilnHarness harness, ulong step)
    {
        var st = KitFakes.State<UiAmmoFakeState>(harness);
        lock (st.Lock) st.Step = step;
        UiAmmoListBridge.Tick();
    }

    public void End(KilnHarness harness) => UiAmmoListBridge.ResetForTests();

    public void AddCoverage(List<CoverageRow> rows) =>
        rows.Add(new CoverageRow("The HUD's ammunition and spell lists", CoverageKind.RecordingFake, [Service, ListRow, PickRow],
            "Ui.AmmoList's own code on the kit's kiln.ui.ammo: a test holds a bow or a staff with the game's own rows " +
            "(KilnHarness.AmmoList.Hold, SetGameRows, Unhold, NewSession, KnownItems); at each step Kiln.Ui asks the providers " +
            "when a list's context changed and publishes one snapshot, which the native rules check (one owner, the context's " +
            "serial and session, 64 rows, no duplicate or item 0, the list's item catalog). Shown and ModeOf read the list as " +
            "the HUD would compose it; the selection stays on its item (Select, Selected); Pick of a mod row the game does not " +
            "list raises Ui.AmmoList.Picked at the next step for the mod that provided it. Offline: the game's hook, its " +
            "drawing and its equip request."));

    internal static UiAmmoFakeState? State =>
        FakeTables.Active(Service) is { } h ? KitFakes.State<UiAmmoFakeState>(h) : null;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Publish(ulong owner, UiAmmoNative.Publish* p)
    {
        try
        {
            if (owner == 0 || p == null || p->Size < (uint)sizeof(UiAmmoNative.Publish) || p->Reserved != 0 ||
                p->Mode > UiAmmoNative.ModeReplace || p->Count > UiAmmoNative.Capacity || (p->Count != 0 && p->Rows == null))
                return FakeTables.InvalidArgument;
            if (FakeTables.Off(ListRow) || FakeTables.Off(PickRow) || State is not { } st) return FakeTables.Unavailable;
            lock (st.Lock)
            {
                if (st.Of(p->Kind) is not { } list) return FakeTables.InvalidArgument;
                if (list.Owner != 0 && list.Owner != owner) return UiAmmoNative.Busy;
                if (p->Mode == UiAmmoNative.ModeGame)
                {
                    list.Owner = 0;
                    list.Mode = UiAmmoNative.ModeGame;
                    list.Rows = [];
                    st.Publishes++;
                    return FakeTables.Ok;
                }
                if (!list.Built) return FakeTables.NotFound;
                if (p->Serial != list.Serial || p->Session != st.Session) return FakeTables.InvalidArgument;
                var rows = new UiAmmoNative.Row[p->Count];
                var seen = new HashSet<uint>();
                for (var i = 0; i < rows.Length; i++)
                {
                    rows[i] = p->Rows[i];
                    if (rows[i].Item == 0 || rows[i].Flags != 0 || !seen.Add(rows[i].Item) ||
                        (list.Known is { } known && !known.Contains(rows[i].Item)))
                        return FakeTables.InvalidArgument;
                }
                list.Owner = owner;
                list.Mode = p->Mode;
                list.Rows = rows;
                st.Publishes++;
                return FakeTables.Ok;
            }
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GetContext(ulong owner, uint kind, UiAmmoNative.Context* o)
    {
        if (owner == 0 || o == null || o->Size < (uint)sizeof(UiAmmoNative.Context)) return FakeTables.InvalidArgument;
        if (FakeTables.Off(ListRow) || FakeTables.Off(PickRow) || State is not { } st) return FakeTables.Unavailable;
        lock (st.Lock)
        {
            if (st.Of(kind) is not { } list) return FakeTables.InvalidArgument;
            if (!list.Built) return FakeTables.NotFound;
            *o = default;
            o->Size = (uint)sizeof(UiAmmoNative.Context);
            o->Ok = 1;
            o->Kind = kind;
            o->Session = st.Session;
            o->LocalPlayer = st.Player;
            o->HeldItem = list.Held;
            o->GameSerial = list.Serial;
            var n = Math.Min(list.Game.Length, UiAmmoNative.Capacity);
            o->GameCount = (uint)n;
            if (list.Game.Length > n) o->Reason = UiAmmoNative.ReasonLimit;
            for (var i = 0; i < n; i++) o->GameRows[i] = new UiAmmoNative.Row { Item = list.Game[i].Item, Count = list.Game[i].Count, Key = (uint)i };
        }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int TakePicks(ulong owner, UiAmmoNative.Pick* o, uint capacity, uint* written)
    {
        if (owner == 0 || written == null || capacity > UiAmmoNative.Capacity || (capacity != 0 && o == null)) return FakeTables.InvalidArgument;
        *written = 0;
        if (FakeTables.Off(ListRow) || FakeTables.Off(PickRow) || State is not { } st) return FakeTables.Unavailable;
        lock (st.Lock)
        {
            var kept = new Queue<(ulong, UiAmmoNative.Pick)>();
            uint n = 0;
            while (st.Picks.Count > 0)
            {
                var (who, pick) = st.Picks.Dequeue();
                if (who == owner && n < capacity) o[n++] = pick;
                else kept.Enqueue((who, pick));
            }
            foreach (var k in kept) st.Picks.Enqueue(k);
            *written = n;
        }
        return FakeTables.Ok;
    }

    /// <summary>The list as the HUD shows it: the game's rows, then (Append) or instead (Replace) the snapshot's; 64 rows
    /// in all (native's capacity counts the game's rows and the mods' together).</summary>
    internal static (uint Item, uint Count, bool Mod)[] Shown(UiAmmoFakeState.List list)
    {
        var game = list.Game.Take(UiAmmoNative.Capacity).Select(g => (g.Item, g.Count, false));
        if (list.Owner == 0 || list.Mode == UiAmmoNative.ModeGame) return game.ToArray();
        var shown = list.Mode == UiAmmoNative.ModeAppend ? game.ToList() : [];
        foreach (var r in list.Rows)
            if (shown.Count < UiAmmoNative.Capacity && !shown.Any(s => s.Item == r.Item)) shown.Add((r.Item, r.Count, true));
        return shown.ToArray();
    }
}
