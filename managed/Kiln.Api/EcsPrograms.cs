// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Kiln;

/// <summary>A worker-thread ECS update. Its context and component spans cannot escape this callback.</summary>
public delegate void EcsProgramCallback(EcsProgramContext context);

/// <summary>The outcome of <see cref="EcsPrograms.TryBind(string, EcsProgramCallback, WorldKind)"/>. Since 0.2.0-dev.29.</summary>
/// <param name="Ok">True when the callback is bound and the world that runs now took the program, or no world of that
/// kind runs yet (the main menu): it runs from the next one.</param>
/// <param name="Reason">Why not, in words; null when <paramref name="Ok"/>.</param>
/// <param name="World">The world it was bound for.</param>
/// <param name="Handle">The binding: dispose it to unbind (the mod's unload does too). Not null when the callback is
/// bound, also with <paramref name="Ok"/> false when only the client world that runs now refused the program (a name it
/// lacks, a full table): the binding then runs in a later client world that takes it. Null when nothing was bound.</param>
public sealed record EcsProgramBinding(bool Ok, string? Reason, WorldKind World, IDisposable? Handle) : IKilnResult;

/// <summary>A client world refused one of the calling mod's bound programs (a name that world's maps lack, a full program
/// table, a fault of its callback there, its managed callback threw): raised once per client world and program on the
/// game's main thread, in that client world's next Pre (on a host too: never on the server's tick). An event of a
/// client world that ended before that Pre is not raised. The binding stays and runs in a later client world that
/// takes the program (a callback that threw stays off until it is bound again). Only Kiln raises it
/// (<see cref="Events.Post{T}"/> refuses it). Since 0.2.0-dev.29.</summary>
public sealed class EcsProgramRefusedEvent : KilnEvent
{
    internal override bool TickDispatchAllowed => false;

    internal EcsProgramRefusedEvent(string modId, string key, WorldKind world, string reason)
    {
        ModId = modId;
        Key = key;
        World = world;
        Reason = reason;
    }

    /// <summary>The mod whose program it is.</summary>
    public string ModId { get; }

    /// <summary>Its key in the mod's kiln.programs.json.</summary>
    public string Key { get; }

    /// <summary>The world that refused it (<see cref="WorldKind.Client"/>, or <see cref="WorldKind.Prediction"/> for a
    /// prediction program, which lives in the client world's table).</summary>
    public WorldKind World { get; }

    /// <summary>Why, in kiln.world's words ("events_in: unknown event 'StopGuardingEvent'").</summary>
    public string Reason { get; }
}

/// <summary>Callbacks bound to scheduler declarations in the mod's kiln.programs.json. The native service reserves
/// these declarations before the world starts. Bind in OnLoad; disposing and hot reload leave an inert scheduler
/// entry, and changing declarations requires a server restart.</summary>
public static unsafe class EcsPrograms
{
    internal static EcsWorldTable* Table => Services.Get<EcsWorldTable>("kiln.world", 2);
    /// <summary>kiln.world's table with its client-world members (since 0.2.0-dev.29), or null on an older kiln.world.</summary>
    internal static EcsClientTable* ClientTable => Services.Get<EcsClientTable>("kiln.world", 4);
    /// <summary>The capability of <see cref="TryBind(string, EcsProgramCallback, WorldKind)"/> for a client's worlds.</summary>
    public const string ClientWorldCapability = "world.programs.client";
    /// <summary>Whether boot declarations and callback binding work on this build.</summary>
    public static bool Available => Table != null && (Table->Capabilities() & 4) != 0;
    /// <summary>Whether tracked other-entity writes work on this build.</summary>
    public static bool SupportsOtherWrites => Table != null && (Table->Capabilities() & 1) != 0;

    /// <summary>Binds one declaration belonging to the calling mod. The callback runs on game worker threads;
    /// ordinary tick-thread services require Scheduler.Post. Mod unload drains the callback before releasing it.</summary>
    [TickThreadOnly]
    public static IDisposable Bind(string key, EcsProgramCallback callback)
    {
        Services.RequireTickThread("EcsPrograms.Bind");
        ArgumentException.ThrowIfNullOrEmpty(key);
        ArgumentNullException.ThrowIfNull(callback);
        var mod = ModContext.CurrentMod ?? ModContext.OwnerOf(callback)
            ?? throw new InvalidOperationException("EcsPrograms.Bind must belong to a loaded mod");
        if (!mod.Alive) throw new InvalidOperationException("the mod is unloading");
        var table = Table;
        if (table == null || (table->Capabilities() & 4) == 0) throw new NotSupportedException("kiln.world programs v2 is unavailable");
        var result = TryBind(mod, key, callback, table, out var binding);
        if (result != 0)
            throw new InvalidOperationException($"cannot bind ECS program '{key}' ({result}); declare it in kiln.programs.json before starting the server");
        return binding!;
    }

