// SPDX-License-Identifier: MIT
// The fake of kiln.world v4's game programs (native/services/world/game_programs.cpp and wraps.cpp) under the real
// Kiln.Systems: the programs a test declares in the harness's server world, wraps on them, and a dispatcher that runs each
// program once per archetype of the fake world's matching entities at the start of every step, through its stages in
// kiln.world's order. The table's functions are SystemsTable.cs; kiln.world's table itself is World/WorldTable.cs.
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln.TestKit;

internal sealed class SystemsFake : IKitFake
{
    public const string WrapsRow = "systems.wraps", TypedRow = "systems.wraps.typed";

    public string Service => WorldAbi.Service;

    public void Register()
    {
        // the table is World/WorldTable.cs's (kiln.world v4): SystemsTable.Fill put the game-program members in it
        FakeTables.Features([WrapsRow, TypedRow]);
        FakeTables.Offline("systems.define", "the test kit has no boot reservations (kiln.programs.json): Systems.Define needs a server");
        FakeTables.Offline("systems.wraps.client", "the test kit fakes a server world only");
        FakeTables.Offline("systems.threads", "the test kit runs every slice on its tick thread: no players mask, no workers");
    }

    public void Start(KilnHarness harness)
    {
        // Kiln.Systems keeps what it learned for the process: the table pointer, the programs' lists and contracts per
        // world session, the wraps' coalesced writes, stages a harness before left behind.
        foreach (var s in Systems.Live.Keys.ToList()) s.Forget(true);
        ProgramLists.Reset();
        WrapWrites.Reset();
        SystemsNative.Reset();
        KitFakes.State<SystemsState>(harness).Begin();
    }

    public void BeforeStep(KilnHarness harness, ulong step) => KitFakes.State<SystemsState>(harness).Step(harness, step);

    public void End(KilnHarness harness) => KitFakes.State<SystemsState>(harness).End();

    public void AddCoverage(List<CoverageRow> rows) =>
        rows.Add(new CoverageRow("Systems", CoverageKind.RecordingFake, [WrapsRow, TypedRow],
            "Kiln.Systems' own code on the kit's kiln.world v4. The programs are the ones the test declares (KilnHarness.Programs.Add, or Add<T> with a " +
            "Kiln.Game view: its lists and contract); each step runs each one once per archetype of the fake world's entities that have its read, write " +
            "and with components (of the types the world knows; template data is not matched), on the tick thread: " +
            "Befores high priority first, then the Replace or the declared update, then the Afters in reverse. Per-entity stages iterate the slice and read " +
            "and write the entities' components in the fake world; marks are recorded (KilnHarness.Programs.Marks), WhenArchetype decides by the entity's " +
            "component set, the contract is FNV-1a-32 of the lists. Not modelled: the game's workers and its scheduling order between programs, " +
            "singleton slots (empty), attribute formulas (Changed() validates and changes nothing), budgets and native faults, client and prediction " +
            "worlds, boot reservations (systems.define offline), native SET."));
}

/// <summary>The fake programs of one harness and the wraps on them. Tick thread for the dispatcher and every table call
/// that changes state; reads are locked.</summary>
internal sealed unsafe class SystemsState
{
    internal static readonly string[] ContractLists = ["tpl", "tpl_opt", "read", "read_opt", "write", "write_opt", "add", "remove",
        "sys_read", "sys_write", "other_tpl", "other_read", "other_write", "other_write_unser", "with", "without", "any_of",
        "all_write", "all_read", "events_in", "events_out"];

    internal sealed class Program
    {
        public required string Name;
        public int Index;
        public required FakeProgramLists Lists;
        public required string[][] Names;        // the 16 KILN_GAME_LIST_* lists
        public required string ContractText;
        public uint Contract;
        public Action<FakeProgramSlice>? Update;
        public nint NameUtf8;
        public NativeStr* ListStrs;                // 16 x 64 NativeStr
        public nint[] Pinned = [];
        public int Updates;
        public long Slices, Entities;
        public bool Runs => (Lists.Flags & 1) == 0;
    }

