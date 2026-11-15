// SPDX-License-Identifier: MIT
// [ImpactNative] (since 0.2.0-dev.29, chapter 06c §4, §7.2, §9.5, §9.7): a mod's static C# method becomes a synchronous
// Impact native with typed parameters. The source generator Kiln.ImpactNatives (managed/Kiln.Analyzers/src) writes, per
// method, a thunk that decodes the call's arguments by the method's parameter types, a NativeRef in the namespace's
// Natives class, and the impact/natives.json entry (an assembly attribute the SDK's KilnImpactNatives task writes out).
// The generated module initializer hands its registration to ImpactNativeAccess.Provide; Kiln.Abilities runs it at the
// next tick under the mod's own context (DeclareTyped, then RegisterSync), so the mod writes no glue.
using System.Collections.Concurrent;
using System.ComponentModel;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>Makes a static method of a mod a synchronous Impact native "&lt;modid&gt;.<see cref="Name"/>" (since
/// 0.2.0-dev.29). The method's parameters are the native's arguments, in ECALL order (the first is <c>args[0]</c>,
/// the value the program pushes last); an optional first parameter <c>in NativeContext</c> is the call itself and no
/// argument. Parameter types: <c>int</c> (sint32), <c>uint</c> (uint32), <c>float</c> (float32), <c>bool</c>,
/// <c>TimeSpan</c> (Time), <see cref="EntityId"/> (ecs.EntityId), <see cref="Vector3"/> (float3), <see cref="DamageSetup"/>
/// (impact.DamageSetup); <c>ref</c> or <c>out</c> for an argument the native writes (a live view of the program's
/// variable: a write is there at once; a <c>ref TimeSpan</c> is copied back after the method); <see cref="Optional{T}"/>
/// for one the program may leave not connected; <c>[Raw] uint</c> for one the program may pass as a raw integer. The
/// return type is <c>bool</c> or <c>byte</c> (what the ECALL pushes) or <c>void</c> (pushes 1). The build generates
/// the native's registration, its typed declaration in <c>impact/natives.json</c> and a <see cref="NativeRef"/>
/// <c>Natives.&lt;Name&gt;</c>; a shape it cannot generate is a build error (KILN170 to KILN173). The method runs as
/// a <see cref="NativeSyncHandler"/> does: inside the program's call, no tick-thread API, no blocking.</summary>
/// <param name="name">Name: <c>[A-Za-z0-9_]</c>, the mod's id is put in front of it.</param>
[AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = false)]
public sealed class ImpactNativeAttribute(string name) : Attribute
{
    /// <summary>The native's name without the mod id.</summary>
    public string Name { get; } = name;
}

/// <summary>On a <c>uint</c> parameter of an <see cref="ImpactNativeAttribute">[ImpactNative]</see> method: the program
/// may pass a raw integer (a literal) as well as a variable.</summary>
[AttributeUsage(AttributeTargets.Parameter, AllowMultiple = false, Inherited = false)]
public sealed class RawAttribute : Attribute;

/// <summary>An argument of an <see cref="ImpactNativeAttribute">[ImpactNative]</see> method that the program may leave
/// not connected (0): <see cref="HasValue"/> false then.</summary>
/// <typeparam name="T">One of the parameter types of <see cref="ImpactNativeAttribute"/>.</typeparam>
public readonly struct Optional<T> where T : struct
{
    private readonly T _value;

    /// <summary>A connected argument.</summary>
    public Optional(T value)
    {
        _value = value;
        HasValue = true;
    }

    /// <summary>The program connected the argument.</summary>
    public bool HasValue { get; }

    /// <summary>The value.</summary>
    /// <exception cref="InvalidOperationException">The argument is not connected.</exception>
    public T Value => HasValue ? _value : throw new InvalidOperationException("the program did not connect this argument");

    /// <summary>The value, or <paramref name="fallback"/> when the argument is not connected.</summary>
    public T GetValueOrDefault(T fallback = default) => HasValue ? _value : fallback;

    /// <inheritdoc/>
    public override string ToString() => HasValue ? $"{_value}" : "(not connected)";
}

/// <summary>The call an <see cref="ImpactNativeAttribute">[ImpactNative]</see> method answers (its optional first
/// parameter, <c>in NativeContext ctx</c>): which instance, program and call site.</summary>
[AnyThread]
public readonly ref struct NativeContext
{
    private readonly NativeCall _call;

    internal NativeContext(NativeCall call) => _call = call;

    /// <summary>The instance's own entity.</summary>
    public EntityId Self => _call.Self;

    /// <summary>The program instance's id.</summary>
    public uint ImpactId => _call.ImpactId;

    /// <summary>The calling program's id (FNV-1a-32 of its GUID).</summary>
    public uint Program => _call.ProgramId;

    /// <summary>The ECALL's call-site id.</summary>
    public uint Site => _call.Site;
}

