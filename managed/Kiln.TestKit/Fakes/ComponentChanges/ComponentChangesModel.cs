// SPDX-License-Identifier: MIT
// The kit's kiln.component_changes, rule for rule the native service's core (native/services/decisions/watch_core.h):
// the same budgets and refusals (32 watches, 4096 present pairs, 128 roots a pair; a watch that does not fit is not made,
// one that grows past them is suspended whole), the same link walk (InventorySetup links, Virtual ones followed: depth
// 3, 32 inventories, 64 setups; a cycle, a missing setup, too many: refused or suspended), the same origin rule over the
// tokens (one source and a chain from the baseline to the bytes after: Kiln; tokens otherwise: Mixed; none: Unknown,
// never Game), the same two-frame queue and batches, and a world that ends every watch. The world is the harness's fake
// world; tokens come from the kit's tracked writes (World.Write, World.Update ...) with the writing mod's id; bytes a
// test sets itself (FakeWorld.SetBytes) carry none, as a game program's write. Kept in step with watch_core.h by
// Kiln.World.Tests/ComponentChangesTests.cs, whose cases are the native host test's.
using N = Kiln.ComponentChangesNative;

namespace Kiln.TestKit;

internal sealed unsafe class ComponentChangesModel
{
    private const int MaxWatches = 32, MaxPairs = 4096, MaxRoots = 128, MaxTokens = 32, MaxBytes = 256, MaxFrames = 2;
    private const int SetupSize = 84, InventorySize = 96, MaxSetups = 64, MaxLeaves = 32, MaxDepth = 3, Graveyard = 64;
    private const ulong LinkedSerialBit = 1ul << 63;
    private static readonly string[] Names = N.Names;

    private sealed class Watch
    {
        public ulong Id, Owner, LastComplete, Epoch;
        public uint Scope, Components, Expansion, Status, Pairs;
        public uint[] Roots = [];
        public DecisionsNative.EntityKey[] RootKeys = [];
        public string Reason = "";
        public bool Fresh = true;
        public List<(ulong Id, bool Present, List<uint> Roots)> Reserved = [];
    }

    private sealed class Token
    {
        public ulong Id;
        public string Source = "";
        public bool Valid;
        public byte[] Before = [], After = [];
    }

    private sealed class Pair
    {
        public DecisionsNative.EntityKey Key;
        public uint Component;
        public bool Present, Carried, Truncated, Typed;
        public byte[] Base = [];
        public ulong Since;
        public int Cur, Max;
        public List<ulong> Watches = [], Prev = [];
        public List<uint> Roots = [], PrevRoots = [];
        public List<Token> Tokens = [];
    }

    private sealed class Want
    {
        public DecisionsNative.EntityKey Key;
        public uint Component;
        public bool Present;
        public byte[]? Bytes;
        public List<uint> Roots = [];
    }

    private sealed class Change
    {
        public uint Component, Origin, Flags, Attribute;
        public ulong Epoch, Step, Since;
        public DecisionsNative.EntityKey Key;
        public byte[] Before = [], After = [];
        public List<uint> Roots = [];
        public List<ulong> Watches = [], Tokens = [];
        public string Source = "";
        public int ValueBefore, ValueAfter, MaxBefore, MaxAfter;
    }

    private readonly object _lock = new();
    private KilnHarness? _harness;
    private readonly List<Watch> _watches = [];
    private readonly LinkedList<(ulong Id, ulong Owner, uint Status, string Reason, ulong LastComplete)> _graves = new();
    private Dictionary<ulong, Pair> _pairs = [];
    private readonly Dictionary<uint, (ulong Serial, uint Generation, ulong Seen)> _linked = [];
    private readonly List<List<Change>> _frames = [];
    private List<Change> _pending = [];
    private List<Change> _batch = [];
    private int _batchFrames;
    private ulong _batchId, _nextBatch, _lastAcked, _consumer, _nextId, _nextToken, _nextLinked, _epoch, _step, _lastComplete;
    private bool _stepped;

    /// <summary>Typed current and maximum values a test gives (kiln.combat's attributes and kiln.items' fuel are not
    /// modelled offline): (entity, component index) to (current, maximum).</summary>
    public readonly Dictionary<(uint, uint), (int Current, int Maximum)> Typed = [];
    public ulong QueueOverflows, FramesDropped, Changes, TokensRecorded;

    // ---- the harness's life ----
    public void Start(KilnHarness harness)
    {
        lock (_lock)
        {
            _harness = harness;
            _epoch = Clock().WorldEpoch;
        }
    }

    /// <summary>The start of step <paramref name="step"/>: the compare of the step that ran (window_end), then the
    /// refresh of this one (window_begin). The kit has no window calls; its world's writes of the last step ran before.</summary>
    public void BeforeStep(ulong step)
    {
        lock (_lock)
        {
            var epoch = Clock().WorldEpoch;
            if (epoch != _epoch)
            {
                WorldStopped("the server world stopped; create a new watch in the next world");
                _epoch = epoch;
            }
            if (_stepped) End(_step);
            _stepped = true;
            _step = step;
            Begin(step);
        }
    }

