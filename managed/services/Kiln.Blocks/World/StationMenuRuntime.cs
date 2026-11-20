// SPDX-License-Identifier: MIT
// Station menus at run time (chapter 16a; dev31 out/16a-station-tabs/DESIGN.md sections 1 and 6). A station's own tabs
// (MenuBlock.Tabs) are an intent recorded when the block is committed: at content ready, once every recipe they name is lowered
// and registered, they are stored as the station's mod (its code's owner, or a data-only mod's service owner, as its other blocks);
// at each later content generation the record is computed again (recipes that went are left out, a key that comes back is bound
// again) and stored again only when it changed. StationMenus.Define lays a mod's own layer over a station at run time, with the
// same upkeep. A mod's retirement or reload takes its layers off. The store is Kiln.Content's StationMenuStore (kiln.stationmenus).
// Once the mods' block files are compiled and every station's own tabs were tried once, StationMenuSync hears it
// (MenuDeclarationsReady). Tick thread for everything that reaches the store.
namespace Kiln;

internal static class StationMenuRuntime
{
    /// <summary>A menu a mod asked for: a station's own tabs (<see cref="Ledger"/> set) or a layer of StationMenus.Define.</summary>
    internal sealed class Intent(StationMenuCompiler.Station station, StationMenuCompiler.Menu menu, ulong owner, string mod, ModBlocks? ledger)
    {
        public StationMenuCompiler.Station Station { get; } = station;
        public StationMenuCompiler.Menu Menu { get; } = menu;
        public ulong Owner { get; } = owner;
        /// <summary>The mod that defines the menu.</summary>
        public string Mod { get; } = mod;
        /// <summary>A station's own tabs: the ledger of the station's mod, whose owner is the layer's.</summary>
        public ModBlocks? Ledger { get; } = ledger;
        public bool Inline => Ledger != null;
        /// <summary>The mod reloaded or unloaded since: its layers go.</summary>
        public bool Stale => Ledger != null && (Ledger.Owner != Owner || Ledger.Revoked);
        public bool Attempted, Accepted, Waiting;
        /// <summary>The record the store holds for it now (null: none yet), and its layer's handle.</summary>
        public string? Record;
        public ulong Handle;
        /// <summary>applied, pending (PendingContent) or refused, and why.</summary>
        public string State = "pending";
        public string Reason = StationMenuCompiler.PendingContent;
        public string? Told;
    }

    private static readonly object Gate = new();
    private static readonly Dictionary<(ulong Owner, Guid Definition), Intent> Intents = new();
    private static readonly Dictionary<ulong, StationMenuRegistration> Registrations = new();
    private static readonly HashSet<ulong> RetiredOwners = new();
    private static readonly HashSet<ModContext> Watched = new();
    private static bool _hooked, _declared;
    private static volatile bool _dirty;
    private static int _againCalls;
    private static IDisposable? _tick;

    /// <summary>False: no service tick of its own (tests call <see cref="Pump"/>).</summary>
    internal static bool AutoTick = true;
    /// <summary>Every loaded mod's block files were compiled (BlocksStartup's pass). Tests replace it.</summary>
    internal static Func<bool> BlockPassDone = DefaultBlockPassDone;
    /// <summary>Kiln.Blocks' part of MenuDeclarationsReady. Tests replace it.</summary>
    internal static Action Declared = StationMenuSync.BlocksDeclared;
    /// <summary><see cref="Declared"/> was called (once).</summary>
    internal static bool IsDeclared
    {
        get { lock (Gate) return _declared; }
    }

    /// <summary>The service's tick: retirements, stale layers, and the declarations' signal. Started by the first station menu, and at
    /// the first service tick by <see cref="StationMenuStartup"/> (once ServiceTick.AutoStart lists it).</summary>
    internal static void Start()
    {
        if (!AutoTick) return;
        lock (Gate) _tick ??= BlocksBridge.AddTick("kiln station menus", step =>
        {
            if (step % 10 == 7) Pump();
        });
    }

    /// <summary>What the tick does: takes off the layers of retired or reloaded mods, then says when the declarations are in.</summary>
    internal static void Pump()
    {
        Cleanup();
        CheckDeclared();
    }

