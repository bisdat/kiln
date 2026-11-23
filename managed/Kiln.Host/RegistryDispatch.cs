// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Runtime.Loader;
using System.Security.Cryptography;

namespace Kiln.Host;

internal sealed class RegistryDispatch(ModManager mods) : IDisposable
{
    private readonly RegistryNative _native = new(mods.Host);
    private readonly List<Author> _authors = [];
    private readonly List<Runtime> _runtime = [];
    private readonly Dictionary<ulong, HashSet<ulong>> _changes = [];
    private readonly Dictionary<ModContext, Action> _retirements = [];
    private volatile bool _closed;
    private readonly object _signals = new();
    private (RegistryReadyPhase Phase, ulong Generation)? _pendingReady;
    private bool _worldChanged;
    private string? _worldDirectory;
    private IDisposable? _ready;
    private bool _dispatchingWorld, _starting, _flushing;
    private readonly RegistryWorldBinding _worldBinding = new();
    private sealed class Author(Mod instance, ModContext context, AssemblyLoadContext assembly, string code)
    {
        internal readonly Mod Instance = instance;
        internal readonly ModContext Context = context;
        internal readonly AssemblyLoadContext Assembly = assembly;
        internal readonly string Code = code;
        internal readonly Dictionary<string, ConfigFile> Inputs = new(StringComparer.Ordinal);
        internal IDisposable? Subscription;
    }
    private sealed class Runtime(Mod instance, ModContext context, RegistryEntryPoint points, string code, bool reload)
    {
        internal readonly Mod Instance = instance;
        internal readonly ModContext Context = context;
        internal readonly RegistryEntryPoint Points = points;
        internal readonly string Code = code;
        internal readonly bool Reload = reload;
        internal bool Pending = true, Faulted;
        internal ulong World;
        internal string? WorldId;
        internal readonly HashSet<ulong> WorldChanges = [];
    }
    internal void Prepare(ulong hostOwner)
    {
        RegistryRuntime.Host = this;
        RegistryRuntime.Available = () => _native.LiveAvailable;
        RegistryRuntime.Manual = Manual;
        RegistryRuntime.Revert = (mod, handle) => _native.Revert(mod.Owner, handle);
        RegistryRuntime.Read = Read;
        RegistryRuntime.WorldChanged = directory =>
        {
            lock (_signals) { _worldDirectory = directory; _worldChanged = true; }
        };
        if (!_native.Available) return;
        _starting = _native.StageStatus().Phase < 1;
        _ready = _native.Subscribe(hostOwner, true, (phase, generation) => Ready((RegistryReadyPhase)phase, generation), e => Log("ready callback", e));
        if (!_starting) return; // a late Host may read/live-edit, but cannot retroactively enter GameStart
        var candidates = mods.RegistryCandidates();
        var directories = candidates.Select(c => c.Directory).ToArray();
        foreach (var (manifest, directory) in candidates)
        {
            Author? author = null;
            try
            {
                if (!manifest.HasCode || !RegistryDeclarations.MightOverride(Path.Combine(directory, manifest.EntryAssembly), directories)) continue;
                var code = Code(directory);
                var loaded = mods.CreateRegistryAuthor(manifest, directory, candidates);
                if (loaded is null) continue;
                var (instance, context, assembly) = loaded.Value;
                author = new Author(instance, context, assembly, code);
                _authors.Add(author);
                author.Subscription = _native.Subscribe(context.Owner, false, (phase, _) => Stage(author, (RegistryPhase)phase), e => Retire(author, e));
            }
            catch (Exception e)
            {
                if (author is not null) Retire(author);
                Log(manifest.Id + " game-start author", e);
            }
        }
    }
    internal void Loaded(Mod instance, ModContext context, string directory, bool reload)
    {
        try
        {
            if (_closed || !context.Alive || _retirements.ContainsKey(context)) return;
            Runtime? record = null;
            Action retirement = () =>
            {
                // Still before core OwnerRevoke. Also covers manual changes if author setup fails below.
                RemoveChanges(context);
                if (record is not null) _runtime.Remove(record);
                _retirements.Remove(context);
            };
            _retirements.Add(context, retirement);
            context.Retiring += retirement;
            var points = RegistryDeclarations.PointsOf(instance.GetType());
            if (points != 0)
            {
                record = new Runtime(instance, context, points, Code(directory), reload);
                _runtime.Add(record);
            }
        }
        catch (Exception e) { Log(context.Info.Id + " runtime author setup", e); }
    }
    internal void FlushLoads()
    {
        Game.AssertTickThread();
        if (_closed || _flushing) return;
        DrainSignals();
        if (!_native.LiveAvailable || _native.StageStatus().Phase < 4) return; // a menu load can precede resources-ready
        _flushing = true;
        try
        {
            var pending = _runtime.Where(r => r.Pending && r.Context.Alive).ToArray();
            foreach (var record in pending) record.Pending = false;
            Apply(pending, RegistryEntryPoint.ModLoad, static r => (r.Points & RegistryEntryPoint.ModLoad) != 0 &&
                (!r.Reload || (r.Points & RegistryEntryPoint.HotReload) == 0));
            Apply(pending, RegistryEntryPoint.HotReload, static r => r.Reload && (r.Points & RegistryEntryPoint.HotReload) != 0);
        }
        finally { _flushing = false; }
        TryWorld();
    }
    private static string Code(string directory)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (var file in Directory.EnumerateFiles(directory, "*.dll").Order(StringComparer.Ordinal))
        {
            hash.AppendData(System.Text.Encoding.UTF8.GetBytes(Path.GetFileName(file)));
            using var input = File.OpenRead(file); hash.AppendData(SHA256.HashData(input));
        }
        return Convert.ToHexStringLower(hash.GetHashAndReset());
    }
    private RegistryWorld? World(bool bound = true)
    {
        var status = _native.Status();
        if (status.World == 0) return null;
        var directory = WorldConfigs.Current();
        var world = new RegistryWorld(status.World, (WorldKind)status.Kind, directory is null ? null : Path.GetFileName(directory));
        return !bound || _worldBinding.Matches(world) ? world : world with { Id = null };
    }
    private RegistryContext Context(ModContext owner, RegistryEntryPoint point, RegistryPhase phase, RegistryWorld? world,
        string code, bool stage, Dictionary<string, ConfigFile> inputs, bool readOnly = false)
    {
        var nativeOwner = owner.Owner; // transport delegates must not retain a collectible mod context
        return new(owner, point, phase, world, code,
            (registry, id, field) => _native.Read(nativeOwner, stage, registry, id, field),
            (registry, pattern) => _native.Select(stage, registry, pattern), inputs)
            { ReadOnly = readOnly, CurrentWorldToken = () => _native.Status().World };
    }
    private void Stage(Author author, RegistryPhase phase)
    {
        if (!author.Context.Alive) return;
        var context = Context(author.Context, RegistryEntryPoint.GameStart, phase, null, author.Code, true, author.Inputs);
        using var scope = context.Enter();
        try
        {
            author.Instance.OnRegistry(context);
            var ir = context.CaptureIr();
            context.Close();
            if (ir is not null) _native.SubmitStage(author.Context.Owner, ir);
        }
        catch (Exception e) { context.Close(); Retire(author, e); }
    }
    private void Retire(Author author, Exception? error = null)
    {
        if (error is not null) Log(author.Context.Info.Id + " OnRegistry", error);
        author.Subscription?.Dispose(); author.Subscription = null;
        author.Context.Retire(); mods.Host.OwnerRevoke(author.Context.Owner);
        author.Inputs.Clear(); _authors.Remove(author); author.Assembly.Unload();
    }
    private void Ready(RegistryReadyPhase phase, ulong generation)
    {
        // Native readiness can arrive on a resource thread. Only the tick owns runtime records and author retirement.
        lock (_signals) { if (!_closed) _pendingReady = (phase, generation); }
    }
    private void DrainSignals()
    {
        (RegistryReadyPhase Phase, ulong Generation)? ready;
        lock (_signals)
        {
            ready = _pendingReady; _pendingReady = null;
            if (_worldChanged) { _worldBinding.Signal(_worldDirectory); _worldChanged = false; }
        }
        if (ready is not { } snapshot) return;
        if (_starting && (uint)snapshot.Phase >= 4)
        {
            _starting = false;
            foreach (var author in _authors.ToArray().Reverse()) Retire(author);
        }
        RegistryRuntime.PublishReady(snapshot.Phase, snapshot.Generation, _native.LiveAvailable ? World() : null);
    }
    private void TryWorld()
    {
        Game.AssertTickThread();
        if (_dispatchingWorld || _flushing || !_native.LiveAvailable || _native.StageStatus().Phase < 4) return;
        _dispatchingWorld = true;
        try
        {
            var candidate = World(bound: false);
            if (candidate is null) return;
            var bound = _worldBinding.Bind(candidate);
            var world = bound ? candidate.Value : candidate.Value with { Id = null };
            var records = _runtime.Where(r => !r.Pending && r.Context.Alive && (r.Points & RegistryEntryPoint.WorldLoad) != 0 &&
                (r.World != world.Token || (bound && r.WorldId is null && world.Id is not null))).ToArray();
            foreach (var record in records)
            {
                // Adoption reauthors this point with its settings, rather than stacking it on the id-less turn.
                foreach (var handle in record.WorldChanges.ToArray())
                    try
                    {
                        var result = _native.Revert(record.Context.Owner, handle);
                        if (result.Outcome is 6 or 7) Applied(record.Context, result);
                        ForgetChange(record.Context, handle); record.WorldChanges.Remove(handle);
                    }
                    catch (Exception e) { Log(record.Context.Info.Id + " previous world point cleanup", e); }
                record.World = world.Token; record.WorldId = world.Id; // our own notifications cannot recurse
            }
            Apply(records, RegistryEntryPoint.WorldLoad, static _ => true);
        }
        finally { _dispatchingWorld = false; }
    }
    private void Apply(IEnumerable<Runtime> records, RegistryEntryPoint point, Func<Runtime, bool> selected)
    {
        Game.AssertTickThread();
        var ordered = records.Where(r => selected(r) && r.Context.Alive).ToArray();
        var inputs = ordered.ToDictionary(r => r, _ => new Dictionary<string, ConfigFile>(StringComparer.Ordinal));
        var committed = ordered.ToDictionary(r => r, _ => new List<ulong>());
        foreach (var record in ordered) record.Faulted = false; // a fault closes this point, not every future world/point
        void Remember(Runtime record, RegistryCompletion result)
        {
            Applied(record.Context, result);
            if (Retained(result))
            {
                committed[record].Add(result.Handle);
                if (point == RegistryEntryPoint.WorldLoad) record.WorldChanges.Add(result.Handle);
            }
        }
        void Fault(Runtime record, Exception error)
        {
            record.Faulted = true;
            foreach (var handle in committed[record].AsEnumerable().Reverse())
                try
                {
                    Applied(record.Context, _native.Revert(record.Context.Owner, handle));
                    ForgetChange(record.Context, handle); record.WorldChanges.Remove(handle);
                }
                catch (Exception e) { Log(record.Context.Info.Id + " point rollback (owner cleanup will retry)", e); }
            committed[record].Clear();
            Log(record.Context.Info.Id + " OnRegistry " + point, error);
        }
        foreach (var phase in new[] { RegistryPhase.Add, RegistryPhase.Alter, RegistryPhase.Final })
        {
            var finals = new List<(Runtime Record, ulong Handle)>();
            foreach (var record in ordered)
            {
                if (!record.Context.Alive || record.Faulted) continue;
                ulong handle = 0;
                RegistryContext? context = null;
                try
                {
                    context = Context(record.Context, point, phase, World(), record.Code, false, inputs[record]);
                    using var scope = context.Enter();
                    record.Instance.OnRegistry(context);
                    var ir = context.CaptureIr(); context.Close();
                    if (ir is null) continue;
                    handle = _native.Begin(record.Context.Owner, point);
                    _native.Submit(record.Context.Owner, handle, ir);
                    if (phase == RegistryPhase.Final) finals.Add((record, handle));
                    else Remember(record, _native.Commit(record.Context.Owner, handle));
                    handle = 0;
                }
                catch (Exception e) { Fault(record, e); }
                finally { context?.Close(); if (handle != 0) try { _native.Revert(record.Context.Owner, handle); } catch { } }
            }
            // Every final handler read the add+alter view, never another mod's final commit.
            foreach (var (record, handle) in finals)
                try { Remember(record, _native.Commit(record.Context.Owner, handle)); }
                catch (Exception e) { try { _native.Revert(record.Context.Owner, handle); } catch { } Fault(record, e); }
        }
    }
    private static bool Retained(RegistryCompletion result) => result.Outcome is 0 or 1 or 8 && result.Accepted != 0 && result.Handle != 0;
    private RegistryCompletion Applied(ModContext owner, RegistryCompletion result)
    {
        if (result.Outcome == 4 && result.Handle != 0)
        {
            _native.Revert(owner.Owner, result.Handle); // a world race leaves an unordered native draft, not a retained change
            result = result with { Handle = 0 };
        }
        if (Retained(result))
        {
            if (!_changes.TryGetValue(owner.Owner, out var handles)) _changes[owner.Owner] = handles = [];
            handles.Add(result.Handle);
        }
        if (result.Refused != 0 || result.Outcome is 2 or 3 or 4 or 6 or 7)
            mods.Host.Log(LogLevel.Warn, $"registry: {owner.Info.Id}: outcome {result.Outcome}, {result.Accepted} accepted, {result.Refused} refused: {result.Reason}");
        return result;
    }
    private RegistryCompletion Manual(ModContext owner, Action<RegistryContext> action)
    {
        Game.AssertTickThread();
        if (!_native.LiveAvailable) throw new KilnUnavailableException("kiln.registry.live");
        var world = World();
        if (world is null) return new(4, 0, 0, 0, 0, 0, 0, "a world is required");
        var context = Context(owner, RegistryEntryPoint.Manual, RegistryPhase.Alter, world, "manual:" + owner.Info.Version, false, new());
        using var scope = context.Enter(); ulong handle = 0;
        try
        {
            action(context); var ir = context.CaptureIr(); context.Close();
            if (ir is null) return new(2, 0, 0, 0, world.Value.Token, 0, 0, "the transaction has no edits");
            handle = _native.Begin(owner.Owner, RegistryEntryPoint.Manual); _native.Submit(owner.Owner, handle, ir);
            var result = Applied(owner, _native.Commit(owner.Owner, handle)); handle = 0; return result;
        }
        finally { context.Close(); if (handle != 0) try { _native.Revert(owner.Owner, handle); } catch { } }
    }
    private void Read(ModContext owner, Action<RegistryContext> action)
    {
        Game.AssertTickThread();
        if (!_native.LiveAvailable || _native.StageStatus().Phase < 4) throw new KilnUnavailableException("kiln.registry");
        var context = Context(owner, RegistryEntryPoint.Manual, RegistryPhase.Final, World(), "resolved", false, new(), readOnly: true);
        using var scope = context.Enter();
        try { action(context); } finally { context.Close(); }
    }
    private void ForgetChange(ModContext owner, ulong handle)
    {
        if (!_changes.TryGetValue(owner.Owner, out var handles)) return;
        handles.Remove(handle);
        if (handles.Count == 0) _changes.Remove(owner.Owner);
    }
    private void RemoveChanges(ModContext owner)
    {
        if (!_changes.Remove(owner.Owner, out var handles)) return;
        foreach (var handle in handles)
            try { _native.Revert(owner.Owner, handle); }
            catch (Exception e) { Log(owner.Info.Id + " cleanup (core retirement will retry)", e); }
    }
    public void Dispose()
    {
        if (_closed) return;
        _closed = true; _ready?.Dispose(); _ready = null;
        foreach (var author in _authors.ToArray().Reverse()) Retire(author);
        foreach (var (owner, retirement) in _retirements)
        {
            owner.Retiring -= retirement;
            RemoveChanges(owner);
        }
        _retirements.Clear(); _runtime.Clear(); _changes.Clear();
        if (ReferenceEquals(RegistryRuntime.Host, this))
        {
            RegistryRuntime.Host = null; RegistryRuntime.Available = null; RegistryRuntime.Manual = null;
            RegistryRuntime.Revert = null; RegistryRuntime.Read = null; RegistryRuntime.WorldChanged = null;
            RegistryRuntime.LastReady = null;
        }
    }
    private void Log(string where, Exception e)
    {
        // Assembly loading and Activator wrap the typed export refusal; keep its entry-point guidance visible.
        for (Exception? cause = e; cause is not null; cause = cause.InnerException)
            if (cause is RegistryContextException) { e = cause; break; }
        mods.Host.Log(LogLevel.Error, $"registry: {where}: {e.GetType().Name}: {e.Message}; this callback's uncommitted edits were discarded");
    }
}

