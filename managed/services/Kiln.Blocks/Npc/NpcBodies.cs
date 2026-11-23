// SPDX-License-Identifier: MIT
// Whether an NPC block's body of its own is in the game (package 16f, chapter 16f §5.4). The game builds roster NPCs once, at start,
// before any mod's code runs: kiln-quests' boot build lowers each authored body (native_pack.cpp) and reports one row per body in its
// manifest (Kiln.Network's NpcBodyRows: ready, failed with the refusal, removed since the last start). What happens later in this
// process is managed: a body defined from C# is refused in this version (it would have no cooked source identity for the join check and
// no material check: review3 M2), and a mod unloaded while its body is Ready leaves the body in the game until the process ends
// (RemovalPendingRestart). NpcBodyBuildChangedEvent says each change
// once: on a dedicated server at the first service tick after it, on a game client on the main thread before the client world's next
// step (Client.Post).
namespace Kiln;

/// <summary>Where an NPC block's body of its own stands (<see cref="NpcBodies.Status(string)"/>).</summary>
public enum NpcBodyBuildState
{
    /// <summary>Defined after this start's boot build, to be built at the next start. Not reported in this version: a body defined from
    /// code is <see cref="Failed"/> (define it in blocks/*.json).</summary>
    PendingRestart,
    /// <summary>This start's boot build published it: the NPC has this body.</summary>
    Ready,
    /// <summary>The boot build refused it (<see cref="NpcBodyBuildResult.Reason"/> says why), or there is no build record at all
    /// (<c>NoDefinition</c>).</summary>
    Failed,
    /// <summary>Its mod was unloaded while the body was Ready: what the game built stays until the process ends; the next start
    /// leaves it out.</summary>
    RemovalPendingRestart,
    /// <summary>Ready at the previous start and gone now (its mod or its body was taken out).</summary>
    Removed,
}

/// <summary>An NPC body's build status. <see cref="Ok"/> is true only when <see cref="State"/> is <see cref="NpcBodyBuildState.Ready"/>;
/// otherwise <see cref="Reason"/> says why. <see cref="BlockKey"/> is <c>mod:block</c>. <see cref="Template"/>, <see cref="Model"/> and
/// <see cref="Graph"/> are the clone, the model and the animation graph the build used (empty when it has none);
/// <see cref="Generation"/> is the boot build's generation for the body (0 when none).</summary>
public sealed record NpcBodyBuildResult(bool Ok, string? Reason, NpcBodyBuildState State, string ModId, string BlockKey, Guid Template,
    Guid Model, Guid Graph, ulong Generation) : IKilnResult;

/// <summary>The build status of NPC blocks with a body of their own (<c>body.model</c>): whether this start's boot build made it, why
/// not, or whether it waits for a restart. A <see cref="BlockHandle{T}"/> from <see cref="Blocks.Define{T}"/> never means the body is
/// there: ask here. <see cref="NpcBodyBuildChangedEvent"/> tells each change.</summary>
public static class NpcBodies
{
    // the reason of a well-formed key that has no build record
    internal const string NoDefinition = "NoDefinition";
    /// <summary>The refusal of a body defined from C# (the native admission's bodyadmit.h kCodeBody says the same).</summary>
    internal const string CodeReason =
        "an authored roster body defined in C# is not built in this version (it has no cooked source identity for the join check and no material " +
        "check): define the NPC block in blocks/*.json";
    internal const string UnloadedReason = "removal staged; restart required: the mod was unloaded, its body stays in the game until the process ends";
    internal const string RemovedReason = "removed: the body was built at the previous start and is not any more";

    /// <summary>The status of the NPC block <paramref name="npc"/>. Any thread.</summary>
    /// <exception cref="ArgumentException">The handle is not one of an NPC block.</exception>
    /// <exception cref="KilnUnavailableException"><c>blocks.npc_body</c> is off here.</exception>
    [AnyThread, RequiresCapability("blocks.npc_body")]
    public static NpcBodyBuildResult Status(BlockHandle<NpcBlock> npc)
    {
        ArgumentNullException.ThrowIfNull(npc);
        if (npc.Block is not { } block || npc.Mod.Length == 0 || npc.Name.Length == 0 || npc.Guid != BlockIds.Derive(npc.Mod, "npc", block.Was ?? npc.Name))
            throw new ArgumentException($"{npc} is not a handle of an NPC block", nameof(npc));
        RequireCapability();
        return Current(npc.Mod, npc.Name, block.Was ?? npc.Name);
    }

