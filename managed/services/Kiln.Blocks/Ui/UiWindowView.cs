// SPDX-License-Identifier: MIT
// A window block, drawn. The plan's rows become widget calls on an IUiCanvas (Kiln.Ui's UiDraw in the game, a recording fake in tests). What a row shows is worked out
// when the window refreshes, every Refresh seconds, from one UiRefreshContext (one world publication, one stats copy, one state snapshot, the configs); each
// refresh builds a new tree of RowViews and the frames between only issue the widget calls with the strings and numbers the refresh left. A refresh reads only
// immutable copies (never the game's world), so it may run on the render thread. A closed window is never drawn, so it reads nothing.
// What a widget holds between frames (a text field's draft, which tab was shown, the branches opened here) is the window's widget memory, render thread only.
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Text;

namespace Kiln;

/// <summary>The widgets a window block uses, as Kiln.Ui's <see cref="UiDraw"/> has them. The rows of 0.2.0-dev.31 have default
/// bodies that draw nothing, so an older canvas still compiles.</summary>
internal interface IUiCanvas
{
    void Text(string text, Color? color, bool wrap);
    void Disabled(string text);
    bool Button(string label);
    bool Checkbox(string label, ref bool value);
    bool SliderInt(string label, ref int value, int min, int max);
    bool SliderFloat(string label, ref float value, float min, float max);
    bool Combo(string label, ref int index, IReadOnlyList<string> items);
    void Separator(string? heading);
    void SameLine();
    void Tooltip(string text);
    void PushId(string id);
    void PopId();

    /// <summary>Whether <see cref="TextField"/> can tell when a field loses the keyboard (kiln.ui.blocks): without it a field
    /// would commit every keystroke and never on blur, so it is shown read-only.</summary>
    bool CanEditText => true;

    /// <summary>A text field over <paramref name="text"/>; true when Enter was pressed (with <paramref name="enterCommits"/>) or
    /// the text changed; <paramref name="state"/> says whether it has the keyboard or just lost it.</summary>
    bool TextField(string label, ref string text, int maxBytes, bool enterCommits, bool multiline, out UiItemState state)
    {
        state = UiItemState.None;
        return false;
    }

    bool BeginTable(string id, IReadOnlyList<string> headers) => false;
    /// <summary>One table row; true when its selectable first cell was clicked.</summary>
    bool TableRow(string key, IReadOnlyList<string> cells, bool selectable, bool selected) => false;
    void EndTable() { }
    bool BeginTabBar(string id) => false;
    bool BeginTab(string label, bool select) => false;
    void EndTab() { }
    void EndTabBar() { }
    /// <summary>A branch, opened or closed as <paramref name="open"/> says unless the player clicks it; true while open
    /// (then <see cref="TreePop"/>).</summary>
    bool TreeNode(string label, bool open, out bool clicked)
    {
        clicked = false;
        return false;
    }
    void TreePop() { }
    bool Selectable(string label, bool selected) => false;
    /// <summary>A picture; true when clicked.</summary>
    bool Image(UiImage image, float width, float height) => false;
}

/// <summary><see cref="UiDraw"/> as a canvas.</summary>
internal sealed class UiDrawCanvas : IUiCanvas
{
    private readonly Stack<UiDraw.IdScope> _ids = new();

    /// <summary>The frame being drawn.</summary>
    public UiDraw Ui { get; set; } = null!;

    public void Text(string text, Color? color, bool wrap)
    {
        if (wrap) Ui.TextWrapped(text);
        else if (color is { } c) Ui.Text(c, text);
        else Ui.Text(text);
    }

    public void Disabled(string text) => Ui.TextDisabled(text);
    public bool Button(string label) => Ui.Button(label);
    public bool Checkbox(string label, ref bool value) => Ui.Checkbox(label, ref value);
    public bool SliderInt(string label, ref int value, int min, int max) => Ui.Slider(label, ref value, min, max);
    public bool SliderFloat(string label, ref float value, float min, float max) => Ui.Slider(label, ref value, min, max);
    public bool Combo(string label, ref int index, IReadOnlyList<string> items) => Ui.Combo(label, ref index, items);

    public void Separator(string? heading)
    {
        if (heading is { Length: > 0 }) Ui.Separator(heading);
        else Ui.Separator();
    }

    public void SameLine() => Ui.SameLine();
    public void Tooltip(string text) => Ui.Tooltip(text);
    public void PushId(string id) => _ids.Push(Ui.Id(id));
    public void PopId() => _ids.Pop().Dispose();

    public bool CanEditText => Ui.HasBlockWidgets;

