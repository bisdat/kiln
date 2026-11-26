// SPDX-License-Identifier: MIT
// Window docking across mods (GitHub issue 149; docs/kiln/ui.md "Docking windows"; design:
// dev31/out/issue-149-window-dock/DESIGN.md). A window docks to another one (also another mod's, when that one accepts
// docks) and Kiln keeps them together in the overlay's frame, on the render thread, in the same frame:
//  - each frame the windows of a dock group are drawn from the one that leads it (the one the player drags or resizes,
//    else one with a waiting MoveTo, else the group's root), and every other one is placed from its neighbour's rect of
//    this frame (SetNextWindowPos, ImGuiCond_Always, with a pivot so the shared edge is exact while its size is unknown);
//  - the group is kept inside the viewport (the leader moved back in before its Begin), and a docked window that does not
//    fit on its side when its dock becomes active goes on the other side;
//  - Shift held while the player drags one: it moves alone and leaves its dock (and the windows docked to it leave it);
//    dropped within 16 px of its old side, it docks again;
//  - a docked window whose target exists but is closed waits (up to 250 ms, not drawn) when it opens, so one key press
//    opening both never shows it at its own place first.
// Native: kiln.ui's window_state (kiln_ui.h, issue 149) says before each Begin where a window is this frame and whether the
// player drags or resizes it; without it (an older kiln-ui.dll) groups lead from their root.
namespace Kiln;

/// <summary>Which side of the other window a docked window sits on (<see cref="UiWindow.DockTo"/>). Since
/// 0.2.0-dev.31.16, mod API 0.2.23 (GitHub issue 149).</summary>
public enum DockSide
{
    /// <summary>Left of it: the docked window's right edge on its left edge.</summary>
    Left = 0,
    /// <summary>Right of it: the docked window's left edge on its right edge.</summary>
    Right = 1,
    /// <summary>Above it: the docked window's bottom edge on its top edge.</summary>
    Top = 2,
    /// <summary>Below it: the docked window's top edge on its bottom edge.</summary>
    Bottom = 3,
}

/// <summary>
/// A window of some mod that accepts docks (<see cref="UiWindowOptions.AcceptDock"/>), by its mod's id and its placement id
/// (<see cref="Ui.FindWindow"/>): what <see cref="UiWindow.DockTo"/> takes. The handle is a name, looked up each time it is
/// used: it keeps nothing of the other mod alive, and when that mod reloads and makes its window again (the same id, still
/// accepting docks), the same handle answers it, and a window docked to it goes back beside it. Two handles of the same mod
/// and id are equal (<see cref="Equals(UiWindowHandle?)"/> and <c>==</c>). Any thread. Since 0.2.0-dev.31.16, mod API 0.2.23 (GitHub issue 149).
/// </summary>
public sealed class UiWindowHandle : IEquatable<UiWindowHandle>
{
    internal UiWindowHandle(string modId, string placementId)
    {
        ModId = modId;
        PlacementId = placementId;
    }

    /// <summary>The id of the mod whose window it is.</summary>
    public string ModId { get; }

    /// <summary>The window's placement id (<see cref="UiWindowOptions.PlacementId"/>, else its title when it was made).</summary>
    public string PlacementId { get; }

    /// <summary>Such a window exists now: its mod is loaded, the window was made and not removed, and it accepts docks.</summary>
    [AnyThread]
    public bool Available => Resolve() != null;

    /// <summary>That window is open now (false when it is not <see cref="Available"/>).</summary>
    [AnyThread]
    public bool Open => Resolve() is { Open: true };

    /// <summary>Where that window was drawn last (<see cref="UiWindow.Placement"/>); null when it is not
    /// <see cref="Available"/> or before its first frame.</summary>
    [AnyThread]
    public UiPlacement? Placement => Resolve()?.Placement;

    internal UiWindow? Resolve() => UiEngine.FindDockable(ModId, PlacementId);

    /// <summary>The same mod and placement id.</summary>
    public bool Equals(UiWindowHandle? other) => other is not null && other.ModId == ModId && other.PlacementId == PlacementId;

    /// <summary>The same mod and placement id.</summary>
    public override bool Equals(object? obj) => Equals(obj as UiWindowHandle);

    /// <summary>The same mod and placement id (both null: true), like <see cref="Equals(UiWindowHandle?)"/>: each
    /// <see cref="Ui.FindWindow"/> makes a new handle, so <c>window.DockedTo == Ui.FindWindow(...)</c> compares the names.</summary>
    public static bool operator ==(UiWindowHandle? left, UiWindowHandle? right) => left is null ? right is null : left.Equals(right);

    /// <summary>Not the same mod and placement id.</summary>
    public static bool operator !=(UiWindowHandle? left, UiWindowHandle? right) => !(left == right);

    /// <summary>Of the mod and placement id.</summary>
    public override int GetHashCode() => HashCode.Combine(ModId, PlacementId);