    internal sealed class Stage
    {
        public ulong Handle, Owner;
        public int Program, Kind, Priority, Order;
        public delegate* unmanaged[Cdecl]<SystemsNative.Slice*, void*, void> Fn;
        public void* User;
        public uint State = SystemsNative.Pending, Reason;
        public bool RemoveQueued, Gone;
        public long Calls, Entities;
        public string Detail = "";
    }

    internal sealed class Frame
    {
        public required SystemsState Owner;
        public required Program Program;
        public required SystemsNative.Slice* Slice;
        public required uint[] Ids;
        public ulong* Iter;
        public int Row = -1;
        public bool OriginalCalled;
        public readonly HashSet<(uint, uint)> Marked = [];
        public ulong Step;
    }

    [ThreadStatic] internal static Frame? Current;

    private static long _sessions;
    private readonly object _lock = new();
    private readonly List<Program> _programs = [];
    private readonly Dictionary<ulong, Stage> _stages = [];
    private readonly List<FakeProgramMark> _marks = [];
    private readonly Dictionary<string, (nuint Layout, uint[] Components)> _archetypes = new(StringComparer.Ordinal);
    private readonly Dictionary<nuint, uint[]> _layouts = [];
    private ulong _next = 1;
    private int _order;

    public ulong Session { get; private set; }

    public void Begin() => Session = (ulong)Interlocked.Increment(ref _sessions);

    public void End()
    {
        lock (_lock)
        {
            foreach (var p in _programs) Free(p);
            _programs.Clear();
            _stages.Clear();
            Session = 0;
        }
    }

    private static void Free(Program p)
    {
        foreach (var x in p.Pinned) NativeMemory.Free((void*)x);
        p.Pinned = [];
        if (p.ListStrs != null) NativeMemory.Free(p.ListStrs);
        p.ListStrs = null;
    }

    // ---- the test's side ----

    public void Add(string name, FakeProgramLists lists, string contractText, Action<FakeProgramSlice>? update)
    {
        var names = new string[16][];
        IReadOnlyList<string>[] from = [lists.Template, lists.TemplateOptional, lists.Read, lists.ReadOptional, lists.Write, lists.WriteOptional,
            lists.SingletonsRead, lists.SingletonsWrite, lists.OtherTemplate, lists.OtherRead, lists.OtherWrite, lists.With, lists.Without,
            lists.AnyOf, lists.EventsIn, lists.EventsOut];
        for (var l = 0; l < 16; l++)
        {
            names[l] = [.. from[l] ?? []];
            if (names[l].Any(string.IsNullOrEmpty)) throw new ArgumentException($"{name}: a list holds an empty name", nameof(lists));
            if (names[l].Length > 64) throw new ArgumentException($"{name}: a list holds more than 64 names", nameof(lists));
        }
        lock (_lock)
        {
            if (_programs.Any(p => p.Name == name)) throw new ArgumentException($"{name} is declared already", nameof(name));
            var p = new Program { Name = name, Index = _programs.Count, Lists = lists, Names = names, ContractText = contractText, Update = update };
            p.Contract = Fnv(contractText);
            var pinned = new List<nint>();
            NativeStr Pin(string s)
            {
                var b = Encoding.UTF8.GetBytes(s);
                var m = (byte*)NativeMemory.Alloc((nuint)Math.Max(1, b.Length));
                b.CopyTo(new Span<byte>(m, b.Length));
                pinned.Add((nint)m);
                return new NativeStr { Ptr = m, Len = (nuint)b.Length };
            }
            p.NameUtf8 = (nint)Pin(name).Ptr;
            p.ListStrs = (NativeStr*)NativeMemory.AllocZeroed((nuint)(16 * 64 * sizeof(NativeStr)));
            for (var l = 0; l < 16; l++)
                for (var i = 0; i < names[l].Length; i++) p.ListStrs[l * 64 + i] = Pin(names[l][i]);
            p.Pinned = [.. pinned];
            _programs.Add(p);
        }
    }

    public IReadOnlyList<string> Names()
    {
        lock (_lock) return [.. _programs.Select(p => p.Name)];
    }

    public int Updates(string name)
    {
        lock (_lock) return _programs.FirstOrDefault(p => p.Name == name)?.Updates ?? 0;
    }

