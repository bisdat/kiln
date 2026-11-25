// SPDX-License-Identifier: MIT
// Where a mod's screen or window sits (GitHub issue 146; since 0.2.0-dev.31.11, mod API 0.2.20; docs/kiln/ui.md "Placing
// screens and windows"): UiScreenOptions.Position, MoveTo / Resize applied once on the next frame the screen or window
// draws (ImGuiCond_Always for that frame only), and the opt-in remembered placement (RememberPlacement): Kiln keeps where
// the player left it in ui-placement.json, in the config folder beside keybinds.json (the active profile's configDir,
// else kiln/config), per mod and per screen or window id.
//
// Units, as UiWindowOptions.Position has always had them: a position is a fraction of the viewport (0..1), the window's
// centre by default (a pivot names another point); a size is pixels at UI scale 1. What Kiln places by a position (the
// option, a remembered placement, MoveTo) and every size a mod or the file gives is kept inside the viewport. ImGui applies
// its own minimum size and any size constraints on top (CalcWindowSizeAfterConstraint in Begin).
//
// Precedence per frame: an explicit MoveTo / Resize (Always, that frame only), then the remembered placement, then the
// options (screens: whenever the screen appears; windows: their FirstUseEver options). Managed only: kiln.ui's
// set_next_window_pos / set_next_window_size / window_rect slots are all it needs.
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>Where a Kiln screen or window was drawn last (<see cref="UiScreen.Placement"/>, <see cref="UiWindow.Placement"/>).
/// Positions are fractions of the viewport (0 = the left or top edge, 1 = the right or bottom edge); sizes are pixels at UI
/// scale 1, the units of <see cref="UiScreenOptions.Width"/>. Since 0.2.0-dev.31.11, mod API 0.2.20.</summary>
/// <remarks>Hand it to <see cref="UiScreen.MoveTo"/> / <see cref="UiWindow.MoveTo"/> of another screen to place that one
/// beside it: <c>MoveTo(p.Right, p.Y, pivotX: 0)</c> puts the other one's left edge on this one's right edge.</remarks>
public readonly record struct UiPlacement
{
    /// <summary>The centre, as a fraction of the viewport's width.</summary>
    public float X { get; init; }
    /// <summary>The centre, as a fraction of the viewport's height.</summary>
    public float Y { get; init; }
    /// <summary>The width in pixels at UI scale 1.</summary>
    public float Width { get; init; }
    /// <summary>The height in pixels at UI scale 1.</summary>
    public float Height { get; init; }
    /// <summary>The left edge, as a fraction of the viewport's width.</summary>
    public float Left { get; init; }
    /// <summary>The top edge, as a fraction of the viewport's height.</summary>
    public float Top { get; init; }
    /// <summary>The right edge, as a fraction of the viewport's width.</summary>
    public float Right { get; init; }
    /// <summary>The bottom edge, as a fraction of the viewport's height.</summary>
    public float Bottom { get; init; }

    internal static UiPlacement FromPixels(in UiViewport vp, float x, float y, float w, float h)
    {
        var s = vp.SafeScale;
        return new UiPlacement
        {
            X = (x + w / 2) / vp.Width,
            Y = (y + h / 2) / vp.Height,
            Width = w / s,
            Height = h / s,
            Left = x / vp.Width,
            Top = y / vp.Height,
            Right = (x + w) / vp.Width,
            Bottom = (y + h) / vp.Height,
        };
    }
}

/// <summary>The overlay's display this frame: pixels and the UI scale.</summary>
internal readonly record struct UiViewport(float Width, float Height, float Scale)
{
    public static UiViewport Of(UiDraw ui) => new(ui.Width, ui.Height, ui.Scale);

    public bool Valid => Width > 0 && Height > 0 && float.IsFinite(Width) && float.IsFinite(Height);

    /// <summary>The scale to divide by (a frame without one: 1).</summary>
    public float SafeScale => Scale > 0 && float.IsFinite(Scale) ? Scale : 1;
}

