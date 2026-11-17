// SPDX-License-Identifier: MIT
using Kiln;

namespace ItemIconsCheck;

/// <summary>
/// The owner's real-client check of the game's item icons and names in Kiln windows (Ui.ItemImage, UiDraw.ItemIcon,
/// Items.DisplayName) and of a crafting panel at a production station's screen (CraftingScreen.IsFactory, Station).
/// <list type="bullet">
/// <item>F9 (or the pause menu's "Item icons check") opens a window: a grid of icons with their names at 32, 64 and 128 pixels,
/// for game items, an item whose icon is not square, an unknown id (the placeholder) and a new item this mod defines (its
/// base's icon, a log's).</item>
/// <item>At any crafting screen a panel "Icon check" shows what Kiln reports (AtStation, IsFactory, Workshop, Station), the
/// workshop's name and the icons of the station's outputs.</item>
/// </list>
/// Every finding is a kiln.log line "iconcheck: ...". On a dedicated server (a lab) the headless part runs alone and writes
/// <c>iconcheck: PASS|FAIL &lt;name&gt;</c> lines: the names from the server's own game data (English: a server has no texts),
/// no icons there, and the empty answers for a base no one has.
/// </summary>
public sealed class ItemIconsCheckMod : Mod
{
    private static readonly Guid NewLog = Guid.Parse("8d6e1c3a-5b7f-4f0e-9a21-4c0b6e9d2f71");

    private static readonly (string Label, ItemId Item)[] Shown =
    [
        ("Wood Logs", new ItemId(0x2b0cf1d5)),
        ("Charcoal", new ItemId(0x162cb6d2)),
        ("Bronze Sword", new ItemId(0x2b76680f)),
        ("Powder Ball", new ItemId(0x88175069)),
        ("Charcoal Kiln", new ItemId(0x15e4e52e)),
        ("Wizard chest", new ItemId(0xcd1535ce)),
        ("Map piece (360x388)", new ItemId(0xfaac460e)),
        ("Unknown 0xdeadbeef", new ItemId(0xdeadbeef)),
        ("New item (this mod)", new ItemId(NewContent.IdOf(NewLog))),
    ];

    private UiWindow? _window;
    private uint _lastStation = uint.MaxValue;

    private void Say(string s) => Log.Info($"iconcheck: {s}");

    public override void OnLoad()
    {
        // the server half (a hosting client runs both): one new item, cloned from the logs (its icon: the logs' until a PNG is served)
        if (NewContent.Available)
            try
            {
                var d = Content.Items.Register(new ItemDefinition
                {
                    Guid = NewLog, Name = "iconcheck_log", BaseOn = "Material_T1_Wood_Logs", DisplayName = "Icon Check Log",
                    Texts = new Dictionary<string, ItemTexts> { ["de"] = new() { Name = "Symbolprüfscheit" } },
                });
                Say($"new item {d}");
            }
            catch (Exception e) when (e is ArgumentException or KilnUnavailableException)
            {
                Say($"new item not defined: {e.Message}");
            }
        if (!Ui.Available)
        {
            Say($"no overlay here (dedicated server {Client.IsDedicatedServer}): Ui.ItemImage = {(Ui.ItemImage(Shown[0].Item) is null ? "null" : "an image")}");
            if (Client.IsDedicatedServer)
            {
                _headlessUntil = Environment.TickCount64 + 90_000;
                Context.Tick.Pre += Headless;
            }
            return;
        }
        Say($"Ui.ItemIconsAvailable = {Ui.ItemIconsAvailable}");
        _window = Ui.Window("Item icons check", Draw, new UiWindowOptions { Width = 760, Height = 620, StartOpen = false });
        KeyBindings.Register("iconcheck.toggle", "F9", () => _window.Toggle(), "Item icons check");
        Ui.Menus.Add(MenuLocation.PauseMenu, "Item icons check", () => _window.Open = true, order: 40);
        Ui.Crafting.AddPanel("Icon check", Panel);
        Context.Tick.Pre += OnPre;
    }

    // ---- the headless checks (a dedicated server) ----
    private long _headlessUntil;
    private readonly HashSet<string> _done = new();

    private void Check(string name, bool ok, string detail)
    {
        if (!_done.Add(name)) return;
        Say($"{(ok ? "PASS" : "FAIL")} {name}: {detail}");
    }