    /// <summary>"modId/placementId".</summary>
    public override string ToString() => ModId + "/" + PlacementId;
}

/// <summary>A window's dock changed (<see cref="UiWindow.DockChanged"/>). Since 0.2.0-dev.31.16, mod API 0.2.23 (GitHub issue 149).</summary>
public readonly struct UiDockChange
{
    internal UiDockChange(UiWindowHandle? dockedTo, DockSide side, float offset, bool byPlayer)
    {
        DockedTo = dockedTo;
        Side = side;
        Offset = offset;
        ByPlayer = byPlayer;
    }

    /// <summary>The window it is docked to now; null: it is no longer docked.</summary>
    public UiWindowHandle? DockedTo { get; }

    /// <summary>The side of the dock (for an undock: the side it had).</summary>
    public DockSide Side { get; }

    /// <summary>The offset along the shared edge, pixels at UI scale 1 (for an undock: the one it had).</summary>
    public float Offset { get; }

    /// <summary>The player did it: dragged it away with Shift held (undocked), or dropped it beside its old target (docked
    /// again). False: your mod's own <see cref="UiWindow.DockTo"/> or <see cref="UiWindow.Undock"/>.</summary>
    public bool ByPlayer { get; }
}

public sealed partial class UiWindow
{
    /// <summary>The id other mods find it under (<see cref="UiWindowOptions.PlacementId"/>, else the title when made).</summary>
    internal string DockKey { get; }

    internal UiDockState Dock { get; } = new();

    /// <summary>Docks this window to <paramref name="target"/> (another window, also another mod's, that accepts docks:
    /// <see cref="Ui.FindWindow"/>), on <paramref name="side"/> of it, moved <paramref name="offset"/> pixels at UI scale 1
    /// along the shared edge (down for <see cref="DockSide.Left"/> / <see cref="DockSide.Right"/>, right for
    /// <see cref="DockSide.Top"/> / <see cref="DockSide.Bottom"/>). While both are open Kiln keeps them together in every
    /// frame: when the player drags or resizes either one, or a mod moves either one (<see cref="MoveTo"/>), the other moves
    /// with it in the same frame, and the pair is kept inside the viewport (a window that does not fit on its side when it
    /// docks goes on the other side). Kiln only moves windows for this, never resizes them. Holding Shift, the player drags
    /// one alone: it leaves the dock (<see cref="DockChanged"/>); dropped within 16 pixels of its old side, it docks again.
    /// When this window opens while the target is closed, it waits for it (up to a quarter of a second, not drawn), so one
    /// key press opening both shows it beside the target at once; the target closed for longer, or not there: this window
    /// is drawn where it is, and goes beside the target when that opens. A later call replaces the dock. Does nothing on
    /// a removed window. Any thread.</summary>
    /// <remarks>A remembering window (<see cref="UiWindowOptions.RememberPlacement"/>) does not remember places it only got
    /// from its dock: opened alone it goes back to where the player left it. Since 0.2.0-dev.31.16, mod API 0.2.23 (GitHub issue 149).</remarks>
    /// <exception cref="ArgumentNullException"><paramref name="target"/> is null.</exception>
    /// <exception cref="ArgumentException"><paramref name="target"/> is this window.</exception>
    /// <exception cref="ArgumentOutOfRangeException">A side that is none of the four, or an offset that is not a finite number.</exception>
    /// <exception cref="InvalidOperationException">The target is docked, directly or through others, to this window.</exception>
    [AnyThread]
    public void DockTo(UiWindowHandle target, DockSide side, float offset = 0)
    {
        ArgumentNullException.ThrowIfNull(target);
        if (side is < DockSide.Left or > DockSide.Bottom) throw new ArgumentOutOfRangeException(nameof(side), side, "Left, Right, Top or Bottom");
        if (!float.IsFinite(offset)) throw new ArgumentOutOfRangeException(nameof(offset), offset, "pixels at UI scale 1 (a finite number)");
        var to = target.Resolve();
        if (ReferenceEquals(to, this) || (Options.AcceptDock && target.ModId == (Owner?.Info.Id ?? "kiln") && target.PlacementId == DockKey))
            throw new ArgumentException("a window cannot dock to itself", nameof(target));
        var seen = new HashSet<UiWindow>(ReferenceEqualityComparer.Instance);
        for (var w = to; w != null && seen.Add(w); w = w.Dock.Current?.Target.Resolve())
            if (ReferenceEquals(w, this))
                throw new InvalidOperationException($"{target} is docked to this window ('{Title}'): docking to it would make a loop");
        if (Disposed) return;
        lock (Dock.Lock)
        {
            if (Dock.Link is { } c && c.Target.Equals(target) && c.Side == side && c.Offset == offset)
            {
                Dock.Snap = c;
                return;
            }
            Dock.Link = Dock.Snap = new UiDockLink(target, side, offset, UiDocking.NextSeq());
        }
        RaiseDockChanged(new UiDockChange(target, side, offset, byPlayer: false));
    }

