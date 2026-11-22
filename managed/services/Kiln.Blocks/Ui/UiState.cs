// SPDX-License-Identifier: MIT
// A window's state at run time (dossier 09 §4.2): the declared entries (WindowBlock.State) and the tab bars' and trees' own
// state, one immutable snapshot at a time. Writes (a widget, an action, BlockUi.SetState) are queued from any thread and
// committed in order on UiRuntime.Pump: a changed write advances the revision once and posts one event per changed entry;
// a write of the same value is accepted and changes nothing. A new world gives a new session token and the initial values
// (UiWindowStateResetEvent). Closing the window keeps its state; unloading the mod drops it. Never saved.
using System.Collections.Concurrent;
using System.Collections.Immutable;
using System.Globalization;
using System.Text;

namespace Kiln;

internal sealed class UiWindowState
{
    internal const int MaxEntries = 64, MaxWindowBytes = 256 * 1024, MaxModBytes = 2 * 1024 * 1024;

    private static long _sessions;
    private static readonly ConcurrentDictionary<string, long> ModBytes = new(StringComparer.Ordinal);

    internal enum WriteKind { Value, Control }

    internal sealed record Write(WriteKind Kind, string Key, UiStateValue? Value, UiControlState? Control, UiStateVersion? Expected, ulong Session,
        UiStateChangeCause Cause, TaskCompletionSource<UiStateResult>? Done, Func<UiStateSnapshot, string?>? Check);

    private readonly object _gate = new();
    private readonly ConcurrentQueue<Write> _queue = new();
    private readonly ConcurrentDictionary<string, UiStateValue> _pending = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, UiControlState> _pendingControls = new(StringComparer.Ordinal);
    private volatile UiStateSnapshot _snapshot;
    private long _bytes;
    private bool _gone;
    private readonly HashSet<string> _refusedLogged = new(StringComparer.Ordinal);  // Pump only

    public UiWindowState(string mod, string window, IReadOnlyDictionary<string, UiStateDefinition>? definitions)
    {
        Mod = mod;
        Window = new WindowRef($"{mod}:{window}");
        Definitions = definitions ?? ImmutableDictionary<string, UiStateDefinition>.Empty;
        _snapshot = Initial(NewSession());
        _bytes = Size(_snapshot);
        ModBytes.AddOrUpdate(mod, _bytes, (_, b) => b + _bytes);
    }

    public string Mod { get; }
    public WindowRef Window { get; }
    public IReadOnlyDictionary<string, UiStateDefinition> Definitions { get; }

    /// <summary>The committed snapshot (any thread).</summary>
    public UiStateSnapshot Snapshot => _snapshot;

    private static ulong NewSession() => (ulong)Interlocked.Increment(ref _sessions);

    private UiStateSnapshot Initial(ulong session) => new(Window, new UiStateVersion(session, 0),
        Definitions.ToImmutableDictionary(kv => kv.Key, kv => InitialOf(kv.Value), StringComparer.Ordinal),
        ImmutableDictionary<string, UiControlState>.Empty.WithComparers(StringComparer.Ordinal));

    internal static UiStateValue InitialOf(UiStateDefinition d) => d switch
    {
        UiBoolState b => new UiStateValue.Bool(b.Initial),
        UiInt64State l => new UiStateValue.Int64(l.Initial),
        UiDecimalState m => new UiStateValue.Decimal(m.Initial),
        UiStringState s => new UiStateValue.String(s.Initial),
        UiStringSetState ss => new UiStateValue.StringSet(Canonical(ss.Initial)),
        _ => throw new ArgumentException("an unknown state definition"),
    };