    public bool TextField(string label, ref string text, int maxBytes, bool enterCommits, bool multiline, out UiItemState state)
    {
        var flags = (enterCommits ? UiTextFieldFlags.EnterReturns : 0) | (multiline ? UiTextFieldFlags.Multiline : 0);
        var r = Ui.TextField(label, ref text, maxBytes, flags, 0, multiline ? 80 * Ui.Scale : 0);
        state = Ui.LastItemState;
        return r;
    }

    public bool BeginTable(string id, IReadOnlyList<string> headers) => Ui.BeginTable(id, [.. headers]);

    public bool TableRow(string key, IReadOnlyList<string> cells, bool selectable, bool selected)
    {
        Ui.NextRow();
        var clicked = false;
        using (Ui.Id(key))
            for (var i = 0; i < cells.Count; i++)
            {
                Ui.NextColumn();
                if (i == 0 && selectable) clicked = Ui.Selectable(cells[0] + "###k", selected);
                else Ui.Text(cells[i]);
            }
        return clicked;
    }

    public void EndTable() => Ui.EndTable();
    public bool BeginTabBar(string id) => Ui.BeginTabBar(id);
    public bool BeginTab(string label, bool select) => Ui.BeginControlledTab(label, select);
    public void EndTab() => Ui.EndTab();
    public void EndTabBar() => Ui.EndTabBar();

    public bool TreeNode(string label, bool open, out bool clicked)
    {
        Ui.SetNextItemOpen(open);
        var r = Ui.TreeNode(label);
        clicked = Ui.ItemClicked();
        return r;
    }

    public void TreePop() => Ui.TreePop();
    public bool Selectable(string label, bool selected) => Ui.Selectable(label, selected);

    public bool Image(UiImage image, float width, float height)
    {
        Ui.Image(image, width * Ui.Scale, height * Ui.Scale);
        return Ui.ItemClicked();
    }
}

/// <summary>Where a window block is shown: a Kiln.Ui window or screen.</summary>
internal interface IUiSurface : IDisposable
{
    bool IsOpen { get; }
    void Open();
    void Close();
}

/// <summary>One row as one refresh worked it out. A refresh makes new ones; a frame only reads them (and a click may show
/// its new value on the row it drew until the next refresh).</summary>
internal sealed class RowView
{
    public required RowPlan Plan { get; init; }
    public bool Visible { get; set; } = true;
    public string Text { get; set; } = "";
    public string Label { get; set; } = "";
    public string? Tooltip { get; set; }
    public string ValueText { get; set; } = "";
    public bool ReadOnly { get; set; }
    public bool Bool { get; set; }
    public int Int { get; set; }
    public float Float { get; set; }
    public int Index { get; set; }
    // 0.2.0-dev.31
    public string Committed { get; set; } = "";
    public ulong Session { get; set; }
    public string? Notice { get; set; }
    public IReadOnlyList<string> Headers { get; set; } = [];
    public IReadOnlyList<(string Key, IReadOnlyList<string> Cells)> TableRows { get; set; } = [];
    public string? SelectedKey { get; set; }
    public IReadOnlyList<TabView> Tabs { get; set; } = [];
    public IReadOnlyList<TreeNodeView> Nodes { get; set; } = [];
    public ImmutableHashSet<string> Expanded { get; set; } = ImmutableHashSet<string>.Empty;
    public UiImage? Image { get; set; }
}

internal sealed record TabView(string Id, string Label, bool Visible, IReadOnlyList<RowView> Rows);

internal sealed record TreeNodeView(string Key, string Label, int Depth, bool Branch, IReadOnlyList<RowView> Rows, IReadOnlyList<int> Children);

/// <summary>A window block on a client: its Kiln.Ui surface, its menu entries, its state and the views its rows draw from.</summary>
internal sealed class WindowRuntime : IDisposable
{
    internal const int MaxTextChars = 1 << 20;

    private readonly List<IDisposable> _menus = new();
    private IUiSurface? _surface;
    private string _titleLanguage = "";
    private volatile RowView[]? _views;
    private long _nextRefresh;
    private readonly UiLifetime _lifetime;
    private readonly Dictionary<string, Widget> _widgets = new(StringComparer.Ordinal);  // render thread
    private readonly Dictionary<string, (ulong Version, string Key)> _invalidated = new(StringComparer.Ordinal);
    private volatile Dictionary<string, ImmutableHashSet<string>> _domains = new(StringComparer.Ordinal);
    private readonly Dictionary<string, (UiImage? Image, string? Problem, Action Release)> _images = new(StringComparer.Ordinal);
    private UiDemandRegistration? _demand;
    private long _demandRetryAt;

    private sealed class Widget
    {
        public string Draft = "";
        public bool Active;
        public ulong Session;
        public string? Shown;          // the tab ImGui showed last frame
        public string? Forcing;        // a tab being selected from the state, until it shows
        public int ForceFrames;
    }