    /// <summary>A recipe with a MenuKey was committed: the menus are resolved again at the next ready tick.</summary>
    internal static void BindingsChanged() => _dirty = true;

    // ---- a station's own tabs ---------------------------------------------------------------------------------------

    /// <summary>A station with tabs of its own was committed (PropCompiler.DefineStation): stored from content ready on.</summary>
    internal static void AddInline(ModBlocks ledger, StationMenuCompiler.Station station, StationMenuCompiler.Menu menu)
    {
        lock (Gate)
        {
            var key = (ledger.Owner, menu.Id);
            var intent = new Intent(station, menu, ledger.Owner, ledger.Id, ledger);
            // the same station compiled again in the same lifetime: what the store holds stays until the new record differs
            if (Intents.TryGetValue(key, out var old)) (intent.Record, intent.Handle) = (old.Record, old.Handle);
            Intents[key] = intent;
        }
        _dirty = true;
        Start();
        Hook();
    }

    private static void Hook()
    {
        lock (Gate)
        {
            if (_hooked) return;
            _hooked = true;
        }
        BlockRuntime.Host.OnReady(Pass, Again);
    }

    /// <summary>At content ready and at each content generation: every station's own tabs and every accepted layer, resolved again.</summary>
    private static bool Pass()
    {
        _dirty = false;
        Cleanup();
        var stateOf = ContentStates();
        Intent[] all;
        lock (Gate) all = Intents.Values.ToArray();
        foreach (var intent in all)
            if (intent.Inline || intent.Accepted) Reconcile(intent, stateOf);
        return true;
    }

    // between content generations: a recipe registered a little after content was ready (PendingContent), now and then
    private static bool Again()
    {
        if (_dirty) return true;
        bool waiting;
        lock (Gate) waiting = Intents.Values.Any(i => i.Waiting);
        return waiting && ++_againCalls % 15 == 0;
    }

    private static Func<Guid, string?> ContentStates()
    {
        var states = new Dictionary<Guid, string>();
        foreach (var d in BlockRuntime.Host.ContentDefinitions())
            if (d.Kind == ContentKind.Recipes) states[d.Guid] = d.State;
        return g => states.TryGetValue(g, out var s) ? s : null;
    }

    private static void Reconcile(Intent intent, Func<Guid, string?> stateOf)
    {
        intent.Attempted = true;
        var res = StationMenuCompiler.Resolve(intent.Menu, intent.Station, StationMenuCompiler.LiveBindings(), stateOf, prune: intent.Accepted);
        if (res.Outcome == StationMenuCompiler.Outcome.Pending)
        {
            (intent.Waiting, intent.State, intent.Reason) = (true, "pending", res.Problem);
            Tell(intent, LogLevel.Info, res.Problem, error: false);
            return;
        }
        if (res.Outcome == StationMenuCompiler.Outcome.Refused)
        {
            (intent.Waiting, intent.State, intent.Reason) = (false, "refused", res.Problem);
            Tell(intent, LogLevel.Error, res.Problem, error: true);
            return;
        }
        intent.Waiting = false;
        if (res.Record == intent.Record)
        {
            (intent.Accepted, intent.State, intent.Reason) = (true, "applied", "");
            return;  // nothing changed: no new generation
        }
        MenuStoreResult r;
        try
        {
            r = StationMenuStore.Define(intent.Owner, intent.Mod, res.Record, intent.Menu.Id, intent.Station.WorkshopId);
        }
        catch (Exception e) when (e is ArgumentException or KilnUnavailableException or InvalidOperationException)
        {
            (intent.State, intent.Reason) = ("refused", e.Message);
            Tell(intent, LogLevel.Error, e.Message, error: true);
            return;
        }
        if (!r.Ok)
        {
            // a server whose players cannot follow yet, content sync off: tried again at the next content generation
            (intent.State, intent.Reason) = ("refused", r.Reason);
            Tell(intent, LogLevel.Warn, r.Reason, error: false);
            return;
        }
        (intent.Accepted, intent.Record, intent.Handle, intent.State, intent.Reason, intent.Told) = (true, res.Record, r.Handle, "applied", "", null);
        var tabs = intent.Menu.Tabs.Count;
        BlocksBridge.Log(LogLevel.Info, $"blocks: {intent.Mod}: station {intent.Station.Mod}:{intent.Station.Key}, menu {intent.Menu.Key}: {tabs} tab(s) stored"
            + (res.Pruned > 0 ? $", {res.Pruned} recipe(s) left out (no longer in the game)" : "") + $" (generation {r.Generation})");
    }