    /// <summary>Takes the window out of its dock (it stays where it is). The player dropping it near its old side does not
    /// dock it again after this (it does after their own Shift drag). Does nothing when it is not docked. Any thread.</summary>
    /// <remarks>Since 0.2.0-dev.31.16, mod API 0.2.23 (GitHub issue 149).</remarks>
    [AnyThread]
    public void Undock()
    {
        UiDockLink old;
        lock (Dock.Lock)
        {
            Dock.Snap = null;
            if (Dock.Link is not { } l) return;
            old = l;
            Dock.Link = null;
        }
        RaiseDockChanged(new UiDockChange(null, old.Side, old.Offset, byPlayer: false));
    }

    /// <summary>The window this one is docked to (<see cref="DockTo"/>); null when it is not docked. It stays set while the
    /// target is closed, or gone with its mod (<see cref="UiWindowHandle.Available"/>): the dock comes back with it. Any
    /// thread.</summary>
    /// <remarks>Since 0.2.0-dev.31.16, mod API 0.2.23 (GitHub issue 149).</remarks>
    [AnyThread]
    public UiWindowHandle? DockedTo => Dock.Current?.Target;

    /// <summary>The window's dock changed: your <see cref="DockTo"/> / <see cref="Undock"/> (on the thread that called it), or
    /// the player's Shift drag or drop beside its old target (<see cref="UiDockChange.ByPlayer"/>, on the render thread,
    /// right after the frame's windows: keep it short, post game work to the game's threads). Runs as your mod.</summary>
    /// <remarks>Since 0.2.0-dev.31.16, mod API 0.2.23 (GitHub issue 149).</remarks>
    public event Action<UiDockChange>? DockChanged;

    internal void RaiseDockChanged(UiDockChange change)
    {
        if (DockChanged is { } h) UiModCalls.Invoke(Owner, h, change, $"window '{Title}' DockChanged");
    }
}

public static partial class Ui
{
    /// <summary>A window of <paramref name="modId"/> that accepts docks (<see cref="UiWindowOptions.AcceptDock"/>), by its
    /// <see cref="UiWindowOptions.PlacementId"/> (else the title it was made with): a handle to dock your windows to
    /// (<see cref="UiWindow.DockTo"/>) or read whether it is open and where it is; null when that mod is not loaded or has no
    /// such window now (or its window does not accept docks). A window that does not accept docks is never found. Your own
    /// mod's windows too. Any thread.</summary>
    /// <remarks>Since 0.2.0-dev.31.16, mod API 0.2.23 (GitHub issue 149).</remarks>
    /// <exception cref="ArgumentException">An empty or blank mod id or placement id.</exception>
    [AnyThread]
    public static UiWindowHandle? FindWindow(string modId, string placementId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(modId);
        ArgumentException.ThrowIfNullOrWhiteSpace(placementId);
        return UiEngine.FindDockable(modId, placementId) != null ? new UiWindowHandle(modId, placementId) : null;
    }
}

public sealed unsafe partial class UiDraw
{
    /// <summary>The player holds the current window to drag it this frame (also while the pointer stays still), as Dear
    /// ImGui has it: tells their drag from your own <see cref="UiWindow.MoveTo"/>. Inside a window's draw (also inside a
    /// child region: its window). False elsewhere, and on a kiln-ui.dll without window_state (an older Kiln).</summary>
    /// <remarks>Since 0.2.0-dev.31.16, mod API 0.2.23 (GitHub issue 149).</remarks>
    public bool WindowMovedByUser => UiNative.ReadWindowState("") is { } s && (s.Flags & UiNative.WindowMoving) != 0;

    /// <summary>The player holds one of the current window's resize grips or borders this frame. Inside a window's draw;
    /// false elsewhere, and on a kiln-ui.dll without window_state.</summary>
    /// <remarks>Since 0.2.0-dev.31.16, mod API 0.2.23 (GitHub issue 149).</remarks>
    public bool WindowResizedByUser => UiNative.ReadWindowState("") is { } s && (s.Flags & UiNative.WindowResizing) != 0;
}

/// <summary>One dock: the target, the side, the offset (pixels at UI scale 1); Seq orders docks (the older wins a loop).</summary>
internal sealed record UiDockLink(UiWindowHandle Target, DockSide Side, float Offset, long Seq);

/// <summary>A window's dock (any thread, under <see cref="Lock"/>) and its render-thread state.</summary>
internal sealed class UiDockState
{
    public readonly object Lock = new();
    /// <summary>The dock (DockTo); null: not docked.</summary>
    public UiDockLink? Link;
    /// <summary>The dock a drop near its side restores: the last DockTo, kept through the player's undock, gone with Undock.</summary>
    public UiDockLink? Snap;

