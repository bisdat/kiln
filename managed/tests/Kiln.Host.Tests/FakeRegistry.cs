// SPDX-License-Identifier: MIT
#pragma warning disable KILNX031
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;
using Kiln.Host;

namespace Kiln.Host.Tests;

// Exercises the real Host transport and dispatcher. This is a copied-state lifecycle fake, not native mapping proof.
internal sealed unsafe class FakeRegistry : IDisposable
{
    private static FakeRegistry? _current;
    internal readonly FakeHost Host = new();
    internal readonly RegistryStageTable* Stage;
    internal readonly RegistryLiveTable* Live;
    internal uint Phase = 4;
    internal ulong World = 1, Generation;
    internal int Submits, StageError, ReadyError, StatusFailures, WorldRequiredCommits;
    internal int Value => _layers.Count == 0 ? 10 : _layers.Last().Value.Value;
    private ulong _next;
    private readonly Dictionary<ulong, (ulong Owner, uint Point, int? Value, bool Restart)> _drafts = [];
    private readonly Dictionary<ulong, (ulong Owner, int Value, ulong World)> _layers = [];
    internal int LayerCount => _layers.Count;
    internal int DraftCount => _drafts.Count;
    internal void ChangeWorld(ulong world)
    {
        World = world;
        foreach (var handle in _layers.Where(x => x.Value.World != 0).Select(x => x.Key).ToArray()) _layers.Remove(handle);
    }
    private readonly Dictionary<ulong, (nint Fn, nint User, bool Ready)> _callbacks = [];
    internal int Subscriptions => _callbacks.Count;
    internal readonly List<uint> Points = [];
    internal FakeRegistry()
    {
        _current = this;
        Stage = (RegistryStageTable*)NativeMemory.AllocZeroed(88);
        Live = (RegistryLiveTable*)NativeMemory.AllocZeroed(56);
        *Stage = new() { Size = 88, Version = 1, Status = &StageStatus, Subscribe = &Subscribe, Submit = &StageSubmit,
            Read = &ReadStage, Select = &Select, Ready = &Ready, Unsubscribe = &Unsubscribe };
        *Live = new() { Size = 56, Version = 1, Begin = &Begin, Submit = &Submit, Commit = &Commit,
            Revert = &Revert, Read = &ReadLive, Status = &Status };
        Host.Services["kiln.registry"] = (nint)Stage;
        Host.Services["kiln.registry.live"] = (nint)Live;
    }
    internal void FireStage(RegistryPhase phase)
    {
        var info = new RegistryStageInfo { Size = 24, Stage = (uint)phase };
        foreach (var callback in _callbacks.Values.Where(c => !c.Ready).ToArray())
            ((delegate* unmanaged[Cdecl]<RegistryStageInfo*, void*, void>)callback.Fn)(&info, (void*)callback.User);
    }
    internal void FireReady()
    {
        var info = new RegistryReadyInfo { Size = 24, Phase = Phase, Generation = ++Generation, World = 1 };
        foreach (var callback in _callbacks.Values.Where(c => c.Ready).ToArray())
            ((delegate* unmanaged[Cdecl]<RegistryReadyInfo*, void*, void>)callback.Fn)(&info, (void*)callback.User);
    }
    private static FakeRegistry Current => _current!;
    private static int Text(string value, byte* output, nuint capacity, nuint* length)
    {
        var bytes = Encoding.UTF8.GetBytes(value);
        if (length != null) *length = (nuint)bytes.Length;
        if (output == null || capacity <= (nuint)bytes.Length) return KilnAbi.ErrBufferTooSmall;
        bytes.CopyTo(new Span<byte>(output, bytes.Length)); output[bytes.Length] = 0; return 0;
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int StageStatus(RegistryStageStatus* value) { if (Current.StageError != 0) return Current.StageError; value->Phase = Current.Phase; value->Generation = Current.Generation; return 0; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Status(RegistryLiveStatus* value) { if (Current.StatusFailures > 0) { --Current.StatusFailures; return KilnAbi.ErrUnavailable; } value->World = Current.World; value->Kind = (uint)WorldKind.Server; value->Generation = Current.Generation; return 0; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReadStage(uint view, KilnStr registry, KilnStr id, KilnStr field, byte* output, nuint capacity, nuint* length) => Text(Current.Value.ToString(System.Globalization.CultureInfo.InvariantCulture), output, capacity, length);
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReadLive(ulong owner, KilnStr registry, KilnStr id, KilnStr field, byte* output, nuint capacity, nuint* length) => Text(Current.Value.ToString(System.Globalization.CultureInfo.InvariantCulture), output, capacity, length);
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Select(uint view, KilnStr registry, KilnStr selector, byte* output, nuint capacity, nuint* length) => Text("[\"game:wood\"]", output, capacity, length);
    private static ulong Add(nint fn, nint user, bool ready)
    {
        var handle = ++Current._next; Current._callbacks.Add(handle, (fn, user, ready)); return handle;
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Subscribe(ulong owner, delegate* unmanaged[Cdecl]<RegistryStageInfo*, void*, void> fn, void* user, ulong* handle) { *handle = Add((nint)fn, (nint)user, false); return 0; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Ready(ulong owner, delegate* unmanaged[Cdecl]<RegistryReadyInfo*, void*, void> fn, void* user, ulong* handle) { if (Current.ReadyError != 0) return Current.ReadyError; *handle = Add((nint)fn, (nint)user, true); return 0; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsubscribe(ulong handle) { Current._callbacks.Remove(handle); return 0; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int StageSubmit(ulong owner, KilnStr ir, RegistrySubmitInfo* value, byte* output, nuint capacity, nuint* length) { ++Current.Submits; value->Accepted = 1; return 0; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Begin(ulong owner, uint point, ulong* handle)
    {
        *handle = ++Current._next; Current.Points.Add(point); Current._drafts.Add(*handle, (owner, point, null, false)); return 0;
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Submit(ulong owner, ulong handle, KilnStr ir, RegistryLiveReply* result, byte* output, nuint capacity, nuint* length)
    {
        ++Current.Submits;
        var doc = JsonNode.Parse(Encoding.UTF8.GetString(ir.Ptr, checked((int)ir.Len)))!;
        var edit = doc["edits"]![0]!;
        var draft = Current._drafts[handle];
        var restart = edit["registry"]!.GetValue<string>() == "balance";
        Current._drafts[handle] = (owner, draft.Point, edit["op"]!.GetValue<int>(), restart);
        result->Accepted = restart ? 0u : 1u; result->Refused = restart ? 1u : 0u; result->Outcome = restart ? 3u : 0u;
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Commit(ulong owner, ulong handle, RegistryLiveReply* result)
    {
        if (Current.WorldRequiredCommits > 0) { --Current.WorldRequiredCommits; result->Outcome = 4; result->World = Current.World; return 0; }
        var draft = Current._drafts[handle]; Current._drafts.Remove(handle);
        result->Outcome = draft.Restart ? 3u : 0u; result->Refused = draft.Restart ? 1u : 0u;
        result->Accepted = draft.Restart ? 0u : 1u; result->World = Current.World;
        if (!draft.Restart) Current._layers.Add(handle, (owner, draft.Value!.Value, draft.Point == 1 ? 0 : Current.World));
        result->Generation = ++Current.Generation;
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Revert(ulong owner, ulong handle, RegistryLiveReply* result)
    {
        if (!Current._drafts.Remove(handle) && !Current._layers.Remove(handle)) return KilnAbi.ErrNotFound;
        result->Outcome = 5; result->Generation = ++Current.Generation; result->World = Current.World; return 0;
    }
    public void Dispose()
    {
        Host.Services.Clear(); NativeMemory.Free(Stage); NativeMemory.Free(Live); _current = null;
    }
}