    /// <summary>Binds the calling mod's declaration <paramref name="key"/> of its kiln.programs.json to
    /// <paramref name="callback"/> for the world of <paramref name="world"/>. Since 0.2.0-dev.29.
    /// <list type="bullet">
    /// <item><see cref="WorldKind.Server"/>: <see cref="Bind"/> without the exceptions (a declaration that is not reserved, or
    /// is bound already, is <c>Ok = false</c>). Tick thread.</item>
    /// <item><see cref="WorldKind.Client"/>: a game client's client world (this player's process only: the local player,
    /// camera and presentation): a declaration with <c>"world": "client"</c> or <c>"both"</c>. The callback runs every
    /// client step, on the game's workers or its main thread (SYSTEM, CHUNKED, PINNED programs), in every client world
    /// from the one that runs now (or the next, in the main menu) until the handle is disposed or the mod unloads. A
    /// name the client world lacks refuses the program for that world only (<c>Ok = false</c> with the reason now, or
    /// <see cref="EcsProgramRefusedEvent"/> for a later world). No other player and no vanilla client sees what it
    /// does: a program that must affect others belongs on the server.</item>
    /// <item><see cref="WorldKind.Prediction"/>: <c>"world": "prediction"</c>: in the client world's table, run by the
    /// prediction storage every prediction step on the main thread. The server must run the same logic as a server
    /// program, or the game corrects the prediction every step.</item>
    /// </list>
    /// Client and prediction: the game's main thread only (OnLoad, which runs there for a client mod in the main menu,
    /// or <c>Client.Post</c>). The callback must be thread-agnostic: no tick-thread API inside it; UI work through
    /// <c>Client.Post</c>.</summary>
    /// <returns>The binding (see <see cref="EcsProgramBinding"/>).</returns>
    /// <exception cref="ArgumentException">An empty key, a null callback, or a world kind that is not one of the three.</exception>
    /// <exception cref="InvalidOperationException">Not called by a loaded mod; a client or prediction bind off the main
    /// thread; a server bind off the tick thread.</exception>
    /// <exception cref="KilnUnavailableException">Client and prediction: <see cref="ClientWorldCapability"/> is off (a
    /// dedicated server, a build without the symbols). Server: kiln.world's programs v2 are not here.</exception>
    [RequiresCapability(ClientWorldCapability)]
    public static EcsProgramBinding TryBind(string key, EcsProgramCallback callback, WorldKind world)
    {
        ArgumentException.ThrowIfNullOrEmpty(key);
        ArgumentNullException.ThrowIfNull(callback);
        if (world is not (WorldKind.Server or WorldKind.Client or WorldKind.Prediction))
            throw new ArgumentException($"no world kind {(int)world}", nameof(world));
        var mod = ModContext.CurrentMod ?? ModContext.OwnerOf(callback)
            ?? throw new InvalidOperationException("EcsPrograms.TryBind must belong to a loaded mod");
        if (!mod.Alive) throw new InvalidOperationException("the mod is unloading");
        if (world == WorldKind.Server)
        {
            Services.RequireTickThread("EcsPrograms.TryBind");
            var table = Table;
            if (table == null || (table->Capabilities() & 4) == 0)
                throw new KilnUnavailableException("world.programs", "kiln.world's boot programs (v2) are not here");
            if (DeclarationChanged(mod, key) is { } changedServer) return new EcsProgramBinding(false, changedServer, world, null);
            var rc = TryBind(mod, key, callback, table, out var bound);
            return rc == 0 ? new EcsProgramBinding(true, null, world, bound)
                : new EcsProgramBinding(false, rc switch
                {
                    2 => $"'{key}' is not reserved for the server world: declare it in kiln.programs.json and restart the server",
                    1 => $"'{key}': another binding holds the program (bind it once, or dispose the first)",
                    3 => $"'{key}': the program is not running (it faulted or its world is gone)",
                    _ => $"'{key}': kiln.world refused the binding (result {rc})",
                }, world, null);
        }
        var row = Capabilities.Get(ClientWorldCapability);
        if (row.State != CapabilityState.Available) throw new KilnUnavailableException(ClientWorldCapability, row.ReasonText);
        var client = ClientTable;
        if (client == null)
            throw new KilnUnavailableException(ClientWorldCapability, "this kiln.world has no client-world programs (an older Kiln core)");
        if (DeclarationChanged(mod, key) is { } changed) return new EcsProgramBinding(false, changed, world, null);
        var binding = new Binding(mod, key, callback, (EcsWorldTable*)client, world);
        binding.Root = GCHandle.Alloc(binding);
        var idBytes = Encoding.UTF8.GetBytes(mod.Info.Id);
        var keyBytes = Encoding.UTF8.GetBytes(key);
        ulong handle = 0;
        int result;
        fixed (byte* id = idBytes, name = keyBytes)
            result = client->BindProgramWorld(mod.Owner, new NativeStr { Ptr = id, Len = (nuint)idBytes.Length },
                new NativeStr { Ptr = name, Len = (nuint)keyBytes.Length }, (uint)world, &Each, null,
                (void*)GCHandle.ToIntPtr(binding.Root), &handle);
        if (result != 0)
        {
            binding.Root.Free();
            return result switch
            {
                4 => throw new InvalidOperationException(
                    $"EcsPrograms.TryBind(\"{key}\", ..., WorldKind.{world}) must run on the game's main thread: in OnLoad (a client mod loads in the main menu) or through Client.Post"),
                6 => throw new InvalidOperationException("the mod is unloading"),
                // the row may not say it yet: kiln.world's own state of the client table tells the process latch apart
                3 when TableLatch(client, world) is { } latched => throw new KilnUnavailableException(ClientWorldCapability, latched),
                3 when row.State == CapabilityState.Available => new EcsProgramBinding(false,
                    $"'{key}': the last binding's calls are still finishing; try again after the next client step", world, null),
                3 => throw new KilnUnavailableException(ClientWorldCapability, row.ReasonText),
                2 => new EcsProgramBinding(false, $"'{key}' is not reserved for the {world} world: declare it in kiln.programs.json " +
                    $"with \"world\": \"{(world == WorldKind.Client ? "client" : "prediction")}\"{(world == WorldKind.Client ? " (or \"both\")" : "")}, rebuild the mod and restart the game", world, null),
                1 => new EcsProgramBinding(false, $"'{key}': another binding holds the program (bind it once, or dispose the first)", world, null),
                _ => new EcsProgramBinding(false, $"'{key}': kiln.world refused the binding (result {result})", world, null),
            };
        }
        binding.Handle = handle;
        mod.Retiring += binding.Dispose;
        ClientBindings.Watch(binding);
        var (state, why) = binding.WorldState();
        return state is ProgramRefused or ProgramFaulted or ProgramSkipped ||
            (state == ProgramPending && why == "registered from the next client world")
            ? new EcsProgramBinding(false, why.Length > 0 ? why : "the client world that runs now refused the program", world, binding)
            : new EcsProgramBinding(true, null, world, binding);
    }