    /// <summary>The status of the NPC block <paramref name="qualifiedBlockKey"/>, <c>mod:block</c>: also for a block whose definition
    /// failed (no handle). A well-formed key without a build record is Failed with the reason <c>NoDefinition</c>. Any thread.</summary>
    /// <exception cref="ArgumentException">The key is not <c>mod:block</c>.</exception>
    /// <exception cref="KilnUnavailableException"><c>blocks.npc_body</c> is off here.</exception>
    [AnyThread, RequiresCapability("blocks.npc_body")]
    public static NpcBodyBuildResult Status(string qualifiedBlockKey)
    {
        ArgumentNullException.ThrowIfNull(qualifiedBlockKey);
        var (mod, name) = Split(qualifiedBlockKey);
        RequireCapability();
        return Current(mod, name);
    }

    // ---- inside ------------------------------------------------------------------------------------------------------------

    private static readonly object Gate = new();
    private static readonly Dictionary<string, (string Mod, string Name)> Code = new(StringComparer.Ordinal);  // mod:block -> defined from code
    private static readonly HashSet<string> SeenLoaded = new(StringComparer.Ordinal);
    private static readonly HashSet<string> Unloaded = new(StringComparer.Ordinal);
    private static readonly Dictionary<(string Mod, string Name), NpcBodyBuildResult> Published = new();
    private static long _version = 1, _flushed, _posted;

    /// <summary>The test kit: why its harness switched blocks.npc_body off, or null while it is on.</summary>
    internal static Func<string?>? KitOff;
    /// <summary>Tests: where events go (null: the bus on a server, the client's main thread on a game client).</summary>
    internal static Action<NpcBodyBuildChangedEvent>? Raise;
    /// <summary>Tests: whether this process is a game client.</summary>
    internal static Func<bool> IsClient = () => NpcServerSide.IsClient();
    /// <summary>Tests: how a game client gets to its main thread.</summary>
    internal static Action<Action> PostToClient = Client.Post;

    internal static (string Mod, string Name) Split(string key)
    {
        var at = key.IndexOf(':');
        if (at <= 0 || at == key.Length - 1 || key.IndexOf(':', at + 1) >= 0 || key.Any(c => char.IsWhiteSpace(c) || char.IsControl(c)))
            throw new ArgumentException($"\"{key}\" is not a qualified block key: mod:block", nameof(key));
        return (key[..at], key[(at + 1)..]);
    }

