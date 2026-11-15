// SPDX-License-Identifier: MIT
// EcsPrograms.TryBind for a game client's client world and prediction storage (chapter 14, since 0.2.0-dev.29) on a fake
// kiln.world that models what the native service answers: reservations per (key, world), the main-thread rule, a
// client world that refused a name, a call still running when a host unbinds from its server thread, a new client world
// session that refuses again. The native side's own tests: native/services/world/tests/client_world_test.cpp.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Api.Tests;

public sealed unsafe class EcsProgramsClientTests : KilnTest
{
    private readonly FakeClientWorld _native;

    public EcsProgramsClientTests()
    {
        _native = new FakeClientWorld(Backend);
        Capabilities.Report(EcsPrograms.ClientWorldCapability, CapabilityState.Available);
    }

    public override void Dispose()
    {
        try { base.Dispose(); } finally { _native.Dispose(); }
    }

    [Fact]
    public void The_client_table_mirror_puts_the_four_members_at_448_to_472()
    {
        Assert.Equal(480, sizeof(EcsClientTable));
        Assert.Equal(432, (int)Marshal.OffsetOf<EcsClientTable>(nameof(EcsClientTable.GameWorldSession)));
        Assert.Equal(448, (int)Marshal.OffsetOf<EcsClientTable>(nameof(EcsClientTable.RegisterProgramWorld)));
        Assert.Equal(456, (int)Marshal.OffsetOf<EcsClientTable>(nameof(EcsClientTable.ProgramWorldState)));
        Assert.Equal(464, (int)Marshal.OffsetOf<EcsClientTable>(nameof(EcsClientTable.BindProgramWorld)));
        Assert.Equal(472, (int)Marshal.OffsetOf<EcsClientTable>(nameof(EcsClientTable.UnbindProgramWorld)));
        Assert.Equal(240, sizeof(EcsWorldTable));  // Bind's own (v2) mirror is unchanged: it binds on an older kiln.world
    }

    [Fact]
    public void A_client_bind_runs_in_the_client_world_only_and_its_context_says_so()
    {
        var mod = NewMod("pinner");
        var worlds = new List<WorldKind>();
        var r = As(mod, () => EcsPrograms.TryBind("pin", c => { Assert.Same(mod, HostAccess.Current); worlds.Add(c.World); }, WorldKind.Client));
        Assert.True(r.Ok, r.Reason);
        Assert.Null(r.Reason);
        Assert.Equal(WorldKind.Client, r.World);
        Assert.NotNull(r.Handle);
        Assert.Equal((uint)WorldKind.Client, _native.BoundWorld["pin"]);
        _native.Run("pin");
        Assert.Equal([WorldKind.Client], worlds);
        Assert.Null(HostAccess.Current);
        r.Handle!.Dispose();
        Assert.False(_native.Bound.ContainsKey("pin"));
        _native.Run("pin");
        Assert.Single(worlds);
    }

