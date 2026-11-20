// SPDX-License-Identifier: MIT
// BlockUi: a window's state (dossier 09 §4.2, §5.2). Owning-mod only: a mod reads and writes the state of its own windows.
namespace Kiln;

public static partial class BlockUi
{
    /// <summary>The capability of window state and of these calls.</summary>
    public const string StateCapability = "ui.blocks.state";

    /// <summary>The state of one of this mod's windows: its declared entries and its tab bars' and trees' own state, at one
    /// version. Refused (not thrown) for an unknown window, a window of another mod, or a game where windows are not drawn.</summary>
    /// <param name="window">The window: what <see cref="Blocks.Define{T}"/> returned, its name, or <c>"this-mod:name"</c>.</param>
    /// <exception cref="InvalidOperationException">Called outside a mod's code (no mod to own the call).</exception>
    /// <exception cref="KilnUnavailableException"><c>ui.blocks.state</c> is off: no overlay here (a dedicated server).</exception>
    [ClientOnly, AnyThread, RequiresCapability(StateCapability)]
    public static UiStateReadResult State(WindowRef window)
    {
        var (w, refusal) = Find(window);
        return w == null ? new UiStateReadResult(false, refusal, null) : new UiStateReadResult(true, null, w.State.Snapshot);
    }

    /// <summary>
    /// Sets one state entry of one of this mod's windows. The write is queued and committed on the client's main thread; the
    /// task ends then with the snapshot after it (a write of the value the entry has is accepted and changes nothing). With
    /// <paramref name="expected"/> it is refused (StaleVersion) unless the window is still at that version; without it, it is
    /// still refused (StaleSession) when the world changes first. A selection entry (a table's, tree's or tabs' Selected)
    /// takes only a key the row shows now, or "".
    /// </summary>
    /// <exception cref="ArgumentException">The value is not of the entry's type, or outside its range or size.</exception>
    /// <exception cref="InvalidOperationException">Called outside a mod's code.</exception>
    /// <exception cref="KilnUnavailableException"><c>ui.blocks.state</c> is off.</exception>
    [ClientOnly, AnyThread, RequiresCapability(StateCapability)]
    public static Task<UiStateResult> SetState(WindowRef window, string key, UiStateValue value, UiStateVersion? expected = null)
    {
        ArgumentNullException.ThrowIfNull(key);
        ArgumentNullException.ThrowIfNull(value);
        var (w, refusal) = Find(window);
        if (w == null) return Task.FromResult(new UiStateResult(false, refusal, null));
        if (!w.State.Definitions.TryGetValue(key, out var def))
            return Task.FromResult(new UiStateResult(false, $"{UiStateReasons.UnknownKey}: window {w.Plan.Mod}:{w.Plan.Name} declares no state \"{key}\"", w.State.Snapshot));
        if (UiWindowState.Problem(def, value) is { } problem) throw new ArgumentException($"state.{key}: {problem}", nameof(value));
        if (value is UiStateValue.String s && w.Shows(key, s.Value) == false)
            return Task.FromResult(new UiStateResult(false, $"{UiStateReasons.UnknownSelection}: \"{s.Value}\" is not shown by the row that selects with state.{key}", w.State.Snapshot));
        Func<UiStateSnapshot, string?>? check = value is UiStateValue.String sel
            ? _ => w.Shows(key, sel.Value) == false ? $"{UiStateReasons.UnknownSelection}: \"{sel.Value}\" is not shown any more" : null
            : null;
        return w.State.Enqueue(UiWindowState.WriteKind.Value, key, value, null, expected, UiStateChangeCause.Api, check);
    }

    private static (WindowRuntime? Window, string? Refusal) Find(WindowRef window)
    {
        if (!UiRuntime.Active) throw new KilnUnavailableException(StateCapability, "no overlay here: windows are drawn only on a player's game");
        var caller = BlocksBridge.CallerModId ?? throw new InvalidOperationException("BlockUi is called from a mod's code: there is no mod here to own the call");
        var text = window.Value ?? "";
        var colon = text.IndexOf(':');
        var (mod, name) = colon < 0 ? (caller, text) : (text[..colon], text[(colon + 1)..]);
        if (mod != caller) return (null, $"{UiStateReasons.AccessDenied}: window {mod}:{name} belongs to mod {mod}");
        var w = UiRuntime.FindWindow(mod, name);
        return w == null ? (null, $"{UiStateReasons.UnknownWindow}: mod {mod} has no window \"{name}\" here") : (w, null);
    }
}