    internal const int ProgramPending = 0, ProgramActive = 1, ProgramRefused = 2, ProgramFaulted = 3, ProgramSkipped = 4;

    // kiln.world's state of the client table itself (program_world_state with handle 0, since 0.2.0-dev.29): the process
    // latch's reason once client-world programs are off until the game restarts, else null.
    private static string? TableLatch(EcsClientTable* t, WorldKind world)
    {
        int state = ProgramPending;
        NativeStr reason = default;
        if (t->ProgramWorldState(0, (uint)world, &state, &reason) != 0 || state != ProgramRefused) return null;
        var text = reason.ToString();
        return text.Length > 0 ? text : "client-world programs are off until the game restarts";
    }

    private static readonly string[] DeclarationLists = ["tpl", "tplOptional", "read", "readOptional", "write", "writeOptional",
        "singletonsRead", "singletonsWrite", "otherTpl", "otherRead", "otherWrite", "with", "without", "anyOf", "eventsIn", "eventsOut"];

    /// <summary>Why the mod's kiln.programs.json now declares <paramref name="key"/> otherwise than kiln.world reserved it when
    /// the game started (its lists, flags or <c>"world"</c>: kiln.world.budget's program_reservation), or null when they agree or
    /// one side cannot be read (an older kiln.world, no file: nothing to compare; a missing reservation is the bind's answer).</summary>
    internal static string? DeclarationChanged(ModContext mod, string key)
    {
        var boot = Reservation(mod.Info.Id, key);
        if (boot is null) return null;
        string? now;
        try { now = Declaration(Path.Combine(mod.Info.Directory, "kiln.programs.json"), key); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException) { return null; }
        if (now is null)
            return $"'{key}' is no longer in the mod's kiln.programs.json; kiln.world reserved it when the game started: restart the game";
        if (now == boot) return null;
        var a = boot.Split('\n');
        var b = now.Split('\n');
        var at = 0;
        while (at < a.Length && at < b.Length && a[at] == b[at]) at++;
        var what = at < b.Length && b[at].Length > 0 ? b[at].Split('=')[0] : at < a.Length && a[at].Length > 0 ? a[at].Split('=')[0] : "declaration";
        return $"'{key}': its {what} differs from the declaration kiln.world reserved when the game started; a changed " +
               "kiln.programs.json is in the game only after a restart";
    }