/// <summary>What Kiln asks of ImGui before a screen's or window's Begin: SetNextWindowSize, then SetNextWindowPos.</summary>
internal readonly record struct UiPlan(
    bool HasSize, float W, float H, uint SizeCond,
    bool HasPos, float X, float Y, uint PosCond, float PivotX, float PivotY)
{
    public const uint Always = 1, FirstUseEver = 4, Appearing = 8;

    public void Apply(UiDraw ui)
    {
        if (HasSize) ui.NextWindowSize(W, H, SizeCond);
        if (HasPos) ui.NextWindowPos(X, Y, PosCond, PivotX, PivotY);
    }

    public UiPlan WithSize(float w, float h, uint cond) => this with { HasSize = true, W = w, H = h, SizeCond = cond };

    public UiPlan WithPos(float x, float y, uint cond, float px = 0.5f, float py = 0.5f) =>
        this with { HasPos = true, X = x, Y = y, PosCond = cond, PivotX = px, PivotY = py };
}

/// <summary>The placement state of one <see cref="UiScreen"/> or <see cref="UiWindow"/>: the calls waiting for the next frame
/// (any thread), what was drawn last (render thread), the remembered placement's key.</summary>
internal sealed class UiPlacer
{
    private readonly object _lock = new();
    private (float X, float Y, float Px, float Py)? _move;
    private (float W, float H)? _size;
    private UiPlacement? _placement;
    private bool _appearing = true;
    // render thread only: the rect drawn last, in pixels
    private (float X, float Y, float W, float H)? _last;

    /// <summary>The owning mod's id (Kiln's own: "kiln").</summary>
    public string Mod { get; }
    /// <summary>The remembered placement's id; null when the screen or window does not remember.</summary>
    public string? Id { get; }
    public bool IsWindow { get; }

    private UiPlacer(string mod, string? id, bool window)
    {
        Mod = mod;
        Id = id;
        IsWindow = window;
        if (id != null) UiPlacements.EnsureLoaded();  // on the creating thread, never the render thread's first frame
    }

    public static UiPlacer For(ModContext? owner, bool remember, string? placementId, Func<string> title, bool window)
    {
        string? id = null;
        if (remember)
        {
            id = placementId ?? title();
            if (string.IsNullOrWhiteSpace(id))
                throw new ArgumentException("RememberPlacement needs a PlacementId or a title to remember the placement under", nameof(placementId));
        }
        return new UiPlacer(owner?.Info.Id ?? "kiln", id, window);
    }

    public UiPlacement? Placement
    {
        get { lock (_lock) return _placement; }
    }

    public void Move(float x, float y, float pivotX, float pivotY)
    {
        CheckMove(x, y, pivotX, pivotY);
        lock (_lock) _move = (x, y, pivotX, pivotY);
    }

    public void Resize(float width, float height)
    {
        CheckSize(width, height);
        lock (_lock) _size = (width, height);
    }

    public static void CheckMove(float x, float y, float pivotX, float pivotY)
    {
        if (!float.IsFinite(x)) throw new ArgumentOutOfRangeException(nameof(x), x, "a fraction of the viewport (a finite number)");
        if (!float.IsFinite(y)) throw new ArgumentOutOfRangeException(nameof(y), y, "a fraction of the viewport (a finite number)");
        if (!(pivotX is >= 0 and <= 1)) throw new ArgumentOutOfRangeException(nameof(pivotX), pivotX, "0 (left) to 1 (right)");
        if (!(pivotY is >= 0 and <= 1)) throw new ArgumentOutOfRangeException(nameof(pivotY), pivotY, "0 (top) to 1 (bottom)");
    }

    public static void CheckSize(float width, float height)
    {
        if (!(float.IsFinite(width) && width > 0)) throw new ArgumentOutOfRangeException(nameof(width), width, "pixels at UI scale 1, more than 0");
        if (!(float.IsFinite(height) && height > 0)) throw new ArgumentOutOfRangeException(nameof(height), height, "pixels at UI scale 1, more than 0");
    }

