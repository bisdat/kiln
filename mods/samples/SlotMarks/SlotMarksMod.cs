// SPDX-License-Identifier: MIT
using Kiln;

namespace SlotMarks;

/// <summary>Client to server: "mark (or clear) this item for me". A wish: the server checks it.</summary>
public sealed record MarkRequest(uint ItemEntity) : INetMessage<MarkRequest>
{
    public void Encode(NetWriter w) => w.UInt32(ItemEntity);
    public static MarkRequest Decode(ref NetReader r) => new(r.UInt32());
}

/// <summary>Client to server: "count the chest I have open" (the chest's entity from Ui.Inventory's chest screen). A wish:
/// the server checks the entity is a container near the sender, not a player.</summary>
public sealed record ChestCountRequest(uint Chest) : INetMessage<ChestCountRequest>
{
    public void Encode(NetWriter w) => w.UInt32(Chest);
    public static ChestCountRequest Decode(ref NetReader r) => new(r.UInt32());
}

/// <summary>Server to client: the chest's count, or why the server refused (Ok false, Why).</summary>
public sealed record ChestCountAnswer(uint Chest, bool Ok, int Stacks, int Items, string Why) : INetMessage<ChestCountAnswer>
{
    public void Encode(NetWriter w) => w.UInt32(Chest).Bool(Ok).Int32(Stacks).Int32(Items).String(Why);
    public static ChestCountAnswer Decode(ref NetReader r) => new(r.UInt32(), r.Bool(), r.Int32(), r.Int32(), r.String());
}

/// <summary>Server to client: the server checked the item; Marked = the mark is now on (false: cleared).</summary>
public sealed record MarkAnswer(uint ItemEntity, bool Marked) : INetMessage<MarkAnswer>
{
    public void Encode(NetWriter w) => w.UInt32(ItemEntity).Bool(Marked);
    public static MarkAnswer Decode(ref NetReader r) => new(r.UInt32(), r.Bool());
}

/// <summary>
/// Ui.Inventory (docs/kiln/ui.md "The game's inventory slots", 0.2.0-dev.30): the game's own slots as something a mod acts
/// on. One mod, both sides (side "both"):
///  - client: Ctrl+left click on a slot in the backpack, the inventory menu, a chest or a station asks the server to mark
///    the item (click again to clear it). The game never sees the press: it does not pick the item up.
///  - server: checks that the item is really in the sender's own inventory (Kiln.Items) before it agrees. A click is a
///    wish, never authority: ItemEntity comes from the player's game and could name anything. A real mod (Kiln Auto Loot's
///    salvage marks) keeps the mark on the item there too (InstanceData, synced back), and checks chests the player may
///    use the same way (Items.Read(chest)).
///  - client: the server's answer draws the red frame (Ui.Inventory.Mark). The mark follows the item, not the slot.
///  - Alt+right click lists the slot in kiln.log and as a toast (watch only: the game gets the click as usual).
///  - Every stack of 50 or more gets a faint tint (a rule asked per drawn slot, on the render thread: keep it a lookup).
///  - A storage chest's screen gets a small panel beside the slots (Ui.Inventory.AddPanel, since dev.30 slice 2) with a
///    "Count this chest" button: the client sends the chest's entity (InventoryScreen.Container); the server checks it is a
///    container within reach of the sender and not a player before it reads it (Items.Read) and answers. The quick path to
///    Kiln Auto Loot's "Salvage" button next to a chest.
/// Without a Kiln server that has this mod, nothing is marked: the client never marks on its own say-so.
/// </summary>
public sealed class SlotMarksMod : Mod
{
    private static readonly SlotMark Red = new(new Color(235, 60, 50), SlotMarkStyle.Border, 3);
    private static readonly SlotMark BigStack = new(new Color(240, 210, 90, 60), SlotMarkStyle.Fill);
    private readonly HashSet<(EntityId Player, uint Item)> _serverMarks = [];  // server: who marked what
    private readonly List<IDisposable> _subs = [];
    private const double Reach = 12;  // metres: the server answers only for a chest this close to the sender
    private NetChannel? _ch;
    private volatile string _chestLine = "";  // the last answer, shown in the panel (render thread reads it)

    public override void OnLoad()
    {
        _ch = Network.Channel("slotmarks:main", "1")
            .Register<MarkRequest>(OnRequest, NetDirection.ToServer)
            .Register<MarkAnswer>(OnAnswer, NetDirection.ToClient)
            .Register<ChestCountRequest>(OnChestCount, NetDirection.ToServer)
            .Register<ChestCountAnswer>(OnChestAnswer, NetDirection.ToClient);
        if (!Ui.Available) return;  // a dedicated server: the server half above is all
        try
        {
            _subs.Add(Ui.Inventory.OnClick(new SlotClickOptions { Modifiers = SlotModifiers.Ctrl }, AskToMark));
            _subs.Add(Ui.Inventory.OnClick(new SlotClickOptions { Button = SlotButton.Right, Modifiers = SlotModifiers.Alt, Take = false },
                Describe));
        }
        catch (KilnUnavailableException e)
        {
            Log.Info($"slotmarks: no slot clicks here: {e.Message}");
        }
        try
        {
            _subs.Add(Ui.Inventory.MarkWhen(s => s.Count >= 50 ? BigStack : null));
        }
        catch (KilnUnavailableException e)
        {
            Log.Info($"slotmarks: no slot marks here: {e.Message}");
        }
        try
        {
            _subs.Add(Ui.Inventory.AddPanel(UiText.Of(("en", "Slot marks"), ("de", "Slot-Markierungen")), DrawChestPanel,
                new InventoryPanelOptions { Screens = InventoryScreens.Chest, Width = 240 }));
            Ui.Inventory.ScreenChanged += OnScreenChanged;  // removed with the mod
        }
        catch (KilnUnavailableException e)
        {
            Log.Info($"slotmarks: no chest panel here: {e.Message}");
        }
    }

