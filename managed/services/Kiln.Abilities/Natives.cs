// SPDX-License-Identifier: MIT
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

/// <summary>A copy of one observed native call (taken on the job thread right after the call).</summary>
internal readonly struct NativeSnapshot
{
    public uint Hash { get; init; }
    public ImpactRef Impact { get; init; }
    public uint Site { get; init; }
    public ulong Step { get; init; }
    public byte Result { get; init; }
    public byte Error { get; init; }
    public uint[] Args { get; init; }
    public uint[] Values { get; init; }
    public byte[] Offsets { get; init; }
    public byte[] Sizes { get; init; }
    public byte[] Payload { get; init; }

    public ReadOnlySpan<byte> Bytes(int i) =>
        i < Offsets.Length && Offsets[i] != 0xFF ? Payload.AsSpan(Offsets[i], Sizes[i]) : ReadOnlySpan<byte>.Empty;
}

/// <summary>A call of a managed Kiln native (tick thread, the step after the program called it). Read and write
/// arguments through the live instance; return the value the program gets.</summary>
public sealed unsafe class NativeInvocation
{
    private readonly uint[] _args;
    private ImpactInstance? _inst;

    internal NativeInvocation(string name, ImpactRef impact, uint site, uint[] args)
    {
        Name = name;
        Impact = impact;
        Site = site;
        _args = args;
    }

    public string Name { get; }
    public ImpactRef Impact { get; }
    /// <summary>The ECALL's call-site id (unique per call site; the editor node id).</summary>
    public uint Site { get; }
    public int ArgCount => _args.Length;
    public ImpactInstance Instance => _inst ??= new ImpactInstance(Impact.ImpactId);

    /// <summary>Argument i as passed: a handle 0xFFFF0000|index, 0 (not connected) or a raw value.</summary>
    public uint Raw(int i) => _args[i];
    public bool IsVariable(int i) => (_args[i] & 0xFFFF0000) == 0xFFFF0000;

    /// <summary>The variable argument i names (null for a raw value or a variable index out of range).</summary>
    public ImpactVariable? Variable(int i)
    {
        if (!IsVariable(i)) return null;
        var idx = (int)(_args[i] & 0xFFFF);
        var vars = Instance.Variables;
        return idx < vars.Count ? vars[idx] : null;
    }

    public int GetInt(int i) => Variable(i) is { } v && Instance.Read(v) is { Length: >= 4 } b ? BitConverter.ToInt32(b) : (int)_args[i];
    public float GetFloat(int i) => Variable(i) is { } v && Instance.Read(v) is { Length: >= 4 } b ? BitConverter.ToSingle(b) : _args[i];
    public bool GetBool(int i) => Variable(i) is { } v && Instance.Read(v) is { Length: >= 1 } b ? b[0] != 0 : _args[i] != 0;
    public EntityId GetEntity(int i) => new((uint)GetInt(i));
    public byte[]? GetBytes(int i) => Variable(i) is { } v ? Instance.Read(v) : null;

    public bool SetInt(int i, int value) => Variable(i) is { } v && Instance.Write(v, ImpactConfigs.Encode(value, v) ?? []);
    public bool SetFloat(int i, float value) => Variable(i) is { } v && Instance.Write(v, ImpactConfigs.Encode(value, v) ?? []);
    public bool SetBool(int i, bool value) => Variable(i) is { } v && Instance.Write(v, ImpactConfigs.Encode(value, v) ?? []);
    public bool SetEntity(int i, EntityId value) => SetInt(i, (int)value.Value);
}

/// <summary>A managed native: returns the value the program's ECALL pushes (0..255).</summary>
public delegate byte NativeHandler(NativeInvocation call);