    public UiDockLink? Current
    {
        get { lock (Lock) return Link; }
    }

    // render thread
    public long DrawnFrame = long.MinValue;
    /// <summary>Its placer's open count (<see cref="UiPlacer.Opens"/>) when it was drawn last: another one means it was
    /// closed and opened again since, also over frames where Kiln.Ui drew no window at all.</summary>
    public long DrawnOpens = -1;
    public bool WasMoving;
    public long WaitStart = -1, WaitFrame = long.MinValue, WaitOpens = -1;
    /// <summary>The last frame its dock held (an accepted edge).</summary>
    public long ActiveFrame = long.MinValue;
    /// <summary>Its place came from its dock: RememberPlacement keeps the remembered position (only the size is written).</summary>
    public bool Placed;
    /// <summary>The side the dock of <see cref="EffectiveSeq"/> uses while it stays active (the other one when it did not fit).</summary>
    public DockSide Effective;
    public long EffectiveSeq = -1;

    internal void SetForTests(UiWindowHandle target, DockSide side, float offset)
    {
        lock (Lock) Link = Snap = new UiDockLink(target, side, offset, UiDocking.NextSeq());
    }

    /// <summary>The player's change (render thread): only when the dock and the snap are still the ones the frame read
    /// (<paramref name="expectedLink"/>, <paramref name="expectedSnap"/>): a DockTo or Undock from another thread since wins.</summary>
    public bool PlayerSet(UiDockLink? expectedLink, UiDockLink? expectedSnap, UiDockLink? link, UiDockLink? snap)
    {
        lock (Lock)
        {
            if (!ReferenceEquals(Link, expectedLink) || !ReferenceEquals(Snap, expectedSnap)) return false;
            Link = link;
            Snap = snap;
            return true;
        }
    }
}

/// <summary>A window as ImGui has it before its Begin this frame (kiln.ui's window_state).</summary>
internal readonly record struct UiWindowLive(bool Exists, bool Moving, bool Resizing, float X, float Y, float W, float H);

internal static class UiDocking
{
    /// <summary>How near (pixels at UI scale 1) a drop must be to its old side to dock again (Fuzen's WindowPair: 16).</summary>
    public const float SnapDistance = 16;

    /// <summary>How long a docked window that opens waits for its closed target, in milliseconds.</summary>
    public const long WaitMilliseconds = 250;

    private static long _seq;
    internal static long FrameNo;

    /// <summary>Milliseconds (Environment.TickCount64; tests and the kit set their own).</summary>
    internal static Func<long> Clock = static () => Environment.TickCount64;

    public static long NextSeq() => Interlocked.Increment(ref _seq);

    /// <summary>kiln.ui's window_state of the window (unknown on a kiln-ui.dll without it).</summary>
    public static UiWindowLive Live(UiWindow w) =>
        UiNative.ReadWindowState(w.Key) is { } s
            ? new UiWindowLive(true, (s.Flags & UiNative.WindowMoving) != 0, (s.Flags & UiNative.WindowResizing) != 0, s.X, s.Y, s.Width, s.Height)
            : default;

    internal static void ResetForTests()
    {
        Clock = static () => Environment.TickCount64;
        FrameNo = 0;
    }
}

/// <summary>One overlay frame's docking (render thread): <see cref="Plan"/> before the windows, then for each window of
/// <see cref="Order"/> <see cref="Place"/> before its Begin and <see cref="Drawn"/> after it, then <see cref="Finish"/>.</summary>
internal sealed class UiDockFrame
{
    private sealed class Node(UiWindow w, UiWindowLive live, bool appearing, long opens, UiDockLink? link)
    {
        public readonly UiWindow W = w;
        public readonly UiWindowLive Live = live;
        public readonly bool Appearing = appearing;
        public readonly long Opens = opens;
        public UiDockLink? Link = link;
        public bool Wait;
        // the accepted dock this frame: the window it docks to, on which side (the effective one), the offset in pixels
        public Node? Target;
        public DockSide Side;
        public float Offset;
        // its dock just became active: its side is chosen this frame (the other one when it does not fit), once known
        public bool Fresh;
        public readonly List<Node> Docked = [];
        // its group: the members from the leader, and what it is placed from (null: the leader)
        public List<Node>? Group;
        public Node? Parent;
        public bool ParentIsTarget;
        public (float X, float Y, float W, float H)? Rect;

        public bool InGroup => Target != null || Docked.Count > 0;
    }

    private readonly UiViewport _vp;
    private readonly long _frame;
    private readonly Dictionary<UiWindow, Node> _nodes = new(ReferenceEqualityComparer.Instance);
    private readonly List<UiWindow> _order = [];
    private readonly List<(UiWindow W, UiDockChange Change)> _changes = [];

