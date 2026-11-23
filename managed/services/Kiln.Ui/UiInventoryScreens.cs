// SPDX-License-Identifier: MIT
// Ui.Inventory's screens (0.2.0-dev.30, slice 2): which of the game's backpack and chest screens is open, with the chest's
// entity (Forge: a container screen and its block entity), events when one opens, closes or changes, and Kiln panels
// docked beside them (Forge: ScreenEvent.InitGuiEvent buttons next to a container screen). Native: kiln.ui.inventory v2's
// screen_view (native/core/abi/services/kiln_ui_inventory.h): where the game drew the slots, in the overlay's pixels.
// Picture and wish only: a mod's server checks the chest (the player may use it, is near it) before it acts.
using System.Runtime.CompilerServices;

namespace Kiln;

/// <summary>Which of the game's inventory screens is open.</summary>
public enum InventoryScreenKind
{
    /// <summary>The backpack (inventory) screen, or the in-game menu's inventory tab.</summary>
    Backpack = 1,
    /// <summary>A storage chest's transfer screen (the chest beside the backpack).</summary>
    Chest = 2,
}

/// <summary>The inventory screens a panel shows on.</summary>
[Flags]
public enum InventoryScreens
{
    /// <summary>The backpack screen.</summary>
    Backpack = 1,
    /// <summary>A chest's transfer screen.</summary>
    Chest = 2,
    /// <summary>Both.</summary>
    All = Backpack | Chest,
}

/// <summary>Where a panel sits beside the game's slots.</summary>
public enum InventoryPanelSide
{
    /// <summary>To the right of the slots the game drew (to their left when the screen has no room there).</summary>
    Right,
    /// <summary>To the left of the slots (to their right when there is no room).</summary>
    Left,
    /// <summary>Under the slots, aligned with the chest's (or the backpack's) left edge.</summary>
    Below,
}

/// <summary>One of the game's inventory screens, open now (<see cref="Ui.Inventory.Screen"/>).</summary>
/// <param name="Kind">Backpack or chest.</param>
/// <param name="Hud">The game's screen (Backpack, InGameMenu, StorageChest, CommerceProductChest).</param>
/// <param name="Container">On a chest screen: the chest's entity (the game's play.target; for a chest the server sent, the
/// server's id of it). Send it to your server half, which checks that the player may use that chest and is near it before
/// it acts. <see cref="EntityId.None"/> on the backpack, on a build where Kiln does not read it, and for the frame or two
/// before the game names it (then the screen changes again, with it).</param>
public readonly record struct InventoryScreen(InventoryScreenKind Kind, HudScreen Hud, EntityId Container)
{
    /// <summary>The inventory screen of a game screen, or null (any other screen: the play view, a station, the map...).
    /// The in-game menu (<see cref="HudScreen.InGameMenu"/>) counts as the backpack unless a crafting menu is built in it;
    /// its other tabs (map, journal) draw no slots, and panels do not show there.</summary>
    public static InventoryScreen? Of(GameScreen screen)
    {
        if (screen.State != GameUiState.Play) return null;
        return screen.Hud switch
        {
            HudScreen.Backpack => new InventoryScreen(InventoryScreenKind.Backpack, screen.Hud, EntityId.None),
            HudScreen.InGameMenu when screen.Crafting is null => new InventoryScreen(InventoryScreenKind.Backpack, screen.Hud, EntityId.None),
            HudScreen.StorageChest or HudScreen.CommerceProductChest => new InventoryScreen(InventoryScreenKind.Chest, screen.Hud, screen.Target),
            _ => null,
        };
    }

    internal InventoryScreens Flag => Kind == InventoryScreenKind.Chest ? InventoryScreens.Chest : InventoryScreens.Backpack;
}

/// <summary>Options of a panel beside the game's inventory screens (<see cref="Ui.Inventory.AddPanel"/>).</summary>
public sealed record InventoryPanelOptions
{
    /// <summary>The screens it shows on (both by default).</summary>
    public InventoryScreens Screens { get; init; } = InventoryScreens.All;
    /// <summary>Where the panel sits beside the slots. Default right.</summary>
    public InventoryPanelSide Side { get; init; } = InventoryPanelSide.Right;
    /// <summary>Width in pixels at scale 1 (height fits the contents).</summary>
    public float Width { get; init; } = 300;
    /// <summary>The panel's look; <see cref="UiStyle.Player"/> by default (the game's look unless the player chose another).</summary>
    public UiStyle Style { get; init; } = UiStyle.Player;
    /// <summary>Your own theme for the panel (<see cref="Ui.Themes.Define"/>), over <see cref="Style"/>.</summary>
    public UiTheme? Theme { get; init; }
    /// <summary>Show only when this returns true (e.g. chests only once their entity is known); null = always.</summary>
    public Func<InventoryScreen, bool>? When { get; init; }
    /// <summary>Buttons in the panel take the mouse while it is over the panel (the game does not see those clicks).</summary>
    public bool Interactive { get; init; } = true;
}