    public void End()
    {
        lock (_lock) WorldStopped("the server world stopped");
    }

    private DecisionsNative.WindowState Clock()
    {
        DecisionsNative.WindowState s = default;
        if (_harness is { } h) DecisionsFake.ModelOf(h).Clock(&s);
        return s;
    }

    private bool WorldSafe() => FakeTables.OnTickThread && Clock().Phase is DecisionsNative.PhasePre or DecisionsNative.PhasePreparing
        or DecisionsNative.PhaseAfterJobs or DecisionsNative.PhasePost;

    // ---- the world, as the service reads it ----
    private bool Read(uint entity, uint component, out byte[] bytes)
    {
        bytes = [];
        if (_harness?.EntitiesFake is not { } w || component is < 1 or > 6) return false;
        var b = w.Bytes(entity, Names[component]);
        if (b is null || b.Length is 0 or > MaxBytes) return false;
        bytes = b;
        return true;
    }

    private byte[]? Setup(uint entity)
    {
        var b = _harness?.EntitiesFake?.Bytes(entity, "InventorySetup");
        return b is { Length: >= SetupSize } ? b : null;
    }

    private bool Key(uint entity, out DecisionsNative.EntityKey key, out bool exists)
    {
        key = default;
        exists = _harness?.EntitiesFake?.Exists(entity) == true;
        if (_harness is not { } h || !exists) return false;
        DecisionsNative.EntityKey k;
        if (DecisionsFake.ModelOf(h).ResolveEntity(h, entity, &k, null, null) != FakeTables.Ok) return false;
        key = k;
        return true;
    }

    private List<uint> Players() =>
        _harness?.PlayersFake is { } p ? p.Snapshot(PlayersAbi.ListInGame).Where(x => x.Entity != 0).Select(x => x.Entity).ToList() : [];

    private bool TypedOf(uint entity, uint component, out int cur, out int max)
    {
        cur = max = 0;
        if (component > 4 || !Typed.TryGetValue((entity, component), out var v)) return false;
        (cur, max) = v;
        return true;
    }

    private static uint Bit(uint c) => 1u << (int)(c - 1);
    private static ulong PairId(uint entity, uint component) => ((ulong)entity << 8) | component;
    private static bool SameLife(in DecisionsNative.EntityKey a, in DecisionsNative.EntityKey b) =>
        a.Entity == b.Entity && a.Flags == b.Flags && a.SpawnTime == b.SpawnTime && a.LifeSerial == b.LifeSerial &&
        a.PlayerHandle == b.PlayerHandle && a.JoinedStep == b.JoinedStep;

    // ---- the table ----
    public int Attach(ulong d)
    {
        lock (_lock)
        {
            if (d == 0) return FakeTables.InvalidArgument;
            if (!WorldSafe()) return FakeTables.WrongThread;
            if (_consumer != 0 && _consumer != d) return FakeTables.Unavailable;
            _consumer = d;
            return FakeTables.Ok;
        }
    }

    public int Detach(ulong d)
    {
        lock (_lock)
        {
            if (_consumer != d) return FakeTables.Unavailable;
            _consumer = 0;
            _batch = [];
            _batchId = 0;
            return FakeTables.Ok;
        }
    }

    public int Create(ulong owner, N.WatchRequest* rq, N.WatchStatus* st)
    {
        if (rq == null || st == null) return FakeTables.InvalidArgument;
        Blank(st);
        if (owner == 0 || rq->Size < (uint)sizeof(N.WatchRequest)) return FakeTables.InvalidArgument;
        if (rq->Components == 0 || (rq->Components & ~63u) != 0 || rq->Expansion > 1 || rq->Scope > 1) return FakeTables.InvalidArgument;
        var roots = new uint[rq->Scope == 0 ? rq->EntityCount : 0];
        if (rq->Scope == 0)
        {
            if (rq->EntityCount is 0 or > MaxRoots || rq->Entities == null) return FakeTables.InvalidArgument;
            for (var i = 0; i < roots.Length; i++) roots[i] = rq->Entities[i];
            if (roots.Contains(0u) || roots.Distinct().Count() != roots.Length) return FakeTables.InvalidArgument;
        }
        else if (rq->EntityCount != 0) return FakeTables.InvalidArgument;
        lock (_lock)
        {
            if (_harness?.EntitiesFake is not { Started: true }) return Refuse(st, 2, "no server world runs in this process (a client that joined a game has none)");
            if (!WorldSafe()) return FakeTables.WrongThread;
            if (_watches.Count >= MaxWatches) return Refuse(st, 1, "32 watches exist already (dispose one first)");
            var w = new Watch { Owner = owner, Scope = rq->Scope, Components = rq->Components, Expansion = rq->Expansion, Roots = roots, Epoch = _epoch };
            var keys = new List<DecisionsNative.EntityKey>();
            foreach (var e in roots)
            {
                if (!Key(e, out var k, out var exists))
                    return Refuse(st, 2, exists ? $"entity {e} has no identity Kiln can follow (no SpawnTime, and not a player in the game)" : $"entity {e} does not exist");
                keys.Add(k);
            }
            w.RootKeys = [.. keys];
            if (!Expand(w, _step, out var plan, out var why, out var whyStatus)) return Refuse(st, whyStatus, why);
            var present = plan.Values.Count(x => x.Present);
            if (!Fits(plan, present, out why))
            {
                st->Pairs = (uint)present;
                return Refuse(st, 1, why);
            }
            w.Id = ++_nextId;
            w.Status = 0;
            w.Pairs = (uint)present;
            w.Reserved = Reservation(plan);
            _watches.Add(w);
            // the kit has no window_begin after the Pre: the baseline is read now, as the native one is before the sim
            Begin(_step, onlyNew: true);
            Fill(w, st);
            return FakeTables.Ok;
        }
    }