    /// <summary>The canonical text of entry <paramref name="key"/> of a kiln.programs.json, as kiln.world writes a boot
    /// reservation (boot_programs.h's reading: an absent "flags" is SYSTEM | HOST_ONLY for the server world, SYSTEM for a
    /// client or prediction declaration), or null when the file has no such entry.</summary>
    internal static string? Declaration(string file, string key)
    {
        if (!File.Exists(file)) throw new IOException("no kiln.programs.json");
        using var doc = JsonDocument.Parse(File.ReadAllText(file));
        if (!doc.RootElement.TryGetProperty("programs", out var programs) || !programs.TryGetProperty(key, out var entry)) return null;
        var text = new StringBuilder();
        foreach (var list in DeclarationLists)
        {
            text.Append(list).Append('=');
            if (entry.TryGetProperty(list, out var names))
                text.AppendJoin(',', names.EnumerateArray().Select(n => n.GetString()));
            text.Append('\n');
        }
        var world = entry.TryGetProperty("world", out var w) ? w.GetString() ?? "server" : "server";
        uint flags;
        if (entry.TryGetProperty("flags", out var given))
        {
            flags = 0;
            foreach (var f in given.EnumerateArray())
                flags |= f.GetString() switch
                {
                    "system" => 4u, "init" => 8u, "hostOnly" => 16u, "exclusive" => 128u, "chunked" => 512u, "pinned" => 1024u,
                    "barrier" => 2048u, _ => 0u,
                };
        }
        else flags = world is "client" or "prediction" ? 4u : 0x14u;
        text.Append($"flags={flags:x8}\ninterval=1\n");
        if (world != "server") text.Append("world=").Append(world).Append('\n');
        return text.ToString();
    }