    private UiDockFrame(UiViewport vp, long frame)
    {
        _vp = vp;
        _frame = frame;
    }

    /// <summary>The windows to draw this frame, in order (a waiting one is left out).</summary>
    public IReadOnlyList<UiWindow> Order => _order;

    public static UiDockFrame Plan(UiWindow[] open, UiViewport vp, KeyMods mods, Func<UiWindow, UiWindowLive> live, long now)
    {
        var f = new UiDockFrame(vp, ++UiDocking.FrameNo);
        // no open window has or had a dock: drawn as before, in their order (kiln.ui is asked only about a window whose
        // place came from a dock, to know when the player or the mod gives it its own again)
        var fast = open.All(w => w.Dock.Current == null && SnapOf(w) == null);
        foreach (var w in open)
        {
            var st = w.Dock;
            var opens = w.Placer.Opens;
            // its first frame of this open: not drawn in the frame before, or closed and opened again since it was drawn
            // (the frame count alone misses that: no frame runs Kiln.Ui's windows while none is open)
            var appearing = st.DrawnFrame != f._frame - 1 || st.DrawnOpens != opens;
            if (appearing) st.WasMoving = false;
            f._nodes[w] = new Node(w, fast && !st.Placed ? default : live(w), appearing, opens, fast ? null : st.Current);
        }
        if (fast)
        {
            foreach (var w in open)
                if (w.Dock.Placed) Track(f._nodes[w]);
            f._order.AddRange(open);
            return f;
        }
        var nodes = open.Select(w => f._nodes[w]).ToArray();
        var scale = vp.SafeScale;

        // the player's Shift drag: the dragged window leaves its dock, and the windows docked to it leave it
        if ((mods & KeyMods.Shift) != 0)
            foreach (var n in nodes.Where(n => n.Live.Moving))
            {
                if (f.TargetOf(n) != null) f.PlayerUndock(n);
                foreach (var m in nodes)
                    if (m != n && ReferenceEquals(f.TargetOf(m), n)) f.PlayerUndock(m);
            }

        // a drop (dragged last frame, not now): dropped near its old side, it docks again; also the other way round
        foreach (var n in nodes.Where(n => n.W.Dock.WasMoving && !n.Live.Moving))
        {
            f.TrySnap(n, scale);
            foreach (var m in nodes)
                if (m != n && m.Link == null && Snap(m) is { } s && ReferenceEquals(f.Open(s, m), n)) f.TrySnap(m, scale);
        }
        foreach (var n in nodes) n.W.Dock.WasMoving = n.Live.Moving;

        // a docked window opening while its target exists but is closed (or waits itself) waits for it, a moment at most
        for (var changed = true; changed;)
        {
            changed = false;
            foreach (var n in nodes)
            {
                if (n.Wait || !n.Appearing || n.Link is not { } l || l.Target.Resolve() is not { } t || ReferenceEquals(t, n.W)) continue;
                if (f._nodes.TryGetValue(t, out var tn) && !tn.Wait) continue;  // open and drawn this frame
                var st = n.W.Dock;
                if (st.WaitStart < 0 || st.WaitFrame < f._frame - 1 || st.WaitOpens != n.Opens)
                {
                    st.WaitStart = now;  // a new open: a new wait
                    st.WaitOpens = n.Opens;
                }
                if (now - st.WaitStart >= UiDocking.WaitMilliseconds) continue;
                st.WaitFrame = f._frame;
                n.Wait = changed = true;
            }
        }

        // the docks that hold this frame, oldest first; one that would close a loop is left out
        var root = new Dictionary<Node, Node>(ReferenceEqualityComparer.Instance);
        Node Find(Node x)
        {
            while (root.TryGetValue(x, out var p) && p != x) x = p;
            return x;
        }
        foreach (var n in nodes.Where(n => !n.Wait && n.Link != null).OrderBy(n => n.Link!.Seq))
        {
            var link = n.Link!;
            if (f.Open(link, n) is not { Wait: false } t) continue;
            Node a = Find(n), b = Find(t);
            if (a == b) continue;
            root[a] = b;
            n.Target = t;
            t.Docked.Add(n);
            var st = n.W.Dock;
            // the side it took when it became active holds while it stays active (both drawn in the frame before)
            var held = !n.Appearing && !t.Appearing && st.ActiveFrame == f._frame - 1 && st.EffectiveSeq == link.Seq;
            st.ActiveFrame = f._frame;
            n.Side = held ? st.Effective : link.Side;
            n.Fresh = !held;
            n.Offset = link.Offset * scale;
        }

        // the groups: the leader, then breadth first; who is placed from whom
        var grouped = new HashSet<Node>(ReferenceEqualityComparer.Instance);
        foreach (var n in nodes)
        {
            if (n.Wait) continue;
            if (!n.InGroup)
            {
                f._order.Add(n.W);
                continue;
            }
            if (!grouped.Add(n)) continue;
            var members = new List<Node> { n };
            for (var i = 0; i < members.Count; i++)
                foreach (var m in Neighbours(members[i]))
                    if (grouped.Add(m)) members.Add(m);
            var top = members.First(m => m.Target == null);
            // the one the player drags or resizes, else one a mod moves, else (the window the others dock to opening beside
            // windows already on the screen) one already shown, so nothing on the screen jumps, else that window
            var leader = members.FirstOrDefault(m => m.Live.Moving) ?? members.FirstOrDefault(m => m.Live.Resizing) ??
                         (top.W.Placer.MovePending ? top : members.FirstOrDefault(m => m.W.Placer.MovePending)) ??
                         (top.Appearing ? members.FirstOrDefault(m => !m.Appearing) : null) ?? top;
            var group = new List<Node> { leader };
            var seen = new HashSet<Node>(ReferenceEqualityComparer.Instance) { leader };
            for (var i = 0; i < group.Count; i++)
            {
                var x = group[i];
                x.Group = group;
                foreach (var m in Neighbours(x))
                {
                    if (!seen.Add(m)) continue;
                    m.Parent = x;
                    m.ParentIsTarget = ReferenceEquals(m.Target, x);
                    group.Add(m);
                }
            }
            f._order.AddRange(group.Select(m => m.W));
        }

        foreach (var n in nodes) Track(n);
        return f;
    }