/// <summary>Posted on Kiln's event bus when one of the game's inventory screens opens, closes or changes (another chest,
/// the chest's entity named a frame late). Tick thread, at the next step. For the same change on the UI thread without
/// delay: <see cref="Ui.Inventory.ScreenChanged"/>.</summary>
public sealed class InventoryScreenChangedEvent(InventoryScreen? previous, InventoryScreen? current) : KilnEvent
{
    /// <summary>The screen before the change; null when none was open.</summary>
    public InventoryScreen? Previous { get; } = previous;
    /// <summary>The screen after the change; null when none is open.</summary>
    public InventoryScreen? Current { get; } = current;
}

public static partial class Ui
{
    public static partial class Inventory
    {
        /// <summary>The capability row of <see cref="AddPanel"/> (where the slots are drawn, the chest's entity).</summary>
        public const string PanelCapability = "ui.inventory.panels";

        /// <summary>The game's backpack or chest screen open now, or null (any thread).</summary>
        [ClientOnly, AnyThread]
        public static InventoryScreen? Screen => InventoryScreen.Of(Screens.Current);

        /// <summary>(previous, current) on the game's UI thread right after the UI frame in which a backpack or chest screen
        /// opened, closed or changed (null: none open). Owned by the calling mod.</summary>
        [ClientOnly]
        public static event Action<InventoryScreen?, InventoryScreen?>? ScreenChanged
        {
            add
            {
                UiEngine.EnsureStarted();
                UiInventoryScreens.Handlers.Add(value);
            }
            remove => UiInventoryScreens.Handlers.Remove(value);
        }

        /// <summary>A Kiln panel docked beside the game's backpack and chest screens while they are open (Kiln.Ui's widgets,
        /// the player's look), drawn every overlay frame on the render thread with the open <see cref="InventoryScreen"/>.
        /// It sits next to the slots the game drew (<see cref="InventoryPanelOptions.Side"/>) and moves with them; it is
        /// not drawn when Kiln cannot tell where they are (fail closed). Owned by the calling mod; Dispose removes it.
        /// Throws KilnUnavailableException when <see cref="PanelCapability"/> is off.</summary>
        [ClientOnly, AnyThread, RequiresCapability(PanelCapability)]
        public static IDisposable AddPanel(UiText title, Action<UiDraw, InventoryScreen> draw, InventoryPanelOptions? options = null) =>
            UiInventoryScreens.AddPanel(title, draw, options ?? new InventoryPanelOptions());
    }
}

internal static unsafe class UiInventoryScreens
{
    internal sealed class Panel(UiText title, Action<UiDraw, InventoryScreen> draw, InventoryPanelOptions options, ModContext? owner)
    {
        public readonly UiText Title = title;
        public readonly Action<UiDraw, InventoryScreen> Draw = draw;
        public readonly InventoryPanelOptions Options = options;
        public readonly ModContext? Owner = owner;
        public readonly string Id = $"###kiln.inv{Interlocked.Increment(ref _ids)}";
        public bool Disposed;
    }

    private static int _ids;
    private static readonly object Lock = new();
    private static Panel[] _panels = [];
    internal static readonly OwnedList<Action<InventoryScreen?, InventoryScreen?>> Handlers = new();
    private static StrongBox<InventoryScreen?> _current = new(null);
    private static readonly HashSet<ModContext> Watched = new(ReferenceEqualityComparer.Instance);

    /// <summary>The inventory screen open as of the last screen change (UI thread writes, any thread reads).</summary>
    internal static InventoryScreen? Current => Volatile.Read(ref _current).Value;