    private static void Tell(Intent intent, LogLevel level, string text, bool error)
    {
        if (intent.Told == text) return;
        intent.Told = text;
        var line = $"station {intent.Station.Mod}:{intent.Station.Key}, menu {intent.Menu.Key}: {text}";
        if (error && intent.Ledger is { } ledger)
        {
            lock (ledger.Errors)
                if (!ledger.Errors.Contains(line)) ledger.Errors.Add(line);
            ledger.ReportDirty = true;
        }
        BlocksBridge.Log(level, $"blocks: {intent.Mod}: {line}");
    }

    // ---- StationMenus.Define ---------------------------------------------------------------------------------------

    internal static MenuDefinitionResult Define(BlockHandle<StationBlock> station, StationMenuDefinition definition)
    {
        ArgumentNullException.ThrowIfNull(station);
        ArgumentNullException.ThrowIfNull(definition);
        Services.RequireTickThread("StationMenus.Define");
        var caller = ModContext.CurrentMod ?? throw new InvalidOperationException("StationMenus.Define must be called from a mod's code (OnLoad or its handlers)");
        if (station is not StationHandle { MenuStation: { } info })
            throw new ArgumentException($"{station}: this handle does not say where the station's workshop is: use the handle Blocks.Define or Blocks.Find gave", nameof(station));
        if (!info.Own)
            throw new ArgumentException($"station {station} has no workshop of its own: a game workshop keeps the game's tabs (set OwnWorkshop, or a kind that works on its own with processing or recipes)", nameof(station));
        StationMenuCompiler.Menu menu;
        try { menu = StationMenuCompiler.Check(caller.Info.Id, definition.Key, info.Mod, definition.Tabs, "definition."); }
        catch (StationMenuCompiler.MenuError e) { throw new ArgumentException($"{e.Field}: {e.Message}", nameof(definition)); }
        StationMenuStore.RequireStore();
        var owner = BlocksBridge.CallerOwner();
        Start();
        Hook();
        Cleanup();
        var res = StationMenuCompiler.Resolve(menu, info, StationMenuCompiler.LiveBindings(), ContentStates(), prune: false);
        if (res.Outcome == StationMenuCompiler.Outcome.Refused) throw new ArgumentException(res.Problem, nameof(definition));
        if (res.Outcome == StationMenuCompiler.Outcome.Pending)
            return new MenuDefinitionResult { Ok = false, Reason = res.Problem, Generation = StationMenuStore.Generation };
        var r = StationMenuStore.Define(owner, caller.Info.Id, res.Record, menu.Id, info.WorkshopId);
        if (!r.Ok) return new MenuDefinitionResult { Ok = false, Reason = r.Reason, Generation = r.Generation };
        StationMenuRegistration? registration;
        lock (Gate)
        {
            Intents[(owner, menu.Id)] = new Intent(info, menu, owner, caller.Info.Id, null)
            {
                Attempted = true, Accepted = true, Record = res.Record, Handle = r.Handle, State = "applied", Reason = "",
            };
            if (!Registrations.TryGetValue(r.Handle, out registration) || registration.Disposed)
                Registrations[r.Handle] = registration = new StationMenuRegistration(owner, r.Handle, menu.Id);
        }
        Watch(caller, owner);
        return new MenuDefinitionResult { Ok = true, Registration = registration, Generation = r.Generation };
    }