    internal static ImmutableArray<string> Canonical(IEnumerable<string> items) => [.. items.Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal)];

    /// <summary>The committed values with what is queued and not committed yet over them (what a refresh shows).</summary>
    public IReadOnlyDictionary<string, UiStateValue> View()
    {
        var s = _snapshot.Values;
        if (_pending.IsEmpty) return s;
        var b = ImmutableDictionary.CreateBuilder<string, UiStateValue>(StringComparer.Ordinal);
        b.AddRange(s);
        foreach (var (k, v) in _pending) b[k] = v;
        return b.ToImmutable();
    }

    /// <summary>A control's state with a queued change over it.</summary>
    public UiControlState? Control(string rowId) =>
        _pendingControls.TryGetValue(rowId, out var p) ? p : _snapshot.Controls.TryGetValue(rowId, out var c) ? c : null;

    // ---- checking ---------------------------------------------------------------------------------------------------

    /// <summary>Whether a value fits the declaration: null when it does, else what is wrong (an author's mistake).</summary>
    internal static string? Problem(UiStateDefinition d, UiStateValue v)
    {
        switch (d, v)
        {
            case (UiBoolState, UiStateValue.Bool): return null;
            case (UiInt64State l, UiStateValue.Int64 x):
                return x.Value < l.Min || x.Value > l.Max ? $"{x.Value} is outside {l.Min}..{l.Max}" : null;
            case (UiDecimalState m, UiStateValue.Decimal x):
                if (x.Value < m.Min || x.Value > m.Max) return $"{UiCanonical.Text(x.Value)} is outside {UiCanonical.Text(m.Min)}..{UiCanonical.Text(m.Max)}";
                return UiCanonical.Scale(x.Value) > m.Scale ? $"{UiCanonical.Text(x.Value)} has more than {m.Scale} digits after the point (it is never rounded)" : null;
            case (UiStringState s, UiStateValue.String x):
                if (x.Value == null) return "a null text";
                if (x.Value.Contains('\0')) return "a text with a NUL";
                var n = Encoding.UTF8.GetByteCount(x.Value);
                return n > s.MaxBytes ? $"{n} UTF-8 bytes is more than {s.MaxBytes}" : null;
            case (UiStringSetState ss, UiStateValue.StringSet x):
                if (x.Value == null) return "a null set";
                var items = x.Value.Distinct(StringComparer.Ordinal).ToList();
                if (items.Count > ss.MaxItems) return $"{items.Count} items is more than {ss.MaxItems}";
                foreach (var i in items)
                {
                    if (i == null) return "a null item";
                    if (Encoding.UTF8.GetByteCount(i) > ss.MaxItemBytes) return $"\"{i}\" is more than {ss.MaxItemBytes} UTF-8 bytes";
                }
                return null;
            default:
                return $"the entry holds a {Name(d)}, not a {v.GetType().Name}";
        }
    }

    internal static string Name(UiStateDefinition d) => d switch
    {
        UiBoolState => "bool", UiInt64State => "int64", UiDecimalState => "decimal", UiStringState => "string", _ => "stringSet",
    };

    internal static UiStateValue Normalize(UiStateValue v) => v is UiStateValue.StringSet s ? new UiStateValue.StringSet(Canonical(s.Value)) : v;

    private static long Size(UiStateSnapshot s)
    {
        long n = 0;
        foreach (var (k, v) in s.Values) n += k.Length + Size(v);
        foreach (var (k, c) in s.Controls) n += k.Length + Encoding.UTF8.GetByteCount(c.Selected ?? "") + c.Expanded.Sum(e => Encoding.UTF8.GetByteCount(e) + 1);
        return n;
    }

    private static long Size(UiStateValue v) => v switch
    {
        UiStateValue.String s => Encoding.UTF8.GetByteCount(s.Value) + 2,
        UiStateValue.StringSet ss => ss.Value.Sum(i => Encoding.UTF8.GetByteCount(i) + 3) + 2,
        UiStateValue.Bool => 5,
        _ => 21,
    };

    // ---- writes ------------------------------------------------------------------------------------------------------

    /// <summary>Queues a write (any thread). Its result comes when Pump commits it.</summary>
    public Task<UiStateResult> Enqueue(WriteKind kind, string key, UiStateValue? value, UiControlState? control, UiStateVersion? expected,
        UiStateChangeCause cause, Func<UiStateSnapshot, string?>? check = null, bool wantResult = true)
    {
        var done = wantResult ? new TaskCompletionSource<UiStateResult>(TaskCreationOptions.RunContinuationsAsynchronously) : null;
        lock (_gate)
        {
            if (_gone)
            {
                var r = new UiStateResult(false, $"{UiStateReasons.Unloaded}: the window is gone", null);
                return Task.FromResult(r);
            }
            if (kind == WriteKind.Value) _pending[key] = value!;
            else _pendingControls[key] = control!;
            _queue.Enqueue(new Write(kind, key, value, control, expected, _snapshot.Version.Session, cause, done, check));
        }
        UiRuntime.WakeForData();
        return done?.Task ?? Task.FromResult(new UiStateResult(true, null, null));
    }

    /// <summary>Commits the queued writes in order. Pump (client main).</summary>
    public void Commit()
    {
        while (_queue.TryDequeue(out var w))
        {
            UiStateResult result;
            List<KilnEvent>? events = null;
            lock (_gate)
            {
                result = Apply(w, ref events);
                // the optimistic view keeps a newer queued write for the same key
                if (!_queue.Any(q => q.Kind == w.Kind && q.Key == w.Key))
                {
                    if (w.Kind == WriteKind.Value) _pending.TryRemove(w.Key, out _);
                    else _pendingControls.TryRemove(w.Key, out _);
                }
            }
            w.Done?.TrySetResult(result);
            if (!result.Ok && w.Done == null && w.Cause is UiStateChangeCause.Widget or UiStateChangeCause.Action && result.Reason?.StartsWith(UiStateReasons.Budget, StringComparison.Ordinal) == true && _refusedLogged.Add(w.Key))
                BlocksBridge.Log(LogLevel.Warn, $"blocks: {Mod}: ui: window {Window}: a widget's write was refused: {result.Reason} (declare a larger entry)");
            if (events != null) foreach (var e in events) Events.Post(e);
        }
    }

    private UiStateResult Apply(Write w, ref List<KilnEvent>? events)
    {
        var cur = _snapshot;
        if (_gone) return new UiStateResult(false, $"{UiStateReasons.Unloaded}: the window is gone", null);
        if (w.Session != cur.Version.Session) return new UiStateResult(false, $"{UiStateReasons.StaleSession}: the world changed and the state was reset", cur);
        if (w.Expected is { } ex && ex != cur.Version)
            return new UiStateResult(false, $"{UiStateReasons.StaleVersion}: expected {ex.Session}/{ex.Revision}, it is {cur.Version.Session}/{cur.Version.Revision}", cur);
        if (w.Check?.Invoke(cur) is { } refusal) return new UiStateResult(false, refusal, cur);
        UiStateSnapshot next;
        if (w.Kind == WriteKind.Value)
        {
            var value = Normalize(w.Value!);
            var before = cur.Values.TryGetValue(w.Key, out var b) ? b : null;
            if (Equals(before, value)) return new UiStateResult(true, null, cur);
            // every write keeps the entry's declaration (a widget's too: a table row's key, a tree's expanded set)
            if (Definitions.TryGetValue(w.Key, out var def) && Problem(def, value) is { } bad)
                return new UiStateResult(false, $"{UiStateReasons.Budget}: state.{w.Key}: {bad}", cur);
            next = cur with { Values = ((ImmutableDictionary<string, UiStateValue>)cur.Values).SetItem(w.Key, value), Version = cur.Version with { Revision = cur.Version.Revision + 1 } };
            if (Budget(next) is { } over) return new UiStateResult(false, over, cur);
            (events ??= new()).Add(new UiWindowStateChangedEvent(Window, w.Key, before, value, next.Version, w.Cause));
        }
        else
        {
            var before = cur.Controls.TryGetValue(w.Key, out var b) ? b : null;
            var after = w.Control! with { Expanded = Canonical(w.Control!.Expanded) };
            if (before != null && before.Selected == after.Selected && before.Expanded.SequenceEqual(after.Expanded, StringComparer.Ordinal))
                return new UiStateResult(true, null, cur);
            next = cur with { Controls = ((ImmutableDictionary<string, UiControlState>)cur.Controls).SetItem(w.Key, after), Version = cur.Version with { Revision = cur.Version.Revision + 1 } };
            if (Budget(next) is { } over) return new UiStateResult(false, over, cur);
            (events ??= new()).Add(new UiControlStateChangedEvent(Window, w.Key, before, after, next.Version, w.Cause));
        }
        _snapshot = next;
        return new UiStateResult(true, null, next);
    }

    private string? Budget(UiStateSnapshot next)
    {
        var size = Size(next);
        if (size > MaxWindowBytes) return $"{UiStateReasons.Budget}: the window's state would be {size} bytes, more than {MaxWindowBytes}";
        var mod = ModBytes.GetValueOrDefault(Mod) - _bytes + size;
        if (mod > MaxModBytes) return $"{UiStateReasons.Budget}: the mod's window state would be {mod} bytes, more than {MaxModBytes}";
        ModBytes.AddOrUpdate(Mod, size, (_, b) => b - _bytes + size);
        _bytes = size;
        return null;
    }

    /// <summary>A new world: the initial values under a new session token. Queued writes of the old session are refused.</summary>
    public void Reset()
    {
        UiStateSnapshot old, now;
        lock (_gate)
        {
            if (_gone) return;
            old = _snapshot;
            now = Initial(NewSession());
            _snapshot = now;
            _pending.Clear();
            _pendingControls.Clear();
            var size = Size(now);
            ModBytes.AddOrUpdate(Mod, size, (_, b) => b - _bytes + size);
            _bytes = size;
        }
        Commit();  // the old session's writes answer StaleSession
        Events.Post(new UiWindowStateResetEvent(Window, old.Version, now.Version));
    }

    /// <summary>The window is gone (its mod unloaded or it was defined again): queued writes answer Unloaded.</summary>
    public void Drop()
    {
        lock (_gate)
        {
            if (_gone) return;
            _gone = true;
            ModBytes.AddOrUpdate(Mod, 0, (_, b) => Math.Max(0, b - _bytes));
        }
        while (_queue.TryDequeue(out var w)) w.Done?.TrySetResult(new UiStateResult(false, $"{UiStateReasons.Unloaded}: the window is gone", null));
        _pending.Clear();
        _pendingControls.Clear();
    }

    internal static void ResetForTests() => ModBytes.Clear();

    // ---- reading a set action's literal ------------------------------------------------------------------------

    /// <summary>A literal of a set action (JSON or C#) as a value of the entry's type, or a problem.</summary>
    internal static UiStateValue? FromLiteral(UiStateDefinition d, object? to, out string? problem)
    {
        problem = null;
        object? v = to is System.Text.Json.JsonElement j ? j.ValueKind switch
        {
            System.Text.Json.JsonValueKind.True => true,
            System.Text.Json.JsonValueKind.False => false,
            System.Text.Json.JsonValueKind.String => j.GetString(),
            System.Text.Json.JsonValueKind.Number => j.GetRawText(),
            System.Text.Json.JsonValueKind.Array => j.EnumerateArray().Select(e => e.ValueKind == System.Text.Json.JsonValueKind.String ? e.GetString() : null).ToList(),
            _ => null,
        } : to;
        UiStateValue? r = (d, v) switch
        {
            (UiBoolState, bool b) => new UiStateValue.Bool(b),
            (UiInt64State, long l) => new UiStateValue.Int64(l),
            (UiInt64State, int i) => new UiStateValue.Int64(i),
            (UiInt64State, string s) when UiCanonical.TryInt64(s, out var l) => new UiStateValue.Int64(l),
            (UiDecimalState, decimal m) => new UiStateValue.Decimal(m),
            (UiDecimalState, long l) => new UiStateValue.Decimal(l),
            (UiDecimalState, int i) => new UiStateValue.Decimal(i),
            (UiDecimalState, string s) when UiCanonical.TryDecimal(s, out var m) => new UiStateValue.Decimal(m),
            (UiStringState, string s) => new UiStateValue.String(s),
            (UiStringSetState, IEnumerable<string?> items) when items.All(i => i != null) => new UiStateValue.StringSet(Canonical(items!)),
            _ => null,
        };
        if (r == null) { problem = $"a {Name(d)} entry takes {(d is UiInt64State or UiDecimalState ? "a number written as a string (\"12\", \"1.5\")" : d is UiStringSetState ? "an array of texts" : d is UiBoolState ? "true or false" : "a text")}"; return null; }
        problem = Problem(d, r);
        return problem == null ? r : null;
    }
}