    internal static IDisposable AddPanel(UiText title, Action<UiDraw, InventoryScreen> draw, InventoryPanelOptions options)
    {
        ArgumentNullException.ThrowIfNull(title);
        ArgumentNullException.ThrowIfNull(draw);
        ArgumentNullException.ThrowIfNull(options);
        if (!float.IsFinite(options.Width) || options.Width <= 0) throw new ArgumentException("Width must be finite and > 0");
        if ((options.Screens & InventoryScreens.All) == 0) throw new ArgumentException("Screens names no screen");
        UiValueBridge.RequireCapability(Ui.Inventory.PanelCapability);
        if (UiInventoryNative.T2 == null)
            throw new KilnUnavailableException(Ui.Inventory.PanelCapability, "kiln.ui.inventory v2 is not there (an older kiln-core)");
        UiEngine.EnsureStarted();
        var owner = UiModCalls.Caller;
        UiModCalls.RequireAlive(owner);
        UiModCalls.Watch(owner);
        Watch(owner);
        var p = new Panel(title, draw, options, owner);
        lock (Lock) _panels = [.. _panels, p];
        UiEngine.Recount();
        return new UiEngine.Handle(() => Remove(p));
    }

    private static void Remove(Panel p)
    {
        p.Disposed = true;
        lock (Lock) _panels = _panels.Where(x => x != p).ToArray();
        UiEngine.Recount();
    }

    private static void Watch(ModContext? mod)
    {
        if (mod is null) return;
        lock (Watched)
            if (!Watched.Add(mod)) return;
        mod.Retiring += () =>
        {
            RemoveOwnedBy(mod);
            lock (Watched) Watched.Remove(mod);
        };
    }

    internal static void RemoveOwnedBy(ModContext mod)
    {
        Handlers.RemoveOwnedBy(mod);
        bool any;
        lock (Lock)
        {
            any = _panels.Any(p => ReferenceEquals(p.Owner, mod));
            foreach (var p in _panels.Where(p => ReferenceEquals(p.Owner, mod))) p.Disposed = true;
            _panels = _panels.Where(p => !ReferenceEquals(p.Owner, mod)).ToArray();
        }
        if (any) UiEngine.Recount();
    }

    // UI thread (Ui.Screens.OnScreen; TestKit's harness)
    internal static void ScreenChanged(GameScreen cur)
    {
        var next = InventoryScreen.Of(cur);
        var prev = Volatile.Read(ref _current).Value;
        if (prev == next) return;
        Volatile.Write(ref _current, new StrongBox<InventoryScreen?>(next));
        if ((prev is null) != (next is null) || prev?.Kind != next?.Kind) UiEngine.Recount();
        foreach (var (h, owner) in Handlers.Snapshot())
            UiModCalls.Invoke(owner, x => h(x.prev, x.next), (prev, next), "Ui.Inventory.ScreenChanged");
        if (Events.HasSubscribers<InventoryScreenChangedEvent>()) Events.Enqueue(new InventoryScreenChangedEvent(prev, next));
    }

    /// <summary>Whether a panel shows on a screen (its screens, then its When; a When that throws hides it).</summary>
    internal static bool Shows(Panel p, InventoryScreen s)
    {
        if (p.Disposed || (p.Options.Screens & s.Flag) == 0) return false;
        if (p.Options.When is not { } when) return true;
        try { return when(s); }
        catch (Exception) { return false; }
    }

    private static Panel[] Snapshot()
    {
        lock (Lock) return _panels;
    }

    /// <summary>The titles of the panels that show on the open screen now (TestKit; the overlay additionally needs the
    /// game's slots placed).</summary>
    internal static IReadOnlyList<string> Showing()
    {
        if (Current is not { } s) return [];
        return Snapshot().Where(p => p.Options.Screens.HasFlag(s.Flag) && Shows(p, s)).Select(p => p.Title.ToString()).ToArray();
    }

    internal static int VisiblePanels => Current is { } s ? Snapshot().Count(p => !p.Disposed && (p.Options.Screens & s.Flag) != 0) : 0;

    internal static int InteractivePanels =>
        Current is { } s ? Snapshot().Count(p => !p.Disposed && p.Options.Interactive && (p.Options.Screens & s.Flag) != 0) : 0;

    /// <summary>A rectangle in the overlay's pixels.</summary>
    internal readonly record struct Box(float X, float Y, float W, float H)
    {
        public float Right => X + W;
        public float Bottom => Y + H;
        public static Box Of(in UiValuesNative.Rect r) => new(r.X, r.Y, r.Width, r.Height);
    }

