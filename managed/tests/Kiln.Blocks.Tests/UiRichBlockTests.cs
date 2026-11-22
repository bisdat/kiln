// SPDX-License-Identifier: MIT
// The richer UI blocks of 0.2.0-dev.31 (dossier 09; docs/kiln/modding/ui-blocks.md): the new rows and their JSON, ids and
// limits, typed exact bindings and Unavailable, window state and its versions, mods' own data, tables/tabs/trees/fields
// through a recording canvas, the live-values demand, and the picture checks. No game: fakes for Kiln.Ui, the world
// collector and 05b's stat source.
using System.Collections.Immutable;
using System.Text;
using System.Text.Json;
using Xunit;

namespace Kiln.BlockTests;

[Collection("ui blocks")]
public sealed class UiRichBlockTests : IDisposable
{
    private const string Mod = "rich-test";

    // ---- fakes ----------------------------------------------------------------------------------------------------

    private sealed class Surface(WindowRuntime w) : IUiSurface
    {
        public WindowRuntime Window => w;
        public bool IsOpen { get; private set; }
        public void Open() => IsOpen = true;
        public void Close() => IsOpen = false;
        public void Dispose() { }
    }

    private sealed class Hud(string text) : IUiHud
    {
        public string Text { get; set; } = text;
        public bool Removed;
        public void Remove() => Removed = true;
    }

    private sealed class Game : IUiBackend
    {
        public bool Available { get; set; } = true;
        public string Language => "en-us";
        public bool JoinedServer { get; set; }
        public ulong WorldSession { get; set; } = 1;
        public List<Hud> Huds { get; } = new();
        public List<string> Toasts { get; } = new();
        public List<string> Commands { get; } = new();
        public IUiSurface CreateSurface(WindowRuntime window, string title) => new Surface(window);
        public IDisposable? AddMenu(UiMenuIn where, UiText text, int order, Action onClick) => null;
        public IUiHud ShowHud(string id, string text, HudAnchor anchor, Color color) { var h = new Hud(text); Huds.Add(h); return h; }
        public IDisposable? BindKey(KeyPlan plan, string category, Action onPress) => null;
        public void Toast(string text, ToastKind kind) => Toasts.Add(text);
        public void RequestCast(string abilityId) { }
        public (bool Ok, string Message) RunCommand(string line) { Commands.Add(line); return (true, ""); }
    }

    /// <summary>Records the widgets of a frame; what the "player" does is set up before it.</summary>
    private sealed class Canvas : IUiCanvas
    {
        public bool CanEditText { get; set; } = true;
        public List<string> Calls { get; } = new();
        public Dictionary<string, (string Text, UiItemState State, bool Enter)> Typing { get; } = new();
        public HashSet<string> Clicks { get; } = new();
        public string? ShowTab;   // the tab ImGui shows this frame (null: the forced one, else the first)
        public HashSet<string> Toggle { get; } = new();
        public string? ThrowOn;
        public int Pushes, Pops, Nodes, TreePops, Tables, TableEnds, Bars, BarEnds, Tabs, TabEnds;

        public void Text(string text, Color? color, bool wrap) { if (ThrowOn != null && text.Contains(ThrowOn)) throw new InvalidOperationException("boom"); Calls.Add("text:" + text); }
        public void Disabled(string text) => Calls.Add("disabled:" + text);
        public bool Button(string label) { Calls.Add("button:" + label); return Clicks.Remove(label); }
        public bool Checkbox(string label, ref bool value) { Calls.Add($"checkbox:{label}={value}"); if (!Clicks.Remove(label)) return false; value = !value; return true; }
        public bool SliderInt(string label, ref int value, int min, int max) { Calls.Add($"sliderInt:{label}={value}"); return false; }
        public bool SliderFloat(string label, ref float value, float min, float max) { Calls.Add($"sliderFloat:{label}={value}"); return false; }
        public bool Combo(string label, ref int index, IReadOnlyList<string> items) => false;
        public void Separator(string? heading) => Calls.Add("separator");
        public void SameLine() => Calls.Add("sameLine");
        public void Tooltip(string text) { }
        public void PushId(string id) => Pushes++;
        public void PopId() => Pops++;

        public bool TextField(string label, ref string text, int maxBytes, bool enterCommits, bool multiline, out UiItemState state)
        {
            Calls.Add($"field:{label}={text}");
            state = UiItemState.None;
            if (!Typing.Remove(label, out var t)) return false;
            text = t.Text;
            state = t.State;
            return t.Enter;
        }

        public bool BeginTable(string id, IReadOnlyList<string> headers) { Tables++; Calls.Add("table:" + string.Join("|", headers)); return true; }
        public bool TableRow(string key, IReadOnlyList<string> cells, bool selectable, bool selected)
        {
            Calls.Add($"row:{key}:{string.Join("|", cells)}{(selected ? "*" : "")}");
            return Clicks.Remove("row:" + key);
        }
        public void EndTable() => TableEnds++;
        public bool BeginTabBar(string id) { Bars++; return true; }
        private string? _forced;
        private bool _shownThisBar;
        public bool BeginTab(string label, bool select)
        {
            Tabs++;
            var id = label[(label.IndexOf("###", StringComparison.Ordinal) + 3)..];
            if (select) _forced = id;
            var shown = ShowTab ?? _forced;
            var open = shown == null ? !_shownThisBar : shown == id;
            if (open) { _shownThisBar = true; Calls.Add("tab:" + id); }
            else Tabs--;
            return open;
        }
        public void EndTab() => TabEnds++;
        public void EndTabBar() { BarEnds++; _shownThisBar = false; }
        public bool TreeNode(string label, bool open, out bool clicked)
        {
            var key = label;
            clicked = Clicks.Remove("node:" + key);
            var now = Toggle.Remove(key) ? !open : open;
            Calls.Add($"node:{key}{(now ? "+" : "-")}");
            if (now) Nodes++;
            return now;
        }
        public void TreePop() => TreePops++;
        public bool Selectable(string label, bool selected) { Calls.Add($"leaf:{label}{(selected ? "*" : "")}"); return Clicks.Remove("leaf:" + label); }
        public bool Image(UiImage image, float width, float height) => false;
    }

    private readonly Game _game = new();
    private long _now = 1_000;
    private UiWorldReadResult _world = new(false, "NoWorld", null);
    private readonly List<(string Mod, UiWorldFields Fields)> _demands = new();
    private readonly List<KilnEvent> _events = new();
    private readonly List<IDisposable> _subs = new();

    private readonly Func<UiWorldReadResult> _read0 = UiWorldSource.Read;
    private readonly Func<string, UiWorldFields, UiDemandRegistration> _demand0 = UiWorldSource.Demand;
    private readonly Func<bool> _hosting0 = UiWorldSource.Hosting;