    public WindowRuntime(WindowPlan plan)
    {
        Plan = plan;
        State = new UiWindowState(plan.Mod, plan.Name, plan.State);
        _lifetime = new UiLifetime(plan.Mod);
        _lifetime.WhenRetired(Release);
    }

    public WindowPlan Plan { get; }
    public UiWindowState State { get; }

    /// <summary>How many times the rows were worked out (tests: a window refreshes at its pace, not every frame).</summary>
    public int Refreshes { get; private set; }

    public bool IsOpen => _surface is { IsOpen: true };

    internal void WhenRetired(Action cleanup) => _lifetime.WhenRetired(cleanup);
    internal void WhenAlive(Action run) => _lifetime.Run(run);

    /// <summary>Makes the surface, the menu entries and the pictures. Client, a thread that may use Kiln.Ui (the tick thread, a mod's OnLoad).</summary>
    public void Attach() => _lifetime.Run(AttachCore);

    private void AttachCore()
    {
        var backend = UiRuntime.Backend;
        _titleLanguage = backend.Language;
        foreach (var file in Images(Plan.Rows))
            if (!_images.ContainsKey(file))
            {
                var loaded = UiImages.Load(Plan.Mod, file);
                if (loaded.Problem != null) BlocksBridge.Log(LogLevel.Warn, $"blocks: {Plan.Mod}: ui: window {Plan.Name}: picture {Path.GetFileName(file)}: {loaded.Problem}");
                _images[file] = loaded;
            }
        _surface = backend.CreateSurface(this, Plan.Title.Render(_titleLanguage, _ => null));
        if (Plan.Block.Menu is { } menu && Plan.MenuText != null)
        {
            void Add(UiMenuIn where)
            {
                if (backend.AddMenu(where, Plan.MenuText.Text, menu.Order, Open) is { } entry) _menus.Add(entry);
            }
            if (menu.In is UiMenuIn.Pause or UiMenuIn.Both) Add(UiMenuIn.Pause);
            if (menu.In is UiMenuIn.Main or UiMenuIn.Both) Add(UiMenuIn.Main);
        }
        if (Plan.Block.OpenAtStart) _surface.Open();
    }

    private static IEnumerable<string> Images(IEnumerable<RowPlan> rows)
    {
        foreach (var r in rows)
        {
            if (r.ImageFile != null) yield return r.ImageFile;
            foreach (var t in r.Tabs)
                foreach (var f in Images(t.Rows)) yield return f;
        }
    }

    public void Open() => _lifetime.Run(OpenCore);

    private void OpenCore()
    {
        // a window's title is fixed when it is made: one made in another language is made again
        if (Plan.Block.Kind == WindowKind.Window && _surface != null && !_surface.IsOpen && UiRuntime.Backend.Language != _titleLanguage)
        {
            _surface.Dispose();
            _titleLanguage = UiRuntime.Backend.Language;
            _surface = UiRuntime.Backend.CreateSurface(this, Plan.Title.Render(_titleLanguage, _ => null));
        }
        // what it shows is read fresh when it opens: the next frame refreshes (this may be the window thread, and the render thread is drawing)
        Volatile.Write(ref _nextRefresh, 0);
        _surface?.Open();
    }

    public void Close() => _lifetime.Run(() => _surface?.Close());

    public void Dispose() => _lifetime.Dispose();

    private void Release()
    {
        foreach (var m in _menus) m.Dispose();
        _menus.Clear();
        _surface?.Dispose();
        _surface = null;
        Interlocked.Exchange(ref _demand, null)?.Dispose();
        State.Drop();
        foreach (var i in _images.Values) i.Release();
        _images.Clear();
    }

    /// <summary>The live values this window asks the collector for: while it is open, every name it can reach. Pump.</summary>
    internal void UpdateDemand()
    {
        var want = IsOpen ? Plan.World : UiWorldFields.None;
        if (want == UiWorldFields.None)
        {
            Interlocked.Exchange(ref _demand, null)?.Dispose();
            return;
        }
        if (_demand is { Ok: true }) return;
        // none yet, or refused (a mod at its demand budget): ask again, at most once a second
        var now = UiRuntime.Clock();
        if (_demand != null && now < _demandRetryAt) return;
        _demandRetryAt = now + 1000;
        Interlocked.Exchange(ref _demand, null)?.Dispose();
        try { _demand = UiWorldSource.Demand(Plan.Mod, want); }
        catch (Exception e) when (e is not OutOfMemoryException) { BlocksBridge.Log(LogLevel.Warn, $"blocks: {Plan.Mod}: ui: window {Plan.Name}: live values: {e.Message}"); }
    }

    internal bool HasDemand => _demand is { Ok: true };

    /// <summary>Whether a selection key is shown now by the row bound to <paramref name="stateKey"/> (null: no such row).</summary>
    internal bool? Shows(string stateKey, string key) => _domains.TryGetValue(stateKey, out var set) ? key.Length == 0 || set.Contains(key) : null;