/// <summary>A synchronous call of a wrapped vanilla native, on the game's job thread inside the sim step. Read or
/// change the argument variables, then let it run (<see cref="CallOriginal"/>, or nothing: it runs after the last
/// handler) or replace it (<see cref="Return"/> / <see cref="Cancel"/>). Only touch the call itself: no tick-thread
/// API, no blocking; queue other work with <c>Context.Scheduler.Post</c>.</summary>
public unsafe ref struct NativeCall
{
    private readonly AbilitiesNative.NativeCall* _c;
    // Whose handler this call is in, and whether that mod may have the raw bytes: decided once where the handler was
    // registered (AbilitiesDispatch.AddWrap), so the check on this per-hit path is a field read. Null: a handler Kiln
    // registered outside any mod.
    private readonly ModContext? _owner;
    private readonly bool _raw;
    // A call of a synchronous Kiln native (RegisterSync): there is no game function to call.
    private readonly bool _sync;

    internal NativeCall(AbilitiesNative.NativeCall* c, ModContext? owner, bool raw, bool sync = false)
    {
        _c = c;
        _owner = owner;
        _raw = raw;
        _sync = sync;
    }

    public uint Native => _c->Hash;
    public int ArgCount => (int)_c->Argc;
    public uint ImpactId => _c->ImpactId;
    public uint ProgramId => _c->ProgramId;
    public uint Site => _c->Site;
    public EntityId Self => new(_c->Self);
    /// <summary>The game's native ran (or a handler returned a value): later handlers see the outcome.</summary>
    public bool Decided => _c->OriginalCalled != 0 || _c->OutError != 0xFF;
    public byte ResultValue => _c->OutValue;

    public uint Raw(int i) => (uint)i < _c->Argc ? _c->Args[i] : throw new IndexOutOfRangeException();
    public bool IsConnected(int i) => (Raw(i) & 0xFFFF0000) == 0xFFFF0000;

    /// <summary>The bytes of argument i's variable, or empty (not connected, out of range). Writes go to the program.
    /// A raw view of the program's memory: advanced mods only (<c>"advanced": true</c> in kiln.mod.json); an ordinary
    /// mod's handler gets <see cref="InvalidOperationException"/> and uses the typed accessors instead.</summary>
    /// <remarks>The advanced permission is captured for the mod that registered the wrap, not looked up on the job
    /// thread. Kiln's wrap dispatcher catches handler exceptions before returning to native code.</remarks>
    [AdvancedApi]
    public Span<byte> Bytes(int i) => _raw ? BytesCore(i) : throw Refused();

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    private InvalidOperationException Refused() => _owner is null
        ? new InvalidOperationException("NativeCall.Bytes must be called from a mod (on the tick thread or in OnLoad)")
        : AbilitiesGate.NotAdvanced("NativeCall.Bytes", _owner);

    // The typed accessors below and DamageCall read and write through this, for every mod.
    internal Span<byte> BytesCore(int i)
    {
        var h = Raw(i);
        if ((h & 0xFFFF0000) != 0xFFFF0000) return Span<byte>.Empty;
        var idx = h & 0xFFFF;
        if (idx >= _c->LayoutCount) return Span<byte>.Empty;
        var e = _c->Layout + idx * 24;
        var size = *(ushort*)(e + 12);
        var off = *(ushort*)(e + 14);
        if ((ulong)off + size > _c->DataSize) return Span<byte>.Empty;
        return new Span<byte>(_c->Data + off, size);
    }

    public uint VariableType(int i)
    {
        var h = Raw(i);
        if ((h & 0xFFFF0000) != 0xFFFF0000 || (h & 0xFFFF) >= _c->LayoutCount) return 0;
        return *(uint*)(_c->Layout + (h & 0xFFFF) * 24 + 8);
    }

    public float GetFloat(int i) => BytesCore(i) is { Length: >= 4 } b ? BitConverter.ToSingle(b) : 0;
    public uint GetU32(int i) => BytesCore(i) is { Length: >= 4 } b ? BitConverter.ToUInt32(b) : 0;
    public EntityId GetEntity(int i) => new(GetU32(i));
    public bool SetFloat(int i, float v) => BytesCore(i) is { Length: 4 } b && BitConverter.TryWriteBytes(b, v);
    public bool SetU32(int i, uint v) => BytesCore(i) is { Length: 4 } b && BitConverter.TryWriteBytes(b, v);

    /// <summary>Runs the game's native now with the current arguments (once).</summary>
    /// <exception cref="InvalidOperationException">The call is one of a synchronous Kiln native
    /// (<see cref="Abilities.Natives.RegisterSync"/>): there is no game function to run.</exception>
    public void CallOriginal()
    {
        if (_sync) throw new InvalidOperationException("a synchronous Kiln native has no original to call");
        if (_c->OriginalCalled == 0) AbilitiesNative.T->CallOriginal(_c);
    }

    /// <summary>The native does not run; the program gets <paramref name="value"/>.</summary>
    public void Return(byte value) { _c->OutError = 0; _c->OutValue = value; }

    /// <summary>The native does not run and the program gets 0 (applyDamage: no hit; applyBuff: no buff).</summary>
    public void Cancel() => Return(0);
}