    public IReadOnlyList<FakeProgramMark> MarksNow()
    {
        lock (_lock) return [.. _marks];
    }

    // ---- the contract (chapter 06a 9.7) ----

    internal static uint Fnv(string text)
    {
        var h = 0x811c9dc5u;
        foreach (var b in Encoding.UTF8.GetBytes(text)) h = (h ^ b) * 0x01000193u;
        return h;
    }

    internal static string ContractText(FakeProgramLists l, ulong sysReadOpt, ulong sysWriteOpt, ulong sysWriteParallel)
    {
        IReadOnlyList<string>[] lists = [l.Template, l.TemplateOptional, l.Read, l.ReadOptional, l.Write, l.WriteOptional, [], [],
            l.SingletonsRead, l.SingletonsWrite, l.OtherTemplate, l.OtherRead, l.OtherWrite, [], l.With, l.Without, l.AnyOf, [], [],
            l.EventsIn, l.EventsOut];
        var sb = new StringBuilder();
        for (var i = 0; i < ContractLists.Length; i++) sb.Append(ContractLists[i]).Append('=').AppendJoin(',', lists[i] ?? []).Append('\n');
        sb.Append($"sys_read_opt={sysReadOpt:x16}\nsys_write_opt={sysWriteOpt:x16}\nsys_write_parallel={sysWriteParallel:x16}\n");
        sb.Append($"flags={l.Flags:x8}\ninterval={l.Interval}\n");
        return sb.ToString();
    }