    /// <summary>The screen or window opened (any thread): the next frame is its first.</summary>
    public void Opened()
    {
        lock (_lock)
        {
            _appearing = true;
            Opens++;
        }
    }

    /// <summary>How many times it opened (the test kit's frames: a reopen is a frame where it appears).</summary>
    internal long Opens { get; private set; }

    private ((float X, float Y, float Px, float Py)? Move, (float W, float H)? Size, bool Appearing) Take()
    {
        lock (_lock)
        {
            var r = (_move, _size, _appearing);
            _move = null;
            _size = null;
            _appearing = false;
            return r;
        }
    }

    private UiPlacements.Entry? Remembered => Id is { } id ? UiPlacements.Get(Mod, IsWindow, id) : null;

    /// <summary>A screen's frame (not a full-screen one): its options' size and place whenever it appears (unchanged from
    /// before 0.2.0-dev.31.11 when nothing new is used), the remembered placement over them, the waiting calls over both.</summary>
    public UiPlan PlanScreen(in UiViewport vp, UiScreenOptions o)
    {
        var (move, size, appearing) = Take();
        var s = vp.Scale;
        var plan = default(UiPlan);
        var remembered = Remembered;
        float bw = o.Width * s, bh = o.Height * s;
        if (remembered is { } r && o.Resizable) (bw, bh) = ClampSize(vp, r.Width * s, r.Height * s);
        plan = plan.WithSize(bw, bh, UiPlan.Appearing);
        if (size is { } z)
        {
            var (w, h) = ClampSize(vp, z.W * s, z.H * s);
            plan = plan.WithSize(w, h, UiPlan.Always);
        }
        var effective = size is null && !appearing && _last is { } l ? (l.W, l.H) : (plan.W, plan.H);
        (float X, float Y)? at = remembered is { } rr ? (rr.X, rr.Y) : o.Position;
        if (at is { } a && float.IsFinite(a.X) && float.IsFinite(a.Y))
        {
            var (x, y) = ClampAnchor(vp, a.X * vp.Width, a.Y * vp.Height, 0.5f, 0.5f, (plan.W, plan.H));
            plan = plan.WithPos(x, y, UiPlan.Appearing);
        }
        else plan = plan.WithPos(vp.Width / 2, vp.Height / 2, UiPlan.Appearing);
        return Explicit(plan, vp, move, size, appearing, effective);
    }

    /// <summary>A window's frame: its FirstUseEver options as before, the remembered placement whenever it appears, the
    /// waiting calls over both.</summary>
    public UiPlan PlanWindow(in UiViewport vp, UiWindowOptions o)
    {
        var (move, size, appearing) = Take();
        var s = vp.Scale;
        var plan = default(UiPlan);
        if (o.Width > 0 || o.Height > 0) plan = plan.WithSize(o.Width * s, o.Height * s, UiPlan.FirstUseEver);
        if (o.Position is { } p) plan = plan.WithPos(p.X * vp.Width, p.Y * vp.Height, UiPlan.FirstUseEver);
        (float W, float H)? known = _last is { } l ? (l.W, l.H) : plan.HasSize && o.Width > 0 && o.Height > 0 ? (plan.W, plan.H) : null;
        if (Remembered is { } r)
        {
            if (!o.AutoResize)
            {
                var (w, h) = ClampSize(vp, r.Width * s, r.Height * s);
                plan = plan.WithSize(w, h, UiPlan.Appearing);
                known = (w, h);
            }
            var (x, y) = ClampAnchor(vp, r.X * vp.Width, r.Y * vp.Height, 0.5f, 0.5f, known);
            plan = plan.WithPos(x, y, UiPlan.Appearing);
        }
        if (size is { } z)
        {
            var (w, h) = ClampSize(vp, z.W * s, z.H * s);
            plan = plan.WithSize(w, h, UiPlan.Always);
        }
        (float, float)? effective = size is not null ? (plan.W, plan.H) : !appearing && _last is { } last ? (last.W, last.H) : known;
        return Explicit(plan, vp, move, size, appearing, effective);
    }