    // kiln.world.budget's program_reservation (offset 16; the table is 24 bytes since 0.2.0-dev.29), or null.
    private static string? Reservation(string mod, string key)
    {
        var t = Services.Get<ReservationTable>("kiln.world.budget", 1);
        if (t == null || t->ProgramReservation == null) return null;
        var modBytes = Encoding.UTF8.GetBytes(mod);
        var keyBytes = Encoding.UTF8.GetBytes(key);
        fixed (byte* m = modBytes, k = keyBytes)
        {
            var who = new NativeStr { Ptr = m, Len = (nuint)modBytes.Length };
            var name = new NativeStr { Ptr = k, Len = (nuint)keyBytes.Length };
            uint length = 0;
            if (t->ProgramReservation(who, name, null, 0, &length) != 0 || length == 0 || length > 65536) return null;
            var bytes = new byte[length];
            fixed (byte* text = bytes)
                if (t->ProgramReservation(who, name, text, length, &length) != 0 || length > bytes.Length) return null;
            return Encoding.UTF8.GetString(bytes, 0, (int)length);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ReservationTable
    {
        public ServiceHeader Header;
        public nint ProgramBudget;
        public delegate* unmanaged[Cdecl]<NativeStr, NativeStr, byte*, uint, uint*, int> ProgramReservation;
    }

    /// <summary>Kiln.World's <c>ClientComponentWrittenEvent</c>: set when that assembly loads. <c>Wanted</c> says (lock-free)
    /// whether anything listens; <c>Raise</c> queues one event (any thread).</summary>
    internal static Func<bool>? ClientWriteWanted;
    internal static Action<EntityId, string, byte[], byte[], string, WorldKind>? ClientWritten;

    /// <summary>Binds <paramref name="key"/> of <paramref name="mod"/> to <paramref name="callback"/>: kiln.world's result
    /// (0, or 2 = KILN_ERR_NOT_FOUND when the mod has no boot reservation under that key) and, on 0, the binding. Tick
    /// thread; the caller checked the mod and the table (Kiln.Systems' Systems.Define tells a missing reservation apart).</summary>
    internal static int TryBind(ModContext mod, string key, EcsProgramCallback callback, EcsWorldTable* table, out Binding? bound)
    {
        bound = null;
        var binding = new Binding(mod, key, callback, table);
        binding.Root = GCHandle.Alloc(binding);
        var idBytes = Encoding.UTF8.GetBytes(mod.Info.Id);
        var keyBytes = Encoding.UTF8.GetBytes(key);
        ulong handle = 0;
        int result;
        fixed (byte* id = idBytes, name = keyBytes)
            result = table->Bind(mod.Owner, new NativeStr { Ptr = id, Len = (nuint)idBytes.Length },
                new NativeStr { Ptr = name, Len = (nuint)keyBytes.Length }, &Each,
                (void*)GCHandle.ToIntPtr(binding.Root), &handle);
        if (result != 0)
        {
            binding.Root.Free();
            return result;
        }
        binding.Handle = handle;
        mod.Retiring += binding.Dispose;
        bound = binding;
        return 0;
    }

    internal sealed class Binding(ModContext mod, string key, EcsProgramCallback callback, EcsWorldTable* table,
        WorldKind world = WorldKind.Server) : IDisposable
    {
        internal readonly ModContext Mod = mod;
        internal readonly string Key = key;
        internal readonly EcsProgramCallback Callback = callback;
        internal readonly EcsWorldTable* Native = table;
        internal readonly WorldKind World = world;
        internal GCHandle Root;
        internal ulong Handle;
        private int _failed;  // 1 once its callback threw: it runs no more until bound again
        private string? _failedReason;
        internal bool Failed => Volatile.Read(ref _failed) != 0;
        /// <summary>Marks the binding failed: true for the first caller only (callbacks on several workers may throw at once).</summary>
        internal bool Fail(string reason)
        {
            if (Interlocked.CompareExchange(ref _failed, 1, 0) != 0) return false;
            Volatile.Write(ref _failedReason, reason);
            return true;
        }
        internal volatile bool DisposeRequested;
        private readonly object _disposeLock = new();
        internal ulong ReportedSession;  // the client world session whose refusal was raised (ClientBindings)

        /// <summary>A client or prediction binding's state in the world that runs now: FAULTED with the managed reason once
        /// its callback threw (kiln.world does not see a managed exception: its state stays ACTIVE), else kiln.world's
        /// program_world_state.</summary>
        internal (int State, string Reason) WorldState()
        {
            if (Failed) return (ProgramFaulted, Volatile.Read(ref _failedReason) ?? "its callback threw");
            var t = (EcsClientTable*)Native;
            int state = ProgramPending;
            NativeStr reason = default;
            var h = Volatile.Read(ref Handle);
            if (h == 0 || t->ProgramWorldState(h, (uint)World, &state, &reason) != 0) return (ProgramPending, "");
            return (state, reason.ToString());
        }

        // What the callbacks looked up, per (kind, name): the native slot, the game type's size and the UTF-8 name. Resolved
        // on first use and published copy-on-write, so the per-entity path is a scan of a small array: no allocation, no
        // native lookup and no lock (workers that miss at the same time both resolve; one array wins, the result is equal).
        private Entry[] _entries = [];
        private int _typesMissingLogged;

        internal sealed class Entry(int kind, string name, byte[] utf8, int slot, int size)
        {
            internal readonly int Kind = kind;
            internal readonly string Name = name;
            internal readonly byte[] Utf8 = utf8;
            /// <summary>The access-list slot (or, for <see cref="HashKind"/>, the component hash); -1 when undeclared.</summary>
            internal readonly int Slot = slot;
            /// <summary>The game type's size; -1 when kiln.types does not know it (or is missing).</summary>
            internal readonly int Size = size;
        }

        internal const int HashKind = -1;

        internal Entry Lookup(int kind, string name)
        {
            var entries = Volatile.Read(ref _entries);
            foreach (var e in entries)
                if (e.Kind == kind && (ReferenceEquals(e.Name, name) || e.Name == name)) return e;
            return Resolve(kind, name);
        }

        private Entry Resolve(int kind, string name)
        {
            var utf8 = Encoding.UTF8.GetBytes(name);
            int slot, size = -1;
            fixed (byte* text = utf8)
            {
                var str = new NativeStr { Ptr = text, Len = (nuint)utf8.Length };
                if (kind == HashKind)
                    slot = unchecked((int)((delegate* unmanaged[Cdecl]<NativeStr, uint>)Native->Prefix.ComponentHash)(str));
                else
                    slot = ((delegate* unmanaged[Cdecl]<ulong, int, NativeStr, int>)Native->Prefix.ProgramSlot)(
                        World == WorldKind.Server ? Handle : Handle & 0xFFUL, kind, str);
            }
            if ((kind is 0 or 1 or 2) && slot >= 0)   // an undeclared name throws in Declared: nothing to size or warn about
            {
                var type = GameTypes.Table == null ? null : GameTypes.Find(name);
                if (type is not null) size = type.Size;
                else if (Interlocked.Exchange(ref _typesMissingLogged, 1) == 0)
                    Mod.Log.Warn(GameTypes.Table == null
                        ? $"ECS program '{Key}': kiln.types is not running on this build, so TryRead/TryWrite always return false (TryReadOther/TryWriteOther still work)"
                        : $"ECS program '{Key}': kiln.types does not know component '{name}', so TryRead/TryWrite on it return false");
            }
            var entry = new Entry(kind, name, utf8, slot, size);
            while (true)
            {
                var current = Volatile.Read(ref _entries);
                foreach (var e in current)
                    if (e.Kind == kind && e.Name == name) return e;
                if (Interlocked.CompareExchange(ref _entries, [.. current, entry], current) == current) return entry;
            }
        }
        public void Dispose()
        {
            if (Handle == 0) return;
            if (World != WorldKind.Server)
            {
                DisposeClient();
                return;
            }
            Services.RequireTickThread("EcsPrograms.Dispose");
            var result = Native->Unregister(Handle);
            if (result != 0) throw new InvalidOperationException($"cannot drain ECS program ({result})");
            Handle = 0;
            Mod.Retiring -= Dispose;
            Root.Free();
        }

        // Keep the handle and root until native unbind confirms that every call has returned.
        // A service tick retries a deferred close, including disposal requested inside a slice.
        private void DisposeClient()
        {
            lock (_disposeLock)
            {
                if (Handle == 0) return;
                var first = !DisposeRequested;
                DisposeRequested = true;
                var result = ((EcsClientTable*)Native)->UnbindProgramWorld(Handle);
                if (result != 0 && result != 2)
                {
                    if (first) Mod.Log.Warn($"ECS program '{Key}' ({World} world): a call was still running when it was unbound; disposal will retry (result {result})");
                    return;
                }
                Handle = 0;
                Mod.Retiring -= Dispose;
                ClientBindings.Forget(this);
                Root.Free();
            }
        }
    }
    // Raises EcsProgramRefusedEvent for client and prediction bindings: polled on Kiln's service tick (the tick thread at
    // the start of a step; in the main menu, the game's UI frame) only while such a binding exists, by its world's state
    // (lock-free native reads), once per client world session and binding.
    internal static class ClientBindings
    {
        private static readonly object Lock = new();
        private static Binding[] _live = [];
        private static IDisposable? _tick;

        internal static void Watch(Binding b)
        {
            lock (Lock)
            {
                _live = [.. _live, b];
                _tick ??= ServiceTick.Add("ecs-programs: client-world unbinds", (in TickInfo _) => RetryDisposals());
            }
            // the refusals are the client world's: polled in its Pre, on the main thread
            ClientEvents.BeforeEvents = PollRefusals;
            ClientEvents.EnsureSubscribed();
        }

        internal static void Forget(Binding b)
        {
            lock (Lock)
            {
                _live = Array.FindAll(_live, x => !ReferenceEquals(x, b));
                if (_live.Length != 0 || _tick is null) return;
                _tick.Dispose();
                _tick = null;
            }
        }

        // a disposal that a running call deferred: retried until kiln.world says no call of it runs (any thread)
        internal static void RetryDisposals()
        {
            foreach (var b in Volatile.Read(ref _live))
                if (b.DisposeRequested) b.Dispose();
        }

        internal static void Poll()
        {
            RetryDisposals();
            PollRefusals();
        }

        // the client world's Pre (main thread): a world that refused a bound program, once per client world session
        internal static void PollRefusals()
        {
            foreach (var b in Volatile.Read(ref _live))
            {
                if (b.DisposeRequested) continue;
                var t = (EcsClientTable*)b.Native;
                var session = t->GameWorldSession((uint)WorldKind.Client);
                if (session == 0 || session == b.ReportedSession) continue;
                var (state, why) = b.WorldState();
                if (state is not (ProgramRefused or ProgramFaulted or ProgramSkipped)) continue;
                b.ReportedSession = session;
                b.Mod.Log.Warn($"ECS program '{b.Key}' is refused in this {b.World} world: {why}");
                if (Events.HasSubscribers<EcsProgramRefusedEvent>())
                    ClientEvents.Dispatch(new EcsProgramRefusedEvent(b.Mod.Info.Id, b.Key, b.World, why));
            }
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Each(void* context, ulong* iterator, uint entity, void* user)
    {
        try
        {
            var binding = (Binding)GCHandle.FromIntPtr((nint)user).Target!;
            if (!binding.Mod.Alive || binding.Failed || binding.DisposeRequested) return;
            var previous = ModContext.CurrentMod;
            try
            {
                ModContext.CurrentMod = binding.Mod;
                SliceGuard.Depth++;
                try { binding.Callback(new EcsProgramContext(binding, context, iterator, entity)); }
                finally { SliceGuard.Depth--; }
            }
            catch (Exception error)
            {
                // the first throw only (workers may throw at once): logged once; a client or prediction binding then says
                // FAULTED (WorldState), and the refusal poll raises EcsProgramRefusedEvent in the client world's next Pre
                // and once in every later client world session, as for a native fault
                if (binding.Fail($"its callback threw {error.GetType().Name}: {error.Message}; it stays off until it is bound again"))
                    binding.Mod.Log.Error($"ECS program disabled: {error}");
            }
            finally { ModContext.CurrentMod = previous; }
        }
        catch { /* no managed exception may cross an unmanaged worker frame */ }
    }
}

/// <summary>Access declared by this program. Entity is zero for a system program. All methods are synchronous,
/// worker-thread only; the context and returned spans last only until the current callback returns.</summary>
public readonly unsafe ref struct EcsProgramContext
{
    private readonly EcsPrograms.Binding _binding;
    private readonly void* _context;
    private readonly ulong* _iterator;
    internal EcsProgramContext(EcsPrograms.Binding binding, void* context, ulong* iterator, uint entity)
    { _binding = binding; _context = context; _iterator = iterator; Entity = new EntityId(entity); }
    public EntityId Entity { get; }
    /// <summary>The world this call runs in: <see cref="WorldKind.Server"/>, or for a client binding
    /// <see cref="WorldKind.Client"/> / <see cref="WorldKind.Prediction"/>. Since 0.2.0-dev.29.</summary>
    public WorldKind World => _binding.World;
    // A client or prediction binding's write, for Kiln.World's ClientComponentWrittenEvent (only while it has listeners).
    private void ClientWrote<T>(uint entity, string component, T before, T after) where T : unmanaged
    {
        if (_binding.World == WorldKind.Server || EcsPrograms.ClientWritten is not { } raise || EcsPrograms.ClientWriteWanted?.Invoke() != true)
            return;
        var b = new byte[sizeof(T)];
        var a = new byte[sizeof(T)];
        System.Runtime.CompilerServices.Unsafe.WriteUnaligned(ref b[0], before);
        System.Runtime.CompilerServices.Unsafe.WriteUnaligned(ref a[0], after);
        raise(new EntityId(entity), component, b, a, _binding.Mod.Info.Id + "/program:" + _binding.Key, _binding.World);
    }
    private EcsPrograms.Binding.Entry Declared(int kind, string name)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        var entry = _binding.Lookup(kind, name);
        if (entry.Slot < 0) throw new ArgumentException($"'{name}' was not declared in access list {kind}", nameof(name));
        return entry;
    }
    private int Slot(int kind, string name) => Declared(kind, name).Slot;
    /// <summary>Reads a declared read component of the current entity. Optional absent components return false.
    /// Set template for a tpl/tplOptional declaration. False on builds without kiln.types (logged once per program).</summary>
    public bool TryRead<T>(string component, out T value, bool template = false) where T : unmanaged
    {
        var entry = Declared(template ? 0 : 1, component);
        var data = (void*)_iterator[entry.Slot];
        if (data == null || entry.Size != sizeof(T)) { value = default; return false; }
        value = *(T*)data;
        return true;
    }
    /// <summary>Writes a declared write component of the current entity. In the server world it marks the write for the
    /// game's save and replication. In a client world or the prediction storage (<see cref="World"/> not
    /// <see cref="WorldKind.Server"/>, since 0.2.0-dev.29) it does not: the write is this player's local presentation
    /// state, never saved or sent, as the game's own client programs write what they draw; it raises
    /// <c>ClientComponentWrittenEvent</c> (Kiln.World) while that has listeners. False on builds without kiln.types
    /// (logged once per program).</summary>
    public bool TryWrite<T>(string component, T value) where T : unmanaged
    {
        var entry = Declared(2, component);
        var data = (void*)_iterator[entry.Slot];
        if (data == null || entry.Size != sizeof(T)) return false;
        if (_binding.World == WorldKind.Server)
        {
            var mark = (delegate* unmanaged[Cdecl]<void*, ulong*, void>)_binding.Native->Prefix.MarkWritten;
            mark(_context, _iterator + entry.Slot);
        }
        var before = *(T*)data;
        *(T*)data = value;
        if (_binding.World != WorldKind.Server) ClientWrote(Entity.Value, component, before, value);
        return true;
    }
    /// <summary>Reads another entity through an otherRead declaration. False when absent or the layout size differs.</summary>
    public bool TryReadOther<T>(EntityId entity, string component, out T value) where T : unmanaged
    {
        uint size = 0;
        var read = (delegate* unmanaged[Cdecl]<void*, ulong, uint, uint*, void*>)_binding.Native->Prefix.ReadOther;
        void* data = read(_context, _iterator[Slot(5, component)], entity.Value, &size);
        if (data == null || size != sizeof(T)) { value = default; return false; }
        value = *(T*)data;
        return true;
    }
    /// <summary>Writes another entity through an otherWrite declaration, marking replication/save dirtiness with
    /// the game's job tracker. False when absent, unavailable, or the layout size differs; always false in a client
    /// world or the prediction storage (since 0.2.0-dev.29: the game's other-entity write marks the change for the save
    /// and replication, which a client program's write must not; kiln.world refuses it there too).</summary>
    public bool TryWriteOther<T>(EntityId entity, string component, T value) where T : unmanaged
    {
        uint size = 0;
        var slot = Slot(6, component);
        if (_binding.World != WorldKind.Server) return false;
        void* data = _binding.Native->WriteOther(_context, _iterator[slot], entity.Value, &size);
        if (data == null || size != sizeof(T)) return false;
        *(T*)data = value;
        return true;
    }
    /// <summary><see cref="TryRead{T}(string, out T, bool)"/> for a generated component type (<c>Kiln.Ecs.Health</c>): the
    /// declared name is the type's. Since 0.2.0-dev.27.</summary>
    public bool TryGet<T>(out T value, bool template = false) where T : unmanaged, IGameComponent<T> => TryRead(T.Name, out value, template);
    /// <summary><see cref="TryWrite{T}(string, T)"/> for a generated component type. Since 0.2.0-dev.27.</summary>
    public bool TrySet<T>(T value) where T : unmanaged, IGameComponent<T> => TryWrite(T.Name, value);
    /// <summary><see cref="TryReadOther{T}(EntityId, string, out T)"/> for a generated component type. Since 0.2.0-dev.27.</summary>
    public bool TryGetOther<T>(EntityId entity, out T value) where T : unmanaged, IGameComponent<T> => TryReadOther(entity, T.Name, out value);
    /// <summary><see cref="TryWriteOther{T}(EntityId, string, T)"/> for a generated component type. Since 0.2.0-dev.27.</summary>
    public bool TrySetOther<T>(EntityId entity, T value) where T : unmanaged, IGameComponent<T> => TryWriteOther(entity, T.Name, value);
    /// <summary>A copy of each event in this declared input stream, in queue order. Multiple input types are allowed.</summary>
    public byte[][] ReadEvents(string name)
    {
        var bytes = Declared(7, name).Utf8;
        var result = new List<byte[]>();
        var root = GCHandle.Alloc(result);
        try
        {
            int error;
            fixed (byte* text = bytes)
                error = _binding.Native->Events(_context, new NativeStr { Ptr = text, Len = (nuint)bytes.Length }, &CopyEvent,
                    (void*)GCHandle.ToIntPtr(root));
            if (error != 0) throw new InvalidOperationException($"cannot read program events '{name}' ({error})");
            return result.ToArray();
        }
        finally { root.Free(); }
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void CopyEvent(void* data, uint size, void* user)
    {
        try { ((List<byte[]>)GCHandle.FromIntPtr((nint)user).Target!).Add(new ReadOnlySpan<byte>(data, checked((int)size)).ToArray()); }
        catch { /* never unwind into a native update */ }
    }
    /// <summary>Sends an exact event payload through a declared output stream. The native service stamps GameEvent
    /// timestamps with program time. False when the per-step event buffer is full.</summary>
    public bool EmitEvent(string name, ReadOnlySpan<byte> data)
    {
        var bytes = Declared(8, name).Utf8;
        int result;
        fixed (byte* text = bytes, payload = data)
            result = _binding.Native->Emit(_context, new NativeStr { Ptr = text, Len = (nuint)bytes.Length }, payload, (uint)data.Length);
        if (result is not (0 or 5)) throw new InvalidOperationException($"cannot emit program event '{name}' ({result})");
        return result == 0;
    }
    /// <summary>Adds component bytes at the end of this step. An existing component keeps its old value.</summary>
    public bool QueueAdd(EntityId entity, string component, ReadOnlySpan<byte> data)
    {
        var queue = (delegate* unmanaged[Cdecl]<void*, uint, uint, void*, uint, int>)_binding.Native->Prefix.QueueAdd;
        int result;
        fixed (byte* payload = data) result = queue(_context, entity.Value, ComponentHash(component), payload, (uint)data.Length);
        if (result is not (0 or 5)) throw new InvalidOperationException($"cannot queue component add ({result})");
        return result == 0;
    }
    /// <summary>Removes a component at the end of this step.</summary>
    public bool QueueRemove(EntityId entity, string component)
    {
        var queue = (delegate* unmanaged[Cdecl]<void*, uint, uint, int>)_binding.Native->Prefix.QueueRemove;
        var result = queue(_context, entity.Value, ComponentHash(component));
        if (result is not (0 or 5)) throw new InvalidOperationException($"cannot queue component removal ({result})");
        return result == 0;
    }
    private uint ComponentHash(string name)
    {
        ArgumentNullException.ThrowIfNull(name);
        return unchecked((uint)_binding.Lookup(EcsPrograms.Binding.HashKind, name).Slot);
    }
}

/// <summary>kiln.world's table up to its client-world members (0.2.0-dev.29: 528 bytes, the last member at 520; a kiln.world
/// whose header.size does not reach it has no client-world programs). Asked for on its own, so that
/// <see cref="EcsWorldTable"/> (v2, 240 bytes) still binds server programs on an older kiln.world.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct EcsClientTable
{
    public EcsWorldTable V2;                          // 0..240
    private fixed byte _v3v4[192];                    // 240..432: tracked writes and the game's programs
    public delegate* unmanaged[Cdecl]<uint, ulong> GameWorldSession;  // 432
    public nint GameProgramContract;                  // 440
    private fixed byte _foreign[48];                  // 448..496: the foreign views (06c S5)
    public delegate* unmanaged[Cdecl]<ulong, uint, void*, ulong*, int> RegisterProgramWorld;  // 496
    public delegate* unmanaged[Cdecl]<ulong, uint, int*, NativeStr*, int> ProgramWorldState;  // 504
    public delegate* unmanaged[Cdecl]<ulong, NativeStr, NativeStr, uint, delegate* unmanaged[Cdecl]<void*, ulong*, uint, void*, void>,
        void*, void*, ulong*, int> BindProgramWorld;  // 512
    public delegate* unmanaged[Cdecl]<ulong, int> UnbindProgramWorld;  // 520
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct EcsWorldTable
{
    public WorldPrefix Prefix;
    public nint Teleport, TeleportCancel, Status;
    public delegate* unmanaged[Cdecl]<void*, ulong, uint, uint*, void*> WriteOther;
    public delegate* unmanaged[Cdecl]<ulong, int> Unregister;
    public delegate* unmanaged[Cdecl]<ulong, NativeStr, NativeStr, delegate* unmanaged[Cdecl]<void*, ulong*, uint, void*, void>, void*, ulong*, int> Bind;
    public delegate* unmanaged[Cdecl]<uint> Capabilities;
    public delegate* unmanaged[Cdecl]<void*, NativeStr, delegate* unmanaged[Cdecl]<void*, uint, void*, void>, void*, int> Events;
    public delegate* unmanaged[Cdecl]<void*, NativeStr, void*, uint, int> Emit;
}
