// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessUiInventory? _uiInventory;

    /// <summary>The fake kiln.ui.inventory of this harness: draw a screen's slots, hover and click one, and ask what mark a
    /// drawn slot would show (Ui.Inventory).</summary>
    public HarnessUiInventory UiInventory => _uiInventory ??= new HarnessUiInventory(this);
}

/// <summary>A slot as the game would draw it: the item, the stack, the item's entity (the server's id of it: what
/// <c>SlotClick.ItemEntity</c> carries), the inventory that holds it, its page and index.</summary>
public readonly record struct FakeInventorySlot(uint Item, uint Count = 1, uint ItemEntity = 0, uint Holder = 0, uint InventoryPage = 0,
    byte SlotIndex = 0);

/// <summary>What a <see cref="HarnessUiInventory.Click"/> did: how many handlers got it, and whether the game would not
/// have seen the press (a handler asked to take it).</summary>
public readonly record struct FakeSlotClickResult(int Delivered, bool Taken);

/// <summary>
/// The fake kiln.ui.inventory of a harness (<see cref="KilnHarness.UiInventory"/>): Ui.Inventory's own code runs on it.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> OnClick's subscriptions (button, exact modifiers, Take; 128 per mod; the rows
/// <c>ui.slots.click</c> and <c>ui.slots.inspect</c> a test can switch off), the press rules (a drawn frame, a screen with a
/// pointer: the in-game menu, player crafting, the backpack, a chest, a station, NPC crafting, a factory; never the play
/// view), delivery to every matching handler with the slot's item and entity, <c>Hovered</c>, and the marks: Mark, Unmark,
/// ClearMarks and MarkWhen rules are Kiln.Ui's own, <see cref="MarkOf"/> asks it. Ui.Values.Slots reads the drawn frame.</para>
/// <para><b>Not modelled.</b> The game's drawing and hooks, pixels (marks are not drawn; the mapping answers no viewport),
/// the 250 ms frame age, client sessions. Clicks are delivered at once on the calling thread (in a game: on the client's
/// main thread after the next UI frame).</para>
/// </remarks>
public sealed class HarnessUiInventory
{
    private readonly KilnHarness _harness;

    internal HarnessUiInventory(KilnHarness harness) => _harness = harness;

    private UiInventoryFakeState State => KitFakes.State<UiInventoryFakeState>(_harness);

    /// <summary>OnClick subscriptions held now.</summary>
    public int ClickSubscriptions
    {
        get { lock (State.Lock) return State.Regs.Count; }
    }

    /// <summary>The slots of the latest <see cref="Draw"/> (empty before the first).</summary>
    public IReadOnlyList<FakeInventorySlot> Drawn
    {
        get
        {
            lock (State.Lock)
                return State.Slots is { } s
                    ? s.Select(v => new FakeInventorySlot(v.Item, v.Count, v.ClientEntity, v.Holder, v.InventoryPage, (byte)v.SlotIndex)).ToArray()
                    : [];
        }
    }

    /// <summary>The game draws <paramref name="slots"/> on <paramref name="screen"/> (the latest UI frame from now on;
    /// the hovered slot is cleared).</summary>
    public unsafe void Draw(HudScreen screen, params FakeInventorySlot[] slots)
    {
        ArgumentNullException.ThrowIfNull(slots);
        var st = State;
        var drawn = new UiValuesNative.VisibleSlot[slots.Length];
        for (var i = 0; i < slots.Length; i++)
        {
            var s = slots[i];
            drawn[i] = new UiValuesNative.VisibleSlot
            {
                Size = (uint)sizeof(UiValuesNative.VisibleSlot), Available = UiValuesNative.OuterRect, Occurrence = (uint)i,
                InventoryPage = s.InventoryPage, SlotIndex = s.SlotIndex, Item = s.Item, Count = s.Count, ClientEntity = s.ItemEntity,
                ViewFlags = (s.ItemEntity != 0 ? 1u : 0u) | 4u, Holder = s.Holder, HolderFlags = s.Holder != 0 ? 1u : 0u,
                OuterUi = new UiValuesNative.Rect { X = 10 + 90 * (i % 8), Y = 20 + 90 * (i / 8), Width = 80, Height = 80 },
            };
        }
        UiValuesNative.SlotsFrame frame = default;
        frame.Size = (uint)sizeof(UiValuesNative.SlotsFrame);
        lock (st.Lock)
        {
            st.Slots = drawn;
            st.Screen = screen;
            st.Frame++;
            st.Hovered = null;
            fixed (UiValuesNative.VisibleSlot* p = drawn) UiValuesFake.FillFrame(st, &frame, p, (uint)drawn.Length, null);
        }
        // Ui.Values.SubscribeSlots' subscribers get the frame, as after a UI frame in a game
        if (FakeTables.Off(UiValuesAbi.SlotsRow) || UiValuesFake.State is not { } vs) return;
        UiValuesFakeState.Reg[] subs;
        lock (vs.Lock) subs = vs.Regs.Where(r => r.List == UiValuesAbi.Slots).ToArray();
        fixed (UiValuesNative.VisibleSlot* p = drawn)
            foreach (var r in subs)
                ((delegate* unmanaged[Cdecl]<UiValuesNative.SlotsFrame*, UiValuesNative.VisibleSlot*, uint, void*, void>)r.Fn)(
                    &frame, p, frame.Count, (void*)r.User);
    }

    /// <summary>The cursor rests on drawn slot <paramref name="index"/> (null: on none): what Ui.Inventory.Hovered reads.</summary>
    public void Hover(int? index)
    {
        var st = State;
        lock (st.Lock)
        {
            if (index is { } i)
            {
                ArgumentOutOfRangeException.ThrowIfNegative(i);
                ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(i, st.Slots?.Length ?? 0);
            }
            st.Hovered = index;
        }
    }

    /// <summary>The player presses <paramref name="button"/> with <paramref name="modifiers"/> on drawn slot
    /// <paramref name="index"/>. Nothing reaches the handlers (Delivered 0, the game gets the press) on a screen without a
    /// pointer, before a Draw, or while a row is off; otherwise every handler whose button and exact modifiers match gets
    /// the click, in subscription order.</summary>
    public FakeSlotClickResult Click(int index, SlotButton button = SlotButton.Left, SlotModifiers modifiers = SlotModifiers.None)
    {
        var (n, taken) = UiInventoryFake.Press(State, index, (uint)button, (uint)modifiers);
        return new FakeSlotClickResult(n, taken);
    }

    /// <summary>The mark drawn slot <paramref name="index"/> would show (null: none): an item mark of any mod first, then
    /// the MarkWhen rules in order, as Kiln.Ui decides each overlay frame.</summary>
    public SlotMark? MarkOf(int index)
    {
        UiValuesNative.VisibleSlot s;
        var st = State;
        lock (st.Lock)
        {
            var slots = st.Slots ?? [];
            ArgumentOutOfRangeException.ThrowIfNegative(index);
            ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(index, slots.Length);
            s = slots[index];
        }
        return UiInventoryBridge.MarkOf(UiInventoryBridge.ToVisible(s));
    }
}