    [Fact]
    public void A_program_waiting_for_the_next_world_is_not_running_in_this_one()
    {
        _native.States["pin"] = (0, "registered from the next client world");
        var r = As(NewMod("pinner"), () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client));
        Assert.False(r.Ok);
        Assert.Contains("next client world", r.Reason);
        Assert.NotNull(r.Handle);
        r.Handle.Dispose();
    }

    [Fact]
    public void Off_the_main_thread_a_client_bind_throws_and_roots_nothing()
    {
        _native.MainThread = false;
        var e = Assert.Throws<InvalidOperationException>(() => As(NewMod("pinner"), () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client)));
        Assert.Contains("main thread", e.Message);
        Assert.False(_native.Bound.ContainsKey("pin"));
    }

    [Fact]
    public void Not_reserved_for_that_world_and_held_bindings_are_answers_not_exceptions()
    {
        var mod = NewMod("pinner");
        var r = As(mod, () => EcsPrograms.TryBind("server_only", _ => { }, WorldKind.Client));
        Assert.False(r.Ok);
        Assert.Null(r.Handle);
        Assert.Contains("\"world\": \"client\"", r.Reason);
        var first = As(mod, () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client));
        var second = As(mod, () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client));
        Assert.True(first.Ok);
        Assert.False(second.Ok);
        Assert.Contains("another binding", second.Reason);
        Assert.Null(second.Handle);
    }

    [Fact]
    public void A_name_the_running_client_world_lacks_binds_but_answers_its_refusal()
    {
        _native.States["lacking"] = (2, "events_in: unknown event 'ServerEvent'");
        var r = As(NewMod("pinner"), () => EcsPrograms.TryBind("lacking", _ => { }, WorldKind.Client));
        Assert.False(r.Ok);
        Assert.Equal("events_in: unknown event 'ServerEvent'", r.Reason);
        Assert.NotNull(r.Handle);  // bound: it runs in a later client world that takes the program
        Assert.True(_native.Bound.ContainsKey("lacking"));
    }

    [Fact]
    public void A_later_client_world_that_refuses_raises_the_event_once_per_session()
    {
        var mod = NewMod("pinner");
        var seen = new List<EcsProgramRefusedEvent>();
        Events.On<EcsProgramRefusedEvent>(seen.Add);
        var r = As(mod, () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client));
        Assert.True(r.Ok);
        Step(3);
        Assert.Empty(seen);  // the world takes it: nothing to say
        _native.Session = 4;
        _native.States["pin"] = (2, "read: unknown component 'RenderTransform'");
        Step(3);
        var e = Assert.Single(seen);
        Assert.Equal(("pinner", "pin", WorldKind.Client), (e.ModId, e.Key, e.World));
        Assert.Contains("RenderTransform", e.Reason);
        Assert.True(Backend.Logged("refused in this Client world"));
        _native.Session = 6;  // the next client world refuses again: once more
        Step(2);
        Assert.Equal(2, seen.Count);
    }

    [Fact]
    public void An_unbind_whose_call_still_runs_keeps_the_callback_rooted()
    {
        var mod = NewMod("pinner");
        var r = As(mod, () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client));
        var binding = (EcsPrograms.Binding)r.Handle!;
        _native.CallRunning = true;  // a host unloading the mod from its server thread while a client step runs it
        binding.Dispose();
        Assert.True(binding.Root.IsAllocated);  // a running call holds it: never freed
        Assert.True(Backend.Logged("still running when it was unbound"));
        _native.CallRunning = false;
        Step(2);
        Assert.False(binding.Root.IsAllocated);
        Assert.Equal(2, _native.Unbinds);
        binding.Dispose();
        Assert.Equal(2, _native.Unbinds);
    }

    [Fact]
    public void The_mods_unload_unbinds_its_client_bindings()
    {
        var mod = NewMod("pinner");
        var r = As(mod, () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client));
        Assert.True(_native.Bound.ContainsKey("pin"));
        mod.Retire();
        Assert.False(_native.Bound.ContainsKey("pin"));
        Assert.False(((EcsPrograms.Binding)r.Handle!).Root.IsAllocated);
    }

    [Fact]
    public void Prediction_binds_its_own_reservation()
    {
        var r = As(NewMod("pinner"), () => EcsPrograms.TryBind("predicted", c => Assert.Equal(WorldKind.Prediction, c.World), WorldKind.Prediction));
        Assert.True(r.Ok, r.Reason);
        Assert.Equal((uint)WorldKind.Prediction, _native.BoundWorld["predicted"]);
        _native.Run("predicted");
        var wrong = As(NewMod("other"), () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Prediction));
        Assert.False(wrong.Ok);
        Assert.Contains("\"world\": \"prediction\"", wrong.Reason);
    }

    [Fact]
    public void Off_where_the_row_is_off_and_on_an_older_kiln_world()
    {
        Capabilities.Report(EcsPrograms.ClientWorldCapability, CapabilityState.Unavailable, CapabilityReason.WrongSide,
            "a client world runs only in the game client");
        var e = Assert.Throws<KilnUnavailableException>(() => As(NewMod("pinner"), () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client)));
        Assert.Equal(EcsPrograms.ClientWorldCapability, e.Capability);
        Capabilities.Report(EcsPrograms.ClientWorldCapability, CapabilityState.Available);
        _native.Table->V2.Prefix.Header.Size = 448;  // a dev.28 kiln.world: no client-world members
        Assert.Throws<KilnUnavailableException>(() => As(NewMod("pinner2"), () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client)));
        Assert.True(EcsPrograms.Available);  // server binds still work on it
    }

    [Fact]
    public void A_client_programs_write_is_queued_as_one_event_with_the_bytes_before_and_after()
    {
        var events = new List<(EntityId, string, byte[], byte[], string, WorldKind)>();
        EcsPrograms.ClientWriteWanted = () => true;
        EcsPrograms.ClientWritten = (e, c, b, a, w, k) => events.Add((e, c, b, a, w, k));
        try
        {
            var mod = NewMod("pinner");
            var r = As(mod, () => EcsPrograms.TryBind("pin", c => Assert.True(c.TryWriteOther(new EntityId(7), "Health", 19)), WorldKind.Client));
            *_native.Component = 12;
            _native.Run("pin");
            var (entity, component, before, after, writer, world) = Assert.Single(events);
            Assert.Equal((7u, "Health", 12, 19, "pinner/program:pin", WorldKind.Client),
                (entity.Value, component, BitConverter.ToInt32(before), BitConverter.ToInt32(after), writer, world));
            EcsPrograms.ClientWriteWanted = () => false;  // no listener: nothing is built
            _native.Run("pin");
            Assert.Single(events);
            r.Handle!.Dispose();
        }
        finally
        {
            EcsPrograms.ClientWriteWanted = null;
            EcsPrograms.ClientWritten = null;
        }
    }

    [Fact]
    public void A_server_try_bind_is_bind_without_the_exceptions()
    {
        var mod = NewMod("pinner");
        var r = As(mod, () => EcsPrograms.TryBind("server_only", _ => { }, WorldKind.Server));
        Assert.True(r.Ok, r.Reason);
        Assert.Equal(WorldKind.Server, r.World);
        var missing = As(mod, () => EcsPrograms.TryBind("nothing", _ => { }, WorldKind.Server));
        Assert.False(missing.Ok);
        Assert.Contains("not reserved for the server world", missing.Reason);
        Backend.TickThread = false;
        Assert.Throws<InvalidOperationException>(() => As(mod, () => EcsPrograms.TryBind("server_only", _ => { }, WorldKind.Server)));
        Backend.TickThread = true;
        r.Handle!.Dispose();
    }

    [Fact]
    public void Bind_is_unchanged_for_the_server_world()
    {
        var mod = NewMod("pinner");
        using var b = As(mod, () => EcsPrograms.Bind("server_only", _ => { }));
        Assert.Throws<InvalidOperationException>(() => As(mod, () => EcsPrograms.Bind("pin", _ => { })));  // a client-only reservation
    }

    [Fact]
    public void A_declaration_rebuilt_for_another_world_under_the_same_key_is_refused_until_a_restart()
    {
        var mod = NewMod("rebuilt");
        Directory.CreateDirectory(mod.Info.Directory);
        var file = Path.Combine(mod.Info.Directory, "kiln.programs.json");
        // what kiln.world reserved at boot (kiln.world.budget's program_reservation): a client program reading RenderTransform
        var boot = string.Concat(new[] { "tpl", "tplOptional", "read", "readOptional", "write", "writeOptional", "singletonsRead",
            "singletonsWrite", "otherTpl", "otherRead", "otherWrite", "with", "without", "anyOf", "eventsIn", "eventsOut" }
            .Select(n => n + "=" + (n == "read" ? "RenderTransform" : "") + "\n")) + "flags=00000000\ninterval=1\nworld=client\n";
        using var budget = new FakeBudget(Backend, boot);
        File.WriteAllText(file, """{"format":1,"programs":{"pin":{"world":"client","read":["RenderTransform"],"flags":[]}}}""");
        var same = As(mod, () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client));
        Assert.True(same.Ok, same.Reason);
        same.Handle!.Dispose();
        File.WriteAllText(file, """{"format":1,"programs":{"pin":{"world":"both","read":["RenderTransform"],"flags":[]}}}""");
        var world = As(mod, () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client));
        Assert.False(world.Ok);
        Assert.Null(world.Handle);
        Assert.Contains("its world differs", world.Reason);
        Assert.Contains("only after a restart", world.Reason);
        Assert.False(_native.Bound.ContainsKey("pin"));
        File.WriteAllText(file, """{"format":1,"programs":{"pin":{"world":"client","read":["CurrentTransform"],"flags":[]}}}""");
        Assert.Contains("its read differs", As(mod, () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client)).Reason);
        File.WriteAllText(file, """{"format":1,"programs":{"other":{"world":"client"}}}""");
        Assert.Contains("no longer in the mod's kiln.programs.json", As(mod, () => EcsPrograms.TryBind("pin", _ => { }, WorldKind.Client)).Reason);
        // a client declaration without "flags" is SYSTEM (boot_programs.h), with it the same text as kiln.world's
        Assert.EndsWith("flags=00000004\ninterval=1\nworld=client\n", EcsPrograms.Declaration(WriteFile("""{"format":1,"programs":{"a":{"world":"client"}}}"""), "a"));
        Assert.EndsWith("flags=00000014\ninterval=1\n", EcsPrograms.Declaration(WriteFile("""{"format":1,"programs":{"a":{}}}"""), "a"));
    }

    private string WriteFile(string text)
    {
        var f = Path.Combine(Root, Guid.NewGuid().ToString("N") + ".json");
        File.WriteAllText(f, text);
        return f;
    }

    // kiln.world.budget v1 with program_reservation (24 bytes): one snapshot for every (mod, key)
    private sealed class FakeBudget : IDisposable
    {
        private static FakeBudget? Current;
        private readonly FakeServiceRegistration _service;
        private readonly byte* _table;
        private readonly byte[] _snapshot;

        internal FakeBudget(FakeBackend backend, string snapshot)
        {
            Current = this;
            _snapshot = System.Text.Encoding.UTF8.GetBytes(snapshot);
            _table = (byte*)NativeMemory.AllocZeroed(24);
            *(ServiceHeader*)_table = new ServiceHeader { Size = 24, Version = 1 };
            *(delegate* unmanaged[Cdecl]<NativeStr, NativeStr, byte*, uint, uint*, int>*)(_table + 16) = &Reservation;
            _service = new(backend.Tables, "kiln.world.budget", (nint)_table);
        }

        public void Dispose()
        {
            _service.Dispose();
            if (ReferenceEquals(Current, this)) Current = null;
            NativeMemory.Free(_table);
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int Reservation(NativeStr mod, NativeStr key, byte* text, uint capacity, uint* size)
        {
            var s = Current!._snapshot;
            *size = (uint)s.Length;
            if (text == null) return 0;
            if (capacity < s.Length) return 5;
            for (var i = 0; i < s.Length; i++) text[i] = s[i];
            return 0;
        }
    }

    // ---- the fake kiln.world (480 bytes: v2, padding, the four client-world members) ----
    private sealed class FakeClientWorld : IDisposable
    {
        private static FakeClientWorld? Current;
        private readonly FakeServiceRegistration _service;
        internal readonly EcsClientTable* Table;
        internal readonly int* Component;
        internal readonly Dictionary<string, (nint Callback, nint User)> Bound = [];
        internal readonly Dictionary<string, uint> BoundWorld = [];
        internal readonly Dictionary<string, (int State, string Reason)> States = [];
        private readonly Dictionary<ulong, string> _handles = [];
        private readonly Dictionary<string, nint> _reasons = [];
        internal bool MainThread = true, CallRunning;
        internal ulong Session = 2;
        internal int Unbinds;
        private ulong _next = 0x1000;

        // (key, world) pairs kiln.world reserved at boot
        private static readonly HashSet<(string, uint)> Reserved =
            [("pin", 2), ("lacking", 2), ("predicted", 1), ("server_only", 0)];

        internal FakeClientWorld(FakeBackend backend)
        {
            Current = this;
            Table = (EcsClientTable*)NativeMemory.AllocZeroed((nuint)sizeof(EcsClientTable));
            Component = (int*)NativeMemory.AllocZeroed(4);
            Table->V2.Prefix.Header = new ServiceHeader { Size = (uint)sizeof(EcsClientTable), Version = 4 };
            Table->V2.Prefix.ProgramSlot = (void*)(delegate* unmanaged[Cdecl]<ulong, int, NativeStr, int>)&Slot;
            Table->V2.WriteOther = &WriteOther;
            Table->V2.Bind = &BindServer;
            Table->V2.Unregister = &UnregisterServer;
            Table->V2.Capabilities = &Caps;
            Table->GameWorldSession = &WorldSession;
            Table->ProgramWorldState = &WorldState;
            Table->BindProgramWorld = &BindWorld;
            Table->UnbindProgramWorld = &UnbindWorld;
            _service = new(backend.Tables, "kiln.world", (nint)Table);
        }

        internal void Run(string key)
        {
            if (!Bound.TryGetValue(key, out var b)) return;
            ulong* iterator = stackalloc ulong[4];
            iterator[0] = 11;
            iterator[1] = 22;
            ((delegate* unmanaged[Cdecl]<void*, ulong*, uint, void*, void>)b.Callback)((void*)1234, iterator, 0, (void*)b.User);
        }

        public void Dispose()
        {
            _service.Dispose();
            if (ReferenceEquals(Current, this)) Current = null;
            foreach (var p in _reasons.Values) Marshal.FreeHGlobal(p);
            NativeMemory.Free(Component);
            NativeMemory.Free(Table);
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static uint Caps() => 7;

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static ulong WorldSession(uint kind) => kind == 2 ? Current!.Session : 0;

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int Slot(ulong handle, int kind, NativeStr name) => (kind, name.ToString()) switch { (6, "Health") => 1, _ => -1 };

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void* WriteOther(void* ctx, ulong accessor, uint entity, uint* size)
        {
            *size = 4;
            return Current!.Component;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int BindServer(ulong owner, NativeStr mod, NativeStr key, delegate* unmanaged[Cdecl]<void*, ulong*, uint, void*, void> each,
            void* user, ulong* handle)
        {
            var self = Current!;
            var k = key.ToString();
            if (!Reserved.Contains((k, 0u))) return 2;
            if (self.Bound.ContainsKey(k)) return 1;
            self.Bound[k] = ((nint)each, (nint)user);
            self.BoundWorld[k] = 0;
            *handle = ++self._next;
            self._handles[*handle] = k;
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int UnregisterServer(ulong handle)
        {
            var self = Current!;
            if (self._handles.Remove(handle, out var k)) self.Bound.Remove(k);
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int BindWorld(ulong owner, NativeStr mod, NativeStr key, uint kind,
            delegate* unmanaged[Cdecl]<void*, ulong*, uint, void*, void> each, void* update, void* user, ulong* handle)
        {
            var self = Current!;
            *handle = 0;
            if (!self.MainThread) return 4;  // KILN_ERR_WRONG_THREAD, as kiln.world off the main thread
            var k = key.ToString();
            if (!Reserved.Contains((k, kind))) return 2;
            if (self.Bound.ContainsKey(k)) return 1;
            if (each == null || update != null) return 1;
            self.Bound[k] = ((nint)each, (nint)user);
            self.BoundWorld[k] = kind;
            *handle = (++self._next << 12) | ((ulong)kind << 8) | 1;
            self._handles[*handle] = k;
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int UnbindWorld(ulong handle)
        {
            var self = Current!;
            self.Unbinds++;
            if (!self._handles.TryGetValue(handle, out var k)) return 2;
            self.Bound.Remove(k);
            if (self.CallRunning) return 4;
            self._handles.Remove(handle);
            return 0;  // WRONG_THREAD: closed, a call still runs
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int WorldState(ulong handle, uint kind, int* state, NativeStr* reason)
        {
            var self = Current!;
            if (!self._handles.TryGetValue(handle, out var k)) return 2;
            var (st, why) = self.States.TryGetValue(k, out var s) ? s : (1, "");
            *state = st;
            if (!self._reasons.TryGetValue(why, out var p)) self._reasons[why] = p = Marshal.StringToHGlobalAnsi(why);
            *reason = new NativeStr { Ptr = (byte*)p, Len = (nuint)why.Length };
            return 0;
        }
    }
}