    // The calls a mod made since the last frame, over the plan: a move places its pivot point (the whole window kept inside
    // the viewport when its size is known); a resize alone keeps the window's top left corner, moved back in if needed.
    private UiPlan Explicit(UiPlan plan, in UiViewport vp, (float X, float Y, float Px, float Py)? move, (float W, float H)? size,
        bool appearing, (float W, float H)? effective)
    {
        if (move is { } m)
        {
            var (x, y) = ClampAnchor(vp, m.X * vp.Width, m.Y * vp.Height, m.Px, m.Py, effective);
            return plan.WithPos(x, y, UiPlan.Always, m.Px, m.Py);
        }
        if (size is not null && !appearing && _last is { } l)
        {
            var (x, y) = ClampAnchor(vp, l.X, l.Y, 0, 0, (plan.W, plan.H));
            return plan.WithPos(x, y, UiPlan.Always, 0, 0);
        }
        return plan;
    }

    /// <summary>After Begin (render thread): where ImGui drew it (pixels, <see cref="UiDraw.WindowRect"/>). A frame whose
    /// window was not visible (collapsed, hidden) or has no rect keeps the last one. A remembering screen or window keeps
    /// the rect in ui-placement.json when it moved by half a pixel or more.</summary>
    public void Drawn(in UiViewport vp, float x, float y, float w, float h, bool visible)
    {
        if (!visible || !vp.Valid || !(w > 0 && h > 0) || !float.IsFinite(x + y + w + h)) return;
        _last = (x, y, w, h);
        var p = UiPlacement.FromPixels(vp, x, y, w, h);
        lock (_lock) _placement = p;
        if (Id is not { } id) return;
        var s = vp.SafeScale;
        if (UiPlacements.Get(Mod, IsWindow, id) is { } old &&
            MathF.Abs(old.X - p.X) * vp.Width < 0.5f && MathF.Abs(old.Y - p.Y) * vp.Height < 0.5f &&
            MathF.Abs(old.Width - p.Width) * s < 0.5f && MathF.Abs(old.Height - p.Height) * s < 0.5f) return;
        UiPlacements.Put(Mod, IsWindow, id, new UiPlacements.Entry(p.X, p.Y, p.Width, p.Height));
    }

    internal static (float W, float H) ClampSize(in UiViewport vp, float w, float h) =>
        (vp.Valid ? MathF.Min(w, vp.Width) : w, vp.Valid ? MathF.Min(h, vp.Height) : h);

    /// <summary>The pivot point <paramref name="x"/>, <paramref name="y"/> moved so the whole rect of <paramref name="size"/>
    /// is inside the viewport (a rect bigger than it: its top left corner at 0); without a size, the point itself.</summary>
    internal static (float X, float Y) ClampAnchor(in UiViewport vp, float x, float y, float px, float py, (float W, float H)? size)
    {
        if (!vp.Valid) return (x, y);
        if (size is not { } z || !(z.W > 0 && z.H > 0)) return (Math.Clamp(x, 0, vp.Width), Math.Clamp(y, 0, vp.Height));
        var left = Math.Clamp(x - px * z.W, 0, MathF.Max(0, vp.Width - z.W));
        var top = Math.Clamp(y - py * z.H, 0, MathF.Max(0, vp.Height - z.H));
        return (left + px * z.W, top + py * z.H);
    }
}

/// <summary>ui-placement.json: the placements of remembering screens and windows, per mod and id, in the config folder
/// beside keybinds.json. Read once (on the thread that creates the first remembering screen or window), written on a
/// worker a moment after a change and at once when a mod unloads (a whole new file put in place, so a crash mid-write
/// leaves the old one; a write that fails is tried again by the next flush). A file that is no JSON is kept aside as
/// ui-placement.json.bad with a warning; an entry that is no placement is skipped.</summary>
internal static class UiPlacements
{
    internal const string FileName = "ui-placement.json";