/// <summary>One parameter of an <see cref="ImpactNativeAttribute">[ImpactNative]</see> native (<see cref="NativeRef"/>):
/// its C# name and its typed declaration.</summary>
/// <param name="Name">The parameter's name in the C# method (what Blocks bind by).</param>
/// <param name="Arg">Its type, size, direction and flags.</param>
public sealed record NativeParameter(string Name, NativeArg Arg);

/// <summary>A mod's own <see cref="ImpactNativeAttribute">[ImpactNative]</see> native, as the build generated it
/// (<c>Natives.&lt;Name&gt;</c>): its name, its parameters and, once Kiln registered it (at the first tick after the
/// mod loaded), its full name and the result.</summary>
public sealed class NativeRef
{
    private readonly NativeParameter[] _parameters;

    /// <summary>Generated code: one per [ImpactNative] method.</summary>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public NativeRef(string name, NativeParameter[] parameters)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        ArgumentNullException.ThrowIfNull(parameters);
        Name = name;
        _parameters = [.. parameters];
    }

    /// <summary>The native's name without the mod id.</summary>
    public string Name { get; }

    /// <summary>The parameters in ECALL order (argument 0 first).</summary>
    public IReadOnlyList<NativeParameter> Parameters => _parameters;

    /// <summary>"&lt;modid&gt;.&lt;Name&gt;" once registered; null before.</summary>
    public string? FullName { get; private set; }

    /// <summary>The registration's result; null until Kiln registered it.</summary>
    public NativeResult? Registration { get; private set; }

    /// <summary>The hash a program's ECALL names for this native in mod <paramref name="modId"/> (FNV-1a-32 of
    /// "&lt;modid&gt;.&lt;Name&gt;").</summary>
    public uint HashFor(string modId)
    {
        ArgumentException.ThrowIfNullOrEmpty(modId);
        return AbilitiesNative.Fnv($"{modId}.{Name}");
    }

    /// <summary>Generated code: registers the native for the mod whose code runs (its typed declaration, then
    /// <see cref="Abilities.Natives.RegisterSync"/>). Tick thread.</summary>
    [EditorBrowsable(EditorBrowsableState.Never)]
    [TickThreadOnly]
    public NativeResult Register(NativeSyncHandler thunk)
    {
        ArgumentNullException.ThrowIfNull(thunk);
        var mod = ModContext.CurrentMod ?? throw new InvalidOperationException("NativeRef.Register must run for a mod");
        var full = $"{mod.Info.Id}.{Name}";
        Abilities.Natives.DeclareTyped(full, [.. _parameters.Select(p => p.Arg)]);
        var r = Abilities.Natives.RegisterSync(Name, _parameters.Length, thunk);
        FullName = full;
        Registration = r;
        return r;
    }

    /// <inheritdoc/>
    public override string ToString() => FullName ?? Name;
}

