// SPDX-License-Identifier: MIT
// What a move's `animation` names, and the one seam to the player graph's states. A GUID or a `0x` id is parsed offline (an id becomes 16 bytes with that
// FNV-1a, BlockIds.WithFnv: the engine only ever hashes the reference). A name is a state of this mod or of the game's graph, which only the player-anim
// lookup knows, and it knows it when content is ready: until it is wired (PlayerStateSeam.Lookup), a name is reported then as not available in this build.
namespace Kiln;

/// <summary>How a sequence enters a state: <c>SetAnimationEvent.animationName</c> (a full-body state) or <c>SetAnimationOverlayEvent.overlayName</c> (an overlay).</summary>
internal enum PlayerStateKind { State = 0, Overlay = 1 }

internal enum PlayerStateStatus { Live = 0, Refused = 1, Pending = 2, Unavailable = 3 }

/// <summary>A state of the game's player graph: its machine, its kind, and every id that enters it (none for one no id enters, such as <c>Idle_Block</c>).</summary>
internal sealed record GameStateAnswer(string Machine, string Name, PlayerStateKind Kind, IReadOnlyList<uint> Ids);

/// <summary>A state a mod defines: its GUID (<c>BlockIds.Derive(mod, "state", name)</c>), kind and status, and why when it is not live.</summary>
internal sealed record ModStateAnswer(Guid Guid, PlayerStateKind Kind, PlayerStateStatus Status, string Reason);

/// <summary>The name and entry-id questions the player-anim lookup answers, by content ready, on both sides, from any thread.</summary>
internal interface IPlayerStateLookup
{
    /// <summary>Why the lookup cannot answer here (no service, the capability off, the graph off or not up yet); null when it can.</summary>
    string? NotAnswering => null;
    /// <summary><c>"State"</c> or <c>"Machine/State"</c>. Empty: none; several: the bare name is in several machines.</summary>
    IReadOnlyList<GameStateAnswer> FindGameStates(string name);
    /// <summary>Null: mod <paramref name="mod"/> defines no state <paramref name="name"/>.</summary>
    ModStateAnswer? Find(string mod, string name);
    IReadOnlyList<AnimationReferenceAnswer> FindById(Guid guid) => FindGameStates($"0x{BlockIds.Fnv(guid):x8}")
        .Select(s => new AnimationReferenceAnswer((PlayerAnimStateKind)s.Kind, true, "")).ToList();
}

/// <summary>Where the lookup plugs in: <see cref="PlayerAnimLookup"/> over <c>Kiln.PlayerAnim</c>. Tests set it; null: no lookup at all.</summary>
internal static class PlayerStateSeam
{
    internal static IPlayerStateLookup? Lookup = PlayerAnimLookup.Instance;
}

/// <summary>A refusal of the lookup while a name is resolved: the capability went off between the question and the answer.</summary>
internal sealed class PlayerStatesNotAnswering(string reason) : Exception(reason);

/// <summary>One name of a move's <c>animation</c>, as the compile read it: a GUID it already is, or a name the lookup answers for at content ready. Null names keep the motion's own.</summary>
internal sealed record AnimationRef(string Written, Guid? Guid, string? Name)
{
    /// <summary>The kind, when the name says it (a mod's or a game's state by name). A GUID or an id says none: the event follows from what the motion has.</summary>
    public PlayerStateKind? Kind { get; init; }
}

internal static class MoveStates
{
    /// <summary>What a written name is, offline. A GUID or <c>0x</c> id is parsed here; existence and kind are checked against the receiving graph at content ready.</summary>
    public static AnimationRef? Parse(BlockContext ctx, string? written, string path)
    {
        if (written == null) return null;
        var w = written.Trim();
        if (w.Length == 0) throw new BlockError(ctx.Block, path, "an animation names a state: its name, its GUID or its 0x id (the catalog's graphs section, docs/kiln/modding/catalogs.md)");
        if (System.Guid.TryParse(w, out var g))
        {
            if (g == System.Guid.Empty) throw new BlockError(ctx.Block, path, "the empty GUID names no state");
            return new AnimationRef(written, g, null);
        }
        if (w.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            if (w.Length != 10 || !uint.TryParse(w.AsSpan(2), System.Globalization.NumberStyles.HexNumber, null, out var id))
                throw new BlockError(ctx.Block, path, $"\"{written}\" is not a state id: 0x and 8 hex digits, as the catalog's graphs section writes them");
            return new AnimationRef(written, BlockIds.WithFnv(id), null);
        }
        if (w.Length > 129 || w.Count(c => c == '/') > 1 || w.StartsWith('/') || w.EndsWith('/'))
            throw new BlockError(ctx.Block, path, $"\"{written}\" is not a state's name: a state of this mod (up to 64 characters, no '/'), a game state as \"State\" or \"Machine/State\", a GUID or a 0x id");
        return new AnimationRef(written, null, w);
    }