    internal static void Dispose(StationMenuRegistration registration)
    {
        Services.RequireTickThread("StationMenuRegistration.Dispose");
        if ((ModContext.CurrentMod?.Owner ?? 0) != registration.Owner)
            throw new ArgumentException("this station menu belongs to another mod, or to an earlier load of this one: only the mod that defined it takes it off");
        lock (Gate)
        {
            if (registration.Disposed) return;
            registration.Disposed = true;
            Registrations.Remove(registration.Handle);
            foreach (var key in Intents.Where(i => i.Value.Owner == registration.Owner && i.Value.Handle == registration.Handle).Select(i => i.Key).ToList())
                Intents.Remove(key);
        }
        var r = StationMenuStore.Remove(registration.Owner, registration.Handle);
        if (!r.Ok) BlocksBridge.Log(LogLevel.Warn, $"blocks: station menu {registration.DefinitionId}: taking it off: {r.Reason}");
    }

    private static void Watch(ModContext mod, ulong owner)
    {
        lock (Gate)
            if (!Watched.Add(mod)) return;
        mod.Retiring += () =>
        {
            lock (Gate)
            {
                RetiredOwners.Add(owner);
                Watched.Remove(mod);
            }
            _dirty = true;
        };
    }

    // ---- retirement and the declarations ----------------------------------------------------------------------------

    /// <summary>The layers of a mod that retired (code) or reloaded or unloaded (its ledger has another owner now) come off the store.</summary>
    private static void Cleanup()
    {
        List<ulong> owners;
        lock (Gate)
        {
            owners = [.. RetiredOwners, .. Intents.Values.Where(i => i.Stale).Select(i => i.Owner)];
            RetiredOwners.Clear();
            if (owners.Count == 0) return;
            foreach (var key in Intents.Where(i => owners.Contains(i.Value.Owner)).Select(i => i.Key).ToList()) Intents.Remove(key);
            foreach (var (handle, reg) in Registrations.Where(x => owners.Contains(x.Value.Owner)).ToList())
            {
                reg.Disposed = true;
                Registrations.Remove(handle);
            }
        }
        foreach (var owner in owners.Distinct())
        {
            try { StationMenuStore.RemoveOwner(owner); }
            catch (Exception e) when (e is ArgumentException or InvalidOperationException)
            {
                BlocksBridge.Log(LogLevel.Warn, $"blocks: station menus of a retired mod: {e.Message}");
            }
        }
    }

    private static void CheckDeclared()
    {
        lock (Gate)
        {
            if (_declared) return;
            if (Intents.Values.Any(i => i.Inline && !i.Attempted)) return;
        }
        if (!BlockPassDone()) return;
        lock (Gate)
        {
            if (_declared) return;
            _declared = true;
        }
        try { Declared(); }
        catch (Exception e) when (e is not OutOfMemoryException) { BlocksBridge.Log(LogLevel.Error, $"blocks: station menus declared: {e}"); }
    }

    private static bool DefaultBlockPassDone()
    {
        var mods = Mods.All;
        if (mods.Any(m => m.State == ModState.Pending)) return false;
        foreach (var m in mods)
        {
            if (!m.IsLoaded || m.Directory.Length == 0 || !Directory.Exists(Path.Combine(m.Directory, "blocks"))) continue;
            if (!Blocks.AllMods.Any(b => b.Id == m.Id && b.DataLoaded)) return false;
        }
        return true;
    }

    // ---- status and tests --------------------------------------------------------------------------------------------

    /// <summary>A menu intent by its owner and definition id (status, tests).</summary>
    internal static Intent? Find(ulong owner, Guid definition)
    {
        lock (Gate) return Intents.GetValueOrDefault((owner, definition));
    }

    internal static void ResetForTests()
    {
        lock (Gate)
        {
            Intents.Clear();
            Registrations.Clear();
            RetiredOwners.Clear();
            Watched.Clear();
            (_hooked, _declared, _againCalls) = (false, false, 0);
            _tick?.Dispose();
            _tick = null;
        }
        _dirty = false;
        BlockPassDone = DefaultBlockPassDone;
        Declared = StationMenuSync.BlocksDeclared;
    }
}

/// <summary>Started at the first service tick (an entry ("Kiln.Blocks", "Kiln.StationMenuStartup") in ServiceTick.AutoStart), so that the
/// declarations' signal comes also when no mod has a station menu.</summary>
internal static class StationMenuStartup
{
    static StationMenuStartup() => StationMenuRuntime.Start();
}