/// <summary>Generated code of <see cref="ImpactNativeAttribute">[ImpactNative]</see> methods: typed views of a call's
/// arguments, and the registration hand-over. Each view checks the variable's type and size against the declaration
/// (the thunk refused a mismatched call before: this is the second guard), so a typed native reads and writes only its
/// own declared variables, also for a mod that is not advanced.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class ImpactNativeAccess
{
    /// <summary>A live view of argument <paramref name="i"/>'s variable as <typeparamref name="T"/>.</summary>
    /// <exception cref="InvalidOperationException">Not a variable, or one of another type (<paramref name="type"/>, 0 =
    /// any) or size.</exception>
    [AnyThread]
    public static ref T Ref<T>(ref NativeCall call, int i, uint type) where T : unmanaged
    {
        var b = call.BytesCore(i);
        if (b.Length != sizeof(T) || (type != 0 && call.VariableType(i) != type))
            throw new InvalidOperationException(
                $"argument {i}: the program's variable is {(b.Length == 0 ? "not there" : $"type 0x{call.VariableType(i):x8}, {b.Length} bytes")}, " +
                $"the native takes type 0x{type:x8}, {sizeof(T)} bytes");
        return ref MemoryMarshal.AsRef<T>(b);
    }

    /// <summary>Argument <paramref name="i"/> is a variable (not 0, not a raw value).</summary>
    [AnyThread]
    public static bool Connected(ref NativeCall call, int i) => call.IsConnected(i);

    /// <summary>A <c>[Raw] uint</c> argument: the variable's value, or the raw value the program pushed.</summary>
    [AnyThread]
    public static uint RawValue(ref NativeCall call, int i) =>
        call.IsConnected(i) ? Ref<uint>(ref call, i, 0) : call.Raw(i);

    /// <summary>A Time variable (int64 nanoseconds) as a <see cref="TimeSpan"/> (ticks = ns / 100).</summary>
    [AnyThread]
    public static TimeSpan Time(ref NativeCall call, int i) => TimeSpan.FromTicks(Ref<long>(ref call, i, TimeType) / 100);

    /// <summary>Writes a <see cref="TimeSpan"/> into a Time variable (ns = ticks * 100, saturated).</summary>
    [AnyThread]
    public static void SetTime(ref NativeCall call, int i, TimeSpan value)
    {
        var ticks = value.Ticks;
        Ref<long>(ref call, i, TimeType) = ticks > long.MaxValue / 100 ? long.MaxValue : ticks < long.MinValue / 100 ? long.MinValue : ticks * 100;
    }

    /// <summary>The call as the method's <see cref="NativeContext"/>.</summary>
    [AnyThread]
    public static NativeContext Context(ref NativeCall call) => new(call);

    private const uint TimeType = 0xdfe4e404;

    /// <summary>The generated module initializer of a mod's [ImpactNative] methods: <paramref name="register"/> runs at
    /// the next tick for the mod whose code it is (its load context), on the tick thread.</summary>
    [AnyThread]
    public static void Provide(Action register)
    {
        ArgumentNullException.ThrowIfNull(register);
        GeneratedNatives.Provide(register);
    }
}

/// <summary>The registrations generated [ImpactNative] code handed over (ImpactNativeAccess.Provide). A mod's module
/// initializer runs while the host creates the mod, before its context is bound: the registration waits for the next
/// service tick, where the delegate's load context names the mod (ModContext.OwnerOf), and runs under it.</summary>
internal static class GeneratedNatives
{
    private sealed class Pending(Action register)
    {
        // cleared when the code's load context unloads first: a queued registration never keeps an unloaded mod alive
        public volatile Action? Register = register;
        public int Ticks;
    }

    private static readonly ConcurrentQueue<Pending> Queue = new();
    private static int _hooked;

    /// <summary>How long a registration waits for its mod's context (the host binds it before OnLoad: one tick).</summary>
    internal const int MaxWait = 250;

    internal static void Provide(Action register)
    {
        var p = new Pending(register);
        if (System.Runtime.Loader.AssemblyLoadContext.GetLoadContext(register.Method.Module.Assembly) is { IsCollectible: true } alc)
            alc.Unloading += _ => p.Register = null;
        Queue.Enqueue(p);
        if (Interlocked.Exchange(ref _hooked, 1) == 0) ServiceTick.Add("kiln.abilities [ImpactNative]", static (in TickInfo _) => Drain());
    }

    // Tick thread (a service tick). Also called by tests.
    internal static int Drain()
    {
        var done = 0;
        for (var n = Queue.Count; n > 0 && Queue.TryDequeue(out var p); n--)
        {
            if (p.Register is not { } register) continue;  // its load context unloaded
            var mod = ModContext.OwnerOf(register);
            if (mod is null)
            {
                if (++p.Ticks < MaxWait) Queue.Enqueue(p);
                else ModLog.Warn($"abilities: generated [ImpactNative] natives of {register.Method.Module.Assembly.GetName().Name} were not registered: no mod owns that assembly");
                continue;
            }
            if (!mod.Alive) continue;
            Run(mod, register);
            done++;
        }
        return done;
    }

    private static void Run(ModContext mod, Action register)
    {
        var outer = ModContext.CurrentMod;
        try
        {
            ModContext.CurrentMod = mod;
            register();
        }
        catch (KilnUnavailableException e)
        {
            mod.Log.Info($"abilities: its [ImpactNative] natives are not registered here: {e.Message}");
        }
        catch (Exception e)
        {
            mod.Log.Error(e, "abilities: registering its [ImpactNative] natives threw");
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }
}

/// <summary>Generated code: the impact/natives.json entry of one [ImpactNative] method ("args" as JSON), which the SDK's
/// KilnImpactNatives build task writes into the output's impact/natives.json with the mod's id in front of the name.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
[AttributeUsage(AttributeTargets.Assembly, AllowMultiple = true, Inherited = false)]
public sealed class ImpactNativeEntryAttribute(string name, string args) : Attribute
{
    /// <summary>The native's name without the mod id.</summary>
    public string Name { get; } = name;

    /// <summary>The entry's "args" array as JSON text.</summary>
    public string Args { get; } = args;
}