    /// <summary>A name, when content is ready: the GUID and kind it stands for, or why it stands for none. A state of the mod comes first, then the game's.</summary>
    public static (Guid Guid, PlayerStateKind? Kind)? Resolve(string mod, AnimationRef a, IPlayerStateLookup? lookup, out string? problem)
    {
        problem = null;
        if (a.Guid is { } known)
        {
            if (lookup == null) { problem = $"animation {a.Written}: receiving graph lookup is unavailable"; return null; }
            try
            {
                if (lookup.NotAnswering is { } why) { problem = $"animation {a.Written}: {why}"; return null; }
                var states = lookup.FindById(known);
                var live = states.Where(s => s.Live).ToList();
                if (live.Count == 0)
                {
                    problem = $"animation {a.Written}: state is not live in the receiving graph" + (states.Count > 0 ? $": {states[0].Reason}" : " (no such entry id)");
                    return null;
                }
                if (live.Select(s => s.Kind).Distinct().Count() != 1)
                { problem = $"animation {a.Written}: entry id is ambiguous between full-body and overlay states"; return null; }
                return (known, (PlayerStateKind)live[0].Kind);
            }
            catch (PlayerStatesNotAnswering e) { problem = $"animation {a.Written}: {e.Message}"; return null; }
        }
        var name = a.Name!;
        if (lookup == null)
        {
            problem = $"\"{name}\" cannot be looked up: player states are not available in this build.; the motion keeps its own";
            return null;
        }
        try { return Resolve(mod, name, lookup, out problem); }
        catch (PlayerStatesNotAnswering e)
        {
            problem = NotAnswering(name, e.Message);
            return null;
        }
    }

    private static string NotAnswering(string name, string why) =>
        $"\"{name}\" cannot be looked up: player states are not available here ({why}). The motion keeps its own; raw references also require this lookup";

    private static (Guid Guid, PlayerStateKind? Kind)? Resolve(string mod, string name, IPlayerStateLookup lookup, out string? problem)
    {
        problem = null;
        if (lookup.NotAnswering is { } why)
        {
            problem = NotAnswering(name, why);
            return null;
        }
        if (!name.Contains('/') && lookup.Find(mod, name) is { } own)
        {
            if (own.Status == PlayerStateStatus.Live) return (own.Guid, own.Kind);
            problem = $"mod {mod}'s state \"{name}\" is {own.Status.ToString().ToLowerInvariant()}{(own.Reason is { Length: > 0 } r ? $": {r}" : "")}; the motion keeps its own";
            return null;
        }
        var game = lookup.FindGameStates(name);
        if (game.Count == 0)
        {
            problem = $"\"{name}\" is no state of mod {mod} and none of the game's player graph (the catalog's graphs section lists the game's)";
            return null;
        }
        if (game.Count > 1)
        {
            problem = $"\"{name}\" is a state of several machines of the game's player graph: {string.Join(", ", game.Select(x => $"{x.Machine}/{x.Name}"))}. Say which, as \"Machine/State\"";
            return null;
        }
        var s = game[0];
        if (s.Ids.Count == 0)
        {
            problem = $"the game's state {s.Machine}/{s.Name} is entered by no id (it follows from other states, as Idle_Block does under Block_FullBody), so no sequence can name it";
            return null;
        }
        if (s.Ids.Count > 1)
        {
            // a jump attack's state has one per weapon family: which one this weapon's graph path takes is the family's, and only its id says it
            problem = $"the game's state {s.Machine}/{s.Name} is entered by {s.Ids.Count} ids ({string.Join(", ", s.Ids.Select(i => $"0x{i:x8}"))}, one per weapon family): write the one you mean as its 0x id";
            return null;
        }
        return (BlockIds.WithFnv(s.Ids[0]), s.Kind);
    }
}