    // ---- frames (render thread) ----

    /// <summary>One frame: refresh when it is time, then the visible rows as widgets.</summary>
    public void Draw(IUiCanvas c) => _lifetime.Run((Window: this, Canvas: c), static state => state.Window.DrawCore(state.Canvas));

    private void DrawCore(IUiCanvas c)
    {
        var now = UiRuntime.Clock();
        var views = _views;
        if (views == null || now >= Volatile.Read(ref _nextRefresh))
        {
            Refresh(now);
            views = _views!;
        }
        DrawRows(c, views, "");
    }

    private void DrawRows(IUiCanvas c, IReadOnlyList<RowView> views, string scope)
    {
        var drawn = false;
        foreach (var v in views)
        {
            if (!v.Visible) continue;
            if (v.Plan.SameLine && drawn) c.SameLine();
            c.PushId(v.Plan.Id);
            try
            {
                DrawRow(c, v, scope + "/" + v.Plan.Id);
                if (v.Tooltip != null) c.Tooltip(v.Tooltip);
            }
            finally { c.PopId(); }
            drawn = true;
        }
    }

    private Widget WidgetOf(string scope)
    {
        if (!_widgets.TryGetValue(scope, out var w)) _widgets[scope] = w = new Widget();
        return w;
    }

    private void DrawRow(IUiCanvas c, RowView v, string scope)
    {
        var p = v.Plan;
        if (v.Notice != null && p.Type is "table" or "tree" or "image")
        {
            c.Disabled(v.Notice);
            return;
        }
        switch (p.Type)
        {
            case "text":
                c.Text(v.Text, p.Color, p.Wrap);
                break;
            case "value":
                c.Text(v.Text, null, false);
                c.SameLine();
                c.Text(v.ValueText, Color.Cyan, false);
                break;
            case "toggle" when v.ReadOnly:
            case "slider" when v.ReadOnly:
            case "choice" when v.ReadOnly:
            case "input" when v.ReadOnly:
                c.Disabled(v.Text);
                break;
            case "toggle":
            {
                var b = v.Bool;
                if (c.Checkbox(v.Label, ref b))
                {
                    v.Bool = b;
                    if (p.Bind!.Source == UiSource.State) WriteState(p.Bind.Key!, new UiStateValue.Bool(b), UiStateChangeCause.Widget);
                    else UiEdits.Set(p.Bind!, b, immediate: true, _lifetime);
                }
                break;
            }
            case "slider":
                if (p.IsInt)
                {
                    var i = v.Int;
                    if (c.SliderInt(v.Label, ref i, (int)p.Min, (int)p.Max))
                    {
                        v.Int = i;
                        if (p.Bind!.Source == UiSource.State) WriteState(p.Bind.Key!, new UiStateValue.Int64(i), UiStateChangeCause.Widget);
                        else UiEdits.Set(p.Bind!, (double)i, immediate: false, _lifetime);
                    }
                }
                else
                {
                    var f = v.Float;
                    if (c.SliderFloat(v.Label, ref f, (float)p.Min, (float)p.Max))
                    {
                        v.Float = f;
                        if (p.Bind!.Source == UiSource.State && p.Bind.State is UiDecimalState ds)
                        {
                            // approximate input, exact storage: rounded half to even at the entry's scale, then clamped
                            var m = Math.Clamp(Math.Round((decimal)f, ds.Scale, MidpointRounding.ToEven), (decimal)p.Min, (decimal)p.Max);
                            WriteState(p.Bind.Key!, new UiStateValue.Decimal(m), UiStateChangeCause.Widget);
                        }
                        else UiEdits.Set(p.Bind!, Math.Clamp((double)f, p.Min, p.Max), immediate: false, _lifetime);
                    }
                }
                break;
            case "choice":
            {
                var index = v.Index;
                if (c.Combo(v.Label, ref index, p.Options))
                {
                    v.Index = index;
                    UiEdits.Set(p.Bind!, p.Options[Math.Clamp(index, 0, p.Options.Count - 1)], immediate: true, _lifetime);
                }
                break;
            }
            case "separator":
                c.Separator(v.Text.Length > 0 ? v.Text : null);
                break;
            case "button":
                if (c.Button(v.Label)) UiActions.Run(p.Does!, Plan.Mod, $"window {Plan.Name}", _lifetime, State);
                break;
            case "input":
                DrawInput(c, v, scope);
                break;
            case "table":
                DrawTable(c, v);
                break;
            case "tabs":
                DrawTabs(c, v, scope);
                break;
            case "tree":
                DrawTree(c, v, scope);
                break;
            case "image":
                if (v.Image != null && c.Image(v.Image, p.Width > 0 ? p.Width : v.Image.Width, p.Height > 0 ? p.Height : v.Image.Height) && p.Does != null)
                    UiActions.Run(p.Does, Plan.Mod, $"window {Plan.Name}", _lifetime, State);
                break;
        }
    }