    public override void OnUnload()
    {
        foreach (var s in _subs) s.Dispose();  // Kiln removes them with the mod anyway
        _subs.Clear();
    }

    // ---- client ----
    private void AskToMark(SlotClick click)
    {
        if (click.ItemEntity.Value == 0) return;  // an empty slot, or a stack the game drew without an entity
        if (_ch is null || !_ch.SendToServer(new MarkRequest(click.ItemEntity.Value)))
            Hud.Toast(Network.ServerHasKiln ? "the server lacks slotmarks" : "not connected to a Kiln server", ToastKind.Warning);
    }

    private void OnAnswer(MarkAnswer m, NetContext ctx)
    {
        var item = new EntityId(m.ItemEntity);
        if (m.Marked) Ui.Inventory.Mark(item, Red);
        else Ui.Inventory.Unmark(item);
        Log.Info($"slotmarks: the server {(m.Marked ? "marked" : "cleared")} item {item}");
    }

    private void OnScreenChanged(InventoryScreen? before, InventoryScreen? now)
    {
        _chestLine = "";
        if (now is { Kind: InventoryScreenKind.Chest } s) Log.Info($"slotmarks: chest screen {s.Hud}, chest {s.Container}");
        else if (before is { Kind: InventoryScreenKind.Chest } b) Log.Info($"slotmarks: chest {b.Container} closed");
    }

    // render thread, every frame while a chest screen shows
    private void DrawChestPanel(UiDraw ui, InventoryScreen s)
    {
        if (s.Container.Value == 0)
        {
            ui.TextDisabled("Waiting for the chest...");
            return;
        }
        ui.TextDisabled($"Chest {s.Container}");
        if (ui.Button("Count this chest"))
        {
            _chestLine = "asking the server...";
            if (_ch is null || !_ch.SendToServer(new ChestCountRequest(s.Container.Value)))
                _chestLine = Network.ServerHasKiln ? "the server lacks slotmarks" : "not connected to a Kiln server";
        }
        if (_chestLine.Length > 0) ui.TextWrapped(_chestLine);
    }

    private void OnChestAnswer(ChestCountAnswer a, NetContext ctx)
    {
        _chestLine = a.Ok ? $"{a.Stacks} stacks, {a.Items} items" : $"refused: {a.Why}";
        Log.Info($"slotmarks: chest {a.Chest}: {_chestLine}");
    }

    private void Describe(SlotClick click)
    {
        var text = $"slot {click.Slot.SlotIndex} of page {click.Slot.InventoryPage}: item {click.Item} x{click.Slot.Count}, " +
                   $"entity {click.ItemEntity}, holder {click.Holder}";
        Log.Info("slotmarks: " + text);
        Hud.Toast(text);
    }

    // ---- server (tick thread) ----
    private void OnChestCount(ChestCountRequest m, NetContext ctx)
    {
        var chest = new EntityId(m.Chest);
        var why = ChestRefusal(ctx.Player, chest);
        if (why is not null)
        {
            Log.Info($"slotmarks: refused {ctx.Sender.Name}: chest {chest}: {why}");
            ctx.Reply(new ChestCountAnswer(m.Chest, false, 0, 0, why));
            return;
        }
        var inv = Items.Read(chest)!;
        var stacks = inv.Slots.Where(s => !s.IsEmpty).ToList();
        ctx.Reply(new ChestCountAnswer(m.Chest, true, stacks.Count, stacks.Sum(s => (int)s.Count), ""));
    }

    // The checks: a real entity with an inventory, not a player (Items.Read reads a player's backpack too), near the sender.
    // A mod that changes the chest (Auto Loot's salvage) checks the same, and that the player may use it (claims, a base).
    private static string? ChestRefusal(EntityId player, EntityId chest)
    {
        if (chest.Value == 0 || player.Value == 0) return "no chest";
        if (Players.IsPlayer(chest)) return "that is a player";
        if (Items.Read(chest) is null) return "not a container";
        if (World.Position(player) is not { } at || World.Position(chest) is not { } where) return "position unknown";
        return where.DistanceTo(at) > Reach ? "too far away" : null;
    }

    private void OnRequest(MarkRequest m, NetContext ctx)
    {
        var player = ctx.Player;
        // the check: the item entity is in this player's own inventory now (not someone else's, not a made-up id)
        var owned = m.ItemEntity != 0 && player.Value != 0 &&
                    Items.Read(player)?.Slots.Any(s => s.ItemEntity.Value == m.ItemEntity) == true;
        if (!owned)
        {
            Log.Info($"slotmarks: refused {ctx.Sender.Name}: item {m.ItemEntity} is not in their inventory");
            return;
        }
        var key = (player, m.ItemEntity);
        var marked = _serverMarks.Add(key) || !_serverMarks.Remove(key);
        ctx.Reply(new MarkAnswer(m.ItemEntity, marked));
    }
}