    // A window's own place again once the player or the mod moves it with no dock holding; the window that docks is placed by it.
    private static void Track(Node n)
    {
        var st = n.W.Dock;
        if (n.Target != null) st.Placed = true;
        else if (n.Live.Moving || n.W.Placer.MovePending) st.Placed = false;
    }

    private static IEnumerable<Node> Neighbours(Node n) => n.Target is { } t ? n.Docked.Prepend(t) : n.Docked;

    private static UiDockLink? Snap(Node n) => SnapOf(n.W);

    private static UiDockLink? SnapOf(UiWindow w)
    {
        lock (w.Dock.Lock) return w.Dock.Snap;
    }

    /// <summary>The open window <paramref name="l"/> names, other than <paramref name="self"/>; null when it is closed or gone.</summary>
    private Node? Open(UiDockLink l, Node self) =>
        l.Target.Resolve() is { } t && !ReferenceEquals(t, self.W) && _nodes.TryGetValue(t, out var n) ? n : null;

    private Node? TargetOf(Node n) => n.Link is { } l ? Open(l, n) : null;

    private void PlayerUndock(Node n)
    {
        if (n.Link is not { } l || !n.W.Dock.PlayerSet(l, Snap(n), null, l)) return;
        n.Link = null;
        _changes.Add((n.W, new UiDockChange(null, l.Side, l.Offset, byPlayer: true)));
    }

    private (float X, float Y, float W, float H)? RectOf(Node n)
    {
        if (n.Live.Exists && n.Live.W > 0 && n.Live.H > 0) return (n.Live.X, n.Live.Y, n.Live.W, n.Live.H);
        return n.W.Placer.Last;
    }

    // dropped near the side of the window its last dock named (the side it asked for, or the other one it was shown on
    // when that did not fit): docked there again, at the offset it was dropped at
    private void TrySnap(Node d, float scale)
    {
        if (d.Link != null || Snap(d) is not { } s || Open(s, d) is not { } t || RectOf(d) is not { } a || RectOf(t) is not { } b) return;
        var st = d.W.Dock;
        var shown = st.EffectiveSeq == s.Seq ? st.Effective : s.Side;
        DockSide side;
        if (Near(a, b, s.Side, scale, out var offset)) side = s.Side;
        else if (shown != s.Side && Near(a, b, shown, scale, out offset)) side = shown;
        else return;
        var link = new UiDockLink(s.Target, side, offset / scale, UiDocking.NextSeq());
        if (!st.PlayerSet(null, s, link, link)) return;
        d.Link = link;
        _changes.Add((d.W, new UiDockChange(s.Target, side, link.Offset, byPlayer: true)));
    }

    // the rect a within the snap distance of side `side` of the rect b, overlapping along it; the offset along the edge
    private static bool Near((float X, float Y, float W, float H) a, (float X, float Y, float W, float H) b, DockSide side, float scale, out float offset)
    {
        float gap;
        bool overlap;
        switch (side)
        {
            case DockSide.Left:
                (gap, offset, overlap) = (a.X + a.W - b.X, a.Y - b.Y, a.Y < b.Y + b.H && a.Y + a.H > b.Y);
                break;
            case DockSide.Right:
                (gap, offset, overlap) = (a.X - (b.X + b.W), a.Y - b.Y, a.Y < b.Y + b.H && a.Y + a.H > b.Y);
                break;
            case DockSide.Top:
                (gap, offset, overlap) = (a.Y + a.H - b.Y, a.X - b.X, a.X < b.X + b.W && a.X + a.W > b.X);
                break;
            default:
                (gap, offset, overlap) = (a.Y - (b.Y + b.H), a.X - b.X, a.X < b.X + b.W && a.X + a.W > b.X);
                break;
        }
        return overlap && MathF.Abs(gap) <= UiDocking.SnapDistance * scale;
    }

