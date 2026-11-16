// SPDX-License-Identifier: MIT
// Gem wave integration: a removed mod's trigger gem. After a restart without its mod, kiln.content keeps its saved gems as
// "[Missing: ...]" placeholders and makes a placeholder's program reference that nothing serves none (refs.cpp,
// placeholder_clear_programs), so the gem does nothing instead of acting as its base gem. The game, though, tries to spawn
// a socketed gem's impact every step and logs "Gem has no valid Impcat Program assigned" each time while such a gem sits
// in an impact slot (lab 2026-11-16: about one line a step). For Kiln's own trigger programs Kiln does better: when a
// placeholder names one that no registered gem serves, Kiln serves it under its own owner; kiln.content's retry of the
// placeholder's references (every ~2 s) resolves it, and the program runs and calls nothing (no registered gem is that
// item: GemRegistry.Dispatch). A program a mod cooked itself stays none. docs/kiln/services/gems-new.md, "Unload".
namespace Kiln;

internal static class GemPlaceholders
{
    private static int _started;
    private static ulong _owner;
    private static readonly HashSet<GemTriggerKind> ServedByKiln = new();

    /// <summary>The trigger programs that placeholder definitions name (their GUID anywhere in the definition's text) and
    /// that neither a registered gem (<paramref name="servedByMods"/>) nor Kiln already serves, in the programs' order.
    /// Pure.</summary>
    internal static IReadOnlyList<GemRegistry.Program> ToServe(IEnumerable<string?> placeholderDefinitions,
        IReadOnlySet<GemTriggerKind> servedByMods, IReadOnlySet<GemTriggerKind> servedByKiln)
    {
        var text = string.Join("\n", placeholderDefinitions.Where(d => d is not null));
        if (text.Length == 0) return [];
        return GemRegistry.Programs
            .Where(p => !servedByMods.Contains(p.Kind) && !servedByKiln.Contains(p.Kind) &&
                        text.Contains(p.Guid.ToString("D"), StringComparison.OrdinalIgnoreCase))
            .ToList();
    }

    private static IDisposable? _sub;

    /// <summary>Kiln.Abilities' start: after each content load (<see cref="ContentReadyEvent"/>: kiln.content made the
    /// placeholders just before), serve what placeholders need. Kiln's own subscription. Never throws.</summary>
    internal static void Start()
    {
        if (Interlocked.Exchange(ref _started, 1) != 0) return;
        try
        {
            if (!NewContent.Available || !Assets.Available) return;  // no placeholders here, or nothing to serve with
            var outer = ModContext.CurrentMod;
            ModContext.CurrentMod = null;  // Kiln's own subscription, not the mod whose call started Kiln.Abilities
            try { _sub = Events.On<ContentReadyEvent>(_ => Serve()); }
            finally { ModContext.CurrentMod = outer; }
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or ArgumentException
                                      or TypeInitializationException or FileNotFoundException or FileLoadException)
        {
            ModLog.Warn($"gems: placeholder trigger programs are not watched: {e.Message}");
        }
    }

    private static void Serve()
    {
        try
        {
            var byMods = GemRegistry.Definitions.Where(d => d.Trigger is not null).Select(d => d.Trigger!.Kind).ToHashSet();
            IReadOnlyList<GemRegistry.Program> todo;
            lock (ServedByKiln) todo = ToServe(NewContent.Placeholders().Select(d => d.DefinitionJson), byMods, ServedByKiln);
            if (todo.Count == 0) return;
            if (_owner == 0) _owner = Backend.Current.OwnerCreate("kiln.abilities gem placeholders");
            foreach (var p in todo)
            {
                Assets.RegisterAs(_owner, new AssetKey(p.Guid, GemRegistry.ProgramType), GemRegistry.Blob(p));
                lock (ServedByKiln) ServedByKiln.Add(p.Kind);
                ModLog.Info($"gems: placeholders of a removed mod's gems name the trigger program {p.Stem} ({p.Guid}): Kiln serves it, " +
                            "so they run it and do nothing");
            }
            if (Abilities.Available && Game.IsTickThread) Abilities.Programs.Refresh();
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or ArgumentException)
        {
            ModLog.Warn($"gems: a placeholder's trigger program could not be served (the placeholder runs no program): {e.Message}");
        }
    }
}
