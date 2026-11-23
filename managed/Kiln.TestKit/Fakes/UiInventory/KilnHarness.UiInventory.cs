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
/// <c>SlotClick.ItemEntity</c> carries), the inventory that holds it, its page and index; since 0.2.0-dev.30.5 the item's
/// <see cref="Rarity"/> and <see cref="Level"/> as the game draws them (VisibleSlot.Rarity / Level). A slot with a Holder
/// and an ItemEntity also carries it as VisibleSlot.ServerEntity (the fake takes every holder for the player or the open
/// container, the two Kiln proves in a game).</summary>
public readonly record struct FakeInventorySlot(uint Item, uint Count = 1, uint ItemEntity = 0, uint Holder = 0, uint InventoryPage = 0,
    byte SlotIndex = 0)
{
    /// <summary>The item's rarity as drawn (null: none). Since 0.2.0-dev.30.5.</summary>
    public ItemRarity? Rarity { get; init; }
    /// <summary>The item's level as drawn. Since 0.2.0-dev.30.5.</summary>
    public int Level { get; init; }
}

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
/// view), delivery to every matching handler with the slot's item and entity (and, since 0.2.0-dev.30.3, the press at the slot's centre, one pixel per UI unit), <c>Hovered</c>, and the marks: Mark, Unmark,
/// ClearMarks and MarkWhen rules are Kiln.Ui's own, <see cref="MarkOf"/> asks it. Ui.Values.Slots reads the drawn frame.</para>
/// <para><b>Not modelled.</b> The game's drawing and hooks, pixels (marks are not drawn; the mapping answers no viewport),
/// the 250 ms frame age, client sessions. Clicks are delivered at once on the calling thread (in a game: on the client's
/// main thread after the next UI frame).</para>
/// <para><b>Screens and panels (0.2.0-dev.30).</b> The screen a <see cref="Draw"/> draws on is the open one:
/// Ui.Inventory.Screen and ScreenChanged (and InventoryScreenChangedEvent) follow it; <see cref="OpenChest"/> names the
/// chest's entity (GameScreen.Target, InventoryScreen.Container), <see cref="OpenBackpack"/> and
/// <see cref="CloseScreen"/> the rest. <see cref="PanelsShown"/> names the Ui.Inventory.AddPanel panels that would
/// show (their screens and When); nothing is drawn or placed (no pixels offline), so what a panel's buttons do is
/// tested by calling the same code the button calls.</para>
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
                    ? s.Select(v => new FakeInventorySlot(v.Item, v.Count, v.ClientEntity, v.Holder, v.InventoryPage, (byte)v.SlotIndex)
                    {
                        Rarity = VisibleSlot.RarityOf(v.Available, v.Rarity), Level = VisibleSlot.LevelOf(v.Available, v.Level),
                    }).ToArray()
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
            var proven = s.Holder != 0 && s.ItemEntity != 0;  // the player's or the open container's (see FakeInventorySlot)
            drawn[i] = new UiValuesNative.VisibleSlot
            {
                Size = (uint)sizeof(UiValuesNative.VisibleSlot), Available = UiValuesNative.OuterRect, Occurrence = (uint)i,
                InventoryPage = s.InventoryPage, SlotIndex = s.SlotIndex, Item = s.Item, Count = s.Count, ClientEntity = s.ItemEntity,
                ViewFlags = (s.ItemEntity != 0 ? 1u : 0u) | 4u, Holder = s.Holder, HolderFlags = s.Holder != 0 ? 1u : 0u,
                Level = (uint)Math.Max(0, s.Level), Rarity = s.Rarity is { } r ? (uint)r : 0xFFu,
                OuterUi = new UiValuesNative.Rect { X = 10 + 90 * (i % 8), Y = 20 + 90 * (i / 8), Width = 80, Height = 80 },
            };
            if (s.Item != 0) drawn[i].Available |= UiValuesNative.ItemValues;
            if (proven)
            {
                drawn[i].ServerEntity = s.ItemEntity;
                drawn[i].ViewFlags |= UiValuesNative.ViewServerEntity;
                drawn[i].HolderFlags |= UiValuesNative.HolderServerProven;
            }
        }
        UiValuesNative.SlotsFrame frame = default;
        frame.Size = (uint)sizeof(UiValuesNative.SlotsFrame);
        GameScreen open;
        lock (st.Lock)
        {
            st.Slots = drawn;
            st.Screen = screen;
            st.Frame++;
            st.Hovered = null;
            fixed (UiValuesNative.VisibleSlot* p = drawn) UiValuesFake.FillFrame(st, &frame, p, (uint)drawn.Length, null);
            open = UiInventoryFake.GameScreenOf(screen, st.Frame, st.Container);
        }
        UiInventoryScreens.ScreenChanged(open);  // the screen it draws on is the open one (UI thread in a game)
        // Ui.Values.SubscribeSlots' subscribers get the frame, as after a UI frame in a game
        if (FakeTables.Off(UiValuesAbi.SlotsRow) || UiValuesFake.State is not { } vs) return;
        UiValuesFakeState.Reg[] subs;
        lock (vs.Lock) subs = vs.Regs.Where(r => r.List == UiValuesAbi.Slots).ToArray();
        var legacy = new byte[drawn.Length * 112];
        fixed (byte* packed = legacy)
        fixed (UiValuesNative.VisibleSlot* p = drawn)
        {
            for (var i = 0; i < drawn.Length; i++)
            {
                Buffer.MemoryCopy(p + i, packed + i * 112, 112, 112);
                ((uint*)(packed + i * 112))[0] = 112;
                ((uint*)(packed + i * 112))[1] &= ~UiValuesNative.ItemValues;
            }
            foreach (var r in subs)
                ((delegate* unmanaged[Cdecl]<UiValuesNative.SlotsFrame*, UiValuesNative.VisibleSlot*, uint, void*, void>)r.Fn)(
                    &frame, r.Item == 120 ? p : (UiValuesNative.VisibleSlot*)packed, frame.Count, (void*)r.User);
        }
    }

    /// <summary>The player opens the backpack with these slots drawn (the backpack screen, HUD Backpack).</summary>
    public void OpenBackpack(params FakeInventorySlot[] slots) => Draw(HudScreen.Backpack, slots);

    /// <summary>The player opens the storage chest <paramref name="chest"/> (its entity: what InventoryScreen.Container
    /// and GameScreen.Target carry) with these slots drawn (the chest's and the backpack's; give the chest's slots
    /// <c>Holder = chest</c>).</summary>
    public void OpenChest(EntityId chest, params FakeInventorySlot[] slots)
    {
        lock (State.Lock) State.Container = chest.Value;
        Draw(HudScreen.StorageChest, slots);
    }

    /// <summary>Every menu closed: the play view (no slots drawn).</summary>
    public void CloseScreen()
    {
        lock (State.Lock) State.Container = 0;
        Draw(HudScreen.InGame);
    }

    /// <summary>The backpack or chest screen open now (what Ui.Inventory.Screen says in a game), or null.</summary>
    public InventoryScreen? Screen => UiInventoryScreens.Current;

    /// <summary>The titles of the Ui.Inventory.AddPanel panels that show on the open screen now (their Screens and When,
    /// in the order they were added).</summary>
    public IReadOnlyList<string> PanelsShown => UiInventoryScreens.Showing();

    /// <summary>The game's own selection (a controller's: what Ui.Inventory.Selected reads, since 0.2.0-dev.30.5) rests on
    /// drawn slot <paramref name="index"/> (null: on none), as the game marks it in the slot draw's state byte 0. A
    /// <see cref="Draw"/> starts with none.</summary>
    public unsafe void Select(int? index)
    {
        var st = State;
        lock (st.Lock)
        {
            var slots = st.Slots ?? [];
            if (index is { } i)
            {
                ArgumentOutOfRangeException.ThrowIfNegative(i);
                ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(i, slots.Length);
            }
            for (var k = 0; k < slots.Length; k++)
            {
                slots[k].Available |= UiValuesNative.NativeState;
                slots[k].NativeState[0] = k == index ? (byte)1 : (byte)0;
            }
        }
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
    /// the click, in subscription order. The click's <c>SlotClick.Pointer</c> (since 0.2.0-dev.30.3) is the centre of the
    /// slot's outer rectangle, as a 1920x1080 game at UI scale 1 draws it (one pixel per UI unit).</summary>
    public FakeSlotClickResult Click(int index, SlotButton button = SlotButton.Left, SlotModifiers modifiers = SlotModifiers.None)
    {
        var (n, taken) = UiInventoryFake.Press(State, index, (uint)button, (uint)modifiers);
        return new FakeSlotClickResult(n, taken);
    }

    /// <summary>The player presses a controller chord (<c>"Pad:LS"</c>, <c>"Pad:LB+A"</c>) while the game's selection rests on a
    /// drawn slot (<see cref="Select"/>): every <c>SlotClickOptions.Pad</c> handler of that chord gets it as a click on that
    /// slot (<c>SlotButton.Pad</c>), in subscription order. This helper also delivers the next simulated UI frame on the harness thread. Nothing (Delivered 0, the game
    /// gets the press) without a selected slot, on a screen without a pointer, or while a row is off. Since
    /// 0.2.0-dev.30.5.</summary>
    public FakeSlotClickResult PressPad(string chord)
    {
        var c = KeyChord.Parse(chord);
        if (!c.IsPad) throw new ArgumentException($"{chord} is not a controller chord (Pad:LS, Pad:LB+A)", nameof(chord));
        var taken = UiInventoryBridge.PadPress(c, out var n);
        UiKeyDispatch.Drain(); // simulate the next game UI frame on the harness thread
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