    private bool Fits((float X, float Y, float Px, float Py) at, (float W, float H) size, DockSide side)
    {
        var (x, y, _, _) = TopLeft(at, size.W, size.H);
        return side is DockSide.Left or DockSide.Right ? x >= -0.5f && x + size.W <= _vp.Width + 0.5f : y >= -0.5f && y + size.H <= _vp.Height + 0.5f;
    }

    private static DockSide Opposite(DockSide s) => s switch
    {
        DockSide.Left => DockSide.Right,
        DockSide.Right => DockSide.Left,
        DockSide.Top => DockSide.Bottom,
        _ => DockSide.Top,
    };

    // where a window docked on side s of the rect t goes: a point and the pivot of the docked window put there
    private static (float X, float Y, float Px, float Py) Beside((float X, float Y, float W, float H) t, DockSide s, float off) => s switch
    {
        DockSide.Left => (t.X, t.Y + off, 1, 0),
        DockSide.Right => (t.X + t.W, t.Y + off, 0, 0),
        DockSide.Top => (t.X + off, t.Y, 0, 1),
        _ => (t.X + off, t.Y + t.H, 0, 0),
    };

    // where the target goes from the rect d of a window docked on side s of it
    private static (float X, float Y, float Px, float Py) Under((float X, float Y, float W, float H) d, DockSide s, float off) => s switch
    {
        DockSide.Left => (d.X + d.W, d.Y - off, 0, 0),
        DockSide.Right => (d.X, d.Y - off, 1, 0),
        DockSide.Top => (d.X - off, d.Y + d.H, 0, 0),
        _ => (d.X - off, d.Y, 0, 1),
    };

    private static (float X, float Y, float W, float H) TopLeft((float X, float Y, float Px, float Py) at, float w, float h) =>
        (at.X - at.Px * w, at.Y - at.Py * h, w, h);

    private static (float X, float Y, float Px, float Py) From(Node n, (float X, float Y, float W, float H) parent) =>
        n.ParentIsTarget ? Beside(parent, n.Side, n.Offset) : Under(parent, n.Parent!.Side, n.Parent.Offset);

    /// <summary>Before <paramref name="w"/>'s Begin: its own <paramref name="plan"/>, with its dock's place (a window placed
    /// from its neighbour), or moved back into the viewport with its group (the group's leader).</summary>
    public UiPlan Place(UiWindow w, UiPlan plan)
    {
        if (!_nodes.TryGetValue(w, out var n) || n.Group == null) return plan;
        if (n.Parent is not { } p) return Clamp(n, plan);
        if (p.Rect is not { } r) return plan;
        var edge = n.ParentIsTarget ? n : p;
        if (edge.Fresh && _vp.Valid && SizeOf(n) is { } z) Choose(edge, r, z, !n.ParentIsTarget);  // the leader's rect was a guess
        var (x, y, px, py) = From(n, r);
        return plan.WithPos(x, y, UiPlan.Always, px, py);
    }

    // The leader's rect this frame as ImGui will have it after Begin (its plan where it applies, else where it is now: a
    // drag is already in), the others' from it at their last sizes (or their options' before their first frame). A dock
    // that just became active takes its side here: the other one when the window placed from the other does not fit on
    // its own, decided once the leader's rect is known (until then it uses its own side). The group outside the viewport:
    // the leader moved in, the window the others dock to first (see Fit).
    private UiPlan Clamp(Node n, UiPlan plan)
    {
        // a guess moves nothing: the next frame knows
        if (!_vp.Valid || Estimate(n, plan) is not { Known: true } lead) return plan;
        var rects = new Dictionary<Node, (float X, float Y, float W, float H)>(ReferenceEqualityComparer.Instance) { [n] = lead.Rect };
        foreach (var m in n.Group!.Skip(1))
        {
            if (SizeOf(m) is not { } z) return plan;
            var parent = rects[m.Parent!];
            var edge = m.ParentIsTarget ? m : m.Parent!;
            if (edge.Fresh) Choose(edge, parent, z, !m.ParentIsTarget);
            rects[m] = TopLeft(From(m, parent), z.W, z.H);
        }
        // the player holds a resize grip or border of the leader: ImGui keeps that edge at the pointer and the opposite one
        // where the window is put, so moving it in would grow it by the move every frame (native window_state_test). It
        // stays; the group goes back inside when they let go.
        if (n.Live.Resizing) return plan;
        var order = n.Group!.OrderBy(Depth).ToArray();
        float dx = Fit(order.Select(m => (rects[m].X, rects[m].W)), _vp.Width), dy = Fit(order.Select(m => (rects[m].Y, rects[m].H)), _vp.Height);
        if (MathF.Abs(dx) < 0.01f && MathF.Abs(dy) < 0.01f) return plan;
        return plan.WithPos(lead.Rect.X + dx, lead.Rect.Y + dy, UiPlan.Always, 0, 0);
    }