public delegate void NativeWrapHandler(ref NativeCall call);

/// <summary>A synchronous Kiln native (<see cref="Abilities.Natives.RegisterSync"/>): runs inside the program's ECALL,
/// on the thread that runs the game's Impact programs, and returns the value the ECALL pushes (0..255). Read and write
/// the arguments through <paramref name="call"/>; there is no original (<see cref="NativeCall.CallOriginal"/> throws).
/// An exception answers 0x11 (the program instance stops with Error; the game goes on).</summary>
[AnyThread]
public delegate byte NativeSyncHandler(ref NativeCall call);

/// <summary>What <see cref="Abilities.Natives.RegisterSync"/> did: <see cref="Ok"/> with the native's
/// <see cref="Hash"/>, or the game-state limit that refused it (<see cref="Reason"/>: the native registry is full, Kiln's
/// native slots are used up, the name is taken or is a vanilla native's).</summary>
/// <param name="Ok">The native is registered.</param>
/// <param name="Reason">Why not; null when <paramref name="Ok"/>.</param>
/// <param name="Hash">FNV-1a-32 of the full name (what a program's ECALL names), also when refused.</param>
public sealed record NativeResult(bool Ok, string? Reason, uint Hash) : IKilnResult;

/// <summary>A synchronous applyDamage call (job thread): change it before the game applies it, or cancel it.</summary>
public unsafe ref struct DamageCall
{
    private NativeCall _call;
    internal DamageCall(NativeCall call) => _call = call;

    public EntityId Attacker => _call.GetEntity(1);
    public EntityId Target => _call.GetEntity(2);
    public uint ImpactId => _call.ImpactId;
    public uint ProgramId => _call.ProgramId;
    public EntityId Self => _call.Self;
    public bool HasAttackMod => _call.IsConnected(6);

    /// <summary>The attack multiplier (variable AttackMod, 1.0 by default). Setting it scales this hit only: Kiln
    /// restores the variable after the call.</summary>
    public float AttackMod
    {
        get => _call.IsConnected(6) ? _call.GetFloat(6) : 1f;
        set
        {
            if (!_call.IsConnected(6)) throw new InvalidOperationException("this call site passes no AttackMod");
            _call.SetFloat(6, value);
        }
    }

    /// <summary>The damage setup argument (per-type factors), when connected.</summary>
    public DamageSetup Damage
    {
        get => DamageSetup.From(_call.BytesCore(14));
        set
        {
            var b = _call.BytesCore(14);
            if (b.Length == 52) value.ToBytes().CopyTo(b);
        }
    }

    public void Cancel() => _call.Cancel();
}

public delegate void DamageHandler(ref DamageCall call);

public static unsafe partial class Abilities
{
    /// <summary>The game's natives and Kiln's: list them, add managed natives, observe or wrap vanilla ones.</summary>
    public static class Natives
    {
        /// <summary>Every native in the server world's registry (the game's 107 plus Kiln's).</summary>
        public static IReadOnlyList<ImpactNativeInfo> All
        {
            get
            {
                var t = AbilitiesNative.T;
                if (t == null) return [];
                var n = t->NativeCount();
                var list = new List<ImpactNativeInfo>((int)n);
                for (uint i = 0; i < n; i++)
                {
                    AbilitiesNative.NativeInfo ni = default;
                    ni.Size = (uint)sizeof(AbilitiesNative.NativeInfo);
                    if (t->NativeAt(i, &ni) != AbilitiesNative.Ok) continue;
                    list.Add(new ImpactNativeInfo(ni.Hash, AbilitiesNative.Text(ni.Name, 64), (int)ni.Argc, (ImpactNativeKind)ni.Kind));
                }
                return list;
            }
        }

        private static readonly ConcurrentDictionary<uint, string> Names = new();

