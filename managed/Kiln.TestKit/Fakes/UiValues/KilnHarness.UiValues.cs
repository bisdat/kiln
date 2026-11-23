// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessUiValues? _uiValues;

    /// <summary>The fake kiln.ui.values of this harness: draw a value or an item detail view through the registered
    /// Ui.Values providers.</summary>
    public HarnessUiValues UiValues => _uiValues ??= new HarnessUiValues(this);
}

/// <summary>An item as the game's UI would show it to a provider (the fields of <c>UiItemView</c> a test sets).
/// <paramref name="Flags"/>: 1 client entity, 4 inventory slot, 8 detail copy, 16 synthetic.</summary>
/// <param name="Item">The item's id.</param>
/// <param name="Fuel">The item's fuel, in raw fuel units.</param>
/// <param name="FuelMax">The item's largest fuel, in raw fuel units.</param>
/// <param name="Count">The stack size (default 1).</param>
/// <param name="InventoryPage">The inventory page of the slot (an entity id); valid with flag 4.</param>
/// <param name="SlotIndex">The slot's index on its page; valid with flag 4.</param>
/// <param name="ClientEntity">The stack's client-world entity; valid with flag 1.</param>
/// <param name="Flags">Which identity fields are valid: 1 client entity, 4 inventory slot, 8 detail copy, 16 synthetic.</param>
/// <param name="UnitsPerCharge">The detail view's divisor for fuel (at least 1; default 1).</param>
public readonly record struct FakeUiItem(uint Item, uint Fuel = 0, uint FuelMax = 0, uint Count = 1, uint InventoryPage = 0,
    byte SlotIndex = 0, uint ClientEntity = 0, uint Flags = 0, uint UnitsPerCharge = 1);

/// <summary>A drawn value: the fraction, the numbers and the label (null: the game's own).</summary>
/// <param name="Fraction">How full the bar is, as a fraction.</param>
/// <param name="Current">The current number.</param>
/// <param name="Max">The maximum number.</param>
/// <param name="Label">The label text; null leaves the game's own.</param>
public readonly record struct FakeUiBar(float Fraction, uint Current, uint Max, string? Label = null);

/// <summary>What one provider did with a value: <paramref name="Result"/> is its native result (0 ok), <paramref name="Edit"/>
/// the fields it committed (KilnUiEditFlags), <paramref name="Invalid"/> when the fake refused the edit as the game would
/// (a fault).</summary>
/// <param name="Kind">The kind of value the provider was asked about (the numbering of <c>UiValueKind</c>: detail, slot, held, health, stamina, mana).</param>
/// <param name="Result">The provider's native result; 0 is ok.</param>
/// <param name="Edit">The fields the provider committed, as the native edit flags (0 when it changed nothing or its edit was refused).</param>
/// <param name="Invalid">True when the fake refused the provider's edit as the game would, counting it as a fault.</param>
public readonly record struct FakeUiDecision(uint Kind, int Result, uint Edit, bool Invalid);

/// <summary>
/// The fake kiln.ui.values of a harness (<see cref="KilnHarness.UiValues"/>): Ui.Values' own code runs on it.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> Registration (128 per mod, item filters, the capability rows a test can switch off), the order
/// of providers, the accumulated value, the kinds' edit masks (detail: fraction, numbers up to 2,147,483,647 and the label;
/// slot: fraction and numbers; held and HUD: numbers, maximum above 0), labels through the provider's text, tooltip lines
/// (16 per detail view; a refused provider adds none).</para>
/// <para><b>Not modelled.</b> The game's drawing and hooks, fault removal after five, the text budgets, slot frames,
/// change events and the Item details window (all answer unavailable).</para>
/// </remarks>
public sealed unsafe class HarnessUiValues
{
    private readonly KilnHarness _harness;

    internal HarnessUiValues(KilnHarness harness) => _harness = harness;

    private UiValuesFakeState State => KitFakes.State<UiValuesFakeState>(_harness);

    /// <summary>Registrations held now (values and tooltip lines).</summary>
    public int Registrations
    {
        get { lock (State.Lock) return State.Regs.Count; }
    }

    /// <summary>Every provider call of <see cref="Draw"/> so far, in order.</summary>
    public IReadOnlyList<FakeUiDecision> Decisions
    {
        get { lock (State.Lock) return State.Decisions.ToArray(); }
    }

    private static readonly uint[] Masks = [1 | 2 | 4 | 8 | 16, 1 | 2 | 4, 2 | 4, 2 | 4, 2 | 4, 2 | 4];