    internal static void RequireCapability()
    {
        var row = BlockCapabilities.CurrentNpcBodyRow();
        if (row.State != CapabilityState.Available) throw new KilnUnavailableException(BlockCapabilities.NpcBody, row.Text);
        if (BlockCapabilities.NpcBodyForTests != null) return;  // a unit test's own row: the process table is not its business
        // the test kit's switch, read from the harness's own report: the process snapshot below may not hold a managed row for
        // blocks.npc_body (bound afresh after another test swapped the backend), and a switch alone then reads as "no row"
        if (KitOff?.Invoke() is { } off) throw new KilnUnavailableException(BlockCapabilities.NpcBody, off);
        Capability reported;
        try { reported = Capabilities.Get(BlockCapabilities.NpcBody); }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or NotSupportedException or DllNotFoundException or EntryPointNotFoundException)
        {
            return;  // no capability table in this process (a tool)
        }
        // a policy (a test's switch, a gate) can hold the row although its dependencies are here
        if (reported.State != CapabilityState.Available && reported.Reason != CapabilityReason.Unknown)
            throw new KilnUnavailableException(BlockCapabilities.NpcBody, reported.ReasonText);
    }

    private static NpcBodyBuildResult NoRecord(string mod, string name) =>
        new(false, NoDefinition, NpcBodyBuildState.Failed, mod, $"{mod}:{name}", Guid.Empty, Guid.Empty, Guid.Empty, 0);

    private static Guid G(string s) => Guid.TryParse(s, out var g) ? g : Guid.Empty;

    /// <summary>The status now: the boot build's row with what this process did since (a definition from code, an unload).</summary>
    private static NpcBodyBuildResult Current(string mod, string name, string? key = null) => Compute(mod, name, NpcBodyRows.All, key);

    /// <summary><paramref name="key"/>: the block's key (its Was, else its name), what the build's row may name instead of the block.</summary>
    private static NpcBodyBuildResult Compute(string mod, string name, IReadOnlyList<NpcBodyRow> rows, string? key = null)
    {
        var blockKey = $"{mod}:{name}";
        bool code, unloaded;
        lock (Gate)
        {
            code = Code.ContainsKey(blockKey);
            unloaded = Unloaded.Contains(mod);
        }
        // a body defined from C# is not built in this version, whatever the start built under that name (review3 M2)
        if (code) return new(false, CodeReason, NpcBodyBuildState.Failed, mod, blockKey, Guid.Empty, Guid.Empty, Guid.Empty, 0);
        key ??= name;
        var row = rows.FirstOrDefault(r => r.Mod == mod && r.Npc == name) ?? rows.FirstOrDefault(r => r.Mod == mod && r.Key == key);
        var built = row == null ? null : new NpcBodyBuildResult(false, null, NpcBodyBuildState.Failed, row.Mod, row.BlockKey, G(row.Template), G(row.Model),
            G(row.Graph), row.Generation);
        if (built == null) return NoRecord(mod, name);
        return row!.State switch
        {
            NpcBodyRow.Ready when unloaded => built with { Reason = UnloadedReason, State = NpcBodyBuildState.RemovalPendingRestart },
            NpcBodyRow.Ready => built with { Ok = true, State = NpcBodyBuildState.Ready },
            NpcBodyRow.Removed => built with { Reason = row.Reason.Length > 0 ? row.Reason : RemovedReason, State = NpcBodyBuildState.Removed },
            NpcBodyRow.Failed => built with { Reason = row.Reason.Length > 0 ? row.Reason : "the boot build refused this body" },
            _ => built with { Reason = $"the boot build reports \"{row.State}\" for this body, a state this Kiln does not know" },
        };
    }

    /// <summary>NpcCompiler: an NPC block with a body of its own was defined from code. Refused in this version (<see cref="CodeReason"/>):
    /// NpcCompiler writes no file for it, and nothing waits for a restart.</summary>
    internal static void DefinedFromCode(string mod, string name)
    {
        var key = $"{mod}:{name}";
        lock (Gate)
        {
            if (!Code.TryAdd(key, (mod, name))) return;
            _version++;
        }
        Kick();
    }

    /// <summary>NpcCompiler: the block comes from a data file (the boot build's own): its row is the status.</summary>
    internal static void FromData(string mod, string name)
    {
        var key = $"{mod}:{name}";
        lock (Gate)
        {
            if (!Code.Remove(key)) return;
            _version++;
        }
        Kick();
    }

    /// <summary>BlocksStartup: the mod is loaded (again).</summary>
    internal static void ModLoaded(string mod)
    {
        lock (Gate)
        {
            SeenLoaded.Add(mod);
            if (!Unloaded.Remove(mod)) return;
            _version++;
        }
        Kick();
    }

    /// <summary>BlocksStartup: the mod was unloaded. A mod that never loaded in this process is not "unloaded".</summary>
    internal static void ModUnloaded(string mod)
    {
        lock (Gate)
        {
            if (!SeenLoaded.Contains(mod) || !Unloaded.Add(mod)) return;
            _version++;
        }
        Kick();
    }

    /// <summary>Sends what changed since the last time, from the right thread: now on a server's tick thread (BlocksStartup's tick),
    /// on a game client from its main thread. Cheap when nothing changed.</summary>
    internal static void Kick()
    {
        if (IsClient())
        {
            // review3 N7: nothing new since the last flush, nothing to post to the main thread
            var rows = NpcBodyRows.All;
            lock (Gate)
                if (_version == _flushed && ReferenceEquals(rows, _flushedRows)) return;
            if (Interlocked.Exchange(ref _posted, 1) != 0) return;
            // Kiln's work, not the mod whose definition or unload led here: Client.Post drops a mod's work when the mod unloads
            var outer = ModContext.CurrentMod;
            ModContext.CurrentMod = null;
            try
            {
                PostToClient(() =>
                {
                    Interlocked.Exchange(ref _posted, 0);
                    Flush(clientSide: true);
                });
            }
            catch
            {
                Interlocked.Exchange(ref _posted, 0);
                throw;
            }
            finally
            {
                ModContext.CurrentMod = outer;
            }
            return;
        }
        if (Raise == null && !NpcServerSide.OnTickThread()) return;  // the next tick does it
        Flush(clientSide: false);
    }

    /// <summary>Publishes one event per body whose state or generation changed since its last event. The boot build's rows are a change
    /// from "no record" the first time.</summary>
    internal static void Flush(bool clientSide)
    {
        var rows = NpcBodyRows.All;
        var events = new List<NpcBodyBuildChangedEvent>();
        lock (Gate)
        {
            var version = _version;
            if (version == _flushed && ReferenceEquals(rows, _flushedRows)) return;
            var keys = new SortedSet<(string Mod, string Name)>(Comparer<(string Mod, string Name)>.Create((a, b) =>
                string.CompareOrdinal(a.Mod, b.Mod) is var c and not 0 ? c : string.CompareOrdinal(a.Name, b.Name)));
            foreach (var r in rows) keys.Add((r.Mod, r.Npc));
            foreach (var s in Code.Values) keys.Add(s);
            foreach (var k in Published.Keys) keys.Add(k);
            foreach (var key in keys)
            {
                var now = Compute(key.Mod, key.Name, rows);
                // "no record" is a state of its own: a body the build refused (Failed, generation 0) is still one change from it
                var known = Published.TryGetValue(key, out var was);
                var before = was ?? NoRecord(key.Mod, key.Name);
                if (known ? before.State == now.State && before.Generation == now.Generation : now.Reason == NoDefinition) continue;
                Published[key] = now;
                events.Add(new NpcBodyBuildChangedEvent(before, now, clientSide));
            }
            _flushed = version;
            _flushedRows = rows;
        }
        foreach (var e in events)
        {
            if (Raise is { } raise) raise(e);
            else if (clientSide) ClientEvents.Dispatch(e);
            else if (Events.HasSubscribers<NpcBodyBuildChangedEvent>()) Events.Post(e);
        }
    }

    private static IReadOnlyList<NpcBodyRow>? _flushedRows;

    internal static void ResetForTests()
    {
        lock (Gate)
        {
            Code.Clear();
            SeenLoaded.Clear();
            Unloaded.Clear();
            Published.Clear();
            _version++;
            _flushed = 0;
            _flushedRows = null;
        }
        Interlocked.Exchange(ref _posted, 0);
        NpcBodyRows.ResetForTests();
    }
}

/// <summary>An NPC body's build status changed (<see cref="NpcBodies"/>): one event per change of state or generation, Failed and
/// RemovalPendingRestart included. A body this start's boot build reports comes once as a change from "no record"
/// (<see cref="Before"/> is Failed with the reason <c>NoDefinition</c>). On a dedicated server it is posted on the tick thread at the
/// first service tick after the change; on a game client (<see cref="ClientSide"/>) on the main thread before the client world's next
/// step, so a handler must not assume the server's tick there. Observe only.</summary>
[AnyThread]
public sealed class NpcBodyBuildChangedEvent : KilnEvent
{
    internal NpcBodyBuildChangedEvent(NpcBodyBuildResult before, NpcBodyBuildResult after, bool clientSide)
    {
        Before = before;
        After = after;
        ClientSide = clientSide;
    }

    /// <summary>The status before the change.</summary>
    public NpcBodyBuildResult Before { get; }
    /// <summary>The status now.</summary>
    public NpcBodyBuildResult After { get; }
    /// <summary>Raised on a game client's main thread (else on a dedicated server's tick thread).</summary>
    public bool ClientSide { get; }

    public override string ToString() => $"NPC body {After.BlockKey}: {Before.State} -> {After.State} (generation {After.Generation})";
}
