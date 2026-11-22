// SPDX-License-Identifier: MIT
// The lab test of station menus with tabs of their own (chapter 16a; docs/kiln/modding/world-blocks.md "Menus with tabs
// of their own"). On a dedicated server: a forge with a workshop of its own and an inline menu (two tabs, a heading, a
// station-list recipe and an item-side recipe named by MenuKey), a station with an empty menu, the store checks below
// (PASS/FAIL lines in kiln.log), and the control channel station-menus-test:ctl that menutest.py's fake clients drive:
// "define winter" (a layer of code over the inline menu), "remove winter", "remove all" (every layer of this mod: the
// tombstone), "status".
using System.Text.Json;
using Kiln;

namespace StationMenusTest;

public sealed class StationMenusTestMod : Mod
{
    private const string Me = "station-menus-test";
    private const string Channel = "station-menus-test:ctl";
    private BlockHandle<StationBlock>? _forge;
    private StationMenuRegistration? _winter;
    private NetChannel? _ctl;
    private bool _checked;
    private ulong _owner;
    private int _events;

    public override void OnLoad()
    {
        _owner = BlocksBridge.CallerOwner();  // the layers this mod stores natively are under its owner
        var twigs = "Material_T1_Twigs";
        _forge = Blocks.Define("forge", new StationBlock
        {
            Name = "Menu Forge", Kind = StationKind.Forge,
            Processing = new ProcessingBlock { Slots = 8 },
            Recipe = new RecipeBlock { Station = StationKind.Workbench.Workshop, Needs = { [twigs] = 1 } },
            Menu = new MenuBlock
            {
                Like = StationKind.Workbench,
                Tabs =
                [
                    new StationMenuTabBlock
                    {
                        Key = "metal", Name = new UiText(new Dictionary<string, string> { ["en"] = "Metal", ["de"] = "Metall" }), IconFromTab = 0,
                        Sets = [new StationMenuSetBlock { Key = "main", Recipes = ["reclaim", "bolts"] }],
                    },
                    new StationMenuTabBlock
                    {
                        Key = "fine", Name = "Fine",
                        Sets =
                        [
                            new StationMenuSetBlock { Key = "main", Recipes = ["wire"] },
                            new StationMenuSetBlock { Key = "plates", Heading = "Fine work", Recipes = ["plate"] },
                        ],
                    },
                ],
            },
            Recipes =
            [
                new RecipeBlock { MenuKey = "reclaim", Seconds = 2, Needs = { ["Material_T1_Metal_Sheets"] = 1 }, Output = "Material_T1_Metal_Scraps", Makes = 2 },
                new RecipeBlock { MenuKey = "wire", Seconds = 2, Needs = { ["Material_T1_Metal_Scraps"] = 2 }, Output = "Material_T1_Metal_Sheets" },
                new RecipeBlock { MenuKey = "plate", Seconds = 3, Needs = { ["Material_T1_Metal_Scraps"] = 4 }, Output = "Material_T1_Metal_Sheets" },
            ],
        });
        Blocks.Define("bolts", new ItemBlock
        {
            Name = "Menu Bolts", Stack = 50,
            Visual = new VisualBlock { Like = "Material_T1_Metal_Scraps" },
            Recipe = new RecipeBlock { Station = _forge, MenuKey = "bolts", Seconds = 2, Needs = { ["Material_T1_Metal_Scraps"] = 1 } },
        });
        // a workshop of its own with a menu and no recipe at all: zero-recipe menus are legal and reach clients
        Blocks.Define("empty-bench", new StationBlock
        {
            Name = "Empty Bench", Kind = StationKind.DryingRack, OwnWorkshop = true,
            Processing = new ProcessingBlock { Slots = 4 },
            Recipe = new RecipeBlock { Station = StationKind.Workbench.Workshop, Needs = { [twigs] = 1 } },
            Menu = new MenuBlock
            {
                Like = StationKind.DryingRack,
                Tabs = [new StationMenuTabBlock { Key = "all", Name = "All", Sets = [new StationMenuSetBlock { Key = "main", Recipes = [] }] }],
            },
        });
        // an icon the drying rack's menu does not have (it has fewer than 15 tabs): a game client refuses this menu
        // (IconSourceMissing) and lists the station's recipes the old way (client check 16a-c6)
        Blocks.Define("bad-icon-rack", new StationBlock
        {
            Name = "Bad Icon Rack", Kind = StationKind.DryingRack, OwnWorkshop = true,
            Processing = new ProcessingBlock { Slots = 4 },
            Recipe = new RecipeBlock { Station = StationKind.Workbench.Workshop, Needs = { [twigs] = 1 } },
            Menu = new MenuBlock
            {
                Like = StationKind.DryingRack,
                Tabs = [new StationMenuTabBlock { Key = "far", Name = "Far", IconFromTab = 14, Sets = [new StationMenuSetBlock { Key = "main", Recipes = ["sticks"] }] }],
            },
            Recipes = [new RecipeBlock { MenuKey = "sticks", Seconds = 2, Needs = { [twigs] = 2 }, Output = "Material_T1_Wood_Logs" }],
        });
        Events.On<StationMenuDefinitionChangedEvent>(e => { if (e.ModId == Me) _events++; });
        Events.On<StationMenuAppliedEvent>(e =>
            Log.Info($"{Me}: client menu of workshop 0x{e.Workshop:x8} {(e.FromServer ? "from the server" : "of this game")} generation {e.Generation}: " +
                     (e.Ok ? "built" : "refused " + e.Reason)));
        _ctl = Network.Channel(Channel, "1", required: false)
            .Register<string>(0, static (m, w) => w.String(m), static (ref NetReader r) => r.String(256), OnControl, NetDirection.ToServer)
            .Register<Reply>(1, static (m, w) => w.String(m.Text), static (ref NetReader r) => new Reply(r.String(4096)), (_, _) => { }, NetDirection.ToClient);
        Context.Tick.Pre += OnTick;
    }

