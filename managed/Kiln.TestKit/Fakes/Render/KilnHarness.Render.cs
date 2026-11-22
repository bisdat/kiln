// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessOutlines? _outlines;

    /// <summary>The fake kiln.render's outlines of this harness (Ui.World.Outline): what mods outlined, and the game's side
    /// of it (its own highlight, models, the player's setting, despawns). Since 0.2.0-dev.30.3.</summary>
    public HarnessOutlines Outlines => _outlines ??= new HarnessOutlines(this);
}

/// <summary>The player's kiln.json <c>"ui": {"outlines": ...}</c> in the kit (<see cref="HarnessOutlines.Setting"/>).
/// Since 0.2.0-dev.30.3.</summary>
public enum FakeOutlineSetting
{
    /// <summary><c>"game-first"</c> (the default): the game's own highlight wins over mods' outlines.</summary>
    GameFirst = 0,
    /// <summary><c>"mods-first"</c>: mods' outlines win over the game's highlight.</summary>
    ModsFirst = 1,
    /// <summary><c>"off"</c>: mods' outlines are accepted and wait (<see cref="OutlineReason.SwitchedOff"/>).</summary>
    Off = 2,
}

/// <summary>An outline request as the kit's kiln.render holds it (<see cref="HarnessOutlines"/>). Since 0.2.0-dev.30.3.</summary>
/// <param name="Entity">The outlined entity.</param>
/// <param name="Color">The colour asked for.</param>
/// <param name="Pulse">The colour breathes.</param>
/// <param name="Priority">The request's priority.</param>
/// <param name="Duration">How long it lasts (null: until removed).</param>
/// <param name="State">Its state now.</param>
/// <param name="Reason">Why it waits or ended.</param>
/// <param name="Group">The game's highlight group its colour uses (4 or 5) while shown, 0 otherwise.</param>
/// <param name="ColorShared">A third colour: it shows with the nearest other colour's group.</param>
/// <param name="Owner">Who asked: the mod's id, or <c>kiln</c>.</param>
public sealed record FakeOutline(EntityId Entity, Color Color, bool Pulse, int Priority, TimeSpan? Duration, OutlineState State,
    OutlineReason Reason, int Group, bool ColorShared, string Owner)
{
    internal ulong Handle { get; init; }

    /// <summary>Asked with <c>OutlineOptions.ThroughWalls</c> (Kiln's silhouette through walls; its drawing is not modelled).
    /// Since 0.2.0-dev.31.</summary>
    public bool ThroughWalls { get; init; }
}

/// <summary>
/// The fake kiln.render's outlines of a harness (<see cref="KilnHarness.Outlines"/>): Ui.World.Outline's own code runs on it.
/// Since 0.2.0-dev.30.3.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> Each request with its entity, colour, pulse, priority and duration (1024 live per mod; the row
/// <c>ui.world_outline</c> a test can switch off). A new request waits (<see cref="OutlineReason.Pending"/>) until the next
/// step, where kiln.render's rules decide every request: on one entity the highest priority, then the newest, shows and the
/// others wait (<see cref="OutlineReason.Outshone"/>); the game's own highlight shows instead
/// (<see cref="OutlineState.GameHighlight"/>, <see cref="GameHighlight"/>) unless <see cref="Setting"/> is mods-first;
/// <see cref="FakeOutlineSetting.Off"/> keeps every request waiting; an entity without a model waits
/// (<see cref="HideModel"/>); two (colour, pulse) pairs show at once and a third shares the nearest one's group
/// (<see cref="FakeOutline.ColorShared"/>); a duration runs out on the harness's virtual time. Dispose and ClearOutlines
/// end requests (<see cref="OutlineReason.Removed"/>), as does the mod's unload; <see cref="Despawn"/> and
/// <see cref="EndWorld"/> end them as the game would.</para>
/// <para><b>Not modelled.</b> The game's highlight itself (nothing is drawn), RenderFrameEvent and its drawing, the camera.</para>
/// </remarks>
public sealed class HarnessOutlines
{
    private readonly KilnHarness _harness;

    internal HarnessOutlines(KilnHarness harness) => _harness = harness;

    private RenderFakeState State => KitFakes.State<RenderFakeState>(_harness);

    /// <summary>Every request this harness got, oldest first, ended ones too.</summary>
    public IReadOnlyList<FakeOutline> All
    {
        get { lock (State.Lock) return State.All.Select(View).ToArray(); }
    }

    /// <summary>The requests that have not ended, oldest first.</summary>
    public IReadOnlyList<FakeOutline> Live
    {
        get { lock (State.Lock) return State.All.Where(r => r.State != RenderFake.Ended).Select(View).ToArray(); }
    }

    /// <summary>The request whose colour <paramref name="entity"/>'s model shows now, or null.</summary>
    public FakeOutline? Showing(EntityId entity)
    {
        lock (State.Lock)
            return State.All.FirstOrDefault(r => r.Entity == entity.Value && r.State == RenderFake.Shown) is { } r ? View(r) : null;
    }