    // A text field edits a draft; Enter (or, with commit blur, losing the keyboard) writes it. Escape: ImGui puts the text the
    // field had when it took the keyboard back, so the write that follows changes nothing. A refresh never overwrites a draft
    // while the field has the keyboard.
    private void DrawInput(IUiCanvas c, RowView v, string scope)
    {
        var p = v.Plan;
        if (!c.CanEditText)
        {
            // an older Kiln without kiln.ui.blocks: the field could not keep a draft (ui.blocks.input is off), so it shows the value
            c.Disabled($"{v.Label}: {v.Committed}");
            return;
        }
        var w = WidgetOf(scope);
        if (!w.Active || w.Session != v.Session)
        {
            w.Draft = v.Committed;
            w.Session = v.Session;
        }
        var text = w.Draft;
        var enter = c.TextField(v.Label + "###in", ref text, p.MaxBytes, enterCommits: !p.Multiline, p.Multiline, out var st);
        w.Draft = text;
        w.Active = (st & UiItemState.Active) != 0;
        var commit = (enter && !p.Multiline) || (p.Commit == UiInputCommit.Blur && (st & UiItemState.DeactivatedAfterEdit) != 0);
        if (!commit) return;
        text = Utf8Cut(text, p.MaxBytes);
        if (text == v.Committed) return;
        if (p.Bind!.Source == UiSource.State)
        {
            var session = w.Session;
            State.Enqueue(UiWindowState.WriteKind.Value, p.Bind.Key!, new UiStateValue.String(text), null, null, UiStateChangeCause.Widget,
                cur => cur.Version.Session == session ? null : $"{UiStateReasons.StaleSession}: the world changed while the field was edited", wantResult: false);
        }
        else UiEdits.Set(p.Bind!, text, immediate: true, _lifetime);
        v.Committed = text;
    }

    /// <summary>At most <paramref name="maxBytes"/> UTF-8 bytes, cut at a character, without NUL.</summary>
    internal static string Utf8Cut(string s, int maxBytes)
    {
        s = s.Replace("\0", "");
        if (Encoding.UTF8.GetByteCount(s) <= maxBytes) return s;
        var b = new StringBuilder();
        var n = 0;
        foreach (var r in s.EnumerateRunes())
        {
            if (n + r.Utf8SequenceLength > maxBytes) break;
            n += r.Utf8SequenceLength;
            b.Append(r.ToString());
        }
        return b.ToString();
    }

    private void DrawTable(IUiCanvas c, RowView v)
    {
        var p = v.Plan;
        if (!c.BeginTable("t", v.Headers)) return;
        try
        {
            foreach (var (key, cells) in v.TableRows)
                if (c.TableRow(key, cells, p.Selected != null, key == v.SelectedKey) && p.Selected != null)
                {
                    v.SelectedKey = key;
                    WriteState(p.Selected.Key!, new UiStateValue.String(key), UiStateChangeCause.Widget);
                }
        }
        finally { c.EndTable(); }
    }

    private void DrawTabs(IUiCanvas c, RowView v, string scope)
    {
        var p = v.Plan;
        var w = WidgetOf(scope);
        var desired = v.SelectedKey;
        if (desired != null && desired != w.Shown && w.Forcing != desired)
        {
            w.Forcing = desired;
            w.ForceFrames = 4;
        }
        string? shown = null;
        if (!c.BeginTabBar("tabs")) return;
        try
        {
            foreach (var tab in v.Tabs)
            {
                if (!tab.Visible) continue;
                var force = w.Forcing == tab.Id;
                if (!c.BeginTab(tab.Label + "###" + tab.Id, force)) continue;
                try
                {
                    shown = tab.Id;
                    c.PushId(tab.Id);
                    try { DrawRows(c, tab.Rows, scope + "/" + tab.Id); }
                    finally { c.PopId(); }
                }
                finally { c.EndTab(); }
            }
        }
        finally { c.EndTabBar(); }
        if (w.Forcing != null)
        {
            if (shown == w.Forcing || --w.ForceFrames <= 0) w.Forcing = null;
            w.Shown = shown;
            return;
        }
        // the player picked another tab
        if (shown != null && shown != w.Shown && w.Shown != null && shown != desired)
        {
            v.SelectedKey = shown;
            if (p.Selected != null) WriteState(p.Selected.Key!, new UiStateValue.String(shown), UiStateChangeCause.Widget);
            else WriteControl(p.Id, new UiControlState(shown, []), UiStateChangeCause.Widget);
        }
        w.Shown = shown;
    }