        public static string NameOf(uint hash)
        {
            if (Names.TryGetValue(hash, out var n)) return n;
            var t = AbilitiesNative.T;
            if (t == null) return $"0x{hash:x8}";
            AbilitiesNative.NativeInfo ni = default;
            ni.Size = (uint)sizeof(AbilitiesNative.NativeInfo);
            n = t->NativeFind(hash, &ni) == AbilitiesNative.Ok ? AbilitiesNative.Text(ni.Name, 64) : $"0x{hash:x8}";
            Names[hash] = n;
            return n;
        }

        internal sealed class Managed
        {
            public required string Name;
            public required uint Hash;
            public required NativeHandler Handler;
            public required ModContext? Mod;
        }

        internal static readonly ConcurrentDictionary<uint, Managed> ManagedNatives = new();

        /// <summary>Registers a managed native "&lt;modid&gt;.<paramref name="name"/>" (hash = FNV-1a-32 of the full
        /// name; ECALL it from a program). The handler runs on the tick thread the step after the call; the program's
        /// ECALL waits one step and then pushes the returned value. Ends when the mod unloads. Tick thread.</summary>
        [TickThreadOnly]
        [RequiresCapability(AbilitiesCaps.Service)]
        public static uint Register(string name, int argc, NativeHandler handler)
        {
            ArgumentException.ThrowIfNullOrEmpty(name);
            ArgumentNullException.ThrowIfNull(handler);
            if (argc is < 0 or > 31) throw new ArgumentOutOfRangeException(nameof(argc));
            Game.AssertTickThread();
            AbilitiesDispatch.Ensure();
            var mod = ModContext.CurrentMod;
            var full = FullName(mod, name);
            var t = AbilitiesNative.Require("Abilities.Natives.Register");
            var bytes = Encoding.UTF8.GetBytes(full);
            uint hash;
            int rc;
            fixed (byte* p = bytes) rc = t->NativeRegister(mod?.Owner ?? 0, new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, (uint)argc, &hash);
            if (rc != AbilitiesNative.Ok)
                throw new InvalidOperationException($"native {full}: {AbilitiesNative.Describe(rc)} (name taken, invalid, or a vanilla hash)");
            ManagedNatives[hash] = new Managed { Name = full, Hash = hash, Handler = handler, Mod = mod };
            Names[hash] = full;
            if (mod is not null) AbilitiesDispatch.Watch(mod);
            mod?.Log.Info($"abilities: native {full} (0x{hash:x8}, argc {argc}) registered");
            return hash;
        }

