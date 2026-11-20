// SPDX-License-Identifier: MIT
// The managed side of the native facade kiln.mod-values (native/core/abi/services/kiln_mod_values.h; dossier chapter
// 05b section 5): at its start Kiln.ModValues hands the core its process-lifetime backend functions through the
// host-only table kiln.mod-values.backend v1 (bind), and forwards the ledger's events to native subscribers (dispatch)
// while the core says one is there (wanted). The core has checked the caller (owner, namespace, argument shapes); here
// the explicit namespace goes to the same ledger, registry and synced copy managed mods use. Nothing here throws across
// the boundary: an exception becomes a KilnResult (ArgumentException INVALID_ARGUMENT, KilnUnavailableException
// UNAVAILABLE, the tick-thread check WRONG_THREAD, anything else INTERNAL, logged).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

/// <summary>Mirrors of kiln_mod_values.h v1, field by field. Sizes: character 24, version 24, definition 144, value 72,
/// result 184, definition result 16, change 248, export 24, backend functions 56, backend table 32 (x64; pinned by
/// Kiln.ModValues.Tests' NativeBackendTests against the header's static asserts).</summary>
internal static unsafe class ModValuesAbi
{
    public const string BackendService = "kiln.mod-values.backend";
    public const uint BackendVersion = 1;

    // KilnResult
    public const int Ok = 0, InvalidArgument = 1, Unavailable = 3, WrongThread = 4, Internal = 7;

    // KilnModResultFlags, KilnModEventScope, KMV_WANT_*, KilnModDefFlags
    public const uint Before = 1, After = 2;
    public const uint CanonicalEvents = 0, SyncedEvents = 1;
    public const uint WantCanonical = 1, WantSynced = 2;
    public const uint TrustImports = 1, AllowPartitionMove = 2, ResetKindChange = 4, HasInitial = 8;

    // KilnModEventKind
    public const uint Initialized = 0, Changed = 1, Migrated = 2, Imported = 3, AvailabilityChanged = 4, ResetValue = 5, EpochReset = 6;

