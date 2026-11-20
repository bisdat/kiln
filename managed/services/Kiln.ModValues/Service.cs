// SPDX-License-Identifier: MIT
// Kiln.ModValues' running parts: the registry (both sides), the server's ledger over Kiln.Saves, the live characters
// (CharacterSync's sessions), the owners' unloads, and the save and import hooks that rebuild projections.
namespace Kiln;

internal static class ModValuesService
{
    private static readonly object Gate = new();
    private static bool _started;
    private static ulong _ticks;
    private static readonly HashSet<CharacterKey> Touched = [];
    private static readonly HashSet<ModContext> Hooked = new(ReferenceEqualityComparer.Instance);

    /// <summary>Definitions of every owner (both sides).</summary>
    internal static readonly Registry Registry = new();

    private static Ledger? _ledger;

    /// <summary>The server's ledger (null on a client that hosts no world, and before the start).</summary>
    internal static Ledger? Ledger => _ledger;

    internal static void EnsureStarted()
    {
        lock (Gate)
        {
            if (_started) return;
            _started = true;
        }
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // the tick, the channel and the subscriptions are Kiln's, not a calling mod's
        try
        {
            ModValuesCaps.Rows.Ensure();
            _ledger = new Ledger(Registry, new SavesValueStore(), IsLive, new BusValueEvents());
            _ledger.Dirty += (c, k) => ModValuesStream.MarkDirty(c, k);
            CharacterData.EnsureStarted();
            ServiceTick.Add("mod-values", Tick);
            Events.On<WorldSavingEvent>(_ => Safe("before the save", () => { if (ModValuesCaps.ServerRole) _ledger.ProjectAll(); }));
            Events.On<WorldLoadedEvent>(_ => Safe("at the world load", () =>
            {
                Touched.Clear();
                if (ModValuesCaps.ServerRole) _ledger.ProjectAll();
            }));
            Events.On<CharacterImportedEvent>(_ => Safe("after a carried import", () => _ledger.ProjectAll()));
            ModValuesStream.Start(_ledger);
            ModValuesNative.Bind();
        }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or NotSupportedException)
        {
            Log(LogLevel.Warn, $"Kiln.ModValues: start: {e.Message}");
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    internal static void Log(LogLevel level, string text) => Backend.Current.Log(0, level, text);

    private static void Safe(string when, Action a)
    {
        try
        {
            a();
        }
        catch (Exception e) when (e is InvalidOperationException or InvalidDataException or IOException or KilnUnavailableException)
        {
            Log(LogLevel.Warn, $"Kiln.ModValues: {when}: {e.Message}");
        }
    }

    // ---------------------------------------------------------------------------------------------- live ----

    internal static bool IsLive(CharacterKey c)
    {
        foreach (var l in CharacterSync.Live())
            if (l.Key == c) return true;
        if (!Players.Available) return false;
        foreach (var p in Players.All)
            if (p.InGame && CharacterStore.KeyOf(p) == c) return true;
        return false;
    }

    internal static IReadOnlyList<CharacterKey> LiveKeys() => CharacterSync.Live().Select(l => l.Key).Distinct().ToList();

    private static void Tick(in TickInfo tick)
    {
        if (!ModValuesCaps.ServerRole)
        {
            ModValuesClient.ClientTick();
            return;
        }
        ModValuesStream.ServerTick();
        if (++_ticks % 5 != 0 || _ledger is null) return;
        Safe("at the character tick", () =>
        {
            if (_ledger.Store.World == Guid.Empty) return;
            var now = LiveKeys().Where(k => !k.IsAccountOnly).ToHashSet();
            foreach (var c in now)
                if (Touched.Add(c)) _ledger.Touch(c);  // the character's first ready server step: its values exist from here
            Touched.IntersectWith(now);
        });
    }

    // ---------------------------------------------------------------------------------------------- owners ----

    /// <summary>The calling mod's namespace; hooks its unload once (its definitions retire then, the data stays).</summary>
    internal static string CallerOwner(string api)
    {
        var mod = ModContext.CurrentMod ?? throw new InvalidOperationException($"{api} must be called from a mod's code (its OnLoad, handlers or tasks)");
        var owner = ModValueRules.OwnerOf(mod);
        if (!ModValueRules.IsOwner(owner)) throw new InvalidOperationException($"{api}: the mod id \"{mod.Info.Id}\" is not a canonical mod id");
        lock (Gate)
            if (Hooked.Add(mod))
                mod.UnloadStarting += () =>
                {
                    Retire(owner, mod);
                    lock (Gate) Hooked.Remove(mod);
                    return null;
                };
        return owner;
    }

    internal static void Retire(string owner, object? token)
    {
        foreach (var d in Registry.Retire(owner, token))
            if (Events.HasSubscribers<ModValueDefinitionChangedEvent>()) Events.Post(new ModValueDefinitionChangedEvent(d.Key, null, "retired"));
        ModValuesStream.DefinitionsChanged();
    }

    internal static ModValueDefinitionResult Describe(Def d, object? token)
    {
        var why = Registry.Describe(d, token, out var added);
        if (why is not null) return new(false, why, d.Key);
        if (added && Events.HasSubscribers<ModValueDefinitionChangedEvent>()) Events.Post(new ModValueDefinitionChangedEvent(d.Key, d.ToPublic(false), "described"));
        return new(true, null, d.Key);
    }

    internal static ModValueDefinitionResult Define(Def d, object? token)
    {
        var why = Registry.Activate(d, token, out var activated);
        if (why is not null) return new(false, why, d.Key);
        if (activated)
        {
            if (Events.HasSubscribers<ModValueDefinitionChangedEvent>()) Events.Post(new ModValueDefinitionChangedEvent(d.Key, d.ToPublic(true), "activated"));
            _ledger?.Activated(d, LiveKeys());
            ModValuesStream.DefinitionsChanged();
        }
        return new(true, null, d.Key);
    }

    internal static Ledger NeedLedger(string api)
    {
        ModValuesCaps.Require(ModValuesCaps.Own);
        Services.RequireTickThread(api);
        return _ledger ?? throw ServiceCaps.Unavailable(ModValuesCaps.Own, "Kiln.ModValues did not start");
    }
}