    public int Resume(ulong owner, ulong id, N.WatchStatus* st)
    {
        if (st == null) return FakeTables.InvalidArgument;
        Blank(st);
        lock (_lock)
        {
            if (!WorldSafe()) return FakeTables.WrongThread;
            var w = _watches.FirstOrDefault(x => x.Id == id);
            if (w is null || w.Owner != owner)
            {
                var g = _graves.FirstOrDefault(x => x.Id == id);
                if (g.Id == id && g.Owner == owner) return FillGrave(g, st);
                return FakeTables.NotFound;
            }
            if (w.Status == 0) return Done(w, st);
            for (var i = 0; i < w.Roots.Length; i++)
                if (!Key(w.Roots[i], out var k, out var exists) || !SameLife(k, w.RootKeys[i]))
                {
                    Suspend(w, 2, $"entity {w.Roots[i]}" + (exists ? " is another entity now (or has no identity)" : " is gone"));
                    return Done(w, st);
                }
            var was = w.Status;
            w.Status = 0;
            if (!Expand(w, _step, out var plan, out var why, out var whyStatus))
            {
                w.Status = was;
                Suspend(w, whyStatus, why);
                return Done(w, st);
            }
            var present = plan.Values.Count(x => x.Present);
            if (!Fits(plan, present, out why, w.Id))
            {
                w.Status = was;
                Suspend(w, 1, why);
                return Done(w, st);
            }
            w.Pairs = (uint)present;
            w.Reason = "";
            w.Fresh = true;
            w.Reserved = Reservation(plan);
            Begin(_step, onlyNew: true);
            return Done(w, st);
        }
    }

    public int Status(ulong id, N.WatchStatus* st)
    {
        if (st == null) return FakeTables.InvalidArgument;
        Blank(st);
        lock (_lock)
        {
            if (_watches.FirstOrDefault(x => x.Id == id) is { } w) return Done(w, st);
            var g = _graves.FirstOrDefault(x => x.Id == id);
            return g.Id == id ? FillGrave(g, st) : FakeTables.NotFound;
        }
    }

    public int Remove(ulong owner, ulong id)
    {
        lock (_lock)
        {
            var w = _watches.FirstOrDefault(x => x.Id == id);
            if (w is null || w.Owner != owner) return FakeTables.NotFound;
            EndWatch(w, "disposed");
            return FakeTables.Ok;
        }
    }

    public int ReleaseOwner(ulong owner)
    {
        lock (_lock)
        {
            foreach (var w in _watches.Where(x => x.Owner == owner).ToList()) EndWatch(w, "its owner went away");
            return FakeTables.Ok;
        }
    }

    public int BeginEvents(ulong d, ulong* batch, uint* records)
    {
        if (batch == null || records == null) return FakeTables.InvalidArgument;
        lock (_lock)
        {
            if (d == 0 || d != _consumer) return FakeTables.Unavailable;
            if (!WorldSafe()) return FakeTables.WrongThread;
            if (_batchId == 0)
            {
                if (_frames.Count == 0)
                {
                    *batch = 0;
                    *records = 0;
                    return FakeTables.Ok;
                }
                _batchFrames = _frames.Count;
                _batch = _frames.SelectMany(f => f).ToList();
                _frames.Clear();
                _batchId = ++_nextBatch;
            }
            *batch = _batchId;
            *records = (uint)_batch.Count;
            return FakeTables.Ok;
        }
    }

    public int ReadEvents(ulong d, ulong batch, N.Change* out_, uint capacity, uint stride, uint* required)
    {
        lock (_lock)
        {
            if (d == 0 || d != _consumer) return FakeTables.Unavailable;
            if (!WorldSafe()) return FakeTables.WrongThread;
            if (batch == 0 || batch != _batchId) return FakeTables.NotFound;
            if (required != null) *required = (uint)_batch.Count;
            if (stride < (uint)sizeof(N.Change)) return FakeTables.InvalidArgument;
            if (out_ == null || capacity < _batch.Count) return FakeTables.BufferTooSmall;
            var at = (byte*)out_;
            foreach (var c in _batch)
            {
                ToWire(c, (N.Change*)at);
                at += stride;
            }
            return FakeTables.Ok;
        }
    }