    [StructLayout(LayoutKind.Sequential)]
    public struct CharacterRec
    {
        public ulong Account;
        public fixed byte Guid[16];  // RFC 4122 byte order
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct VersionRec
    {
        public fixed byte World[16];
        public ulong Revision;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct DefinitionRec
    {
        public uint Size, Kind, Persistence, Scale, Flags, Version, CarryEpoch, ReadPolicy, SyncPolicy;
        public NativeStr LocalId, LabelJson, NumberFormatJson;
        public long Minimum, Maximum, Initial;
        public long* Thresholds;
        public uint ThresholdCount, Reserved0;
        public NativeStr* ReadMods;
        public uint ReadModCount, Reserved1;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ValueRec
    {
        public uint Size, Kind, Persistence, Scale, Level, DefinitionVersion, CarryEpoch, Reserved;
        public long Value;
        public ulong ValueRevision, PartitionRevision;
        public fixed byte World[16];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ResultRec
    {
        public uint Size, Ok, Reason, Flags;
        public CharacterRec Character;
        public ValueRec Before, After;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct DefinitionResultRec
    {
        public uint Size, Ok, Reason, Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ChangeRec
    {
        public uint Size, EventKind, EventScope, Reserved;
        public ulong RequestId;
        public CharacterRec Character;
        public NativeStr QualifiedKey;
        public ResultRec Result;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ExportRec
    {
        public uint Size, ReadPolicy, SyncPolicy, ReadModCount;
        public NativeStr* ReadMods;
    }

    /// <summary>KilnModValuesBackend: what bind hands the core.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct BackendFns
    {
        public uint Size, Reserved;
        public delegate* unmanaged[Cdecl]<NativeStr, DefinitionRec*, DefinitionResultRec*, int> Describe;
        public delegate* unmanaged[Cdecl]<NativeStr, DefinitionRec*, DefinitionResultRec*, int> Define;
        public delegate* unmanaged[Cdecl]<NativeStr, CharacterRec*, NativeStr, ResultRec*, int> Read;
        public delegate* unmanaged[Cdecl]<NativeStr, CharacterRec*, NativeStr, uint, long, VersionRec*, ulong, ResultRec*, int> Change;
        public delegate* unmanaged[Cdecl]<NativeStr, NativeStr, ResultRec*, int> ReadSynced;
        public delegate* unmanaged[Cdecl]<NativeStr, void> Retire;
    }

    /// <summary>KilnModValuesBackendApi: the core's host-only table.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct BackendTable
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<BackendFns*, int> Bind;
        public delegate* unmanaged[Cdecl]<uint> Wanted;
        public delegate* unmanaged[Cdecl]<ChangeRec*, ExportRec*, int> Dispatch;
    }
}

internal static unsafe class ModValuesNative
{
    private static readonly object Gate = new();
    private static bool _tried;
    private static ModValuesAbi.BackendTable* _table;
    private static int _logged;

    /// <summary>The tick-thread check failed (KILN_ERR_WRONG_THREAD), as distinct from any other
    /// InvalidOperationException (KILN_ERR_INTERNAL).</summary>
    private sealed class WrongThreadException(string message) : Exception(message);

    private static readonly UTF8Encoding StrictUtf8 = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);

    /// <summary>Binds the backend once per process (Kiln.ModValues' start, the first service tick): without
    /// kiln.mod-values.backend (an older core, no core) native callers get UNAVAILABLE and nothing else changes.</summary>
    internal static void Bind()
    {
        lock (Gate)
        {
            if (_tried) return;
            _tried = true;
        }
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // the subscriptions are Kiln's
        try
        {
            var t = (ModValuesAbi.BackendTable*)Backend.Current.Service(ModValuesAbi.BackendService, ModValuesAbi.BackendVersion);
            if (t == null) return;  // no core, or one without the native facade
            if (t->Header.Size < (uint)sizeof(ModValuesAbi.BackendTable))
            {
                ModValuesService.Log(LogLevel.Warn, $"Kiln.ModValues: {ModValuesAbi.BackendService} has a {t->Header.Size}-byte table; native callers stay unavailable");
                return;
            }
            var fns = Functions();
            var rc = t->Bind(&fns);
            if (rc != ModValuesAbi.Ok)
            {
                ModValuesService.Log(LogLevel.Warn, $"Kiln.ModValues: the core refused the native backend (rc {rc}); native callers stay unavailable");
                return;
            }
            _table = t;
            Events.On<ModValueChangedEvent>(OnChanged);
            Events.On<ModValueSyncedEvent>(OnSynced);
        }
        catch (Exception e)
        {
            ModValuesService.Log(LogLevel.Warn, $"Kiln.ModValues: binding the native backend failed: {e.Message}");
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    /// <summary>The functions bind hands over (process lifetime: static methods).</summary>
    internal static ModValuesAbi.BackendFns Functions() => new()
    {
        Size = (uint)sizeof(ModValuesAbi.BackendFns),
        Describe = &DescribeEntry,
        Define = &DefineEntry,
        Read = &ReadEntry,
        Change = &ChangeEntry,
        ReadSynced = &ReadSyncedEntry,
        Retire = &RetireEntry,
    };

    // ------------------------------------------------------------------------------------------- entry points ----

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int DescribeEntry(NativeStr ns, ModValuesAbi.DefinitionRec* def, ModValuesAbi.DefinitionResultRec* o) =>
        Describe(d =>
        {
            ModValuesCaps.Require(ModValuesCaps.Definitions);
            return ModValuesService.Describe(d, token: null);
        }, ns, def, o);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int DefineEntry(NativeStr ns, ModValuesAbi.DefinitionRec* def, ModValuesAbi.DefinitionResultRec* o) =>
        Describe(d =>
        {
            TickLedger("kiln.mod-values define");
            return ModValuesService.Define(d, token: null);
        }, ns, def, o, "define");

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReadEntry(NativeStr ns, ModValuesAbi.CharacterRec* c, NativeStr key, ModValuesAbi.ResultRec* o) =>
        Read(() => TickLedger("kiln.mod-values read"), ns, c, key, o);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ChangeEntry(NativeStr ns, ModValuesAbi.CharacterRec* c, NativeStr key, uint op, long amount,
        ModValuesAbi.VersionRec* expected, ulong requestId, ModValuesAbi.ResultRec* o) =>
        Change(() => TickLedger("kiln.mod-values change"), ns, c, key, op, amount, expected, requestId, o);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReadSyncedEntry(NativeStr ns, NativeStr key, ModValuesAbi.ResultRec* o) =>
        ReadSynced((reader, k) =>
        {
            ModValuesCaps.Require(ModValuesCaps.Sync);
            return ModValuesClient.Read(reader, k);
        }, () => ModValuesClient.State.Character, ns, key, o);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void RetireEntry(NativeStr ns)
    {
        try
        {
            var owner = OwnerOf(ns);
            if (Backend.Current.IsTickThread)
            {
                ModValuesService.Retire(owner, token: null);
                return;
            }
            // a revoke off the tick thread: the definitions leave now, their events go out at the next step
            foreach (var d in ModValuesService.Registry.Retire(owner, token: null))
                if (Events.HasSubscribers<ModValueDefinitionChangedEvent>()) Events.Enqueue(new ModValueDefinitionChangedEvent(d.Key, null, "retired"));
            ModValuesStream.DefinitionsChanged();
        }
        catch (Exception e)
        {
            Fail("retire", e);
        }
    }

    /// <summary>The ledger for a tick-thread call: stats.own on (else UNAVAILABLE), the tick thread (else WRONG_THREAD).</summary>
    private static Ledger TickLedger(string api)
    {
        ModValuesCaps.Require(ModValuesCaps.Own);
        try
        {
            Services.RequireTickThread(api);
        }
        catch (InvalidOperationException e)
        {
            throw new WrongThreadException(e.Message);
        }
        return ModValuesService.Ledger ?? throw ServiceCaps.Unavailable(ModValuesCaps.Own, "Kiln.ModValues did not start");
    }

    // ------------------------------------------------------------------- the calls (tests pass their own seams) ----

    /// <summary>describe or define: the definition from its record, then <paramref name="apply"/> (which checks the
    /// capability and thread). Arguments are checked first, as the core's order says.</summary>
    internal static int Describe(Func<Def, ModValueDefinitionResult> apply, NativeStr ns, ModValuesAbi.DefinitionRec* def,
        ModValuesAbi.DefinitionResultRec* o, string what = "describe")
    {
        try
        {
            if (def == null || o == null) return ModValuesAbi.InvalidArgument;
            var r = apply(DefOf(OwnerOf(ns), def));
            o->Ok = r.Ok ? 1u : 0u;
            o->Reason = r.Ok ? 0u : ModValueReasons.Code(r.Reason);
            return ModValuesAbi.Ok;
        }
        catch (Exception e)
        {
            return Fail(what, e);
        }
    }

    internal static int Read(Func<Ledger> ledger, NativeStr ns, ModValuesAbi.CharacterRec* c, NativeStr key, ModValuesAbi.ResultRec* o)
    {
        try
        {
            if (c == null || o == null) return ModValuesAbi.InvalidArgument;
            var owner = OwnerOf(ns);
            var who = CharacterOf(c);
            var k = KeyOf(key);
            Fill(o, ledger().Read(owner, who, k), who);
            return ModValuesAbi.Ok;
        }
        catch (Exception e)
        {
            return Fail("read", e);
        }
    }

    internal static int Change(Func<Ledger> ledger, NativeStr ns, ModValuesAbi.CharacterRec* c, NativeStr key, uint op, long amount,
        ModValuesAbi.VersionRec* expected, ulong requestId, ModValuesAbi.ResultRec* o)
    {
        try
        {
            if (c == null || o == null) return ModValuesAbi.InvalidArgument;
            var owner = OwnerOf(ns);
            var who = CharacterOf(c);
            var k = KeyOf(key);
            if (op > (uint)ChangeOp.Reset) throw new ArgumentException($"unknown operation {op}", nameof(op));
            ModValueVersion? want = expected == null ? null : new ModValueVersion(GuidOf(expected->World), expected->Revision);
            Fill(o, ledger().Change(owner, who, k, (ChangeOp)op, amount, want, requestId), who);
            return ModValuesAbi.Ok;
        }
        catch (Exception e)
        {
            return Fail("change", e);
        }
    }

    internal static int ReadSynced(Func<string, ModValueKey, ModValueReadResult> read, Func<CharacterKey> local, NativeStr ns, NativeStr key,
        ModValuesAbi.ResultRec* o)
    {
        try
        {
            if (o == null) return ModValuesAbi.InvalidArgument;
            var owner = OwnerOf(ns);
            var k = KeyOf(key);
            var r = read(owner, k);
            Fill(o, r, r.Value?.Character ?? local());
            return ModValuesAbi.Ok;
        }
        catch (Exception e)
        {
            return Fail("read_synced", e);
        }
    }

    /// <summary>The one error rule across the boundary.</summary>
    internal static int Code(Exception e) => e switch
    {
        WrongThreadException => ModValuesAbi.WrongThread,
        KilnUnavailableException => ModValuesAbi.Unavailable,
        ArgumentException => ModValuesAbi.InvalidArgument,
        _ => ModValuesAbi.Internal,
    };

    private static int Fail(string what, Exception e)
    {
        var code = Code(e);
        if (code == ModValuesAbi.Internal && Interlocked.Increment(ref _logged) <= 20)
        {
            try
            {
                ModValuesService.Log(LogLevel.Error, $"Kiln.ModValues: native {what} failed: {e}");
            }
            catch (Exception)
            {
                // never let a log line cross the boundary either
            }
        }
        return code;
    }

    // ------------------------------------------------------------------------------------------- conversions ----

    /// <summary>The bytes of a GUID in RFC 4122 (network) order: 00112233-4455-6677-8899-aabbccddeeff is 00 11 .. ff.</summary>
    internal static void WriteGuid(Guid g, byte* rfc4122)
    {
        if (!g.TryWriteBytes(new Span<byte>(rfc4122, 16), bigEndian: true, out _)) throw new InvalidOperationException("a GUID takes 16 bytes");
    }

    internal static Guid GuidOf(byte* rfc4122) => new(new ReadOnlySpan<byte>(rfc4122, 16), bigEndian: true);

    internal static CharacterKey CharacterOf(ModValuesAbi.CharacterRec* c) => new(c->Account, GuidOf(c->Guid));

    internal static void Write(ModValuesAbi.CharacterRec* to, CharacterKey c)
    {
        to->Account = c.Account;
        WriteGuid(c.Id, to->Guid);
    }

    internal static string Utf8(NativeStr s, string what)
    {
        if (s.Len == 0) return "";
        if (s.Ptr == null || s.Len > int.MaxValue) throw new ArgumentException($"{what}: no bytes");
        try
        {
            return StrictUtf8.GetString(s.Ptr, (int)s.Len);
        }
        catch (DecoderFallbackException)
        {
            throw new ArgumentException($"{what} is not UTF-8");
        }
    }

    /// <summary>The namespace the core passed (it validated it; checked again, it is a name we write under).</summary>
    internal static string OwnerOf(NativeStr ns)
    {
        var s = Utf8(ns, "the owner's namespace");
        return ModValueRules.IsOwner(s) ? s : throw new ArgumentException($"\"{s}\" is not a namespace");
    }

    internal static ModValueKey KeyOf(NativeStr key)
    {
        var s = Utf8(key, "the qualified key");
        return ModValueKey.TryParse(s, out var k) ? k : throw new ArgumentException($"\"{s}\" is not a key: namespace:id", nameof(key));
    }

    /// <summary>A definition record as Kiln.ModValues' checked definition (its rules throw ArgumentException). Strings
    /// and arrays are copied here.</summary>
    internal static Def DefOf(string owner, ModValuesAbi.DefinitionRec* d)
    {
        if (d->Size < (uint)sizeof(ModValuesAbi.DefinitionRec)) throw new ArgumentException($"a definition record of {d->Size} bytes (v1 is {sizeof(ModValuesAbi.DefinitionRec)})");
        const uint known = ModValuesAbi.TrustImports | ModValuesAbi.AllowPartitionMove | ModValuesAbi.ResetKindChange | ModValuesAbi.HasInitial;
        if ((d->Flags & ~known) != 0) throw new ArgumentException($"unknown definition flags 0x{d->Flags & ~known:x}");
        if (d->Scale > ModValueRules.MaxScale) throw new ArgumentException($"scale is 0 to {ModValueRules.MaxScale}, not {d->Scale}");
        if (d->ThresholdCount > ModValueRules.MaxThresholds) throw new ArgumentException($"at most {ModValueRules.MaxThresholds} thresholds");
        if (d->ReadModCount > ModValueRules.MaxReadMods) throw new ArgumentException($"at most {ModValueRules.MaxReadMods} read_mods");
        if (d->ThresholdCount > 0 && d->Thresholds == null || d->ReadModCount > 0 && d->ReadMods == null) throw new ArgumentException("a count without its array");
        var id = Utf8(d->LocalId, "local_id");
        var name = $"mod value {owner}:{id}";
        var label = ModValueRules.MapOfJson(Utf8(d->LabelJson, $"{name}: label_json"), $"{name}: label");
        var format = d->NumberFormatJson.Len == 0 ? null : ModValueRules.MapOfJson(Utf8(d->NumberFormatJson, $"{name}: number_format_json"), $"{name}: number format");
        var thresholds = new long[d->ThresholdCount];
        for (var i = 0; i < thresholds.Length; i++) thresholds[i] = d->Thresholds[i];
        var readMods = new string[d->ReadModCount];
        for (var i = 0; i < readMods.Length; i++) readMods[i] = Utf8(d->ReadMods[i], $"{name}: read_mods[{i}]");
        return Def.Create(owner, id, (ModValueKind)d->Kind, label, format, d->Minimum, d->Maximum,
            (d->Flags & ModValuesAbi.HasInitial) != 0 ? d->Initial : null, (int)d->Scale, thresholds, (ModValuePersistence)d->Persistence,
            (d->Flags & ModValuesAbi.TrustImports) != 0, d->Version, d->CarryEpoch, (ModValueReadPolicy)d->ReadPolicy, readMods,
            (ModValueSyncPolicy)d->SyncPolicy, (d->Flags & ModValuesAbi.AllowPartitionMove) != 0, (d->Flags & ModValuesAbi.ResetKindChange) != 0);
    }

    internal static void Fill(ModValuesAbi.ValueRec* v, ModValueSnapshot s)
    {
        v->Size = (uint)sizeof(ModValuesAbi.ValueRec);
        v->Kind = (uint)s.Kind;
        v->Persistence = (uint)s.Persistence;
        v->Scale = (uint)s.Scale;
        v->Level = (uint)Math.Max(0, s.Level);
        v->DefinitionVersion = s.DefinitionVersion;
        v->CarryEpoch = s.CarryEpoch;
        v->Reserved = 0;
        v->Value = s.Value;
        v->ValueRevision = s.ValueRevision;
        v->PartitionRevision = s.PartitionRevision;
        WriteGuid(s.World, v->World);
    }

    /// <summary>A read: AFTER when there is a value; the reason's KMV_* code when refused.</summary>
    internal static void Fill(ModValuesAbi.ResultRec* o, ModValueReadResult r, CharacterKey character)
    {
        o->Ok = r.Ok ? 1u : 0u;
        o->Reason = r.Ok ? 0u : ModValueReasons.Code(r.Reason);
        o->Flags = 0;
        Write(&o->Character, character);
        if (r.Ok && r.Value is { } v)
        {
            Fill(&o->After, v);
            o->Flags = ModValuesAbi.After;
        }
    }

    /// <summary>A change: BEFORE and AFTER for an accepted one.</summary>
    internal static void Fill(ModValuesAbi.ResultRec* o, ModValueResult r, CharacterKey character)
    {
        o->Ok = r.Ok ? 1u : 0u;
        o->Reason = r.Ok ? 0u : ModValueReasons.Code(r.Reason);
        o->Flags = 0;
        Write(&o->Character, character);
        if (!r.Ok) return;
        if (r.Before is { } b)
        {
            Fill(&o->Before, b);
            o->Flags |= ModValuesAbi.Before;
        }
        if (r.After is { } a)
        {
            Fill(&o->After, a);
            o->Flags |= ModValuesAbi.After;
        }
    }

    /// <summary>KilnModEventKind of a <see cref="ModValueChangedEvent.Cause"/>.</summary>
    internal static uint EventKind(string cause) => cause switch
    {
        ModValueCauses.Initialized => ModValuesAbi.Initialized,
        ModValueCauses.Migrated => ModValuesAbi.Migrated,
        ModValueCauses.Imported => ModValuesAbi.Imported,
        ModValueCauses.Reset => ModValuesAbi.ResetValue,
        ModValueCauses.EpochReset => ModValuesAbi.EpochReset,
        _ => ModValuesAbi.Changed,
    };

    // ---------------------------------------------------------------------------------------------- events ----

    private static void OnChanged(ModValueChangedEvent e)
    {
        var t = _table;
        if (t == null || (t->Wanted() & ModValuesAbi.WantCanonical) == 0) return;
        try
        {
            var d = ModValuesService.Registry.Active(e.Key);  // the value's export policy now
            Dispatch(t, ModValuesAbi.CanonicalEvents, EventKind(e.Cause), e.RequestId, e.Character, e.Key, e.Before, e.After,
                d?.ReadPolicy ?? ModValueReadPolicy.OwnerOnly, d?.Sync ?? ModValueSyncPolicy.None, d?.ReadMods ?? []);
        }
        catch (Exception x)
        {
            Fail("dispatch", x);
        }
    }

    private static void OnSynced(ModValueSyncedEvent e)
    {
        var t = _table;
        if (t == null || (t->Wanted() & ModValuesAbi.WantSynced) == 0) return;
        try
        {
            var state = ModValuesClient.State;
            state.Definitions.TryGetValue(e.Key.ToString(), out var d);
            var kind = e.Before is not null && e.After is not null ? ModValuesAbi.Changed : ModValuesAbi.AvailabilityChanged;
            var character = (e.After ?? e.Before)?.Character ?? state.Character;
            Dispatch(t, ModValuesAbi.SyncedEvents, kind, 0, character, e.Key, e.Before, e.After,
                (ModValueReadPolicy)(d?.ReadPolicy ?? 0), (ModValueSyncPolicy)(d?.Sync ?? 0), d?.ReadMods ?? []);
        }
        catch (Exception x)
        {
            Fail("dispatch", x);
        }
    }

    /// <summary>One event as a KilnModValueChange (borrowed by the core until dispatch returns).</summary>
    internal static int Dispatch(ModValuesAbi.BackendTable* t, uint scope, uint kind, ulong requestId, CharacterKey character, ModValueKey key,
        ModValueSnapshot? before, ModValueSnapshot? after, ModValueReadPolicy read, ModValueSyncPolicy sync, IReadOnlyList<string> readMods)
    {
        var table = (IntPtr)t;
        return WithChange(scope, kind, requestId, character, key, before, after, read, sync, readMods,
            (c, x) => ((ModValuesAbi.BackendTable*)table)->Dispatch(c, x));
    }

    /// <summary>Receives an event's records (valid only during the call).</summary>
    internal delegate int ChangeSink(ModValuesAbi.ChangeRec* change, ModValuesAbi.ExportRec* export);

    /// <summary>Builds the event's records and hands them to <paramref name="send"/>.</summary>
    internal static int WithChange(uint scope, uint kind, ulong requestId, CharacterKey character, ModValueKey key, ModValueSnapshot? before,
        ModValueSnapshot? after, ModValueReadPolicy read, ModValueSyncPolicy sync, IReadOnlyList<string> readMods, ChangeSink send)
    {
        var rec = new ModValuesAbi.ChangeRec
        {
            Size = (uint)sizeof(ModValuesAbi.ChangeRec), EventKind = kind, EventScope = scope, RequestId = requestId,
        };
        Write(&rec.Character, character);
        rec.Result.Size = (uint)sizeof(ModValuesAbi.ResultRec);
        rec.Result.Ok = 1;
        rec.Result.Character = rec.Character;
        rec.Result.Before.Size = rec.Result.After.Size = (uint)sizeof(ModValuesAbi.ValueRec);
        if (before is not null)
        {
            Fill(&rec.Result.Before, before);
            rec.Result.Flags |= ModValuesAbi.Before;
        }
        if (after is not null)
        {
            Fill(&rec.Result.After, after);
            rec.Result.Flags |= ModValuesAbi.After;
        }
        var n = Math.Min(readMods.Count, ModValueRules.MaxReadMods);
        var keyText = key.ToString();
        var lengths = new int[n + 1];
        lengths[0] = Encoding.UTF8.GetByteCount(keyText);
        var total = lengths[0];
        for (var i = 0; i < n; i++) total += lengths[i + 1] = Encoding.UTF8.GetByteCount(readMods[i]);
        var bytes = new byte[Math.Max(1, total)];
        var at = Encoding.UTF8.GetBytes(keyText.AsSpan(), bytes.AsSpan());
        for (var i = 0; i < n; i++) at += Encoding.UTF8.GetBytes(readMods[i].AsSpan(), bytes.AsSpan(at));
        NativeStr* mods = stackalloc NativeStr[Math.Max(1, n)];
        fixed (byte* p = bytes)
        {
            rec.QualifiedKey = new NativeStr { Ptr = p, Len = (nuint)lengths[0] };
            var offset = lengths[0];
            for (var i = 0; i < n; i++)
            {
                mods[i] = new NativeStr { Ptr = p + offset, Len = (nuint)lengths[i + 1] };
                offset += lengths[i + 1];
            }
            var export = new ModValuesAbi.ExportRec
            {
                Size = (uint)sizeof(ModValuesAbi.ExportRec), ReadPolicy = (uint)read, SyncPolicy = (uint)sync, ReadModCount = (uint)n,
                ReadMods = n > 0 ? mods : null,
            };
            return send(&rec, &export);
        }
    }
}