    /// <summary>The request behind <paramref name="outline"/> (null: not this harness's, or it ended before the kit saw it).</summary>
    public FakeOutline? Of(WorldOutline outline)
    {
        ArgumentNullException.ThrowIfNull(outline);
        lock (State.Lock)
        {
            var r = Find(outline);
            return r is null ? null : View(r);
        }
    }

    /// <summary>The player's kiln.json <c>"ui": {"outlines": ...}</c>; applies at the next step.</summary>
    public FakeOutlineSetting Setting
    {
        get { lock (State.Lock) return (FakeOutlineSetting)State.Setting; }
        set
        {
            if (value is not (FakeOutlineSetting.GameFirst or FakeOutlineSetting.ModsFirst or FakeOutlineSetting.Off))
                throw new ArgumentOutOfRangeException(nameof(value), value, "GameFirst, ModsFirst or Off");
            lock (State.Lock) State.Setting = (uint)value;
        }
    }

    /// <summary>The game highlights <paramref name="entity"/> itself (the building cursor rests on it), or stops; applies
    /// at the next step.</summary>
    public void GameHighlight(EntityId entity, bool on = true)
    {
        lock (State.Lock)
            if (on) State.GameHighlights.Add(entity.Value);
            else State.GameHighlights.Remove(entity.Value);
    }

    /// <summary><paramref name="entity"/> has no model instance (hidden, not streamed in, a voxel model), or has one again;
    /// applies at the next step.</summary>
    public void HideModel(EntityId entity, bool hidden = true)
    {
        lock (State.Lock)
            if (hidden) State.NoModel.Add(entity.Value);
            else State.NoModel.Remove(entity.Value);
    }

    /// <summary><paramref name="entity"/> leaves the client world: every live request on it ends now
    /// (<see cref="OutlineReason.Despawned"/>). Returns how many ended.</summary>
    public int Despawn(EntityId entity)
    {
        var n = 0;
        lock (State.Lock)
            foreach (var r in State.All)
                if (r.Entity == entity.Value && r.State != RenderFake.Ended)
                {
                    RenderFake.EndReq(r, RenderFake.Despawned);
                    n++;
                }
        return n;
    }

    /// <summary>The client world ends (the player leaves the game): every live request ends now
    /// (<see cref="OutlineReason.WorldEnded"/>).</summary>
    public void EndWorld()
    {
        lock (State.Lock)
            foreach (var r in State.All)
                if (r.State != RenderFake.Ended) RenderFake.EndReq(r, RenderFake.World);
    }

    /// <summary>Pins the state of <paramref name="outline"/>'s request (the steps no longer decide it): what
    /// <see cref="WorldOutline.State"/> and <see cref="WorldOutline.Reason"/> read. <see cref="OutlineState.Ended"/>
    /// ends it for good and needs an end reason (<see cref="OutlineReason.Removed"/> and later).</summary>
    /// <exception cref="ArgumentException">The outline is not this harness's, or it ended already, or an end without an
    /// end reason (or an end reason without the end).</exception>
    public void SetState(WorldOutline outline, OutlineState state, OutlineReason reason = OutlineReason.None)
    {
        ArgumentNullException.ThrowIfNull(outline);
        if (!Enum.IsDefined(state) || !Enum.IsDefined(reason)) throw new ArgumentOutOfRangeException(nameof(state));
        if ((state == OutlineState.Ended) != ((int)reason >= (int)OutlineReason.Removed))
            throw new ArgumentException("Ended goes with an end reason (Removed and later), and only Ended does", nameof(reason));
        lock (State.Lock)
        {
            var r = Find(outline) ?? throw new ArgumentException("not a request of this harness's kiln.render", nameof(outline));
            if (r.State == RenderFake.Ended) throw new ArgumentException("the outline ended already", nameof(outline));
            r.State = (uint)state;
            r.Reason = (uint)reason;
            r.Pinned = true;
            r.Group = state == OutlineState.Shown ? 4u : 0u;
            r.Merged = 0;
        }
    }

    private RenderFakeState.Req? Find(WorldOutline outline)
    {
        var h = outline.Handle;
        return h == 0 ? null : State.All.FirstOrDefault(r => r.Handle == h);
    }

    private static FakeOutline View(RenderFakeState.Req r) =>
        new(new EntityId(r.Entity), Color.FromRgba(r.Rgba), (r.Flags & RenderNative.OutlinePulse) != 0, r.Priority,
            r.DurationMs == 0 ? null : TimeSpan.FromMilliseconds(r.DurationMs), (OutlineState)r.State, (OutlineReason)r.Reason,
            (int)r.Group, r.Merged != 0, OwnerName(r.Owner))
        {
            Handle = r.Handle,
            ThroughWalls = (r.Flags & RenderNative.OutlineThroughWalls) != 0,
        };

    private static string OwnerName(ulong owner)
    {
        var name = KitBackend.Instance.OwnerName(owner);
        return name.StartsWith("mod:", StringComparison.Ordinal) ? name[4..] : name;
    }
}