    /// <summary>Where a panel of width <paramref name="w"/> goes: beside <paramref name="all"/> (every slot the game
    /// drew), level with <paramref name="focus"/> (the chest's slots, or the backpack's), <paramref name="used"/> pixels
    /// below earlier panels on that side; kept on a screen of <paramref name="screenW"/> x <paramref name="screenH"/>.</summary>
    internal static (float X, float Y) Dock(InventoryPanelSide side, Box all, Box focus, float w, float used, float screenW,
        float screenH, float scale)
    {
        var gap = 16 * scale;
        var margin = 8 * scale;
        float RightOf() => all.Right + gap;
        float LeftOf() => all.X - gap - w;
        bool Fits(float x) => x >= margin && x + w <= screenW - margin;
        float x, y;
        switch (side)
        {
            case InventoryPanelSide.Below:
                x = focus.X;
                y = all.Bottom + gap + used;
                break;
            case InventoryPanelSide.Left:
                x = Fits(LeftOf()) ? LeftOf() : Fits(RightOf()) ? RightOf() : margin;
                y = focus.Y + used;
                break;
            default:
                x = Fits(RightOf()) ? RightOf() : Fits(LeftOf()) ? LeftOf() : screenW - margin - w;
                y = focus.Y + used;
                break;
        }
        x = Math.Clamp(x, margin, Math.Max(margin, screenW - margin - w));
        y = Math.Clamp(y, margin, Math.Max(margin, screenH - margin));
        return (x, y);
    }

    // render thread
    internal static void Draw(UiDraw ui)
    {
        if (Current is not { } s) return;
        var ps = Snapshot();
        if (ps.Length == 0) return;
        var t = UiInventoryNative.T2;
        if (t == null) return;
        UiInventoryNative.ScreenView v = default;
        v.Size = (uint)sizeof(UiInventoryNative.ScreenView);
        if (t->GetScreenView(UiValueBridge.OwnerOf(null), ui.Width, ui.Height, &v) != UiValuesNative.Ok || v.Ok == 0) return;
        if (v.Kind != (uint)s.Kind) return;  // the slot frame is of another screen than the one the panels are for
        if (s.Kind == InventoryScreenKind.Chest && v.Container != 0 && s.Container.Value == 0) s = s with { Container = new EntityId(v.Container) };
        var all = Box.Of(v.AllPx);
        var focus = s.Kind == InventoryScreenKind.Chest && v.ContainerSlots > 0 ? Box.Of(v.ContainerPx)
            : s.Kind == InventoryScreenKind.Backpack && v.PlayerSlots > 0 ? Box.Of(v.PlayerPx) : all;
        var scale = ui.Scale;
        var used = new Dictionary<InventoryPanelSide, float>();
        foreach (var p in ps)
        {
            if (!Shows(p, s)) continue;
            var o = p.Options;
            var w = o.Width * scale;
            used.TryGetValue(o.Side, out var u);
            var (x, y) = Dock(o.Side, all, focus, w, u, ui.Width, ui.Height, scale);
            ui.NextWindowPos(x, y, 1 /* always: docked to the game's screen */, 0, 0);
            ui.NextWindowSize(w, 0, 1);
            // no resize/move/collapse, auto-size, no focus on appearing
            uint flags = (1u << 1) | (1u << 2) | (1u << 5) | (1u << 6) | (1u << 12);
            if (!o.Interactive) flags |= (1u << 9) | (1u << 18) | (1u << 19);
            var looks = ui.LookDepth;
            ui.PushLookRaw(o.Theme, o.Style);
            if (ui.BeginWindowNoClose($"{p.Title}{p.Id}", flags))
            {
                var inner = ui.LookDepth;
                if (!UiModCalls.Invoke(p.Owner, x2 => p.Draw(x2, s), ui, $"inventory panel '{p.Title}'")) p.Disposed = true;
                ui.RestoreLooks(inner);
                var (_, _, _, hh) = ui.WindowRect;
                used[o.Side] = u + hh + 10 * scale;
            }
            ui.EndWindow();
            ui.RestoreLooks(looks);
        }
    }

    /// <summary>No inventory screen open, without events (TestKit: a new harness).</summary>
    internal static void ForgetScreen() => Volatile.Write(ref _current, new StrongBox<InventoryScreen?>(null));

    internal static void ResetForTests()
    {
        lock (Lock) _panels = [];
        foreach (var (h, _) in Handlers.Snapshot()) Handlers.Remove(h);
        Volatile.Write(ref _current, new StrongBox<InventoryScreen?>(null));
    }
}