    private void DrawTree(IUiCanvas c, RowView v, string scope)
    {
        var roots = new List<int>();
        for (var i = 0; i < v.Nodes.Count; i++) if (v.Nodes[i].Depth == 0) roots.Add(i);
        foreach (var r in roots) DrawNode(c, v, r, scope);
    }

    private void DrawNode(IUiCanvas c, RowView v, int index, string scope)
    {
        var p = v.Plan;
        var n = v.Nodes[index];
        c.PushId(n.Key);
        try
        {
            var selectable = p.Selected != null;
            var label = selectable && n.Key == v.SelectedKey ? "> " + n.Label : n.Label;
            if (n.Branch)
            {
                var open = v.Expanded.Contains(n.Key);
                var nowOpen = c.TreeNode(label + "###n", open, out var clicked);
                if (clicked && selectable) Select(v, n.Key);
                if (nowOpen != open)
                {
                    v.Expanded = nowOpen ? v.Expanded.Add(n.Key) : v.Expanded.Remove(n.Key);
                    WriteExpanded(v);
                }
                if (!nowOpen) return;
                try
                {
                    DrawRows(c, n.Rows, scope + "/" + n.Key);
                    foreach (var child in n.Children) DrawNode(c, v, child, scope);
                }
                finally { c.TreePop(); }
                return;
            }
            if (selectable)
            {
                if (c.Selectable(label + "###n", n.Key == v.SelectedKey)) Select(v, n.Key);
            }
            else c.Text(n.Label, null, false);
            DrawRows(c, n.Rows, scope + "/" + n.Key);
        }
        finally { c.PopId(); }
    }

    private void Select(RowView v, string key)
    {
        v.SelectedKey = key;
        WriteState(v.Plan.Selected!.Key!, new UiStateValue.String(key), UiStateChangeCause.Widget);
    }

    private void WriteExpanded(RowView v)
    {
        var set = v.Expanded.Order(StringComparer.Ordinal).ToImmutableArray();
        if (v.Plan.Expanded != null) WriteState(v.Plan.Expanded.Key!, new UiStateValue.StringSet(set), UiStateChangeCause.Widget);
        else WriteControl(v.Plan.Id, new UiControlState(State.Control(v.Plan.Id)?.Selected, set), UiStateChangeCause.Widget);
    }

    private void WriteState(string key, UiStateValue value, UiStateChangeCause cause) =>
        State.Enqueue(UiWindowState.WriteKind.Value, key, value, null, null, cause, wantResult: false);

    private void WriteControl(string rowId, UiControlState control, UiStateChangeCause cause) =>
        State.Enqueue(UiWindowState.WriteKind.Control, rowId, null, control, null, cause, wantResult: false);

    // ---- refresh ----

    /// <summary>Reads every value the rows need once (one context), and builds the rows' views anew.</summary>
    internal void Refresh(long now)
    {
        Refreshes++;
        Volatile.Write(ref _nextRefresh, now + (long)(Plan.Block.Refresh * 1000));
        var language = UiRuntime.Backend.Language;
        var ctx = new UiRefreshContext(Plan.Scope ?? new UiScope { Mod = Plan.Mod }, State.View(), b => UiEdits.Pending(b));
        var build = new Build(this, language, State.Snapshot.Version.Session);
        var views = build.Rows(Plan.Rows, ctx);
        if (build.Chars > MaxTextChars)
            views = [new RowView { Plan = new RowPlan { Type = "text", Id = "budget" }, Text = KilnText.T("blocks.ui.too_much_text") }];
        _domains = build.Domains;
        _views = views;
    }

    private sealed class Build(WindowRuntime w, string language, ulong session)
    {
        public long Chars;
        public readonly Dictionary<string, ImmutableHashSet<string>> Domains = new(StringComparer.Ordinal);

        private string Count(string s)
        {
            Chars += s.Length;
            return s;
        }

        public RowView[] Rows(IReadOnlyList<RowPlan> plans, UiRefreshContext ctx)
        {
            var views = new RowView[plans.Count];
            for (var i = 0; i < plans.Count; i++) views[i] = Row(plans[i], ctx);
            return views;
        }

