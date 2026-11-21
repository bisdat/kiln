// SPDX-License-Identifier: MIT
// A fake kiln.markers table for Kiln.Markers' own code: it records what the wrapper passes, answers from a small keyed
// store with the native service's result shapes (or a scripted answer), and delivers queued changes through the callback
// the wrapper subscribed with when the test runs a "client step" (Dispatch). It is not the store under test: the store's
// rules are tested natively (native/services/markers/tests/store_test.cpp).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Kiln;
using Xunit;

[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace MarkersTests;

internal sealed record FakeSpec(ulong Owner, uint Size, uint Surfaces, string Key, double X, double Y, double Z, ulong Follow, float Height,
    float MaxDistance, uint Icon, uint Rgba, int Priority, uint MapLabel, string Text);

internal static unsafe class FakeMarkers
{
    public static MarkersNative.Table* Table;
    public static readonly List<string> Calls = new();
    public static readonly List<FakeSpec> Specs = new();
    public static readonly Dictionary<(ulong Owner, string Key), FakeSpec> Records = new();
    public static readonly Dictionary<ulong, string> Bound = new();
    public static readonly List<ulong> Released = new();
    public static int BindAnswer;           // KilnResult bind_owner gives
    public static (int Rc, uint Status, string Reason)? Next;  // the next mutation's scripted answer
    public static nint ChangeFn;
    public static nint ChangeUser;
    public static ulong SubscribedOwner;
    public static int Subscribes;
    public static ulong ServerEpoch, ClientEpoch;   // world_epoch(0) and world_epoch(2)
    // the next server send's answer: (status, committed, queued, refused, affected, reason); null: accepted, 1 affected
    public static (uint Status, uint Committed, uint Queued, uint Refused, uint Affected, string Reason)? SendAnswer;
    private static readonly List<MarkersNative.Change> Queue = new();
    private static ulong _sequence;

    public static void Install(uint size = 0)
    {
        Free();
        Table = (MarkersNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(MarkersNative.Table));
        Table->Header.Size = size != 0 ? size : (uint)sizeof(MarkersNative.Table);
        Table->Header.Version = 1;
        Table->BindOwner = &BindOwner;
        Table->ReleaseOwner = &ReleaseOwner;
        Table->SetLocal = &SetLocal;
        Table->RemoveLocal = &RemoveLocal;
        Table->ClearLocal = &ClearLocal;
        Table->Subscribe = &Subscribe;
        Table->Unsubscribe = &Unsubscribe;
        Table->ReleaseAnchor = &ReleaseAnchor;
        Table->Send = &Send;
        Table->Unsend = &Unsend;
        Table->ClearSent = &ClearSent;
        Table->SuppressSent = &SuppressSent;
        Table->WorldEpoch = &WorldEpoch;
        ServerEpoch = ClientEpoch = 0;
        SendAnswer = null;
        Calls.Clear();
        Specs.Clear();
        Records.Clear();
        Bound.Clear();
        Released.Clear();
        Queue.Clear();
        BindAnswer = 0;
        Next = null;
        ChangeFn = ChangeUser = 0;
        SubscribedOwner = 0;
        Subscribes = 0;
        _sequence = 0;
    }

    public static void Free()
    {
        if (Table != null) NativeMemory.Free(Table);
        Table = null;
    }

    /// <summary>The client step: delivers the queued changes, in order, through the subscribed callback. A change made by
    /// a handler waits for the next step.</summary>
    public static void Dispatch()
    {
        var batch = Queue.ToArray();
        Queue.Clear();
        if (ChangeFn == 0) return;
        var fn = (delegate* unmanaged[Cdecl]<MarkersNative.Change*, void*, void>)ChangeFn;
        foreach (var c in batch)
        {
            var copy = c;
            fn(&copy, (void*)ChangeUser);
        }
    }

    private static string Str(NativeStr s) => s.Ptr == null ? "" : Encoding.UTF8.GetString(s.Ptr, checked((int)s.Len));

    private static void Say(MarkersNative.Result* r, string text)
    {
        var bytes = Encoding.UTF8.GetBytes(text);
        var n = Math.Min(bytes.Length, 255);
        for (var i = 0; i < n; i++) r->Reason[i] = bytes[i];
        r->Reason[n] = 0;
    }

    private static int Answer(MarkersNative.Result* r, uint status, uint committed, uint affected, string reason = "")
    {
        r->Status = status;
        r->Committed = committed;
        r->Affected = affected;
        Say(r, reason);
        return 0;
    }

    private static bool Scripted(MarkersNative.Result* r, out int rc)
    {
        rc = 0;
        if (Next is not { } n) return false;
        Next = null;
        r->Status = n.Status;
        Say(r, n.Reason);
        rc = n.Rc;
        return true;
    }

    private static void Enqueue(ulong owner, uint kind, string key, uint affected, uint cause = 0)
    {
        var c = new MarkersNative.Change
        {
            Size = (uint)sizeof(MarkersNative.Change), Kind = kind, Audience = 0, WorldEpoch = 7, PublisherGeneration = owner,
            Sequence = ++_sequence, Affected = affected, Cause = cause,
        };
        var o = Encoding.UTF8.GetBytes(Bound.TryGetValue(owner, out var ns) ? ns : "?");
        for (var i = 0; i < Math.Min(o.Length, 64); i++) c.Owner[i] = o[i];
        var k = Encoding.UTF8.GetBytes(key);
        for (var i = 0; i < Math.Min(k.Length, 64); i++) c.Key[i] = k[i];
        Queue.Add(c);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int BindOwner(ulong owner, NativeStr ns)
    {
        Calls.Add($"bind {owner} {Str(ns)}");
        if (BindAnswer != 0) return BindAnswer;
        Bound[owner] = Str(ns);
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReleaseOwner(ulong owner)
    {
        Calls.Add($"release {owner}");
        Released.Add(owner);
        var mine = Records.Keys.Where(k => k.Owner == owner).ToList();
        foreach (var k in mine) Records.Remove(k);
        if (mine.Count > 0) Enqueue(owner, 3, "", (uint)mine.Count, cause: 1);
        Bound.Remove(owner);
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SetLocal(ulong owner, MarkersNative.Spec* s, MarkersNative.Result* r)
    {
        if (r == null || r->Size < (uint)sizeof(MarkersNative.Result)) return 1;
        var spec = new FakeSpec(owner, s->Size, s->Surfaces, Str(s->Key), s->Position.X, s->Position.Y, s->Position.Z, s->Follow, s->Height,
            s->MaxDistance, s->IconType, s->Rgba, s->Priority, s->MapLabelMode, Str(s->Text));
        Specs.Add(spec);
        Calls.Add($"set {owner} {spec.Key}");
        if (Scripted(r, out var rc)) return rc;
        if (!Bound.ContainsKey(owner)) return 1;
        var key = (owner, spec.Key);
        if (Records.TryGetValue(key, out var old) && old with { Owner = owner } == spec) return Answer(r, 0, 1, 0);
        var added = !Records.ContainsKey(key);
        Records[key] = spec;
        Enqueue(owner, added ? 0u : 1u, spec.Key, 1);
        return Answer(r, 0, 1, 1);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RemoveLocal(ulong owner, NativeStr k, MarkersNative.Result* r)
    {
        var key = Str(k);
        Calls.Add($"remove {owner} {key}");
        if (Scripted(r, out var rc)) return rc;
        if (!Records.Remove((owner, key))) return Answer(r, 1, 0, 0, $"NotFound: no local marker '{key}'");
        Enqueue(owner, 2, key, 1);
        return Answer(r, 0, 1, 1);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ClearLocal(ulong owner, MarkersNative.Result* r)
    {
        Calls.Add($"clear {owner}");
        if (Scripted(r, out var rc)) return rc;
        var mine = Records.Keys.Where(x => x.Owner == owner).ToList();
        foreach (var x in mine) Records.Remove(x);
        if (mine.Count > 0) Enqueue(owner, 3, "", (uint)mine.Count);
        return Answer(r, 0, 1, (uint)mine.Count);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Subscribe(ulong owner, delegate* unmanaged[Cdecl]<MarkersNative.Change*, void*, void> changed,
        delegate* unmanaged[Cdecl]<MarkersNative.Visibility*, void*, void> visibility, delegate* unmanaged[Cdecl]<MarkersNative.CatalogChange*, void*, void> catalog,
        void* user, ulong* h)
    {
        Subscribes++;
        SubscribedOwner = owner;
        ChangeFn = (nint)changed;
        ChangeUser = (nint)user;
        *h = 1;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsubscribe(ulong h) => 0;

    // ---- the server layers (what the wrapper passes; the layers' rules are tested natively, tests/link_test.cpp) ----

    private static int SentAnswer(MarkersNative.Result* r)
    {
        if (Scripted(r, out var rc)) return rc;
        var a = SendAnswer ?? (0u, 1u, 0u, 0u, 1u, "");
        SendAnswer = null;
        r->Queued = a.Queued;
        r->Refused = a.Refused;
        return Answer(r, a.Status, a.Committed, a.Affected, a.Reason);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Send(ulong owner, uint player, MarkersNative.Spec* s, MarkersNative.Result* r)
    {
        if (r == null || r->Size < (uint)sizeof(MarkersNative.Result)) return 1;
        var spec = new FakeSpec(owner, s->Size, s->Surfaces, Str(s->Key), s->Position.X, s->Position.Y, s->Position.Z, s->Follow, s->Height,
            s->MaxDistance, s->IconType, s->Rgba, s->Priority, s->MapLabelMode, Str(s->Text));
        Specs.Add(spec);
        Calls.Add($"send {owner} {player} {spec.Key}");
        return SentAnswer(r);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsend(ulong owner, uint player, NativeStr k, MarkersNative.Result* r)
    {
        Calls.Add($"unsend {owner} {player} {Str(k)}");
        return SentAnswer(r);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SuppressSent(ulong owner, uint player, NativeStr k, MarkersNative.Result* r)
    {
        Calls.Add($"suppress {owner} {player} {Str(k)}");
        return SentAnswer(r);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ClearSent(ulong owner, uint player, MarkersNative.Result* r)
    {
        Calls.Add($"clear_sent {owner} {player}");
        return SentAnswer(r);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int WorldEpoch(uint kind, ulong* epoch)
    {
        if (epoch == null || (kind != 0 && kind != 2)) return 1;
        *epoch = kind == 0 ? ServerEpoch : ClientEpoch;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReleaseAnchor(ulong owner, ulong anchor)
    {
        Calls.Add($"release_anchor {owner} {anchor}");
        return 0;
    }
}

internal sealed class FakeBackend : IKilnBackend
{
    public readonly List<string> Lines = new();
    public string Version => "test";
    public bool IsTickThread => true;
    public bool HasFeature(string name) => false;
    public void Log(ulong owner, LogLevel level, string text) => Lines.Add($"{level}: {text}");
    public unsafe nint Service(string name, uint minVersion) =>
        name == MarkersNative.Service && FakeMarkers.Table != null && FakeMarkers.Table->Header.Version >= minVersion ? (nint)FakeMarkers.Table
        : name == MarkersNative.BridgeService && FakeBridge.Table != null ? (nint)FakeBridge.Table : 0;
    public ulong OwnerCreate(string id) => 4242;
}

/// <summary>Each test: a fresh fake backend and table, registry and Kiln.Markers caches; a mod context to call as.</summary>
public abstract class MarkersFixture : IDisposable
{
    private readonly IKilnBackend _previous;
    internal readonly FakeBackend Backend;
    private readonly string _dir;
    private readonly List<ModContext> _mods = new();

    protected MarkersFixture(bool table = true)
    {
        _previous = Kiln.Backend.Current;
        Backend = new FakeBackend();
        Kiln.Backend.Current = Backend;
        ServiceTick.AutoStartEnabled = false;
        Capabilities.Reset();
        if (table) FakeMarkers.Install();
        else FakeMarkers.Free();
        MarkersNative.Forget();
        Markers.ForgetSubscription();
        _dir = Path.Combine(Path.GetTempPath(), "kiln-markers-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    internal ModContext Mod(string id, ulong owner)
    {
        var mod = HostAccess.CreateContext(new ModInfo(id, id, "1.0.0", _dir), owner, _dir);
        _mods.Add(mod);
        return mod;
    }

    internal static T As<T>(ModContext? mod, Func<T> f)
    {
        var previous = HostAccess.Current;
        HostAccess.Current = mod;
        try
        {
            return f();
        }
        finally
        {
            HostAccess.Current = previous;
        }
    }

    internal static void As(ModContext? mod, Action f) => As(mod, () =>
    {
        f();
        return 0;
    });

    public void Dispose()
    {
        foreach (var m in _mods) m.Retire();
        FakeMarkers.Free();
        MarkersNative.Forget();
        Markers.ForgetSubscription();
        Capabilities.Reset();
        Kiln.Backend.Current = _previous;
        try { Directory.Delete(_dir, recursive: true); } catch { /* temp */ }
    }
}