    public int AckEvents(ulong d, ulong batch)
    {
        lock (_lock)
        {
            if (d == 0 || d != _consumer) return FakeTables.Unavailable;
            if (!WorldSafe()) return FakeTables.WrongThread;
            if (batch != 0 && batch == _lastAcked) return FakeTables.Ok;
            if (batch == 0 || batch != _batchId) return FakeTables.NotFound;
            _batch = [];
            _batchFrames = 0;
            _lastAcked = _batchId;
            _batchId = 0;
            return FakeTables.Ok;
        }
    }

    public int RecordWrite(ulong owner, N.KnownWrite* w)
    {
        if (w == null) return FakeTables.InvalidArgument;
        lock (_lock)
        {
            if (_watches.Count == 0) return FakeTables.Ok;
            if (owner == 0 || w->Size < (uint)sizeof(N.KnownWrite) || w->Component is < 1 or > 6 || w->Before == null || w->After == null ||
                w->Bytes is 0 or > MaxBytes || w->Entity.Entity == 0)
                return FakeTables.InvalidArgument;
            if (w->WorldEpoch != 0 && w->WorldEpoch != _epoch) return FakeTables.NotFound;
            if (!_pairs.TryGetValue(PairId(w->Entity.Entity, w->Component), out var pr)) return FakeTables.Ok;
            if (w->Entity.LifeSerial != 0 && w->Entity.LifeSerial != pr.Key.LifeSerial) return FakeTables.NotFound;
            TokensRecorded++;
            if (pr.Tokens.Count >= MaxTokens)
            {
                pr.Truncated = true;
                return FakeTables.Ok;
            }
            var n = (int)w->SourceMod.Len;
            var source = n is >= 1 and <= 127 && w->SourceMod.Ptr != null ? Utf8(w->SourceMod.Ptr, n) : null;
            pr.Tokens.Add(new Token
            {
                Id = w->OperationToken != 0 ? w->OperationToken : ++_nextToken,
                Source = source ?? "",
                Valid = source is not null,
                Before = new ReadOnlySpan<byte>(w->Before, (int)w->Bytes).ToArray(),
                After = new ReadOnlySpan<byte>(w->After, (int)w->Bytes).ToArray(),
            });
            return FakeTables.Ok;
        }
    }

    private static string? Utf8(byte* p, int n)
    {
        try
        {
            var s = new System.Text.UTF8Encoding(false, true).GetString(p, n);
            return s.Contains('\0') ? null : s;
        }
        catch (ArgumentException)
        {
            return null;
        }
    }

    /// <summary>The kit's tracked write changed a component: the same token kiln.world's write program gives the native
    /// service (the whole component before and after, the mod's id).</summary>
    public void NoteWrite(uint entity, string component, byte[] before, byte[] after, string mod)
    {
        var index = N.IndexOf(component);
        if (index == 0 || before.Length != after.Length || before.Length is 0 or > MaxBytes) return;
        var source = string.IsNullOrEmpty(mod) ? "kiln.world" : mod;
        var id = System.Text.Encoding.UTF8.GetBytes(source);
        fixed (byte* b = before, a = after, s = id)
        {
            var w = new N.KnownWrite { Size = (uint)sizeof(N.KnownWrite), Component = index, Before = b, After = a, Bytes = (uint)before.Length };
            w.Entity.Entity = entity;
            w.SourceMod = new NativeStr { Ptr = s, Len = (nuint)id.Length };
            RecordWrite(1, &w);
        }
    }