    /// <summary>Draws a value of <paramref name="kind"/> (<c>UiValueKind</c>'s number: 0 detail, 1 slot, 2 held, 3 health,
    /// 4 stamina, 5 mana) for <paramref name="item"/>: the providers run in registration order over <paramref name="game"/>
    /// and the drawn value comes back.</summary>
    /// <exception cref="ArgumentOutOfRangeException">A kind above 5.</exception>
    public FakeUiBar Draw(uint kind, FakeUiItem item, FakeUiBar game)
    {
        ArgumentOutOfRangeException.ThrowIfGreaterThan(kind, 5u);
        var st = State;
        UiValuesFakeState.Reg[] regs;
        lock (st.Lock) regs = st.Regs.Where(r => r.List == kind && (r.Item != 0 ? r.Item == item.Item : kind >= 3 || item.Item != 0)).ToArray();
        var acc = new UiValuesAbi.BarValue
        {
            Size = (uint)sizeof(UiValuesAbi.BarValue), Kind = kind, Fraction = game.Fraction, Current = game.Current, Maximum = game.Max,
        };
        if (game.Label is { } l)
        {
            acc.LabelIsOverride = 1;
            acc.LabelId = UiValuesFake.TextId(st, l);
        }
        var view = ViewOf(item);
        foreach (var r in regs)
        {
            var ctx = Context(kind, r.Owner);
            var input = acc;
            var edit = new UiValuesAbi.BarEdit { Size = (uint)sizeof(UiValuesAbi.BarEdit) };
            var rc = ((delegate* unmanaged[Cdecl]<UiValuesAbi.ProviderContext*, UiValuesAbi.ItemView*, UiValuesAbi.BarValue*, UiValuesAbi.BarEdit*, void*, int>)r.Fn)(
                &ctx, &view, &input, &edit, (void*)r.User);
            var invalid = rc == 0 && edit.Flags != 0 && !Apply(kind, edit, ref acc);
            lock (st.Lock) st.Decisions.Add(new FakeUiDecision(kind, rc, rc == 0 && !invalid ? edit.Flags : 0, invalid));
        }
        return new FakeUiBar(acc.Fraction, acc.Current, acc.Maximum, acc.LabelIsOverride != 0 ? UiValuesFake.TextOf(st, acc.LabelId) : null);
    }

    private static bool Apply(uint kind, in UiValuesAbi.BarEdit e, ref UiValuesAbi.BarValue acc)
    {
        var f = e.Flags;
        if ((f & ~Masks[kind]) != 0 || (f & 8) != 0 && (f & 16) != 0) return false;
        if ((f & 1) != 0 && !(float.IsFinite(e.Fraction) && e.Fraction is >= 0 and <= 1)) return false;
        if (kind == 0 && ((f & 2) != 0 && e.Current > int.MaxValue || (f & 4) != 0 && e.Maximum > int.MaxValue)) return false;
        if (kind >= 2 && (f & 4) != 0 && e.Maximum == 0) return false;
        if ((f & 1) != 0) acc.Fraction = e.Fraction;
        if ((f & 2) != 0) acc.Current = e.Current;
        if ((f & 4) != 0) acc.Maximum = e.Maximum;
        if ((f & 8) != 0) (acc.LabelIsOverride, acc.LabelId) = (1, e.LabelId);
        if ((f & 16) != 0) (acc.LabelIsOverride, acc.LabelId) = (0, 0);
        if (kind >= 2) acc.Fraction = acc.Maximum == 0 ? 0 : Math.Min(1f, (float)acc.Current / acc.Maximum);
        return true;
    }

    /// <summary>Draws <paramref name="item"/>'s detail view: the tooltip-lines providers run in order and the lines they
    /// added come back (at most 16; a refused provider, or one whose lines do not fit, adds none).</summary>
    public IReadOnlyList<string> DetailLines(FakeUiItem item)
    {
        var st = State;
        UiValuesFakeState.Reg[] regs;
        lock (st.Lock) regs = st.Regs.Where(r => r.List == UiValuesAbi.Lines && (r.Item == 0 || r.Item == item.Item)).ToArray();
        var lines = new List<string>();
        var view = ViewOf(item);
        foreach (var r in regs)
        {
            if (lines.Count >= 16) break;
            var ctx = Context(0, r.Owner);
            var w = new UiValuesAbi.LineWriter { Size = (uint)sizeof(UiValuesAbi.LineWriter), Capacity = (uint)(16 - lines.Count) };
            w.Append = &Append;
            var rc = ((delegate* unmanaged[Cdecl]<UiValuesAbi.ProviderContext*, UiValuesAbi.ItemView*, UiValuesAbi.LineWriter*, void*, int>)r.Fn)(
                &ctx, &view, &w, (void*)r.User);
            if (rc != 0 || w.Status != 0) continue;
            var ids = (ulong*)w.Lines;
            for (var i = 0; i < w.Count; i++) lines.Add(UiValuesFake.TextOf(st, ids[i * 2 + 1]) ?? "");
        }
        return lines;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Append(UiValuesAbi.LineWriter* w, ulong id)
    {
        if (w->Count >= w->Capacity)
        {
            w->Status = 2;  // KUI_TEXT_BUDGET
            return FakeTables.BufferTooSmall;
        }
        var line = (uint*)w->Lines + w->Count * 4;  // KilnUiLine {u32 size, u32 flags, u64 text}
        line[0] = 16;
        line[1] = 0;
        *(ulong*)(line + 2) = id;
        w->Count++;
        return FakeTables.Ok;
    }

    private static UiValuesAbi.ProviderContext Context(uint kind, ulong owner) => new()
    {
        Size = (uint)sizeof(UiValuesAbi.ProviderContext), Kind = kind, Owner = owner, Text = UiValuesFake.Ops, Session = 1,
    };

    private static UiValuesAbi.ItemView ViewOf(FakeUiItem i) => new()
    {
        Size = (uint)sizeof(UiValuesAbi.ItemView), Flags = i.Flags, Item = i.Item, Fuel = i.Fuel, FuelMax = i.FuelMax, Count = i.Count,
        InventoryPage = i.InventoryPage, SlotIndex = i.SlotIndex, ClientEntity = i.ClientEntity, UnitsPerCharge = i.UnitsPerCharge,
        Session = 1,
    };
}