        private RowView Row(RowPlan p, UiRefreshContext ctx)
        {
            var v = new RowView { Plan = p, Session = session };
            v.Visible = p.When?.Test(ctx.Value) ?? true;
            if (!v.Visible) return v;
            v.Tooltip = p.Tooltip?.Render(language, ctx.Value) is { } tip ? Count(tip) : null;
            v.Label = Count(p.Label?.Render(language, ctx.Value) ?? "");
            v.Text = Count(p.Text?.Render(language, ctx.Value) ?? "");
            switch (p.Type)
            {
                case "table": Table(v, ctx); return v;
                case "tabs": Tabs(v, ctx); return v;
                case "tree": Tree(v, ctx); return v;
                case "image":
                    if (p.ImageFile != null && w._images.TryGetValue(p.ImageFile, out var img))
                    {
                        v.Image = img.Image;
                        if (img.Image == null) v.Notice = KilnText.T("blocks.ui.no_picture", ("reason", img.Problem ?? ""));
                    }
                    return v;
            }
            if (p.Bind == null) return v;
            var value = ctx.Value(p.Bind);
            v.ValueText = Count(UiBindings.Text(value, p.Format));
            if (p.Type is "toggle" or "slider" or "choice" or "input") v.ReadOnly = p.Bind.IsConfig && UiEdits.ReadOnly(p.Bind);
            // what the frames draw as it is: no string is made per frame
            if (p.Type == "value") v.Text = v.Label + ":";
            else if (v.ReadOnly) v.Text = $"{v.Label}: {v.ValueText}";
            switch (p.Type)
            {
                case "toggle":
                    v.Bool = value is true;
                    break;
                case "slider" when p.IsInt:
                    v.Int = UiBindings.ToDouble(value) is { } di ? (int)Math.Clamp(Math.Round(di), p.Min, p.Max) : (int)p.Min;
                    break;
                case "slider":
                    v.Float = UiBindings.ToDouble(value) is { } df ? (float)Math.Clamp(df, p.Min, p.Max) : (float)p.Min;
                    break;
                case "choice":
                    v.Index = value is string s ? Math.Max(0, p.Options.ToList().FindIndex(o => string.Equals(o, s, StringComparison.OrdinalIgnoreCase))) : 0;
                    break;
                case "input":
                    v.Committed = value as string ?? "";
                    break;
            }
            return v;
        }

        private string? SelectedOf(RowPlan p, UiRefreshContext ctx) =>
            p.Selected != null ? ctx.Value(p.Selected) as string : w.State.Control(p.Id)?.Selected;

        private void Table(RowView v, UiRefreshContext ctx)
        {
            var p = v.Plan;
            v.Headers = p.Columns.Select(col => Count(col.Label.Render(language, ctx.Value))).ToList();
            v.SelectedKey = SelectedOf(p, ctx);
            var items = ctx.Value(p.Items!);
            if (items is not UiRowsValue rows)
            {
                // absence was not established: the selection stays
                v.Notice = KilnText.T("blocks.ui.unavailable", ("reason", (items as UiUnavailable)?.Reason ?? "NoData"));
                return;
            }
            if (rows.Rows.Length > p.MaxRows)
            {
                v.Notice = KilnText.T("blocks.ui.too_many_rows", ("count", rows.Rows.Length), ("max", p.MaxRows));
                return;
            }
            var list = new List<(string, IReadOnlyList<string>)>(rows.Rows.Length);
            foreach (var r in rows.Rows)
            {
                var rc = ctx.ForRow(r);
                list.Add((r.Key, p.Columns.Select(col => Count(col.Cell.Render(language, rc.Value))).ToArray()));
            }
            v.TableRows = list;
            if (p.Selected != null)
            {
                var keys = rows.Rows.Select(r => r.Key).ToImmutableHashSet(StringComparer.Ordinal);
                Domains[p.Selected.Key!] = keys;
                if (v.SelectedKey is { Length: > 0 } sel && !keys.Contains(sel)) w.Invalidate(p.Selected.Key!, rows.Version, sel, "", v);
            }
        }

        private void Tabs(RowView v, UiRefreshContext ctx)
        {
            var p = v.Plan;
            var tabs = p.Tabs.Select(t =>
            {
                var visible = t.When?.Test(ctx.Value) ?? true;
                return new TabView(t.Id, Count(t.Label.Render(language, ctx.Value)), visible, visible ? Rows(t.Rows, ctx) : []);
            }).ToList();
            v.Tabs = tabs;
            var visibleIds = tabs.Where(t => t.Visible).Select(t => t.Id).ToImmutableHashSet(StringComparer.Ordinal);
            if (p.Selected != null) Domains[p.Selected.Key!] = visibleIds;
            var selected = SelectedOf(p, ctx);
            var first = tabs.FirstOrDefault(t => t.Visible)?.Id;
            if (selected is { Length: > 0 } && visibleIds.Contains(selected))
            {
                v.SelectedKey = selected;
                // valid again: the next time this tab is hidden it moves again (the cache only stops repeats of one hide)
                w.ForgetInvalidation(p.Selected != null ? p.Selected.Key! : "#" + p.Id);
            }
            else
            {
                v.SelectedKey = first;
                // a hidden or removed selected tab: the first visible one, reported
                if (selected is { Length: > 0 } && first != selected)
                {
                    if (p.Selected != null) w.Invalidate(p.Selected.Key!, 0, selected, first ?? "", v);
                    else w.InvalidateControl(p.Id, selected, first);
                }
            }
        }