        /// <summary>Registers a synchronous native "&lt;modid&gt;.<paramref name="name"/>" (hash = FNV-1a-32 of the full
        /// name): <paramref name="handler"/> runs inside the program's ECALL, in the same simulation step, and its return
        /// value is what the ECALL pushes. Use it for quick work on the call's own arguments; <see cref="Register"/> (a
        /// handler on the tick thread, one step later) stays the tool for slow work or tick-thread API.
        /// </summary>
        /// <remarks>The handler runs on the thread that runs the game's Impact programs (the server world's thread on a
        /// dedicated server; never the tick thread's Pre or Post), so it calls no tick-thread API and does not block:
        /// queue other work with <c>Context.Scheduler.Post</c>. A handler over 0.2 ms is logged (the whole server step
        /// waits for it). The native ends when the mod unloads: from then on programs that call it stop with Error 0x1F.
        /// A program can call it once it is registered; programs registered earlier wait for it.</remarks>
        /// <param name="name">Name, or the full "&lt;modid&gt;.&lt;Name&gt;" (the mod's own id); Name is
        /// [A-Za-z0-9_].</param>
        /// <param name="argc">Values the ECALL pops, 0..31.</param>
        /// <param name="handler">The native.</param>
        /// <returns>Ok, or <c>Ok = false</c> when a game-state limit refused it (see <see cref="NativeResult"/>).</returns>
        /// <exception cref="ArgumentException">A bad name or argc, a full name with another mod's id, a null
        /// handler.</exception>
        /// <exception cref="InvalidOperationException">Not called from a mod, or not on the tick thread.</exception>
        /// <exception cref="KilnUnavailableException">Capability <c>abilities.natives.sync</c> is off here.</exception>
        [ServerOnly]
        [TickThreadOnly]
        [RequiresCapability(AbilitiesCaps.NativesSync)]
        public static NativeResult RegisterSync(string name, int argc, NativeSyncHandler handler)
        {
            ArgumentException.ThrowIfNullOrEmpty(name);
            ArgumentNullException.ThrowIfNull(handler);
            if (argc is < 0 or > 31) throw new ArgumentOutOfRangeException(nameof(argc), argc, "argc is 0..31");
            Game.AssertTickThread();
            var mod = ModContext.CurrentMod ?? throw new InvalidOperationException("Natives.RegisterSync must be called from a mod");
            var full = name.Contains('.') ? name : $"{mod.Info.Id}.{name}";
            if (!full.StartsWith(mod.Info.Id + ".", StringComparison.Ordinal))
                throw new ArgumentException($"native {full}: a mod registers natives under its own id ({mod.Info.Id}.<Name>)", nameof(name));
            if (!ValidName(full)) throw new ArgumentException($"native {full}: Name must be [A-Za-z0-9_], 90 bytes in all", nameof(name));
            AbilitiesCaps.Require(AbilitiesCaps.NativesSync);
            var t = AbilitiesNative.V2(nameof(AbilitiesNative.TableV2.NativeRegisterSync));
            if (t == null) throw new KilnUnavailableException(AbilitiesCaps.NativesSync, "kiln.abilities is older than 0.2.0-dev.29");
            AbilitiesDispatch.Ensure();
            var entry = AbilitiesDispatch.AddSync(full, handler, mod);
            var bytes = Encoding.UTF8.GetBytes(full);
            uint hash = 0, refusal = 0;
            int rc;
            fixed (byte* p = bytes)
                rc = t->NativeRegisterSync(mod.Owner, new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, (uint)argc,
                    AbilitiesDispatch.SyncDispatcher, (void*)entry.Id, &hash, &refusal);
            if (rc != AbilitiesNative.Ok)
            {
                AbilitiesDispatch.RemoveSync(entry);
                if (rc == AbilitiesNative.InvalidArgument && refusal != AbilitiesNative.RefusedNone)
                {
                    mod.Log.Warn($"abilities: sync native {full} refused: {AbilitiesNative.Refusal(refusal)}");
                    return new NativeResult(false, AbilitiesNative.Refusal(refusal), hash != 0 ? hash : AbilitiesNative.Fnv(full));
                }
                if (rc == AbilitiesNative.Unavailable) throw new KilnUnavailableException(AbilitiesCaps.NativesSync, "kiln.abilities has no natives on this build");
                if (rc == AbilitiesNative.WrongThread) throw new InvalidOperationException("Natives.RegisterSync: not on the tick thread (or inside a native call)");
                throw new ArgumentException($"native {full}: {AbilitiesNative.Describe(rc)}", nameof(name));
            }
            entry.Hash = hash;
            Names[hash] = full;
            AbilitiesDispatch.Watch(mod);
            mod.Log.Info($"abilities: sync native {full} (0x{hash:x8}, argc {argc}) registered");
            return new NativeResult(true, null, hash);
        }

        // natives_core.h valid_native_name: "<modid>.<Name>", Name [A-Za-z0-9_], 90 bytes at most
        internal static bool ValidName(string full)
        {
            var dot = full.IndexOf('.');
            if (dot <= 0 || dot + 1 >= full.Length || Encoding.UTF8.GetByteCount(full) > 90) return false;
            for (var i = dot + 1; i < full.Length; i++)
                if (!char.IsAsciiLetterOrDigit(full[i]) && full[i] != '_') return false;
            return true;
        }

        /// <summary>Lab use: the native thunks' counters (sync calls, faults, calls over budget, calls of a removed
        /// native), or null without a dev.29 kiln.abilities.</summary>
        [System.Diagnostics.CodeAnalysis.Experimental("KILNX001")]
        [System.ComponentModel.EditorBrowsable(System.ComponentModel.EditorBrowsableState.Never)]
        public static (ulong SyncCalls, ulong SyncFaults, ulong SyncSlow, ulong DeadCalls, uint Inflight)? LabCounters =>
            AbilitiesNative.Counters() is { } c ? (c.SyncCalls, c.SyncFaults, c.SyncSlow, c.DeadCalls, c.Inflight) : null;

        /// <summary>Declares a native from a mod's impact/natives.json before it is registered, so programs that call
        /// it pass the verifier now. Any thread.</summary>
        [RequiresCapability(AbilitiesCaps.Service)]
        public static void Declare(string fullName, int argc)
        {
            var t = AbilitiesNative.Require("Abilities.Natives.Declare");
            t->NativeDeclare(AbilitiesNative.Fnv(fullName), (uint)argc);
        }