    /// <summary>One remembered placement: the centre as fractions of the viewport, the size in pixels at UI scale 1.</summary>
    internal readonly record struct Entry(float X, float Y, float Width, float Height);

    private static readonly object Lock = new();
    private static readonly object WriteLock = new();
    private static Dictionary<(string Mod, bool Window, string Id), Entry> _entries = [];
    private static string? _path;
    private static bool _loaded, _dirty, _scheduled;
    // the file is there but could not be read (locked, no permission): kept as it is this start, placements stay in memory
    private static bool _unreadable;

    /// <summary>How long after a change the file is written (a drag changes it every frame). Tests set it.</summary>
    internal static TimeSpan Debounce = TimeSpan.FromSeconds(1);

    /// <summary>The file: the profile's config folder when one is active, else kiln/config; null when it cannot be worked out.</summary>
    public static string? FilePath
    {
        get
        {
            try
            {
                return Path.Combine(KilnPaths.Config, FileName);
            }
            catch
            {
                return null;
            }
        }
    }

    public static void EnsureLoaded()
    {
        lock (Lock) LoadLocked();
    }

    private static void LoadLocked()
    {
        var path = FilePath;
        if (_loaded && path == _path) return;
        _loaded = true;
        _path = path;
        _dirty = false;
        _entries = Read(path, out _unreadable);
    }

    public static Entry? Get(string mod, bool window, string id)
    {
        lock (Lock)
        {
            LoadLocked();
            return _entries.TryGetValue((mod, window, id), out var e) ? e : null;
        }
    }

    public static void Put(string mod, bool window, string id, Entry entry)
    {
        lock (Lock)
        {
            LoadLocked();
            _entries[(mod, window, id)] = entry;
            _dirty = true;
            if (_scheduled) return;
            _scheduled = true;
        }
        var wait = Debounce;
        _ = Task.Run(async () =>
        {
            if (wait > TimeSpan.Zero) await Task.Delay(wait).ConfigureAwait(false);
            Flush();
        });
    }

    /// <summary>Writes what changed now (on the calling thread); nothing when nothing changed. The debounce worker calls it,
    /// and so does a mod's unload (<see cref="UiEngine.Retire"/>: also the game's shutdown, which unloads every mod), so a
    /// drag just before a quit is kept. Not on the render thread.</summary>
    public static void Flush()
    {
        // the snapshot is taken under WriteLock: two flushes at once (the worker and an unload) write in the order they
        // copied, so an older copy never lands over a newer one
        lock (WriteLock)
        {
            string? path;
            KeyValuePair<(string Mod, bool Window, string Id), Entry>[] copy;
            lock (Lock)
            {
                _scheduled = false;
                if (!_dirty) return;
                _dirty = false;
                path = _unreadable ? null : _path;
                copy = [.. _entries];
            }
            if (path is null || Write(path, copy)) return;
            // not written (a virus scanner or sync tool holding the file, a full disk): still to be written by the next
            // flush (the next change, a mod's unload, the game's quit), unless the file was read again from elsewhere
            lock (Lock)
                if (_path == path && !_unreadable) _dirty = true;
        }
    }

    /// <summary>Forgets what was read and not written (a game start reads the file again; the test kit's Restart).</summary>
    internal static void Forget()
    {
        lock (Lock)
        {
            _entries = [];
            _path = null;
            _loaded = _dirty = _scheduled = _unreadable = false;
        }
    }

    internal static void ResetForTests()
    {
        Forget();
        Debounce = TimeSpan.FromSeconds(1);
    }