        private void Tree(RowView v, UiRefreshContext ctx)
        {
            var p = v.Plan;
            UiTreeValue tree;
            if (p.StaticTree != null) tree = p.StaticTree;
            else if (ctx.Value(p.Nodes!) is UiTreeValue t) tree = t;
            else
            {
                v.Notice = KilnText.T("blocks.ui.unavailable", ("reason", (ctx.Value(p.Nodes!) as UiUnavailable)?.Reason ?? "NoData"));
                return;
            }
            var keys = tree.Nodes.Select(n => n.Key).ToImmutableHashSet(StringComparer.Ordinal);
            var branches = tree.Nodes.Where(n => n.HasChildren).Select(n => n.Key).ToImmutableHashSet(StringComparer.Ordinal);
            v.SelectedKey = SelectedOf(p, ctx);
            var expanded = p.Expanded != null
                ? (ctx.Value(p.Expanded) is ImmutableArray<string> set ? set : ImmutableArray<string>.Empty)
                : (w.State.Control(p.Id)?.Expanded ?? []).ToImmutableArray();
            v.Expanded = expanded.Where(branches.Contains).ToImmutableHashSet(StringComparer.Ordinal);
            if (p.Selected != null)
            {
                Domains[p.Selected.Key!] = keys;
                if (v.SelectedKey is { Length: > 0 } sel && !keys.Contains(sel)) w.Invalidate(p.Selected.Key!, tree.Version, sel, "", v);
            }
            if (p.Expanded != null) Domains[p.Expanded.Key!] = branches;
            if (v.Expanded.Count != expanded.Length) w.Prune(p, tree.Version, v.Expanded);
            var index = new Dictionary<string, int>(StringComparer.Ordinal);
            var children = new List<int>[tree.Nodes.Length];
            for (var i = 0; i < tree.Nodes.Length; i++)
            {
                index[tree.Nodes[i].Key] = i;
                children[i] = new List<int>();
                if (tree.Nodes[i].Parent is { } parent && index.TryGetValue(parent, out var pi)) children[pi].Add(i);
            }
            var nodes = new TreeNodeView[tree.Nodes.Length];
            for (var i = 0; i < tree.Nodes.Length; i++)
            {
                var n = tree.Nodes[i];
                var nc = ctx.ForNode(n);
                nodes[i] = new TreeNodeView(n.Key, Count(n.Label.For(language)), n.Depth, n.HasChildren, p.NodeRows.Count > 0 ? Rows(p.NodeRows, nc) : [], children[i]);
            }
            v.Nodes = nodes;
        }
    }

    // A selection that valid data no longer shows is cleared (or moved to the first tab) once per source version.
    private void Invalidate(string stateKey, ulong version, string was, string now, RowView v)
    {
        lock (_invalidated)
        {
            if (_invalidated.TryGetValue(stateKey, out var last) && last == (version, was)) return;
            _invalidated[stateKey] = (version, was);
        }
        v.SelectedKey = now.Length == 0 ? null : now;
        State.Enqueue(UiWindowState.WriteKind.Value, stateKey, new UiStateValue.String(now), null, null, UiStateChangeCause.SelectionInvalidated,
            cur => cur.Values.TryGetValue(stateKey, out var x) && x is UiStateValue.String s && s.Value == was ? null : "Moved", wantResult: false);
    }

    private void ForgetInvalidation(string key)
    {
        lock (_invalidated) _invalidated.Remove(key);
    }

    private void InvalidateControl(string rowId, string was, string? now)
    {
        lock (_invalidated)
        {
            if (_invalidated.TryGetValue("#" + rowId, out var last) && last == (0, was)) return;
            _invalidated["#" + rowId] = (0, was);
        }
        State.Enqueue(UiWindowState.WriteKind.Control, rowId, null, new UiControlState(now, []), null, UiStateChangeCause.SelectionInvalidated, wantResult: false);
    }

    private void Prune(RowPlan p, ulong version, ImmutableHashSet<string> kept)
    {
        var key = "!" + p.Id;
        var was = string.Join("\n", kept.Order(StringComparer.Ordinal));
        lock (_invalidated)
        {
            if (_invalidated.TryGetValue(key, out var last) && last == (version, was)) return;
            _invalidated[key] = (version, was);
        }
        var set = kept.Order(StringComparer.Ordinal).ToImmutableArray();
        if (p.Expanded != null) State.Enqueue(UiWindowState.WriteKind.Value, p.Expanded.Key!, new UiStateValue.StringSet(set), null, null, UiStateChangeCause.Pruned, wantResult: false);
        else State.Enqueue(UiWindowState.WriteKind.Control, p.Id, null, new UiControlState(State.Control(p.Id)?.Selected, set), null, UiStateChangeCause.Pruned, wantResult: false);
    }
}
