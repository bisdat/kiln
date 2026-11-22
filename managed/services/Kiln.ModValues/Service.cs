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
    private static readonly List<(Def Def, object? Token)> PendingDefines = [];
    private static bool _wasServer;

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
            Registry.Report = text => Log(LogLevel.Warn, text);
            _ledger = new Ledger(Registry, new SavesValueStore(), IsLive, new BusValueEvents());
            _ledger.Dirty += (c, k) => ModValuesStream.MarkDirty(c, k);
            CharacterData.EnsureStarted();
            ServiceTick.Add("mod-values", Tick);
            Events.On<WorldSavingEvent>(_ => Safe("before the save", () => { if (ModValuesCaps.ServerRole) _ledger.ProjectAll(); }));
            Events.On<WorldLoadedEvent>(_ => Safe("at the world load", () =>
            {
                Touched.Clear();
                ServerRoleStep();
                ModValuesAuthoring.Poll();
                if (ModValuesCaps.ServerRole) _ledger.ProjectAll();
            }));
            Events.On<CharacterImportedEvent>(_ => Safe("after a carried import", () => _ledger.ProjectAll()));
            ModValuesStream.Start(_ledger);
            ModValuesNative.Bind();
            ModValuesAuthoring.Poll();
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

    /// <summary>A post from any thread: on the tick thread now, elsewhere at the next step (Events.Post is tick-only).</summary>
    internal static void Post<T>(T ev) where T : KilnEvent
    {
        if (!Events.HasSubscribers<T>()) return;
        if (Backend.Current.IsTickThread) Events.Post(ev);
        else Events.Enqueue(ev);
    }

    /// <summary>A game client that is not running a world: a Define (a both-side mod's OnLoad in the main menu) is
    /// described now and activated when this client hosts a world.</summary>
    internal static ModValueDefinitionResult DeferDefine(Def d, object? token)
    {
        var r = Describe(d, token);
        if (!r.Ok) return r;
        lock (Gate)
        {
            PendingDefines.RemoveAll(p => p.Def.Key == d.Key);
            PendingDefines.Add((d, token));
        }
        return r;
    }

    /// <summary>The server role turned on (a hosted world started): deferred Defines and the mods' JSON values become active.</summary>
    private static void ServerRoleStep()
    {
        var server = ModValuesCaps.ServerRole;
        var turnedOn = server && !_wasServer;
        _wasServer = server;
        if (!server) return;
        (Def Def, object? Token)[] pending;
        lock (Gate)
        {
            pending = PendingDefines.Where(p => p.Token is not ModContext { Alive: false }).ToArray();
            PendingDefines.Clear();
        }
        foreach (var (d, token) in pending)
        {
            var r = Define(d, token);
            if (!r.Ok) Log(LogLevel.Error, $"mod-values: {d.Qualified}: {r.Reason}");
        }
        if (turnedOn) ModValuesAuthoring.Poll();
    }

    private static void Tick(in TickInfo tick)
    {
        _ticks++;
        Safe("the server role", ServerRoleStep);
        if (_ticks % 25 == 0) Safe("reading mod-values/*.json", ModValuesAuthoring.Poll);
        if (!ModValuesCaps.ServerRole)
        {
            ModValuesClient.ClientTick();
            return;
        }
        if (_ledger is null) return;
        if (_ticks % 5 == 0)
            Safe("at the character tick", () =>
            {
                if (_ledger.Store.World == Guid.Empty) return;
                var now = LiveKeys().Where(k => !k.IsAccountOnly).ToHashSet();
                foreach (var c in now)
                    if (Touched.Add(c)) _ledger.Touch(c);  // the character's first ready server step: its values exist from here
                Touched.IntersectWith(now);
            });
        Safe("streaming", ModValuesStream.ServerTick);
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
        foreach (var d in Registry.Retire(owner, token)) Post(new ModValueDefinitionChangedEvent(d.Key, null, "retired"));
        lock (Gate) PendingDefines.RemoveAll(p => p.Def.Owner == owner && (token is null || Equals(p.Token, token)));
        ModValuesStream.DefinitionsChanged();
    }

    internal static ModValueDefinitionResult Describe(Def d, object? token)
    {
        var why = Registry.Describe(d, token, out var added);
        if (why is not null) return new(false, why, d.Key);
        if (added) Post(new ModValueDefinitionChangedEvent(d.Key, d.ToPublic(false), "described"));
        return new(true, null, d.Key);
    }

    internal static ModValueDefinitionResult Define(Def d, object? token)
    {
        var why = Registry.Activate(d, token, out var activated);
        if (why is not null) return new(false, why, d.Key);
        if (activated)
        {
            Post(new ModValueDefinitionChangedEvent(d.Key, d.ToPublic(true), "activated"));
            if (Backend.Current.IsTickThread) _ledger?.Activated(d, LiveKeys());  // off the tick: the next character pass initializes
            ModValuesStream.DefinitionsChanged();
        }
        return new(true, null, d.Key);
    }

    /// <summary>The test kit: the real ledger on <paramref name="store"/>, characters live by <paramref name="isLive"/>;
    /// definitions start over. No channel, no native backend, no save hooks.</summary>
    internal static void UseForTests(IValueStore store, Func<CharacterKey, bool> isLive)
    {
        lock (Gate) _started = true;
        Registry.Clear();
        Touched.Clear();
        lock (Gate) PendingDefines.Clear();
        _ledger = new Ledger(Registry, store, isLive, new BusValueEvents());
        ModValuesCaps.Rows.Ensure();
    }

    internal static Ledger NeedLedger(string api)
    {
        EnsureStarted();  // a mod's OnLoad runs before the first service tick
        ModValuesCaps.Require(ModValuesCaps.Own);
        Services.RequireTickThread(api);
        return _ledger ?? throw ServiceCaps.Unavailable(ModValuesCaps.Own, "Kiln.ModValues did not start");
    }
}