    // ---- the rules (watch_core.h) ----
    private bool Expand(Watch w, ulong step, out Dictionary<ulong, Want> plan, out string why, out uint whyStatus)
    {
        plan = [];
        why = "";
        whyStatus = 0;
        var roots = new List<(uint Id, DecisionsNative.EntityKey Key)>();
        if (w.Scope == 0) for (var i = 0; i < w.Roots.Length; i++) roots.Add((w.Roots[i], w.RootKeys[i]));
        else
            foreach (var p in Players())
                if (Key(p, out var k, out _)) roots.Add((p, k));
        var rootMask = w.Expansion == 1 ? w.Components & ~(Bit(5) | Bit(4)) : w.Components;
        foreach (var (root, key) in roots)
        {
            for (uint c = 1; c <= 6; c++)
                if ((rootMask & Bit(c)) != 0) Add(plan, key, c, root);
            if (w.Expansion != 1) continue;
            bool inv = (w.Components & Bit(5)) != 0, fuel = (w.Components & Bit(4)) != 0;
            if (!inv && !fuel) continue;
            if (!WalkTree(root, out var leaves, out why))
            {
                why = $"entity {root}: {why}";
                whyStatus = 2;
                return false;
            }
            foreach (var leaf in leaves)
            {
                var lk = Linked(leaf, 0, step);
                if (inv) Add(plan, lk, 5, root);
                if (!fuel || !Read(leaf, 5, out var stacks) || stacks.Length < InventorySize) continue;
                for (var k = 0; k < 8; k++)
                {
                    var item = BitConverter.ToUInt32(stacks, 12 * k);
                    var count = BitConverter.ToUInt32(stacks, 12 * k + 4);
                    var pide = BitConverter.ToUInt32(stacks, 12 * k + 8);
                    if (pide == 0xFFFFFFFFu) pide = 0;
                    if (item == 0 || count == 0 || pide == 0) continue;
                    Add(plan, Linked(pide, item, step), 4, root);
                }
            }
        }
        foreach (var (id, want) in plan)
        {
            if (_pairs.TryGetValue(id, out var have) && SameLife(have.Key, want.Key)) want.Present = have.Present;
            else
            {
                want.Present = Read(want.Key.Entity, want.Component, out var bytes);
                want.Bytes = want.Present ? bytes : null;
            }
        }
        return true;
    }

    private static void Add(Dictionary<ulong, Want> plan, DecisionsNative.EntityKey key, uint component, uint root)
    {
        var id = PairId(key.Entity, component);
        if (!plan.TryGetValue(id, out var want)) plan[id] = want = new Want { Key = key, Component = component };
        if (!want.Roots.Contains(root)) want.Roots.Add(root);
    }

    private DecisionsNative.EntityKey Linked(uint entity, uint generation, ulong step)
    {
        if (!_linked.TryGetValue(entity, out var l) || l.Serial == 0 || l.Generation != generation) l = (LinkedSerialBit | ++_nextLinked, generation, step);
        _linked[entity] = (l.Serial, l.Generation, step);
        return new DecisionsNative.EntityKey { Entity = entity, LifeSerial = l.Serial };
    }

    private bool WalkTree(uint root, out List<uint> leaves, out string why)
    {
        var found = new List<uint>();
        leaves = found;
        why = "";
        if (Setup(root) is not { } setup) return true;
        var setups = 1;
        var path = new List<uint> { root };
        string err = "";
        bool Go(byte[] s, int depth)
        {
            for (var k = 0; k < 16; k++)
            {
                var id = BitConverter.ToUInt32(s, 4 * k);
                if (id == 0) break;
                var cat = s[0x40 + k];
                if (cat == 5)
                {
                    if (depth >= MaxDepth) return Fail("its inventory links nest deeper than 3");
                    if (path.Contains(id)) return Fail($"its inventory links form a cycle (entity {id})");
                    if (++setups > MaxSetups) return Fail("more than 64 inventory setups are linked");
                    if (Setup(id) is not { } inner) return Fail($"a linked inventory setup is missing (entity {id})");
                    path.Add(id);
                    if (!Go(inner, depth + 1)) return false;
                    path.RemoveAt(path.Count - 1);
                    continue;
                }
                if (found.Contains(id)) continue;
                if (found.Count >= MaxLeaves) return Fail("more than 32 inventories are linked");
                found.Add(id);
            }
            return true;
        }
        bool Fail(string text)
        {
            err = text;
            return false;
        }
        var ok = Go(setup, 0);
        why = err;
        return ok;
    }

    private bool Fits(Dictionary<ulong, Want> plan, int planPresent, out string why, ulong except = 0)
    {
        why = "";
        var present = new HashSet<ulong>();
        var roots = new Dictionary<ulong, HashSet<uint>>();
        bool Take(ulong id, bool isPresent, IEnumerable<uint> rs)
        {
            if (isPresent) present.Add(id);
            if (!roots.TryGetValue(id, out var r)) roots[id] = r = [];
            r.UnionWith(rs);
            return r.Count <= MaxRoots;
        }
        foreach (var w in _watches.Where(x => x.Id != except && x.Status == 0))
            foreach (var r in w.Reserved) Take(r.Id, r.Present, r.Roots);
        foreach (var (id, want) in plan)
            if (!Take(id, want.Present, want.Roots))
            {
                why = "a component would be reached from more than 128 roots";
                return false;
            }
        if (present.Count > MaxPairs)
        {
            why = $"it needs {planPresent} present (entity, component) pairs and the watches would hold {present.Count} of at most 4096";
            return false;
        }
        return true;
    }

    private static List<(ulong, bool, List<uint>)> Reservation(Dictionary<ulong, Want> plan) =>
        plan.Select(x => (x.Key, x.Value.Present, x.Value.Roots.ToList())).ToList();