internal sealed partial class ModManager
{
    private RegistryDispatch? _registry;
    private bool _registryReload;
    private string? _registryFault;
    internal void PrepareRegistry(ulong hostOwner)
    {
        _registry = new(this);
        try { _registry.Prepare(hostOwner); }
        catch (Exception e) { _host.Log(LogLevel.Warn, $"registry: game-start authors were not prepared: {e.Message}"); }
    }
    internal void TickRegistry()
    {
        try { _registry?.FlushLoads(); _registryFault = null; }
        catch (Exception e)
        {
            if (_registryFault != e.Message) _host.Log(LogLevel.Warn, $"registry: runtime dispatch deferred: {e.Message}");
            _registryFault = e.Message;
        }
    }
    internal List<(Manifest Manifest, string Directory)> RegistryCandidates()
    {
        var byId = BootCandidates().GroupBy(c => c.Manifest.Id, StringComparer.Ordinal).ToDictionary(g => g.Key, g => g.First(), StringComparer.Ordinal);
        return RegistryOrder().Where(x => byId.ContainsKey(x.Id)).Select(x => (byId[x.Id].Manifest, byId[x.Id].Directory)).ToList();
    }
    internal (Mod Instance, ModContext Context, AssemblyLoadContext Assembly)? CreateRegistryAuthor(Manifest manifest, string directory,
        IReadOnlyList<(Manifest Manifest, string Directory)> snapshot)
    {
        var candidates = snapshot.ToDictionary(x => x.Manifest.Id, x => x.Manifest, StringComparer.Ordinal);
        var exports = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var seen = new HashSet<string>(StringComparer.Ordinal) { manifest.Id };
        var queue = new Queue<Manifest>([manifest]);
        while (queue.Count > 0)
            foreach (var dependency in queue.Dequeue().Dependencies)
                if (seen.Add(dependency.Id) && candidates.TryGetValue(dependency.Id, out var candidate))
                {
                    foreach (var file in candidate.Exports) exports.TryAdd(file, candidate.Id);
                    queue.Enqueue(candidate);
                }
        var alc = new ModLoadContext(manifest.Id + ":registry-author", directory,
            name => RegistryDeclarations.GameStartExport(manifest.Id, exports, name));
        ulong owner = 0;
        try
        {
            var assembly = alc.LoadFile(Path.Combine(directory, manifest.EntryAssembly));
            var type = manifest.EntryType is { } name ? assembly.GetType(name, true)!
                : assembly.GetExportedTypes().Single(t => t.IsSubclassOf(typeof(Mod)) && !t.IsAbstract);
            if ((RegistryDeclarations.PointsOf(type) & RegistryEntryPoint.GameStart) == 0) { alc.Unload(); return null; }
            owner = _host.OwnerCreate("registry-author:" + manifest.Id);
            var context = HostAccess.CreateContext(Info(manifest, directory), owner, Path.Combine(_dataDirectory, manifest.Id));
            var instance = (Mod)Activator.CreateInstance(type)!; HostAccess.Bind(instance, context);
            return (instance, context, alc);
        }
        catch { if (owner != 0) _host.OwnerRevoke(owner); alc.Unload(); throw; }
    }
}