    // how many docks away from the window the others dock to (the group's root: 0)
    private static int Depth(Node m)
    {
        var d = 0;
        for (var x = m.Target; x != null; x = x.Target) d++;
        return d;
    }

    // The smallest move along one axis that keeps the windows inside the viewport (0..max), taken nearest the group's
    // root first: a window that cannot be inside with the ones before it is left out, so a group too big for the viewport
    // keeps the window the others dock to inside and a docked window (a wide one, a far offset) never pushes it out. A
    // window bigger than the viewport: its start at 0.
    private static float Fit(IEnumerable<(float At, float Length)> spans, float max)
    {
        float lo = float.NegativeInfinity, hi = float.PositiveInfinity;
        foreach (var (at, length) in spans)
        {
            float a = -at, b = MathF.Max(a, max - at - length);
            if (a <= hi && b >= lo) (lo, hi) = (MathF.Max(lo, a), MathF.Min(hi, b));
        }
        return Math.Clamp(0f, lo, hi);
    }

    // A dock that just became active (edge: the window that docks): its own side, or the other one when the window placed
    // from the other (the docked one, or with targetMoves its target) does not fit on its own side but fits on the other.
    private void Choose(Node edge, (float X, float Y, float W, float H) from, (float W, float H) size, bool targetMoves)
    {
        bool FitsOn(DockSide s) => Fits(targetMoves ? Under(from, s, edge.Offset) : Beside(from, s, edge.Offset), size, s);
        var side = edge.Link!.Side;
        if (!FitsOn(side) && FitsOn(Opposite(side))) side = Opposite(side);
        edge.Side = side;
        edge.Fresh = false;
        edge.W.Dock.Effective = side;
        edge.W.Dock.EffectiveSeq = edge.Link.Seq;
    }

    private (float W, float H)? SizeOf(Node m)
    {
        if (m.Live.Exists && m.Live.W > 0 && m.Live.H > 0) return (m.Live.W, m.Live.H);
        if (m.W.Placer.Last is { } l) return (l.W, l.H);
        var o = m.W.Options;
        return o.Width > 0 && o.Height > 0 && !o.AutoResize ? (o.Width * _vp.SafeScale, o.Height * _vp.SafeScale) : null;
    }

    // Known: from what Kiln asks this frame or where ImGui has it; not known: a guess (the options' first place, which
    // ImGui skips when imgui.ini has the window).
    private (bool Known, (float X, float Y, float W, float H) Rect)? Estimate(Node n, UiPlan plan)
    {
        var last = n.W.Placer.Last;
        var never = !n.Live.Exists && last == null;
        bool Applies(uint cond) => cond == UiPlan.Always || (cond == UiPlan.Appearing && n.Appearing) || (cond == UiPlan.FirstUseEver && never);
        (float W, float H)? size = plan.HasSize && Applies(plan.SizeCond) && plan.W > 0 && plan.H > 0 ? (plan.W, plan.H) : SizeOf(n);
        if (size is not { } z) return null;
        if (plan.HasPos && Applies(plan.PosCond))
            return (plan.PosCond != UiPlan.FirstUseEver, (plan.X - plan.PivotX * z.W, plan.Y - plan.PivotY * z.H, z.W, z.H));
        if (n.Live.Exists) return (true, (n.Live.X, n.Live.Y, z.W, z.H));
        return last is { } r ? (true, (r.X, r.Y, z.W, z.H)) : null;
    }

    /// <summary>The window's place came from its dock: RememberPlacement keeps its remembered position.</summary>
    public bool KeepsPosition(UiWindow w) => _nodes.ContainsKey(w) && w.Dock.Placed;

    /// <summary>After <paramref name="w"/>'s Begin: where ImGui drew it (pixels).</summary>
    public void Drawn(UiWindow w, float x, float y, float width, float height)
    {
        var st = w.Dock;
        st.DrawnFrame = _frame;
        st.WaitStart = -1;
        var found = _nodes.TryGetValue(w, out var n);
        st.DrawnOpens = found ? n!.Opens : w.Placer.Opens;
        if (found && width > 0 && height > 0 && float.IsFinite(x + y + width + height))
            n!.Rect = (x, y, width, height);
    }

    /// <summary>After the windows: the player's dock changes go to their mods (DockChanged).</summary>
    public void Finish()
    {
        foreach (var (w, c) in _changes) w.RaiseDockChanged(c);
        _changes.Clear();
    }
}