    // window_begin: the pairs of this interval (onlyNew: a watch was just made or resumed: baselines of its new pairs now)
    private void Begin(ulong step, bool onlyNew = false)
    {
        if (!onlyNew)
            foreach (var w in _watches.Where(x => x.Epoch != _epoch).ToList()) EndWatch(w, "its server world stopped; create a new watch");
        var next = new Dictionary<ulong, Pair>();
        var present = 0;
        var fresh = new HashSet<ulong>();
        foreach (var w in _watches)
        {
            if (w.Status != 0) continue;
            if (w.Scope == 0)
            {
                var ok = true;
                for (var i = 0; i < w.Roots.Length && ok; i++)
                    if (!Key(w.Roots[i], out var k, out var exists) || !SameLife(k, w.RootKeys[i]))
                    {
                        Suspend(w, 2, $"entity {w.Roots[i]}" + (exists ? " is another entity now (or has no identity)" : " is gone"));
                        ok = false;
                    }
                if (!ok) continue;
            }
            if (!Expand(w, step, out var plan, out var why, out var whyStatus))
            {
                Suspend(w, whyStatus, why);
                continue;
            }
            var added = 0;
            string? bad = null;
            foreach (var (id, want) in plan)
            {
                if (!next.TryGetValue(id, out var have))
                {
                    if (want.Present) added++;
                    continue;
                }
                if (!SameLife(have.Key, want.Key))
                {
                    bad = $"entity {want.Key.Entity} is reached as two different lives";
                    break;
                }
                if (have.Roots.Union(want.Roots).Count() > MaxRoots)
                {
                    bad = "a component would be reached from more than 128 roots";
                    break;
                }
            }
            if (bad is not null)
            {
                Suspend(w, 1, bad);
                continue;
            }
            if (present + added > MaxPairs)
            {
                Suspend(w, 1, $"it would take the watched pairs past 4096 ({present + added})");
                continue;
            }
            present += added;
            w.Pairs = (uint)plan.Values.Count(x => x.Present);
            w.Reserved = Reservation(plan);
            if (w.Fresh) fresh.Add(w.Id);
            w.Fresh = false;
            foreach (var (id, want) in plan)
            {
                if (!next.TryGetValue(id, out var pr))
                {
                    if (_pairs.TryGetValue(id, out var old) && SameLife(old.Key, want.Key))
                    {
                        pr = old;
                        pr.PrevRoots = pr.Roots;
                        pr.Roots = [];
                        pr.Watches = [];
                        pr.Carried = true;
                    }
                    else
                    {
                        pr = new Pair { Key = want.Key, Component = want.Component };
                        Baseline(pr, want, step);
                    }
                    next[id] = pr;
                }
                if (!pr.Watches.Contains(w.Id)) pr.Watches.Add(w.Id);
                foreach (var r in want.Roots)
                    if (!pr.Roots.Contains(r)) pr.Roots.Add(r);
            }
        }
        foreach (var pr in next.Values)
        {
            if (pr.Carried)
            {
                var gains = false;
                var kept = new List<ulong>();
                foreach (var wid in pr.Watches)
                {
                    if (!pr.Prev.Contains(wid) || fresh.Contains(wid)) gains = true;
                    else kept.Add(wid);
                }
                if (gains) Split(pr, kept, step);
            }
            pr.Carried = false;
            pr.Prev = [.. pr.Watches];
            pr.PrevRoots = [];
        }
        _pairs = next;
        foreach (var entity in _linked.Keys.ToArray())
            if (!_pairs.Values.Any(pr => pr.Key.Entity == entity && pr.Key.LifeSerial == _linked[entity].Serial))
                _linked.Remove(entity);
    }

    private void Baseline(Pair pr, Want want, ulong step)
    {
        pr.Present = want.Present;
        pr.Base = want.Present ? want.Bytes ?? [] : [];
        pr.Typed = want.Present && TypedOf(pr.Key.Entity, pr.Component, out pr.Cur, out pr.Max);
        pr.Since = step;
        pr.Tokens.Clear();
        pr.Truncated = false;
    }

    private void Split(Pair pr, List<ulong> kept, ulong step)
    {
        var present = Read(pr.Key.Entity, pr.Component, out var now);
        var changed = present != pr.Present || (present && !now.AsSpan().SequenceEqual(pr.Base));
        var typed = present & TypedOf(pr.Key.Entity, pr.Component, out var cur, out var max);
        if (changed && kept.Count > 0)
        {
            var c = MakeChange(pr, present, now, step, typed, cur, max);
            c.Watches = kept;
            c.Roots = pr.PrevRoots.Count > 0 ? [.. pr.PrevRoots] : [.. pr.Roots];
            _pending.Add(c);
        }
        if (changed)
        {
            pr.Present = present;
            pr.Base = present ? now : [];
            pr.Typed = typed;
            pr.Cur = cur;
            pr.Max = max;
        }
        pr.Since = step;
        pr.Tokens.Clear();
        pr.Truncated = false;
    }