    private static Dictionary<(string, bool, string), Entry> Read(string? path, out bool unreadable)
    {
        unreadable = false;
        var entries = new Dictionary<(string, bool, string), Entry>();
        if (path is null) return entries;
        string text;
        try
        {
            if (!File.Exists(path)) return entries;
            text = File.ReadAllText(path);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // writing it later would put this start's few placements over every other one the file holds
            unreadable = true;
            UiEngine.Log(LogLevel.Warn, $"{path}: {e.Message} (no remembered placements this start; the file is left as it is)");
            return entries;
        }
        // JsonDocument, not JsonNode: a key given twice (a hand-edited file) is read as the last one, where JsonObject throws
        // ArgumentException out of the read (the first Ui.Screen that remembers would throw in its mod's OnLoad)
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(text, new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
        }
        catch (JsonException e)
        {
            Corrupt(path, e.Message);
            return entries;
        }
        using (doc)
        {
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                Corrupt(path, "not a JSON object");
                return entries;
            }
            if (!root.TryGetProperty("mods", out var mods) || mods.ValueKind != JsonValueKind.Object) return entries;
            foreach (var m in mods.EnumerateObject())
            {
                if (m.Value.ValueKind != JsonValueKind.Object) continue;
                foreach (var (kind, window) in new[] { ("screens", false), ("windows", true) })
                {
                    if (!m.Value.TryGetProperty(kind, out var ids) || ids.ValueKind != JsonValueKind.Object) continue;
                    foreach (var id in ids.EnumerateObject())
                        if (Parse(id.Value) is { } e) entries[(m.Name, window, id.Name)] = e;
                }
            }
        }
        return entries;
    }

    private static Entry? Parse(JsonElement v)
    {
        if (v.ValueKind != JsonValueKind.Object) return null;
        static float? Num(JsonElement o, string k)
        {
            if (!o.TryGetProperty(k, out var n) || n.ValueKind != JsonValueKind.Number || !n.TryGetDouble(out var d)) return null;
            var f = (float)d;
            return float.IsFinite(f) ? f : null;
        }
        if (Num(v, "x") is not { } x || Num(v, "y") is not { } y || Num(v, "width") is not { } w || Num(v, "height") is not { } h) return null;
        if (!(w > 0 && h > 0)) return null;
        return new Entry(x, y, w, h);
    }

    private static void Corrupt(string path, string why)
    {
        try
        {
            File.Copy(path, path + ".bad", overwrite: true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // the warning below still says what happened
        }
        UiEngine.Log(LogLevel.Warn, $"{path}: {why} (kept as {FileName}.bad; screens and windows start from their options)");
    }

    private static bool Write(string path, KeyValuePair<(string Mod, bool Window, string Id), Entry>[] entries)
    {
        try
        {
            var mods = new JsonObject();
            foreach (var g in entries.GroupBy(e => e.Key.Mod).OrderBy(g => g.Key, StringComparer.Ordinal))
            {
                var m = new JsonObject();
                foreach (var (kind, window) in new[] { ("screens", false), ("windows", true) })
                {
                    var ids = new JsonObject();
                    foreach (var (k, e) in g.Where(e => e.Key.Window == window).OrderBy(e => e.Key.Id, StringComparer.Ordinal))
                        ids[k.Id] = new JsonObject
                        {
                            ["x"] = Math.Round((double)e.X, 6),
                            ["y"] = Math.Round((double)e.Y, 6),
                            ["width"] = Math.Round((double)e.Width, 1),
                            ["height"] = Math.Round((double)e.Height, 1),
                        };
                    if (ids.Count > 0) m[kind] = ids;
                }
                mods[g.Key] = m;
            }
            var root = new JsonObject
            {
                ["comment"] = "Kiln: where the player left mods' screens and windows that remember their placement (RememberPlacement). " +
                              "x, y: the centre as a fraction of the screen; width, height: pixels at UI scale 1. Delete an entry, or the file, to forget it.",
                ["version"] = 1,
                ["mods"] = mods,
            };
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var tmp = path + ".tmp";
            File.WriteAllText(tmp, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));
            File.Move(tmp, path, overwrite: true);
            return true;
        }
        catch (Exception e)
        {
            UiEngine.Log(LogLevel.Warn, $"saving {path}: {e.Message}");
            return false;
        }
    }
}