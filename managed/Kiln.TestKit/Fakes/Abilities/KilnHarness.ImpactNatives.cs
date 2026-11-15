// SPDX-License-Identifier: MIT
using System.Numerics;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessImpactNatives? _impactNatives;

    /// <summary>The kit's fake kiln.abilities natives of this harness: the game's programs and natives a test declares,
    /// and the ECALLs it makes to a mod's natives and attachments. Since 0.2.0-dev.29.</summary>
    public HarnessImpactNatives ImpactNatives => _impactNatives ??= new HarnessImpactNatives(this);
}

/// <summary>
/// The fake kiln.abilities natives of a harness (<see cref="KilnHarness.ImpactNatives"/>): Kiln.Abilities' own code
/// (<c>Abilities.Natives.RegisterSync</c>, <c>Attach</c>, <c>DeclareTyped</c>, an <c>[ImpactNative]</c> method's generated
/// registration, <c>NativeContext.TryRead</c> and <c>TryWrite</c>) runs on it, and <see cref="Call"/> makes one ECALL as a
/// program would. Since 0.2.0-dev.29.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> A dedicated server's order: the world's native registry is there before the mods load, so a
/// native registered in OnLoad is live at once and takes one of the 19 keys the registry leaves for mods (the 20th is
/// refused); the world's programs come in at the second step, so an attachment made in OnLoad that names a program is
/// Pending until then. A call of a Kiln native runs its thunk's checks (a typed declaration: 0x2C for another type or
/// size, 0x18 for a required argument not connected), then its handler, on a thread of its own while the tick thread
/// waits (tick-thread API throws there, as in the game); a handler that threw answers 0x11, a native taken off (its mod
/// unloaded) 0x1F. A call of a game native runs the attachments whose program and site match, in the service's order
/// (Before by priority, one Instead, the game's native once, After), and the game's native does what
/// <see cref="SetGameNative"/> says (else it pushes 1). The programs that exist and the natives they call at which sites
/// are the ones <see cref="AddProgram"/> declares. NativeContext reads and writes the fake world's components
/// (<see cref="KilnHarness.Entities"/>) of the types in <see cref="OtherRead"/> and <see cref="OtherWrite"/>, on from the
/// first Kiln native call of the world; a write is stored as written and raises Kiln.Abilities'
/// <c>ComponentWrittenEvent</c> in the next step.</para>
/// <para><b>Not modelled</b> (offline): the programs' code (nothing runs a program: the test makes each call), program
/// registration, instances, attributes and their formulas, sequences, buffs, <c>Natives.Register</c>'s suspending natives,
/// the watchdog and the 0.2 ms budget.</para>
/// </remarks>
public sealed class HarnessImpactNatives
{
    private readonly KilnHarness _harness;

    internal HarnessImpactNatives(KilnHarness harness) => _harness = harness;

    private AbilitiesModel Model => AbilitiesFake.ModelOf(_harness);

    /// <summary>Declares a game program in the world's ImpactRegistry with the natives it calls and where (the ECALL's
    /// call-site id): what <c>Abilities.Natives.Attach</c> checks a program and site against. Each native named is one of
    /// the game's. Attachments made before are checked again now (one that names it turns on).</summary>
    /// <param name="program">The program's id (FNV-1a-32 of its GUID).</param>
    /// <param name="calls">Its ECALLs.</param>
    public void AddProgram(uint program, params FakeEcall[] calls)
    {
        ArgumentNullException.ThrowIfNull(calls);
        _harness.Run(() => Model.AddProgram(program, calls));
    }

    /// <summary>Makes <paramref name="native"/> one of the game's natives (its node name, <c>"applyDamage"</c>, or "0x" and
    /// its hash) and says what it does when it runs: <paramref name="handler"/> may change its arguments' variables and
    /// returns what the ECALL pushes. Null: it pushes 1 and changes nothing.</summary>
    public void SetGameNative(string native, FakeGameNativeHandler? handler = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(native);
        _harness.Run(() => Model.SetGameNative(native, handler));
    }

    /// <summary>Makes one ECALL of <see cref="FakeImpactCall.Native"/>, as a program's step would, and waits for it. Tick
    /// thread is not required: the call is made while the harness's tick thread waits, on a thread of its own.</summary>
    /// <returns>The ECALL's error and value, and the arguments' variables after the call.</returns>
    public FakeImpactResult Call(FakeImpactCall call)
    {
        ArgumentNullException.ThrowIfNull(call);
        return _harness.Run(() => Model.Run(call));
    }