    // window_end: compare every pair
    private void End(ulong step)
    {
        var live = _pairs.Where(x => Read(x.Value.Key.Entity, x.Value.Component, out _)).Select(x => x.Key).ToHashSet();
        if (live.Count > MaxPairs)
        {
            var accepted = new HashSet<ulong>();
            foreach (var w in _watches.Where(w => w.Status == 0))
            {
                var added = _pairs.Where(x => live.Contains(x.Key) && !accepted.Contains(x.Key) && x.Value.Watches.Contains(w.Id))
                    .Select(x => x.Key).ToArray();
                if (accepted.Count + added.Length > MaxPairs) Suspend(w, 1, "it would take the watched pairs past 4096");
                else accepted.UnionWith(added);
            }
        }
        foreach (var pr in _pairs.Values)
        {
            if ((pr.Key.LifeSerial & (1UL << 63)) != 0) continue;
            var known = Key(pr.Key.Entity, out var key, out var exists);
            if (!exists || (known && SameLife(key, pr.Key))) continue;
            foreach (var wid in pr.Watches)
                if (_watches.Find(w => w.Id == wid) is { Status: 0 } w)
                    Suspend(w, 2, $"entity {pr.Key.Entity} is another entity now (or has no identity)");
        }
        bool Inactive(ulong wid) => !_watches.Any(w => w.Id == wid && w.Status == 0);
        foreach (var pr in _pairs.Values) pr.Watches.RemoveAll(Inactive);
        foreach (var change in _pending) change.Watches.RemoveAll(Inactive);
        _pending.RemoveAll(change => change.Watches.Count == 0);
        var frame = _pending;
        _pending = [];
        foreach (var pr in _pairs.Values)
        {
            var present = Read(pr.Key.Entity, pr.Component, out var now);
            var changed = present != pr.Present || (present && !now.AsSpan().SequenceEqual(pr.Base));
            if (changed)
            {
                var typed = present & TypedOf(pr.Key.Entity, pr.Component, out var cur, out var max);
                if (pr.Watches.Count > 0) frame.Add(MakeChange(pr, present, now, step, typed, cur, max));
                pr.Present = present;
                pr.Base = present ? now : [];
                pr.Typed = typed;
                pr.Cur = cur;
                pr.Max = max;
            }
            pr.Since = step;
            pr.Tokens.Clear();
            pr.Truncated = false;
        }
        if (frame.Count > 0)
        {
            if (_frames.Count + _batchFrames >= MaxFrames)
            {
                QueueOverflows++;
                FramesDropped++;
                foreach (var w in _watches.Where(x => x.Status == 0)) Suspend(w, 1, "the change queue is full (two steps were not read); resume it");
                return;
            }
            Changes += (ulong)frame.Count;
            _frames.Add(frame);
        }
        foreach (var w in _watches.Where(x => x.Status == 0)) w.LastComplete = step;
        _lastComplete = step;
    }

    private Change MakeChange(Pair pr, bool present, byte[] now, ulong step, bool typed, int cur, int max)
    {
        var c = new Change
        {
            Component = pr.Component,
            Epoch = _epoch,
            Step = step,
            Since = pr.Since,
            Key = pr.Key,
            Before = pr.Present ? pr.Base : [],
            After = present ? now : [],
            Flags = (pr.Present ? 1u : 0) | (present ? 2u : 0) | (pr.Truncated ? 4u : 0),
            Tokens = pr.Tokens.Select(t => t.Id).ToList(),
            Watches = [.. pr.Watches],
            Roots = [.. pr.Roots],
        };
        (c.Origin, c.Source) = OriginOf(pr.Base, pr.Present, now, present, pr.Tokens, pr.Truncated);
        if (pr.Typed && typed)
        {
            c.Attribute = pr.Component;
            c.ValueBefore = pr.Cur;
            c.MaxBefore = pr.Max;
            c.ValueAfter = cur;
            c.MaxAfter = max;
        }
        return c;
    }

    /// <summary>watch_core.h origin_of, the same rule.</summary>
    private static (uint Origin, string Source) OriginOf(byte[] @base, bool basePresent, byte[] final, bool finalPresent, List<Token> tokens, bool truncated)
    {
        if (tokens.Count == 0) return (0, "");
        if (tokens.Any(t => !t.Valid)) return (0, "");
        var sources = tokens.Select(t => t.Source).Distinct().ToList();
        var source = string.Join(", ", sources);
        var reached = new List<byte[]>();
        if (basePresent) reached.Add(@base);
        bool In(byte[] b) => reached.Any(x => x.AsSpan().SequenceEqual(b));
        foreach (var t in tokens)
            if (In(t.Before)) reached.Add(t.After);
        var chain = finalPresent && In(final);
        return (sources.Count == 1 && chain && !truncated ? 2u : 3u, source);
    }

    private void Suspend(Watch w, uint status, string why)
    {
        w.Status = status;
        w.Reason = why;
        w.Pairs = 0;
        w.Reserved = [];
    }