    public UiRichBlockTests()
    {
        UiRuntime.ResetForTests(_game);
        UiRuntime.Clock = () => _now;
        UiRuntime.SubscribeTick = _ => new Sub(() => { });
        UiCompile.Later = (_, _) => { };
        UiConfigs.OfMod = _ => [];
        UiConfigs.Find = _ => null;
        UiWorldSource.Read = () => _world;
        UiWorldSource.Demand = (mod, f) =>
        {
            var e = (mod, f);
            _demands.Add(e);
            return new UiDemandRegistration(true, null, 1, () => _demands.Remove(e));
        };
        UiWorldSource.Hosting = () => true;
        UiStats.Metadata = key => key.Mod == Mod && key.Id == "xp" ? Def(key) : null;
        UiStats.Read = (reader, key) => new ModValueReadResult(false, "NoValue", null);
        _subs.Add(Events.On<UiWindowStateChangedEvent>(e => _events.Add(e)));
        _subs.Add(Events.On<UiControlStateChangedEvent>(e => _events.Add(e)));
        _subs.Add(Events.On<UiWindowStateResetEvent>(e => _events.Add(e)));
        _subs.Add(Events.On<UiBindingValueChangedEvent>(e => _events.Add(e)));
    }

    public void Dispose()
    {
        foreach (var s in _subs) s.Dispose();
        UiRuntime.ResetForTests(new KilnUiBackend());
        UiRuntime.SubscribeTick = static tick => BlocksBridge.AddTick("kiln blocks ui", tick);
        UiConfigs.OfMod = static mod => [.. Config.All.Where(f => f.ModId == mod).Select(f => (IUiConfig)new RealConfig(f))];
        UiConfigs.Find = static id => Config.Find(id) is { } f ? new RealConfig(f) : null;
        UiCompile.Later = BlocksBridge.After;
        UiWorldSource.Read = _read0;
        UiWorldSource.Demand = _demand0;
        UiWorldSource.Hosting = _hosting0;
    }

    private sealed class Sub(Action a) : IDisposable { public void Dispose() => a(); }

    private static ModValueDefinition Def(ModValueKey key) => new(key, ModValueKind.Counter, "XP", "0", 0, long.MaxValue, 0, 2, [],
        ModValuePersistence.World, 1, 1, ModValueReadPolicy.OwnerOnly, [], ModValueSyncPolicy.OwnerMod, true);

    private static ModBlocks NewMod() => new(Mod, 1) { ManifestClients = "mod" };

    private static void Define<T>(string name, T block, ModBlocks? mod = null) where T : class, IBlock
    {
        mod ??= NewMod();
        var (handle, low, _) = Compiler.Lower(mod, name, block);
        mod.Handles[name] = handle;
        low.Commit();
    }

    private static BlockError Refused<T>(T block) where T : class, IBlock => Assert.Throws<BlockError>(() => Compiler.Lower(NewMod(), "w", block));

    private static WindowBlock W(IReadOnlyDictionary<string, UiStateDefinition>? state, params UiRow[] rows) => new() { Title = "Rich", Rows = rows, State = state };

    private static T Json<T>(string json) where T : class, IBlock => BlockJson.Parse<T>(json);

    private WindowRuntime Open(string name = "w")
    {
        var w = UiRuntime.FindWindow(Mod, name)!;
        w.Open();
        return w;
    }

    private Canvas Frame(WindowRuntime w, Canvas? c = null, bool refresh = true)
    {
        c ??= new Canvas();
        if (refresh) _now += 10_000;
        w.Draw(c);
        return c;
    }

    private static UiWorldSnapshot Snap(uint mask = 1 | 2 | 4, uint health = 413, IReadOnlyList<UiInventoryValue>? inv = null, ulong session = 7) =>
        new(1, mask, new UiSnapshotStamp(session, 3, 0), new UiSnapshotStamp(0, 0, 0), 42, health, 500, 10, 20, 1, 2,
            (long)(12.5 * 4294967296), -4294967296, 0, 0m, inv ?? [], new string?[] { null, null, null, "DeferredSource" });

    // ---- shapes ---------------------------------------------------------------------------------------------------

    private const string RichJson = """
        {
          "title": "Camp", "kind": "screen",
          "state": {
            "note": { "type": "string", "maxBytes": 64 },
            "big": { "type": "int64", "initial": "9007199254740993" },
            "price": { "type": "decimal", "initial": "1.25", "min": "0", "max": "100", "scale": 2 },
            "open": { "type": "stringSet", "initial": [] },
            "pick": { "type": "string" }
          },
          "rows": [
            { "type": "input", "id": "note", "label": "Note", "bind": "state.note", "maxBytes": 64, "commit": "enter" },
            { "type": "tabs", "id": "tabs", "tabs": [
              { "id": "bag", "label": "Bag", "rows": [
                { "type": "table", "id": "inv", "items": "world.local.inventory", "selected": "state.pick", "maxRows": 4,
                  "columns": [ { "id": "item", "label": "Item", "cell": "{row.item}" }, { "id": "n", "label": "Count", "cell": "{row.count}" } ] } ] },
              { "id": "map", "label": "Map", "when": "world.local.available", "rows": [
                { "type": "tree", "id": "places", "expanded": "state.open", "fields": [ { "id": "level", "type": "int64" } ],
                  "roots": [ { "key": "a", "label": "A", "values": { "level": 1 }, "children": [ { "key": "a1", "label": "A1", "values": { "level": "2" } } ] } ] } ] } ] },
            { "type": "value", "label": "Big", "bind": "state.big" }
          ]
        }
        """;