    /// <summary>The types of impact_simulation's other_read list: what NativeContext.TryRead may read of any entity (with
    /// <see cref="OtherWrite"/>). This game build's list; a test may change it (tick thread or between steps).</summary>
    public ISet<string> OtherRead => Model.OtherRead;

    /// <summary>The types of impact_simulation's other_write list: what NativeContext.TryWrite may write (and TryRead
    /// read). This game build's list; a test may change it.</summary>
    public ISet<string> OtherWrite => Model.OtherWrite;

    /// <summary>Every call the test made, in order.</summary>
    public IReadOnlyList<FakeImpactCallRecord> Calls => Model.Calls;
}

/// <summary>One ECALL of a game program: the native it calls and the call-site id.</summary>
/// <param name="Native">The native's node name (<c>"applyDamage"</c>) or "0x" and its hash.</param>
/// <param name="Site">The ECALL's call-site id.</param>
public readonly record struct FakeEcall(string Native, uint Site);

/// <summary>What a game native does when it runs in <see cref="HarnessImpactNatives.Call"/>
/// (<see cref="HarnessImpactNatives.SetGameNative"/>): it may read and change its arguments' variables, and returns what the
/// ECALL pushes.</summary>
public delegate byte FakeGameNativeHandler(FakeGameNativeCall call);

/// <summary>The call a game native answers in the fake (valid only while its handler runs).</summary>
public readonly unsafe struct FakeGameNativeCall
{
    private readonly AbilitiesAbi.NativeCall* _c;

    internal FakeGameNativeCall(AbilitiesAbi.NativeCall* c) => _c = c;

    /// <summary>The values the ECALL pops.</summary>
    public int ArgCount => (int)_c->Argc;

    /// <summary>The calling program.</summary>
    public uint Program => _c->ProgramId;

    /// <summary>The call site.</summary>
    public uint Site => _c->Site;

    /// <summary>Argument <paramref name="index"/>'s variable (empty for a literal or one not connected).</summary>
    public Span<byte> Bytes(int index)
    {
        if ((uint)index >= _c->Argc) throw new ArgumentOutOfRangeException(nameof(index));
        var h = _c->Args[index];
        if ((h & 0xFFFF0000u) != 0xFFFF0000u || (h & 0xFFFF) >= _c->LayoutCount) return [];
        var v = ((AbilitiesAbi.Variable*)_c->Layout)[h & 0xFFFF];
        return new Span<byte>(_c->Data + v.Offset, v.Size);
    }

    /// <summary>Argument <paramref name="index"/>'s value.</summary>
    public T Get<T>(int index) where T : unmanaged
    {
        var b = Bytes(index);
        return b.Length >= sizeof(T) ? MemoryMarshal.Read<T>(b) : default;
    }

    /// <summary>Writes argument <paramref name="index"/>'s variable (false for a literal or a smaller variable).</summary>
    public bool Set<T>(int index, T value) where T : unmanaged
    {
        var b = Bytes(index);
        return b.Length >= sizeof(T) && MemoryMarshal.TryWrite(b, in value);
    }
}

/// <summary>One value an ECALL pops (<see cref="FakeImpactCall"/>): a variable of the instance of an impact type, a raw
/// literal, or nothing connected.</summary>
public sealed class FakeImpactArg
{
    private FakeImpactArg(bool variable, uint type, byte[] bytes, uint raw)
    {
        IsVariable = variable;
        TypeHash = type;
        Bytes = bytes;
        RawValue = raw;
    }

    internal bool IsVariable { get; }
    internal uint TypeHash { get; }
    internal byte[] Bytes { get; }
    internal uint RawValue { get; }

    /// <summary>A variable of impact type <paramref name="type"/> (<c>"sint32"</c>, <c>"impact.DamageSetup"</c>, ...)
    /// holding <paramref name="value"/>'s bytes.</summary>
    public static FakeImpactArg Variable<T>(string type, T value) where T : unmanaged
    {
        ArgumentException.ThrowIfNullOrEmpty(type);
        return new FakeImpactArg(true, AbilitiesAbi.Fnv(type), MemoryMarshal.AsBytes(new ReadOnlySpan<T>(in value)).ToArray(), 0);
    }