    private void EndWatch(Watch w, string why)
    {
        _graves.AddLast((w.Id, w.Owner, 3u, why, w.LastComplete));
        if (_graves.Count > Graveyard) _graves.RemoveFirst();
        _watches.Remove(w);
        if (_watches.Count == 0)  // nothing watched: nothing kept (as the service)
        {
            _pairs = [];
            _linked.Clear();
            _pending = [];
        }
    }

    private void WorldStopped(string why)
    {
        foreach (var w in _watches.ToList()) EndWatch(w, why);
        _pairs = [];
        _linked.Clear();
        _pending = [];
        FramesDropped += (ulong)(_frames.Count + _batchFrames);
        _frames.Clear();
        _batch = [];  // a frozen batch of the old world goes too (as the service)
        _batchId = 0;
        _batchFrames = 0;
    }

    // ---- records ----
    private static void Blank(N.WatchStatus* st)
    {
        *st = default;
        st->Size = (uint)sizeof(N.WatchStatus);
        st->Limit = MaxPairs;
    }

    private static int Refuse(N.WatchStatus* st, uint status, string why)
    {
        st->Status = status;
        st->WatchId = 0;
        Text(st->Reason, why);
        return FakeTables.Ok;
    }

    private static int Done(Watch w, N.WatchStatus* st)
    {
        Fill(w, st);
        return FakeTables.Ok;
    }

    private static void Fill(Watch w, N.WatchStatus* st)
    {
        Blank(st);
        st->Status = w.Status;
        st->Pairs = w.Pairs;
        st->WatchId = w.Id;
        st->LastCompleteStep = w.LastComplete;
        Text(st->Reason, w.Reason);
    }

    private static int FillGrave((ulong Id, ulong Owner, uint Status, string Reason, ulong LastComplete) g, N.WatchStatus* st)
    {
        Blank(st);
        st->Status = g.Status;
        st->WatchId = g.Id;
        st->LastCompleteStep = g.LastComplete;
        Text(st->Reason, g.Reason);
        return FakeTables.Ok;
    }

    private static void Text(byte* dst, string text)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(text);
        var n = Math.Min(bytes.Length, 191);
        for (var i = 0; i < n; i++) dst[i] = bytes[i];
        dst[n] = 0;
    }

    private static void ToWire(Change c, N.Change* r)
    {
        *r = default;
        r->Size = (uint)sizeof(N.Change);
        r->Version = 1;
        r->Kind = 1;
        r->Component = c.Component;
        r->WorldEpoch = c.Epoch;
        r->Step = c.Step;
        r->Entity = c.Key;
        r->BeforeBytes = (uint)c.Before.Length;
        r->AfterBytes = (uint)c.After.Length;
        r->Origin = c.Origin;
        r->Flags = c.Flags;
        r->RootCount = (uint)Math.Min(c.Roots.Count, 128);
        r->WatchCount = (uint)Math.Min(c.Watches.Count, 32);
        r->TokenCount = (uint)Math.Min(c.Tokens.Count, 32);
        for (var i = 0; i < r->RootCount; i++) r->Roots[i] = c.Roots[i];
        for (var i = 0; i < r->WatchCount; i++) r->Watches[i] = c.Watches[i];
        for (var i = 0; i < r->TokenCount; i++) r->Tokens[i] = c.Tokens[i];
        c.Before.CopyTo(new Span<byte>(r->Before, 256));
        c.After.CopyTo(new Span<byte>(r->After, 256));
        var src = System.Text.Encoding.UTF8.GetBytes(c.Source);
        var n = Math.Min(src.Length, 127);
        for (var i = 0; i < n; i++) r->SourceMod[i] = src[i];
        r->WorldKind = 0;
        r->Attribute = c.Attribute;
        r->ValueBefore = c.ValueBefore;
        r->ValueAfter = c.ValueAfter;
        r->MaximumBefore = c.MaxBefore;
        r->MaximumAfter = c.MaxAfter;
        r->SinceStep = c.Since;
    }

    public (int Watches, int Pairs, int Present, int QueuedFrames) Counts()
    {
        lock (_lock) return (_watches.Count, _pairs.Count, _pairs.Values.Count(p => p.Present), _frames.Count + _batchFrames);
    }

    public void Counters(N.Counters* c)
    {
        lock (_lock)
        {
            var size = c->Size;
            *c = default;
            c->Size = size;
            c->State = 1;
            c->Watches = (uint)_watches.Count;
            c->Pairs = (uint)_pairs.Count;
            c->PresentPairs = (uint)_pairs.Values.Count(p => p.Present);
            c->QueuedFrames = (uint)(_frames.Count + _batchFrames);
            c->QueuedRecords = (uint)(_frames.Sum(f => f.Count) + _batch.Count);
            c->WorldEpoch = _epoch;
            c->LastCompleteStep = _lastComplete;
            c->Changes = Changes;
            c->QueueOverflows = QueueOverflows;
            c->FramesDropped = FramesDropped;
            c->TokensRecorded = TokensRecorded;
        }
    }
}