    [Fact]
    public void New_rows_read_from_JSON_and_write_back_the_same()
    {
        var w = Json<WindowBlock>(RichJson);
        Assert.IsType<InputRow>(w.Rows[0]);
        var tabs = Assert.IsType<TabsRow>(w.Rows[1]);
        Assert.IsType<TableRow>(tabs.Tabs[0].Rows[0]);
        Assert.IsType<TreeRow>(tabs.Tabs[1].Rows[0]);
        Assert.Equal(9007199254740993L, ((UiInt64State)w.State!["big"]).Initial);
        Assert.Equal(1.25m, ((UiDecimalState)w.State!["price"]).Initial);
        var options = new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase, Converters = { new System.Text.Json.Serialization.JsonStringEnumConverter(JsonNamingPolicy.CamelCase) },
            DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull };
        var once = JsonSerializer.Serialize(w, options);
        Assert.Contains("\"initial\":\"9007199254740993\"", once);
        var twice = JsonSerializer.Serialize(Json<WindowBlock>(once), options);
        Assert.Equal(once, twice);
    }

    [Theory]
    [InlineData("""{ "title": "T", "state": { "n": { "type": "int64", "initial": 5 } }, "rows": [ { "type": "text", "text": "x" } ] }""")]
    [InlineData("""{ "title": "T", "state": { "n": { "type": "decimal", "initial": "1e3" } }, "rows": [ { "type": "text", "text": "x" } ] }""")]
    [InlineData("""{ "title": "T", "state": { "n": { "type": "int64", "initial": "007" } }, "rows": [ { "type": "text", "text": "x" } ] }""")]
    public void State_numbers_are_canonical_strings(string json) => Assert.ThrowsAny<JsonException>(() => Json<WindowBlock>(json));

    [Fact]
    public void The_rich_window_compiles_and_lowers()
    {
        Define("w", Json<WindowBlock>(RichJson));
        var w = UiRuntime.FindWindow(Mod, "w")!;
        Assert.Equal(UiWorldFields.LocalInventory, w.Plan.World & UiWorldFields.LocalInventory);
        Assert.Equal(9007199254740993L, ((UiStateValue.Int64)w.State.Snapshot.Values["big"]).Value);
    }

    // ---- compile errors -----------------------------------------------------------------------------------------

    [Fact]
    public void Ids_limits_and_shapes_are_compile_errors_by_field()
    {
        var text = new TextRow { Text = "x" };
        var state = new Dictionary<string, UiStateDefinition> { ["s"] = new UiStringState(), ["n"] = new UiInt64State(0), ["set"] = new UiStringSetState([]) };
        Assert.Contains("needs the keyboard", Refused(W(state, new InputRow { Id = "f", Label = "F", Bind = "state.s" }) with { Interactive = false }).Problem);
        Assert.Equal("rows[1].id", Refused(W(state, new InputRow { Id = "f", Label = "F", Bind = "state.s" }, new ImageRow { Id = "f", Asset = "x.png" })).Field);
        Assert.Equal("rows[0].id", Refused(W(state, new InputRow { Id = "Bad", Label = "F", Bind = "state.s" })).Field);
        Assert.Contains("string state entry", Refused(W(state, new InputRow { Id = "f", Label = "F", Bind = "state.n" })).Problem);
        Assert.Contains("is not rows", Refused(W(state, new TableRow { Id = "t", Items = "state.s", Columns = [new("c", "C", "x")] })).Problem);
        Assert.Contains("more than 16", Refused(W(state, new TableRow { Id = "t", Items = "world.local.inventory",
            Columns = [.. Enumerable.Range(0, 17).Select(i => new UiColumn($"c{i}", "C", "x"))] })).Problem);
        Assert.Equal("rows[0].tabs[1].id", Refused(W(state, new TabsRow { Id = "t", Tabs = [new("a", "A", [text]), new("a", "B", [text])] })).Field);
        Assert.Contains("exactly one of", Refused(W(state, new TreeRow { Id = "t" })).Problem);
        Assert.Contains("twice", Refused(W(state, new TreeRow { Id = "t", Roots = [new("a", "A"), new("a", "B")] })).Problem);
        Assert.Contains("text and value rows only", Refused(W(state, new TreeRow { Id = "t", Roots = [new("a", "A")],
            NodeRows = [new ButtonRow { Label = "B", Does = new UiAction { Toast = "x" } }] })).Problem);
        Assert.Contains("only inside a table", Refused(W(state, new TextRow { Text = "{row.item}" })).Problem);
        Assert.Contains("stringSet", Refused(W(state, new TreeRow { Id = "t", Roots = [new("a", "A")], Expanded = "state.s" })).Problem);
    }

    [Fact]
    public void A_condition_compares_with_a_literal_of_its_own_type()
    {
        var state = new Dictionary<string, UiStateDefinition> { ["n"] = new UiInt64State(0) };
        Assert.Contains("whole number", Refused(W(state, new TextRow { Text = "x", When = "state.n > 0.5" })).Problem);
        Assert.Contains("whole number", Refused(W(state, new TextRow { Text = "x", When = "world.local.health > 0.5" })).Problem);
    }

    [Fact]
    public void Stats_need_a_description_or_an_import()
    {
        var ok = W(null, new TextRow { Text = "{stats.rich-test/xp:N0}" });
        Define("w", ok);
        Assert.Contains("has no stat", Refused(W(null, new TextRow { Text = "{stats.rich-test/nope}" })).Problem);
        Assert.Contains("needs an import", Refused(W(null, new TextRow { Text = "{stats.other/xp}" })).Problem);
        using var import = new Sub(UiStats.Import(Mod, new ModValueKey("other", "xp")));
        Define("w2", W(null, new TextRow { Text = "{stats.other/xp}" }));
    }

    [Fact]
    public void State_declarations_are_checked()
    {
        Assert.Equal("state.Bad", Refused(W(new Dictionary<string, UiStateDefinition> { ["Bad"] = new UiBoolState() }, new TextRow { Text = "x" })).Field);
        Assert.Contains("digits after the point", Refused(W(new Dictionary<string, UiStateDefinition> { ["d"] = new UiDecimalState(1.234m, 0, 10, 2) }, new TextRow { Text = "x" })).Problem);
        Assert.Contains("outside", Refused(W(new Dictionary<string, UiStateDefinition> { ["n"] = new UiInt64State(5, 0, 3) }, new TextRow { Text = "x" })).Problem);
    }

    // ---- typed values -------------------------------------------------------------------------------------------

    [Fact]
    public void An_int64_beyond_a_double_formats_and_compares_exactly()
    {
        var state = new Dictionary<string, UiStateDefinition> { ["big"] = new UiInt64State(9007199254740993) };
        Define("w", W(state, new TextRow { Text = "v={state.big}" }, new TextRow { Text = "eq", When = "state.big == 9007199254740993" },
            new TextRow { Text = "ne", When = "state.big == 9007199254740992" }));
        var c = Frame(Open());
        Assert.Contains("text:v=9007199254740993", c.Calls);
        Assert.Contains("text:eq", c.Calls);
        Assert.DoesNotContain("text:ne", c.Calls);
    }

    [Fact]
    public void Unavailable_shows_a_dash_and_fails_every_test()
    {
        Define("w", W(null, new TextRow { Text = "hp {world.local.health}" }, new TextRow { Text = "not", When = "!world.local.health" },
            new TextRow { Text = "ne", When = "world.local.health != 5" }, new TextRow { Text = "avail", When = "world.local.health.available" },
            new TextRow { Text = "absent", When = "!world.local.health.available" }));
        var c = Frame(Open());
        Assert.Contains("text:hp \u2014", c.Calls);
        Assert.DoesNotContain("text:not", c.Calls);
        Assert.DoesNotContain("text:ne", c.Calls);
        Assert.DoesNotContain("text:avail", c.Calls);
        Assert.Contains("text:absent", c.Calls);
        _world = new UiWorldReadResult(true, null, Snap());
        c = Frame(UiRuntime.FindWindow(Mod, "w")!);
        Assert.Contains("text:hp 413", c.Calls);
        Assert.Contains("text:avail", c.Calls);
    }

    [Fact]
    public void Positions_are_metres_to_six_places_and_joined_host_time_is_deferred()
    {
        _world = new UiWorldReadResult(true, null, Snap());
        Define("w", W(null, new TextRow { Text = "x={world.local.position.x} y={world.local.position.y} h={world.server.hours}" }));
        var c = Frame(Open());
        Assert.Contains("text:x=12.5 y=-1 h=\u2014", c.Calls);
        Assert.Equal(1.000000m, UiRefreshContext.Metres(4294967296));
        Assert.Equal(0.000000m, UiRefreshContext.Metres(1));
    }

    [Fact]
    public void Old_config_ints_are_exact_and_still_compare_with_any_number()
    {
        var cfg = new FakeIntConfig();
        UiConfigs.OfMod = _ => [cfg];
        UiConfigs.Find = _ => cfg;
        var b = UiBindings.Resolve(Mod, "config.size");
        Assert.Equal(UiValueType.Int64, b.Type);
        Assert.Equal(14L, UiBindings.Read(b));
        var cond = UiCondition.Parse("config.size > 13.5", s => UiBindings.Resolve(Mod, s));
        Assert.True(cond.Test(UiBindings.Read));
    }

    private sealed class FakeIntConfig : IUiConfig
    {
        public string Id => "c";
        public bool ServerKind => false;
        public IReadOnlyList<ConfigEntry> Entries { get; } = [new("size", "int", null, 14, 14, 14, 8, 32, [], false, false)];
        public object? Get(string path) => 14;
        public bool IsFromServer(string path) => false;
        public bool TrySet(string path, object? value, out string error) { error = ""; return true; }
    }

    // ---- state ---------------------------------------------------------------------------------------------------

    [Fact]
    public async Task State_writes_commit_on_the_pump_with_one_revision_and_events()
    {
        Define("w", W(new Dictionary<string, UiStateDefinition> { ["n"] = new UiInt64State(1, 0, 10) }, new TextRow { Text = "{state.n}" }));
        var s = UiRuntime.FindWindow(Mod, "w")!.State;
        var v0 = s.Snapshot.Version;
        var same = s.Enqueue(UiWindowState.WriteKind.Value, "n", new UiStateValue.Int64(1), null, null, UiStateChangeCause.Api);
        var changed = s.Enqueue(UiWindowState.WriteKind.Value, "n", new UiStateValue.Int64(5), null, null, UiStateChangeCause.Api);
        var stale = s.Enqueue(UiWindowState.WriteKind.Value, "n", new UiStateValue.Int64(6), null, v0, UiStateChangeCause.Api);
        UiRuntime.Pump();
        Assert.True((await same).Ok);
        Assert.Equal(v0, (await same).Snapshot!.Version);
        Assert.Equal(v0.Revision + 1, (await changed).Snapshot!.Version.Revision);
        Assert.StartsWith(UiStateReasons.StaleVersion, (await stale).Reason);
        var e = Assert.Single(_events.OfType<UiWindowStateChangedEvent>());
        Assert.Equal(new UiStateValue.Int64(5), e.After);
    }

    [Fact]
    public async Task A_new_world_resets_state_with_a_new_session_and_refuses_old_writes()
    {
        Define("w", W(new Dictionary<string, UiStateDefinition> { ["s"] = new UiStringState("a") }, new TextRow { Text = "{state.s}" }));
        var s = UiRuntime.FindWindow(Mod, "w")!.State;
        UiRuntime.Pump();
        var first = s.Enqueue(UiWindowState.WriteKind.Value, "s", new UiStateValue.String("b"), null, null, UiStateChangeCause.Api);
        UiRuntime.Pump();
        Assert.Equal(new UiStateValue.String("b"), s.Snapshot.Values["s"]);
        var old = s.Snapshot.Version;
        var queued = s.Enqueue(UiWindowState.WriteKind.Value, "s", new UiStateValue.String("c"), null, null, UiStateChangeCause.Api);
        _game.WorldSession = 2;
        UiRuntime.Pump();
        Assert.StartsWith(UiStateReasons.StaleSession, (await queued).Reason);
        Assert.Equal(new UiStateValue.String("a"), s.Snapshot.Values["s"]);
        Assert.NotEqual(old.Session, s.Snapshot.Version.Session);
        Assert.Equal(0ul, s.Snapshot.Version.Revision);
        Assert.Single(_events.OfType<UiWindowStateResetEvent>());
        Assert.True((await first).Ok);
    }

    [Fact]
    public async Task An_edit_over_the_window_budget_is_refused_and_the_snapshot_kept()
    {
        Define("w", W(new Dictionary<string, UiStateDefinition> { ["s"] = new UiStringSetState([], 512, 128) }, new TextRow { Text = "x" }));
        var s = UiRuntime.FindWindow(Mod, "w")!.State;
        var before = s.Snapshot;
        // within the entry's limits but not the window's: 512 items of 128 bytes is 64 KiB; four entries' worth is checked by the window
        var big = Enumerable.Range(0, 512).Select(i => i.ToString("D4") + new string('x', 124)).ToList();
        Assert.Null(UiWindowState.Problem(new UiStringSetState([], 512, 128), new UiStateValue.StringSet(big)));
        var r = s.Enqueue(UiWindowState.WriteKind.Value, "s", new UiStateValue.StringSet(big), null, null, UiStateChangeCause.Api);
        UiRuntime.Pump();
        Assert.True((await r).Ok);  // 66 KiB fits the 256 KiB window
        Assert.NotSame(before, s.Snapshot);
    }

    // ---- mods' own data -------------------------------------------------------------------------------------------

    private static UiDataRow Row(string key, string name) => new(key, new Dictionary<string, UiScalar> { ["name"] = new UiScalar.String(name) });

    [Fact]
    public async Task Publications_commit_on_the_pump_and_a_bad_one_keeps_the_last()
    {
        using var reg = UiDataRegistry.Define(Mod, "quests", new UiDataSchema(UiDataShape.Rows, Fields: [new("name", UiScalarType.String)]));
        Assert.True(reg.Ok);
        var first = UiDataRegistry.Publish(Mod, "quests", new UiDataSnapshot.Rows([Row("q1", "Spring")]));
        var second = UiDataRegistry.Publish(Mod, "quests", new UiDataSnapshot.Rows([Row("q1", "Spring"), Row("q2", "Well")]));
        Assert.StartsWith(UiStateReasons.Superseded, (await first).Reason);
        UiDataRegistry.Commit();
        Assert.Equal(1ul, (await second).Version);
        Assert.Equal(2, ((UiRowsValue)UiDataRegistry.Latest(Mod, "quests")!).Rows.Length);
        var dup = await UiDataRegistry.Publish(Mod, "quests", new UiDataSnapshot.Rows([Row("q1", "a"), Row("q1", "b")]));
        Assert.False(dup.Ok);
        Assert.Throws<ArgumentException>(() => { _ = UiDataRegistry.Publish(Mod, "quests", new UiDataSnapshot.Rows([new UiDataRow("q3", new Dictionary<string, UiScalar>())])); });
        UiDataRegistry.Commit();
        Assert.Equal(2, ((UiRowsValue)UiDataRegistry.Latest(Mod, "quests")!).Rows.Length);
        Assert.Single(_events.OfType<UiBindingValueChangedEvent>());
    }

    [Theory]
    [InlineData("b", "a", "a", "b")]       // a loop
    [InlineData("a", null, "a", "zz")]     // a missing parent
    [InlineData("a", null, "a", null)]     // a key twice
    public async Task A_bad_tree_is_refused_whole(string k1, string? p1, string k2, string? p2)
    {
        using var reg = UiDataRegistry.Define(Mod, "tree", new UiDataSchema(UiDataShape.Tree, Fields: []));
        var good = UiDataRegistry.Publish(Mod, "tree", new UiDataSnapshot.Tree([new("r", null, "R", new Dictionary<string, UiScalar>())]));
        UiDataRegistry.Commit();
        Assert.True((await good).Ok);
        var bad = await UiDataRegistry.Publish(Mod, "tree", new UiDataSnapshot.Tree([
            new(k1, p1, "1", new Dictionary<string, UiScalar>()), new(k2, p2, "2", new Dictionary<string, UiScalar>())]));
        Assert.False(bad.Ok);
        Assert.Single(((UiTreeValue)UiDataRegistry.Latest(Mod, "tree")!).Nodes);
    }

    [Fact]
    public void A_tree_deeper_than_eight_is_refused()
    {
        var nodes = Enumerable.Range(0, 9).Select(i => ($"n{i}", i == 0 ? null : $"n{i - 1}", (UiText)"x", (IReadOnlyDictionary<string, UiScalar>?)null)).ToList();
        Assert.Throws<UiDataRegistry.UiDataRefusal>(() => UiDataRegistry.Tree([], nodes));
        Assert.Equal(8, UiDataRegistry.Tree([], nodes.Take(8).ToList()).Item1.Nodes.Length);
    }

    // ---- tables, tabs, trees, fields --------------------------------------------------------------------------------

    private static UiInventoryValue Slot(int i, uint item) => new($"l:00000000:{i}", 42, 0, (byte)i, item, 3, 100u + (uint)i);

    [Fact]
    public void A_table_keeps_selection_by_key_and_clears_it_only_after_a_valid_removal()
    {
        _world = new UiWorldReadResult(true, null, Snap(inv: [Slot(0, 0xabc), Slot(1, 0xdef)]));
        Define("w", Json<WindowBlock>(RichJson));
        var w = Open();
        var c = new Canvas { ShowTab = "bag" };
        c.Clicks.Add("row:l:00000000:1");
        Frame(w, c);
        Assert.Contains("row:l:00000000:0:0x00000abc|3", c.Calls);
        UiRuntime.Pump();
        Assert.Equal(new UiStateValue.String("l:00000000:1"), w.State.Snapshot.Values["pick"]);
        // reordered: still the same key
        _world = new UiWorldReadResult(true, null, Snap(inv: [Slot(1, 0xdef), Slot(0, 0xabc)]));
        c = Frame(w, new Canvas { ShowTab = "bag" });
        Assert.Contains("row:l:00000000:1:0x00000def|3*", c.Calls);
        // no world: absence is not established, the selection stays
        _world = new UiWorldReadResult(false, "NoWorld", null);
        Frame(w, new Canvas { ShowTab = "bag" });
        UiRuntime.Pump();
        Assert.Equal(new UiStateValue.String("l:00000000:1"), w.State.Snapshot.Values["pick"]);
        // removed in a valid publication: cleared once, reported as such
        _world = new UiWorldReadResult(true, null, Snap(inv: [Slot(0, 0xabc)]));
        Frame(w, new Canvas { ShowTab = "bag" });
        UiRuntime.Pump();
        Assert.Equal(new UiStateValue.String(""), w.State.Snapshot.Values["pick"]);
        Assert.Contains(_events.OfType<UiWindowStateChangedEvent>(), e => e.Cause == UiStateChangeCause.SelectionInvalidated);
    }

    [Fact]
    public void A_source_with_more_rows_than_the_table_shows_a_notice_not_a_cut_list()
    {
        _world = new UiWorldReadResult(true, null, Snap(inv: [.. Enumerable.Range(0, 5).Select(i => Slot(i, 1))]));
        Define("w", Json<WindowBlock>(RichJson));
        var c = Frame(Open(), new Canvas { ShowTab = "bag" });
        Assert.DoesNotContain(c.Calls, x => x.StartsWith("row:", StringComparison.Ordinal));
        Assert.Contains(c.Calls, x => x.StartsWith("disabled:5 rows", StringComparison.Ordinal));
    }

    [Fact]
    public void Tabs_follow_their_state_and_a_hidden_selected_tab_moves_to_the_first()
    {
        _world = new UiWorldReadResult(true, null, Snap());
        Define("w", Json<WindowBlock>(RichJson));
        var w = Open();
        var c = Frame(w);
        Assert.Contains("tab:bag", c.Calls);
        // the player picks Map
        Frame(w, new Canvas { ShowTab = "map" }, refresh: false);
        UiRuntime.Pump();
        Assert.Equal("map", w.State.Snapshot.Controls["tabs"].Selected);
        Assert.Contains(_events.OfType<UiControlStateChangedEvent>(), e => e.RowId == "tabs");
        // Map's condition goes false: the first visible tab, reported
        _world = new UiWorldReadResult(false, "NoWorld", null);
        Frame(w);
        UiRuntime.Pump();
        Assert.Equal("bag", w.State.Snapshot.Controls["tabs"].Selected);
        Assert.Equal(c.Bars, c.BarEnds);
    }

    [Fact]
    public void A_selected_tab_hidden_a_second_time_moves_again()
    {
        // review S6: the first hide moved Map to Bag; the player picks Map again and it is hidden again: it moves again
        _world = new UiWorldReadResult(true, null, Snap());
        Define("w", Json<WindowBlock>(RichJson));
        var w = Open();
        Frame(w);
        for (var round = 1; round <= 2; round++)
        {
            _world = new UiWorldReadResult(true, null, Snap());
            Frame(w);
            Frame(w, new Canvas { ShowTab = "map" }, refresh: false);
            UiRuntime.Pump();
            Assert.Equal("map", w.State.Snapshot.Controls["tabs"].Selected);
            _world = new UiWorldReadResult(false, "NoWorld", null);
            Frame(w);
            UiRuntime.Pump();
            Assert.True(w.State.Snapshot.Controls["tabs"].Selected == "bag", $"round {round}: {w.State.Snapshot.Controls["tabs"].Selected}");
        }
    }

    [Fact]
    public void Tree_scopes_balance_even_when_a_node_row_throws()
    {
        var state = new Dictionary<string, UiStateDefinition> { ["open"] = new UiStringSetState(["a"]) };
        Define("w", W(state, new TreeRow { Id = "t", Expanded = "state.open", Roots = [new("a", "A", [new("a1", "A1")])], NodeRows = [new TextRow { Text = "row {node.key}" }] }));
        var w = Open();
        var c = Frame(w);
        Assert.Contains("node:A###n+", c.Calls);
        Assert.Contains("text:row a1", c.Calls);
        Assert.Equal(c.Nodes, c.TreePops);
        var boom = new Canvas { ThrowOn = "row a1" };
        Assert.Throws<InvalidOperationException>(() => Frame(w, boom));
        Assert.Equal(boom.Nodes, boom.TreePops);
        Assert.Equal(boom.Pushes, boom.Pops);
        // the player closes the branch: the set entry follows
        var close = new Canvas();
        close.Toggle.Add("A###n");
        Frame(w, close);
        UiRuntime.Pump();
        Assert.Empty(((UiStateValue.StringSet)w.State.Snapshot.Values["open"]).Value);
    }

    [Fact]
    public void An_id_like_an_old_rows_ordinal_is_refused()
    {
        // review nit: old rows get the ids r0, r1, ...; an authored id of that form would push the same ImGui id
        var tabs = new TabsRow { Id = "r1", Tabs = [new UiTab("a", "A", [new TextRow { Text = "x" }])] };
        Assert.Contains("r1", Refused(W(null, new TextRow { Text = "first" }, tabs)).Problem);
        Define("w", W(null, new TextRow { Text = "first" }, tabs with { Id = "r1x" }));
    }

    [Fact]
    public void A_selection_entry_too_small_for_a_tab_id_is_a_compile_error()
    {
        // review S2: a tabs row writes its tab ids into the entry; a 4-byte entry cannot hold "quests"
        var state = new Dictionary<string, UiStateDefinition> { ["tab"] = new UiStringState("", MaxBytes: 4) };
        var row = new TabsRow
        {
            Id = "t", Selected = "state.tab",
            Tabs = [new UiTab("bag", "Bag", [new TextRow { Text = "x" }]), new UiTab("quests", "Quests", [new TextRow { Text = "y" }])],
        };
        Assert.Contains("quests", Refused(W(state, row)).Problem);
        var fits = new Dictionary<string, UiStateDefinition> { ["tab"] = new UiStringState("", MaxBytes: 6) };
        Define("w", W(fits, row));
    }

    [Fact]
    public void A_widget_never_writes_past_its_state_entrys_limits()
    {
        // review S2: a tree's expanded set with maxItems 2 does not take a third branch; the state keeps its declaration
        var state = new Dictionary<string, UiStateDefinition> { ["open"] = new UiStringSetState([], MaxItems: 2) };
        Define("w", W(state, new TreeRow { Id = "t", Expanded = "state.open", Roots = [new("a", "A", [new("a1", "A1")]), new("b", "B", [new("b1", "B1")]), new("c", "C", [new("c1", "C1")])] }));
        var w = Open();
        Frame(w);
        foreach (var node in (string[])["A###n", "B###n", "C###n"])
        {
            var c = new Canvas();
            c.Toggle.Add(node);
            Frame(w, c, refresh: false);
            UiRuntime.Pump();
            Frame(w);
        }
        var open = ((UiStateValue.StringSet)w.State.Snapshot.Values["open"]).Value;
        Assert.True(open.Count <= 2, string.Join(",", open));
        Assert.Null(UiWindowState.Problem(w.State.Definitions["open"], w.State.Snapshot.Values["open"]));
    }

    [Fact]
    public void A_text_field_keeps_its_draft_commits_on_Enter_and_cuts_at_a_character()
    {
        Define("w", Json<WindowBlock>(RichJson));
        var w = Open();
        var c = new Canvas();
        c.Typing["Note###in"] = ("Grüße", UiItemState.Active, false);
        Frame(w, c);
        UiRuntime.Pump();
        Assert.Equal(new UiStateValue.String(""), w.State.Snapshot.Values["note"]);
        // a refresh while it has the keyboard keeps the draft
        var c2 = new Canvas();
        c2.Typing["Note###in"] = ("Grüße!", UiItemState.Active | UiItemState.Deactivated, true);
        Frame(w, c2);
        Assert.Contains("field:Note###in=Grüße", c2.Calls);
        UiRuntime.Pump();
        Assert.Equal(new UiStateValue.String("Grüße!"), w.State.Snapshot.Values["note"]);
        Assert.Equal("Grü", WindowRuntime.Utf8Cut("Grüße", 4));
        Assert.Equal("ab", WindowRuntime.Utf8Cut("a\0b", 8));
        Assert.Equal("", WindowRuntime.Utf8Cut("\U0001F600", 3));
    }

    [Fact]
    public void Without_kiln_ui_blocks_a_text_field_is_off_and_shown_read_only()
    {
        // review S5: without kiln.ui.blocks a field cannot tell when it loses the keyboard, so it would commit every keystroke
        // (and never on blur): ui.blocks.input is reported off like tabs and trees, and the field shows its text read-only
        UiBlocksCaps.Report();
        Assert.False(Capabilities.Is("ui.blocks.input"));
        Assert.Equal(CapabilityReason.ServiceOlder, Capabilities.Get("ui.blocks.input").Reason);
        Define("w", Json<WindowBlock>(RichJson));
        var w = Open();
        var c = new Canvas { CanEditText = false };
        c.Typing["Note###in"] = ("G", UiItemState.None, true);
        Frame(w, c);
        UiRuntime.Pump();
        Assert.Equal(new UiStateValue.String(""), w.State.Snapshot.Values["note"]);
        Assert.DoesNotContain(c.Calls, x => x.StartsWith("field:Note", StringComparison.Ordinal));
    }

    [Fact]
    public void A_blur_field_commits_when_it_loses_the_keyboard_after_an_edit()
    {
        // review S8: commit "blur" (the default) writes when ImGui says the field was deactivated after an edit, not before
        var state = new Dictionary<string, UiStateDefinition> { ["n"] = new UiStringState() };
        Define("w", W(state, new InputRow { Id = "n", Label = "N", Bind = "state.n", Commit = UiInputCommit.Blur }));
        var w = Open();
        var typing = new Canvas();
        typing.Typing["N###in"] = ("hi", UiItemState.Active | UiItemState.Edited, false);  // typing: the draft only
        Frame(w, typing);
        UiRuntime.Pump();
        Assert.Equal(new UiStateValue.String(""), w.State.Snapshot.Values["n"]);
        var blur = new Canvas();
        blur.Typing["N###in"] = ("hi", UiItemState.Deactivated | UiItemState.DeactivatedAfterEdit, false);
        Frame(w, blur, refresh: false);
        UiRuntime.Pump();
        Assert.Equal(new UiStateValue.String("hi"), w.State.Snapshot.Values["n"]);
    }

    [Fact]
    public void State_is_kept_when_the_window_closes_and_opens_again()
    {
        // review S8: the note, the selected tab, the table selection and the open branches stay through a close and an open
        _world = new UiWorldReadResult(true, null, Snap(inv: [Slot(0, 1)]));
        Define("w", Json<WindowBlock>(RichJson));
        var w = Open();
        var c = new Canvas();
        c.Typing["Note###in"] = ("kept", UiItemState.Active | UiItemState.Deactivated, true);
        Frame(w, c);
        Frame(w, new Canvas { ShowTab = "map" }, refresh: false);
        UiRuntime.Pump();
        var open = new Canvas { ShowTab = "map" };
        open.Toggle.Add("A###n");
        Frame(w, open);
        UiRuntime.Pump();
        var before = w.State.Snapshot;
        w.Close();
        UiRuntime.Pump();
        w.Open();
        var again = Frame(w, new Canvas { ShowTab = "map" });
        UiRuntime.Pump();
        Assert.Equal(before.Version, w.State.Snapshot.Version);
        Assert.Equal(new UiStateValue.String("kept"), w.State.Snapshot.Values["note"]);
        Assert.Equal("map", w.State.Snapshot.Controls["tabs"].Selected);
        Assert.Equal(new[] { "a" }, ((UiStateValue.StringSet)w.State.Snapshot.Values["open"]).Value);
        Assert.Contains("field:Note###in=kept", again.Calls);
        Assert.Contains("node:A###n+", again.Calls);
    }

    [Fact]
    public void Escape_restores_the_text_and_writes_nothing()
    {
        Define("w", Json<WindowBlock>(RichJson) with { Rows = [new InputRow { Id = "note", Label = "Note", Bind = "state.note", MaxBytes = 64 }] });
        var w = Open();
        var c = new Canvas();
        c.Typing["Note###in"] = ("", UiItemState.Deactivated | UiItemState.DeactivatedAfterEdit, false);
        Frame(w, c);
        UiRuntime.Pump();
        Assert.Equal(0ul, w.State.Snapshot.Version.Revision);
        Assert.Empty(_events);
    }

    [Fact]
    public void State_and_data_events_go_to_the_clients_main_thread_queue()
    {
        // review S3: the Pump commits on the service tick (the server's thread on a host); the state and data events are handed
        // to the client event queue (the game's main thread in the client world's next step), not posted from the Pump's thread
        var queued = new List<KilnEvent>();
        var before = UiRuntime.PostEvent;
        UiRuntime.PostEvent = queued.Add;
        try
        {
            Define("w", Json<WindowBlock>(RichJson));
            var w = Open();
            var c = new Canvas();
            c.Typing["Note###in"] = ("hi", UiItemState.Active | UiItemState.Deactivated, true);
            Frame(w, c);
            UiRuntime.Pump();
            using var reg = UiDataRegistry.Define(Mod, "s3", new UiDataSchema(UiDataShape.Scalar, ScalarType: UiScalarType.String));
            _ = UiDataRegistry.Publish(Mod, "s3", new UiDataSnapshot.Scalar(new UiScalar.String("x")));
            UiRuntime.Pump();
            w.State.Reset();
        }
        finally
        {
            UiRuntime.PostEvent = before;
        }
        Assert.Contains(queued, e => e is UiWindowStateChangedEvent);
        Assert.Contains(queued, e => e is UiBindingValueChangedEvent);
        Assert.Contains(queued, e => e is UiWindowStateResetEvent);
        Assert.Empty(_events);
    }

    // ---- live values: demand -----------------------------------------------------------------------------------------

    [Fact]
    public void A_closed_window_asks_for_nothing_and_an_open_one_for_every_reachable_name()
    {
        Define("w", Json<WindowBlock>(RichJson));
        UiRuntime.Pump();
        Assert.Empty(_demands);
        var w = Open();
        UiRuntime.Pump();
        Assert.Equal(UiWorldFields.LocalInventory, Assert.Single(_demands).Fields);
        w.Close();
        UiRuntime.Pump();
        Assert.Empty(_demands);
    }

    [Fact]
    public void A_hidden_HUD_line_asks_for_its_condition_and_shows_after_the_first_sample()
    {
        Define("hp", new HudLineBlock { Text = "HP {world.local.health}", When = "world.local.health > 0", Refresh = 0.1 });
        _now += 1000;
        UiRuntime.Pump();
        Assert.Contains(_demands, d => d.Fields == UiWorldFields.LocalVitals);
        Assert.Empty(_game.Huds);
        _world = new UiWorldReadResult(true, null, Snap());
        _now += 1000;
        UiRuntime.Pump();
        Assert.Equal("HP 413", Assert.Single(_game.Huds).Text);
    }

    [Fact]
    public void A_line_that_only_asks_whether_there_is_a_world_still_gets_a_publication()
    {
        // world.local.available, player, session and step come with any publication, and the collector publishes only
        // what is demanded (review S1): on their own they ask for the cheapest group, the vitals, and so get a value.
        Define("here", new HudLineBlock { Text = "In a world, step {world.local.step}", When = "world.local.available", Refresh = 0.1 });
        _now += 1000;
        UiRuntime.Pump();
        Assert.Contains(_demands, d => d.Fields != UiWorldFields.None);
        Assert.Empty(_game.Huds);
        _world = new UiWorldReadResult(true, null, Snap());
        _now += 1000;
        UiRuntime.Pump();
        Assert.Equal("In a world, step 3", Assert.Single(_game.Huds).Text);
        // a window whose only live name is world.local.player asks too
        Define("w", W(null, new TextRow { Text = "player {world.local.player}" }));
        var w = Open();
        UiRuntime.Pump();
        Assert.True(w.HasDemand);
    }

    [Fact]
    public void A_refused_demand_is_asked_again_at_the_next_refresh()
    {
        // a mod at its 64-demand budget gets Budget; once others go, the open window and the HUD line ask again (review S7)
        var refuse = true;
        UiWorldSource.Demand = (mod, f) =>
        {
            if (refuse) return new UiDemandRegistration(false, "Budget", 0, null);
            var e = (mod, f);
            _demands.Add(e);
            return new UiDemandRegistration(true, null, 1, () => _demands.Remove(e));
        };
        Define("w", Json<WindowBlock>(RichJson));
        Define("hp", new HudLineBlock { Text = "HP {world.local.health}", Refresh = 0.1 });
        var w = Open();
        _now += 1000;
        UiRuntime.Pump();
        Assert.False(w.HasDemand);
        Assert.Empty(_demands);
        refuse = false;
        _now += 1000;
        UiRuntime.Pump();
        Assert.True(w.HasDemand);
        Assert.Contains(_demands, d => d.Fields == UiWorldFields.LocalInventory);
        Assert.Contains(_demands, d => d.Fields == UiWorldFields.LocalVitals);
    }

    // ---- pictures --------------------------------------------------------------------------------------------------

    private static byte[] Png(uint w, uint h)
    {
        var b = new byte[33];
        new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }.CopyTo(b, 0);
        System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(b.AsSpan(8), 13);
        "IHDR"u8.CopyTo(b.AsSpan(12));
        System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(b.AsSpan(16), w);
        System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(b.AsSpan(20), h);
        return b;
    }

    [Fact]
    public void A_picture_is_checked_from_its_header_before_anything_is_decoded()
    {
        Assert.Null(UiImages.HeaderOf(Png(64, 32), out var w, out var h));
        Assert.Equal((64, 32), (w, h));
        Assert.Contains("1..2048", UiImages.HeaderOf(Png(4096, 4), out _, out _));
        Assert.Contains("1..2048", UiImages.HeaderOf(Png(0, 4), out _, out _));
        Assert.Equal("not a PNG", UiImages.HeaderOf(Encoding.ASCII.GetBytes("GIF89a.........................xx"), out _, out _));
    }

    // ---- the sample ----------------------------------------------------------------------------------------------

    [Fact]
    public void The_UiBlocks_samples_camp_window_compiles_with_its_data_its_stat_and_its_picture()
    {
        var dir = Path.Combine(Repo(), "mods", "samples", "UiBlocks");
        using var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, "blocks", "camp.json")));
        var camp = Json<WindowBlock>(doc.RootElement.GetProperty("windows").GetProperty("camp").GetRawText());
        const string sample = "ui-blocks";
        UiStats.Metadata = key => key == new ModValueKey(sample, "rests") ? Def(key) : null;
        using var quests = UiDataRegistry.Define(sample, "quests", new UiDataSchema(UiDataShape.Rows,
            Fields: [new("name", UiScalarType.String), new("reward", UiScalarType.Decimal), new("done", UiScalarType.Bool)]));
        using var places = UiDataRegistry.Define(sample, "places", new UiDataSchema(UiDataShape.Tree, Fields: [new("danger", UiScalarType.Int64)]));
        var mod = new ModBlocks(sample, 1) { ManifestClients = "mod", BootDirectory = dir };
        var (_, low, _) = Compiler.Lower(mod, "camp", camp);
        var rows = low.Json["window"]!["rows"]!.AsArray();
        Assert.Contains(rows, r => (string?)r!["type"] == "tabs");
        Assert.Contains(rows, r => (string?)r!["does"]?["command"] == "uiblocks-rest");
        var reads = low.Json["window"]!["reads"]!.AsArray().Select(x => (string?)x).ToHashSet();
        foreach (var name in new[] { "world.local.inventory", "world.local.health", "world.local.position.x", "world.server.hours", "stats.ui-blocks/rests", "mod.quests", "mod.places", "state.note" })
            Assert.Contains(name, reads);
    }

    [Fact]
    public void The_UiBlocks_samples_camp_window_compiles_on_a_game_without_an_overlay()
    {
        // A dedicated server has no overlay, yet it checks and keeps the blocks (review M1): the mod defines its data on every
        // side through the public BlockUi.Bindings.Define, and the camp window that binds mod.quests and mod.places compiles.
        _game.Available = false;
        Assert.False(UiRuntime.Active);
        var dir = Path.Combine(Repo(), "mods", "samples", "UiBlocks");
        using var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, "blocks", "camp.json")));
        var camp = Json<WindowBlock>(doc.RootElement.GetProperty("windows").GetProperty("camp").GetRawText());
        const string sample = "ui-blocks";
        UiStats.Metadata = key => key == new ModValueKey(sample, "rests") ? Def(key) : null;
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = HostAccess.CreateContext(new ModInfo(sample, "UI blocks", "1", dir), 0x6909, dir);
        try
        {
            using var quests = BlockUi.Bindings.Define("quests", new UiDataSchema(UiDataShape.Rows,
                Fields: [new("name", UiScalarType.String), new("reward", UiScalarType.Decimal), new("done", UiScalarType.Bool)]));
            using var places = BlockUi.Bindings.Define("places", new UiDataSchema(UiDataShape.Tree, Fields: [new("danger", UiScalarType.Int64)]));
            Assert.True(quests.Ok && places.Ok, quests.Reason ?? places.Reason);
            var mod = new ModBlocks(sample, 1) { ManifestClients = "mod", BootDirectory = dir };
            var (_, low, _) = Compiler.Lower(mod, "camp", camp);
            var reads = low.Json["window"]!["reads"]!.AsArray().Select(x => (string?)x).ToHashSet();
            Assert.Contains("mod.quests", reads);
            Assert.Contains("mod.places", reads);
            // publishing is for a game that draws the window: off here, and it says so
            Assert.Throws<KilnUnavailableException>(() => { _ = BlockUi.Bindings.Publish("quests", new UiDataSnapshot.Rows([])); });
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    // ---- the boundary: the block runtime never reads a world itself ----------------------------------------------------

    [Fact]
    public void The_block_UI_runtime_names_no_server_bound_world_facade()
    {
        var dir = Path.Combine(Repo(), "managed", "services", "Kiln.Blocks");
        var files = Directory.EnumerateFiles(Path.Combine(dir, "Ui"), "*.cs").Concat(Directory.EnumerateFiles(dir, "BlockUi*.cs"));
        var pattern = new System.Text.RegularExpressions.Regex(@"(?<![\w.])(World|Items|Time)\.[A-Z]");
        foreach (var f in files)
            foreach (var (line, n) in File.ReadLines(f).Select((l, i) => (l, i + 1)))
            {
                var code = line.Split("//")[0];
                Assert.False(pattern.IsMatch(code), $"{Path.GetFileName(f)}:{n} names a server-bound facade: {line.Trim()}");
            }
    }

    private static string Repo([System.Runtime.CompilerServices.CallerFilePath] string self = "")
    {
        for (var d = new DirectoryInfo(Path.GetDirectoryName(self)!); d != null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "VERSION"))) return d.FullName;
        throw new DirectoryNotFoundException("no repository");
    }
}