    private void Headless(in TickInfo t)
    {
        if (t.Step % 30 != 0) return;
        var late = Environment.TickCount64 > _headlessUntil;
        // a server has no overlay: no image, no icons
        Check("icons.server", Ui.ItemImage(Shown[0].Item) is null && !Ui.ItemIconsAvailable,
            $"Ui.ItemImage {(Ui.ItemImage(Shown[0].Item) is null ? "null" : "an image")}, ItemIconsAvailable {Ui.ItemIconsAvailable}");
        // names: the server's own data (registries, items); English (a server has no texts of the game's)
        var logs = Items.DisplayName(new ItemId(0x2b0cf1d5));
        Check("names.item", logs == "Wood Logs", $"Items.DisplayName(Material_T1_Wood_Logs) = {logs ?? "null"}");
        Check("names.unknown", Items.DisplayName(new ItemId(0xdeadbeef)) is null, "an id that is no item has no name");
        var shop = Stations.WorkshopName(0x2546ae49);
        if (shop != null || late) Check("names.workshop", shop == "Charcoal Kiln", $"Stations.WorkshopName(0x2546ae49) = {shop ?? "null"} (read from WorkshopRegistryResource)");
        var recipe = Stations.RecipeName(0x002c36a0);
        if (recipe != null || late) Check("names.recipe", recipe is { Length: > 0 }, $"Stations.RecipeName(0x002c36a0) = {recipe ?? "null"} (its first output's name)");
        var mine = Items.DisplayName(new ItemId(NewContent.IdOf(NewLog)));
        if (mine != null || late) Check("names.new-item", mine == "Icon Check Log", $"Items.DisplayName(this mod's new item) = {mine ?? "null"}");
        // a base no one has: empty answers, no KeyNotFoundException (0.2.0-dev.29.15)
        if (Stations.Available)
            try
            {
                var gone = new BaseId(Guid.NewGuid());
                Check("bases.unknown-empty", Bases.Stock(gone).Count == 0 && Stations.InBase(gone).Count == 0 && Bases.Storage(gone).Count == 0 &&
                    Bases.StockWithStations(gone).Count == 0, "Stock, InBase, Storage of an id no base has are empty");
            }
            catch (Exception e)
            {
                Check("bases.unknown-empty", false, $"{e.GetType().Name}: {e.Message}");
            }
        else if (late) Check("bases.unknown-empty", false, "Stations.Available is false on this server");
        // EntityData: the calling mod's own counts
        try
        {
            Check("saves.waiting", EntityData.Waiting >= 0 && EntityData.Matched >= 0, $"Waiting {EntityData.Waiting}, Matched {EntityData.Matched}");
        }
        catch (Exception e) when (e is KilnUnavailableException)
        {
            Check("saves.waiting", true, $"kiln.saves not here: {e.Message}");
        }
        if (_done.Count >= 8 || late)
        {
            Say($"headless checks done: {_done.Count} of 8");
            Context.Tick.Pre -= Headless;
        }
    }

    private int _namesLogged;
    private long _next;

    private void OnPre(in TickInfo t)
    {
        if (Environment.TickCount64 < _next) return;
        _next = Environment.TickCount64 + 5000;
        NamesOnce();
    }

    // the names come from the game's texts, read in the background on first use: log them until they are there
    private void NamesOnce()
    {
        if (_namesLogged >= 3) return;
        _namesLogged++;
        Say($"names ({GameLanguage.Current}): " + string.Join(", ", Shown.Select(s => $"{s.Label} = {Items.DisplayName(s.Item) ?? "null"}")));
        Say($"workshop 0x2546ae49 = {Stations.WorkshopName(0x2546ae49) ?? "null"}");
    }

    private void Draw(UiDraw ui)
    {
        ui.Text($"Ui.ItemIconsAvailable = {Ui.ItemIconsAvailable}, language {GameLanguage.Current}");
        foreach (var size in new[] { 32f, 64f, 128f })
        {
            ui.Separator($"{size} px");
            for (var i = 0; i < Shown.Length; i++)
            {
                if (i % 5 != 0) ui.SameLine();
                ui.ItemIcon(Shown[i].Item, size * ui.Scale);
                ui.Tooltip($"{Shown[i].Label}: {Items.DisplayName(Shown[i].Item) ?? "(no name)"} (0x{Shown[i].Item.Value:x8})");
            }
        }
        ui.Separator("names");
        foreach (var (label, item) in Shown)
        {
            var img = Ui.ItemImage(item);
            if (img != null && ui.ImageButton($"##b{item.Value:x8}", img, 24 * ui.Scale, 24 * ui.Scale)) Say($"clicked {label}");
            ui.SameLine();
            ui.Text($"{label}: {Items.DisplayName(item) ?? "(no name)"}");
        }
    }

    private void Panel(UiDraw ui, CraftingScreen c)
    {
        ui.Text($"AtStation {c.AtStation}, IsFactory {c.IsFactory}");
        ui.Text($"Workshop 0x{c.Workshop:x8} = {Stations.WorkshopName(c.Workshop) ?? "(no name)"}");
        ui.Text($"Station {c.Station}, recipe 0x{c.Recipe:x8}");
        if (c.Recipe != 0)
        {
            ui.ItemIcon(new ItemId(0), 0);  // the placeholder, next to the recipe's name
            ui.SameLine();
            ui.Text(Stations.RecipeName(c.Recipe) ?? "(no recipe name)");
        }
        var key = c.Station.Value ^ c.Workshop;
        if (key != _lastStation)
        {
            _lastStation = key;
            Say($"crafting screen: AtStation {c.AtStation}, IsFactory {c.IsFactory}, workshop 0x{c.Workshop:x8} ({Stations.WorkshopName(c.Workshop) ?? "no name"}), station {c.Station}");
        }
    }

    public override void OnUnload()
    {
        Context.Tick.Pre -= OnPre;
        Context.Tick.Pre -= Headless;
    }
}