    /// <summary>A variable of impact type <paramref name="type"/> holding <paramref name="bytes"/>.</summary>
    public static FakeImpactArg Variable(string type, ReadOnlySpan<byte> bytes)
    {
        ArgumentException.ThrowIfNullOrEmpty(type);
        if (bytes.IsEmpty || bytes.Length > 0xFFFF) throw new ArgumentException("a variable has 1 to 65535 bytes", nameof(bytes));
        return new FakeImpactArg(true, AbilitiesAbi.Fnv(type), bytes.ToArray(), 0);
    }

    /// <summary>An sint32 variable.</summary>
    public static FakeImpactArg Int(int value) => Variable("sint32", value);

    /// <summary>A uint32 variable.</summary>
    public static FakeImpactArg UInt(uint value) => Variable("uint32", value);

    /// <summary>A float32 variable.</summary>
    public static FakeImpactArg Float(float value) => Variable("float32", value);

    /// <summary>A bool variable (one byte).</summary>
    public static FakeImpactArg Bool(bool value) => Variable("bool", (byte)(value ? 1 : 0));

    /// <summary>A Time variable (int64 nanoseconds).</summary>
    public static FakeImpactArg Time(TimeSpan value) => Variable("Time", value.Ticks * 100);

    /// <summary>An ecs.EntityId variable.</summary>
    public static FakeImpactArg Entity(EntityId value) => Variable("ecs.EntityId", value.Value);

    /// <summary>A float3 variable.</summary>
    public static FakeImpactArg Vector(Vector3 value) => Variable("float3", value);

    /// <summary>A raw value the program pushes (a literal, not a variable).</summary>
    public static FakeImpactArg Literal(uint value) => new(false, 0, [], value);

    /// <summary>An argument the program leaves not connected (0).</summary>
    public static FakeImpactArg NotConnected { get; } = new(false, 0, [], 0);
}

/// <summary>One ECALL for <see cref="HarnessImpactNatives.Call"/>.</summary>
/// <param name="native">A Kiln native's full name ("&lt;modid&gt;.&lt;Name&gt;"), a game native's node name
/// (<c>"applyDamage"</c>), or "0x" and a hash.</param>
/// <param name="args">The values it pops, argument 0 first (the last value the program pushes).</param>
public sealed class FakeImpactCall(string native, params FakeImpactArg[] args)
{
    /// <summary>The native.</summary>
    public string Native { get; } = !string.IsNullOrEmpty(native) ? native : throw new ArgumentException("a native's name", nameof(native));

    /// <summary>The values it pops.</summary>
    public IReadOnlyList<FakeImpactArg> Args { get; } = args ?? [];

    /// <summary>The calling program's id (FNV-1a-32 of its GUID).</summary>
    public uint Program { get; init; }

    /// <summary>The ECALL's call-site id.</summary>
    public uint Site { get; init; }

    /// <summary>The instance's own entity.</summary>
    public EntityId Self { get; init; }

    /// <summary>The instance's id.</summary>
    public uint ImpactId { get; init; } = 1;
}

/// <summary>What an ECALL answered (<see cref="HarnessImpactNatives.Call"/>).</summary>
public sealed class FakeImpactResult
{
    private readonly byte[][] _after;

    internal FakeImpactResult(byte error, byte value, bool originalCalled, int handlers, byte[][] after)
    {
        Error = error;
        Value = value;
        OriginalCalled = originalCalled;
        Handlers = handlers;
        _after = after;
    }

    /// <summary>The ECALL's error: 0 ok; 0x11 the handler threw; 0x18 a required argument not connected; 0x1F a native
    /// the registry does not hold (taken off, refused, unknown); 0x2C an argument of another type or size. Not 0: the
    /// instance stops with Error.</summary>
    public byte Error { get; }

    /// <summary>What the ECALL pushed (when <see cref="Error"/> is 0).</summary>
    public byte Value { get; }

    /// <summary>The game's native ran.</summary>
    public bool OriginalCalled { get; }

    /// <summary>The handlers that ran (a Kiln native's; the attachments that entered).</summary>
    public int Handlers { get; }

    /// <summary>Argument <paramref name="index"/>'s variable after the call (empty for a literal or one not connected).</summary>
    public ReadOnlySpan<byte> Bytes(int index) => _after[index];

    /// <summary>Argument <paramref name="index"/>'s value after the call.</summary>
    public T Arg<T>(int index) where T : unmanaged =>
        _after[index].Length >= System.Runtime.CompilerServices.Unsafe.SizeOf<T>() ? MemoryMarshal.Read<T>(_after[index]) : default;
}