    private sealed record Reply(string Text);

    private void Say(bool ok, string name, string detail = "") =>
        Log.Info($"{Me}: {(ok ? "PASS" : "FAIL")} {name}{(ok || detail.Length == 0 ? "" : ": " + detail)}");

    private static string? ExportRecord(string definitionKey)
    {
        var export = StationMenuStore.Backend.Export();
        if (export is null) return null;
        using var doc = JsonDocument.Parse(export);
        foreach (var m in doc.RootElement.GetProperty("menus").EnumerateArray())
            if (m.GetProperty("definition").GetProperty("key").GetString() == definitionKey && m.GetProperty("definition").GetProperty("mod").GetString() == Me)
                return m.GetRawText();
        return null;
    }

    // the owner's client pass: kiln/config/station-menus-test.json {"winter": true} puts the winter layer on, false takes it off
    private bool? _fileWinter;

    private void FileTrigger()
    {
        try
        {
            var file = Path.Combine(Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "..")), "config", Me + ".json");
            if (!File.Exists(file)) return;
            using var doc = JsonDocument.Parse(File.ReadAllText(file));
            var on = doc.RootElement.TryGetProperty("winter", out var w) && w.ValueKind == JsonValueKind.True;
            if (_fileWinter == on) return;
            _fileWinter = on;
            Log.Info($"{Me}: kiln/config/{Me}.json winter {on}: {(on ? DefineWinter() : RemoveWinter())}");
        }
        catch (Exception e) when (e is IOException or JsonException)
        {
            Log.Warn($"{Me}: kiln/config/{Me}.json unreadable: {e.Message}");
        }
    }

    private void OnTick(in TickInfo tick)
    {
        if (Kiln.Client.IsClient && !Kiln.Client.IsHosting) return;  // the checks and the trigger run where the server world is
        if (_checked && tick.Step % 50 == 25) FileTrigger();
        if (_checked || tick.Step % 10 != 0 || _forge is null) return;
        var inline = ExportRecord("forge.menu");
        if (inline is null || ExportRecord("empty-bench.menu") is null)
        {
            if (tick.Step > 6000)
            {
                _checked = true;
                Say(false, "inline-menus", "the inline menus were not stored within 6000 steps");
            }
            return;
        }
        _checked = true;
        Say(true, "inline-menus", "");
        var caps = Capabilities.Get(StationMenuStore.StoreRow);
        Say(caps.State == CapabilityState.Available, "store-on-server-without-ui", caps.ToString());
        var render = Capabilities.Get(StationMenuStore.RenderRow);
        Say(render.State == CapabilityState.Unavailable && render.Reason == CapabilityReason.WrongSide, "render-wrong-side-on-server", render.ToString());

        // define -> export -> define again (same owner, the exported bytes) is "unchanged"; remote_replace takes the export
        var owner = _owner;
        var (rc, _, gen0, state, error) = StationMenuStore.Backend.Define(owner, inline);
        var exported = StationMenuStore.Backend.Export()!;
        var (rrc, rerr) = StationMenuStore.Backend.RemoteReplace(owner, exported);
        StationMenuStore.Backend.RemoteClear(owner);
        Say(rc == 0 && state == "unchanged" && rrc == 0, "menu-wire-roundtrip", $"define rc {rc} {state} {error}; remote rc {rrc} {rerr}");

        // the same definition twice: one generation, one event
        var before = _events;
        var a = StationMenus.Define(_forge, Winter());
        var b = StationMenus.Define(_forge, Winter());
        Say(a.Ok && b.Ok && a.Generation == b.Generation && _events == before + 1, "replay-idempotent",
            $"{a.Ok}/{b.Ok} {a.Reason} {b.Reason} gen {a.Generation}/{b.Generation} events {_events - before}");
        // the newer layer wins its workshop; taking it off shows the inline menu again
        var winterShown = ExportRecord("forge.winter") is not null && ExportRecord("forge.menu") is null;
        a.Registration?.Dispose();
        Say(winterShown && ExportRecord("forge.menu") is not null, "owner-reveal", $"winter shown {winterShown}");
        Log.Info($"{Me}: checks done");
    }

    private static StationMenuDefinition Winter() => new()
    {
        Key = "forge.winter",
        Tabs = [new StationMenuTabBlock { Key = "winter", Name = "Winter", IconFromTab = 1, Sets = [new StationMenuSetBlock { Key = "main", Recipes = ["bolts"] }] }],
    };

    private void OnControl(string cmd, NetContext ctx)
    {
        string reply;
        try
        {
            reply = cmd switch
            {
                "define winter" => DefineWinter(),
                "remove winter" => RemoveWinter(),
                "remove all" => RemoveAll(),
                "status" => $"menus {StationMenuStore.Count} generation {StationMenuStore.Generation}",
                "restore" => "ok",
                _ => "unknown",
            };
        }
        catch (Exception e)
        {
            reply = $"error {e.GetType().Name}: {e.Message}";
        }
        Log.Info($"{Me}: ctl {cmd}: {reply}");
        ctx.Reply(new Reply(reply));
    }

    private string DefineWinter()
    {
        var r = StationMenus.Define(_forge!, Winter());
        if (!r.Ok) return "refused " + r.Reason;
        _winter = r.Registration;
        return "ok";
    }

    private string RemoveWinter()
    {
        _winter?.Dispose();
        _winter = null;
        return "ok";
    }

    private string RemoveAll()
    {
        RemoveWinter();
        StationMenuStore.RemoveOwner(_owner);
        return "ok";
    }
}