    // A view's Lists (the contract text without its empty lists): the lists, and the whole text again.
    internal static (FakeProgramLists Lists, string Text) FromView(string viewLists)
    {
        var given = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var line in viewLists.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            var eq = line.IndexOf('=');
            if (eq > 0) given[line[..eq]] = line[(eq + 1)..];
        }
        string[] Of(string list) => given.TryGetValue(list, out var v) && v.Length > 0 ? v.Split(',') : [];
        ulong Hex(string key) => given.TryGetValue(key, out var v) ? Convert.ToUInt64(v, 16) : 0;
        var lists = new FakeProgramLists
        {
            Template = Of("tpl"), TemplateOptional = Of("tpl_opt"), Read = Of("read"), ReadOptional = Of("read_opt"), Write = Of("write"),
            WriteOptional = Of("write_opt"), SingletonsRead = Of("sys_read"), SingletonsWrite = Of("sys_write"), OtherTemplate = Of("other_tpl"),
            OtherRead = Of("other_read"), OtherWrite = Of("other_write"), With = Of("with"), Without = Of("without"), AnyOf = Of("any_of"),
            EventsIn = Of("events_in"), EventsOut = Of("events_out"),
            Flags = (uint)Hex("flags"), Interval = given.TryGetValue("interval", out var iv) ? uint.Parse(iv) : 1,
        };
        var sb = new StringBuilder();
        foreach (var l in ContractLists) sb.Append(l).Append('=').Append(given.GetValueOrDefault(l, "")).Append('\n');
        sb.Append($"sys_read_opt={Hex("sys_read_opt"):x16}\nsys_write_opt={Hex("sys_write_opt"):x16}\nsys_write_parallel={Hex("sys_write_parallel"):x16}\n");
        sb.Append($"flags={lists.Flags:x8}\ninterval={lists.Interval}\n");
        return (lists, sb.ToString());
    }

    // ---- what the table answers ----

    internal Program? At(int index)
    {
        lock (_lock) return index >= 0 && index < _programs.Count ? _programs[index] : null;
    }

    internal int Find(string name)
    {
        lock (_lock) return _programs.FindIndex(p => p.Name == name);
    }

    internal int Count
    {
        get { lock (_lock) return _programs.Count; }
    }

    internal Stage? StageOf(ulong handle)
    {
        lock (_lock) return _stages.GetValueOrDefault(handle);
    }

    internal (int Active, int Pending, int Wraps) Counts(int program = -1)
    {
        lock (_lock)
        {
            var s = _stages.Values.Where(x => !x.Gone && (program < 0 || x.Program == program)).ToList();
            return (s.Count(x => x.State == SystemsNative.Active), s.Count(x => x.State == SystemsNative.Pending), s.Count);
        }
    }

    internal ulong Wrap(ulong owner, Program p, int kind, int priority, delegate* unmanaged[Cdecl]<SystemsNative.Slice*, void*, void> fn, void* user)
    {
        lock (_lock)
        {
            var st = new Stage { Handle = _next++, Owner = owner, Program = p.Index, Kind = kind, Priority = priority, Order = _order++, Fn = fn, User = user };
            if (kind == SystemsNative.Replace && _stages.Values.FirstOrDefault(o => o.Program == p.Index && o.Kind == SystemsNative.Replace && !o.Gone &&
                    o.State is SystemsNative.Active or SystemsNative.Pending) is { } holder)
            {
                st.State = SystemsNative.Refused;
                st.Reason = SystemsNative.ReasonReplaceTaken;
                st.Gone = true;
                st.Detail = $"owner {holder.Owner} replaces {p.Name} already (wrap {holder.Handle})";
            }
            _stages[st.Handle] = st;
            return st.Handle;
        }
    }

    internal bool Unwrap(ulong handle)
    {
        lock (_lock)
        {
            if (!_stages.TryGetValue(handle, out var st)) return false;
            if (st.State == SystemsNative.Pending) Remove(st);  // never applied
            else if (!st.Gone) st.RemoveQueued = true;        // out at the next step's start
            return true;
        }
    }

    internal void Release(ulong owner)
    {
        lock (_lock)
            foreach (var st in _stages.Values.Where(s => s.Owner == owner && !s.Gone)) Remove(st, SystemsNative.ReasonOwnerGone);
    }

    private static void Remove(Stage st, uint reason = SystemsNative.ReasonUnwrapped)
    {
        st.State = SystemsNative.Removed;
        st.Reason = reason;
        st.Gone = true;
        st.RemoveQueued = false;
    }

    internal void Mark(Frame f, uint entity, uint hash, string component)
    {
        if (!f.Marked.Add((entity, hash))) return;
        lock (_lock) _marks.Add(new FakeProgramMark(f.Program.Name, new EntityId(entity), component, f.Step));
    }

    internal nuint LayoutOf(uint entity)
    {
        var h = KitBackend.Instance.Harness?.EntitiesFake;
        if (h?.ComponentsOf(entity) is not { } comps) return 0;
        var key = string.Join(',', comps);
        lock (_lock)
        {
            if (_archetypes.TryGetValue(key, out var a)) return a.Layout;
            var layout = (nuint)(0x10000 + 0x10 * _archetypes.Count);
            _archetypes[key] = (layout, comps);
            _layouts[layout] = comps;
            return layout;
        }
    }

    internal uint[]? ComponentsOfLayout(nuint layout)
    {
        lock (_lock) return _layouts.GetValueOrDefault(layout);
    }

    // ---- the dispatcher: the world's step ----

    public void Step(KilnHarness harness, ulong step)
    {
        List<Program> programs;
        lock (_lock)
        {
            foreach (var st in _stages.Values)
            {
                if (st.RemoveQueued) Remove(st);
                else if (st.State == SystemsNative.Pending && !st.Gone) st.State = SystemsNative.Active;
            }
            _marks.RemoveAll(m => m.Step + 1000 < step);
            programs = [.. _programs];
        }
        if (programs.Count == 0 || harness.EntitiesFake is not { } world || !FakeTables.OnTickThread) return;
        var entities = world.Entities;
        foreach (var p in programs)
        {
            if (!p.Runs) continue;
            foreach (var (_, ids) in Slices(world, p, entities)) RunSlice(p, ids, step);
        }
    }

    // One slice per archetype (component set) of the entities that match the program's lists, in entity order: every read,
    // write and with component of a type the fake world knows (template data and the game's Dynamic* state, which the
    // type dump has no layout for, are not on the fake's entities), none of without, one of any_of.
    private static IEnumerable<(string Key, uint[] Ids)> Slices(EntitiesFake world, Program p, IReadOnlyList<uint> entities)
    {
        uint[] H(int list) => [.. p.Names[list].Select(WorldAbi.Hash)];
        var need = H(SystemsNative.ListRead).Concat(H(SystemsNative.ListWrite)).Concat(H(11)).Where(world.KnowsComponent).ToArray();
        var without = H(12);
        var anyOf = H(13);
        var groups = new Dictionary<string, List<uint>>(StringComparer.Ordinal);
        var order = new List<string>();
        foreach (var e in entities)
        {
            if (world.ComponentsOf(e) is not { } comps) continue;
            var set = new HashSet<uint>(comps);
            if (!need.All(set.Contains) || without.Any(set.Contains) || (anyOf.Length > 0 && !anyOf.Any(set.Contains))) continue;
            var key = string.Join(',', comps);
            if (!groups.TryGetValue(key, out var list)) { groups[key] = list = []; order.Add(key); }
            list.Add(e);
        }
        foreach (var k in order) yield return (k, [.. groups[k]]);
    }

    private void RunSlice(Program p, uint[] ids, ulong step)
    {
        List<Stage> befores, afters;
        Stage? replace;
        lock (_lock)
        {
            var live = _stages.Values.Where(s => s.Program == p.Index && !s.Gone && s.State == SystemsNative.Active).ToList();
            befores = [.. live.Where(s => s.Kind == SystemsNative.Before).OrderByDescending(s => s.Priority).ThenBy(s => s.Order)];
            afters = [.. live.Where(s => s.Kind == SystemsNative.After).OrderBy(s => s.Priority).ThenByDescending(s => s.Order)];
            replace = live.FirstOrDefault(s => s.Kind == SystemsNative.Replace);
            p.Slices++;
            p.Entities += ids.Length;
        }
        var slice = (SystemsNative.Slice*)NativeMemory.AllocZeroed((nuint)sizeof(SystemsNative.Slice));
        var idMem = (uint*)NativeMemory.AllocZeroed((nuint)(Math.Max(1, ids.Length) * sizeof(uint)));
        try
        {
            for (var i = 0; i < ids.Length; i++) idMem[i] = ids[i];
            *slice = new SystemsNative.Slice
            {
                Size = (uint)sizeof(SystemsNative.Slice), WorldKind = 0, ProgramCtx = slice, Count = (uint)ids.Length, ThreadIsWorld = 1,
                EntityIds = idMem, EntityStride = 4, ProgramIndex = p.Index, Step = step,
                Program = new NativeStr { Ptr = (byte*)p.NameUtf8, Len = (nuint)Encoding.UTF8.GetByteCount(p.Name) },
            };
            var frame = new Frame { Owner = this, Program = p, Slice = slice, Ids = ids, Step = step };
            slice->Internal = (void*)GCHandle.ToIntPtr(GCHandle.Alloc(frame));
            try
            {
                foreach (var b in befores) Call(frame, b);
                if (replace is not null) Call(frame, replace);
                else RunUpdate(frame);
                foreach (var a in afters) Call(frame, a);
            }
            finally
            {
                GCHandle.FromIntPtr((nint)slice->Internal).Free();
            }
        }
        finally
        {
            NativeMemory.Free(idMem);
            NativeMemory.Free(slice);
        }
    }

    private void Call(Frame f, Stage st)
    {
        if (st.Gone) return;
        f.Slice->Stage = st.Kind;
        f.OriginalCalled = false;
        f.Marked.Clear();
        var outer = Current;
        Current = f;
        try { st.Fn(f.Slice, st.User); }
        finally { Current = outer; }
        lock (_lock)
        {
            st.Calls++;
            st.Entities += f.Ids.Length;
        }
    }

    internal void RunUpdate(Frame f)
    {
        f.OriginalCalled = true;
        lock (_lock) f.Program.Updates++;
        f.Program.Update?.Invoke(new FakeProgramSlice(f.Program.Name, [.. f.Ids.Select(i => new EntityId(i))], f.Step));
    }

    internal string ListName(Program p, int list, int i) => list is >= 0 and < 16 && i >= 0 && i < p.Names[list].Length ? p.Names[list][i] : "";
}