        internal static string FullName(ModContext? mod, string name)
        {
            if (name.Contains('.')) return name;
            var id = mod?.Info.Id ?? throw new InvalidOperationException("Natives.Register must be called from a mod");
            return $"{id}.{name}";
        }

        internal static void Unregister(ModContext mod)
        {
            foreach (var kv in ManagedNatives)
                if (ReferenceEquals(kv.Value.Mod, mod) && ManagedNatives.TryRemove(kv.Key, out _))
                    if (AbilitiesNative.T is var t && t != null && Game.IsTickThread) t->NativeUnregister(kv.Key);
            AbilitiesDispatch.RemoveSyncs(mod);
        }

        /// <summary>Observes a vanilla native: its calls arrive as <see cref="ImpactNativeEvent"/> (applyDamage,
        /// applyBuff and spawnEntity are observed by default). Tick thread.</summary>
        [RequiresCapability(AbilitiesCaps.Service)]
        public static void Observe(string nameOrHash)
        {
            var t = AbilitiesNative.Require("Abilities.Natives.Observe");
            var rc = t->NativeObserve(Resolve(nameOrHash), 1);
            if (rc != AbilitiesNative.Ok) throw new InvalidOperationException($"Natives.Observe({nameOrHash}): {AbilitiesNative.Describe(rc)}");
        }

        internal static uint Resolve(string nameOrHash)
        {
            if (nameOrHash.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                return Convert.ToUInt32(nameOrHash[2..], 16);
            foreach (var n in All)
                if (string.Equals(n.Name, nameOrHash, StringComparison.OrdinalIgnoreCase)) return n.Hash;
            // vanilla hashes are FNV-1a of the PascalCase node name ("ApplyDamage")
            var pascal = char.ToUpperInvariant(nameOrHash[0]) + nameOrHash[1..];
            return AbilitiesNative.Fnv(pascal);
        }

        /// <summary>Wraps a vanilla native: <paramref name="handler"/> runs synchronously on the game's job thread for
        /// every call, before the game's function (see <see cref="NativeCall"/> for the rules). Several handlers run by
        /// <paramref name="priority"/> (highest first). Dispose the handle, or it ends when the mod unloads.</summary>
        [TickThreadOnly]
        [RequiresCapability(AbilitiesCaps.Service)]
        public static IDisposable Wrap(string nameOrHash, NativeWrapHandler handler, int priority = 0)
        {
            ArgumentNullException.ThrowIfNull(handler);
            return AbilitiesDispatch.AddWrap(Resolve(nameOrHash), handler, priority);
        }
    }

    /// <summary>Synchronous applyDamage hook (job thread): scale or cancel a program's hit before the game computes it
    /// (armour, crit and resistances still apply after). <see cref="DamageCall.AttackMod"/> changes only this call.</summary>
    [TickThreadOnly]
    public static IDisposable OnDamage(DamageHandler handler, int priority = 0)
    {
        ArgumentNullException.ThrowIfNull(handler);
        return AbilitiesDispatch.AddWrap(DamageHash, DamageWrap(handler), priority);
    }

    internal const uint DamageHash = 0xb04d0e7a;  // applyDamage

    // The wrap OnDamage registers: its DamageCall reads and writes through NativeCall's internal core, so an ordinary
    // mod's damage handler works although NativeCall.Bytes is gated.
    internal static NativeWrapHandler DamageWrap(DamageHandler handler) =>
        (ref NativeCall c) =>
        {
            var before = c.IsConnected(6) ? c.GetFloat(6) : 1f;
            var d = new DamageCall(c);
            handler(ref d);
            if (c.IsConnected(6) && !c.Decided && c.GetFloat(6) != before)
            {
                // run the hit with the changed multiplier, then restore the program's own value
                c.CallOriginal();
                c.SetFloat(6, before);
            }
        };

    /// <summary>Holds only instances of these programs for <see cref="ImpactStartingEvent"/> (the union over all
    /// callers; no filter = every new instance is held while anyone subscribes). Dispose to drop the filter.</summary>
    public static IDisposable HoldPrograms(params uint[] programIds) => AbilitiesDispatch.AddHoldFilter(programIds);
}
