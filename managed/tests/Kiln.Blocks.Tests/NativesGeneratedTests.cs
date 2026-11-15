// SPDX-License-Identifier: MIT
using System.Numerics;
using System.Reflection;
using System.Runtime.InteropServices;
using Xunit;
using static Kiln.Blocks.Tests.RawGateTests;

namespace Kiln.Blocks.Tests.GeneratedNatives
{
    // [ImpactNative] methods as a mod writes them; Kiln.ImpactNatives generates their thunks into this test assembly.
    public static class Methods
    {
        public static (EntityId Self, uint Program, uint Site, EntityId Target) Seen;

        [ImpactNative("GenOnHit")]
        public static bool OnHit(in NativeContext ctx, EntityId target, ref float attackMod)
        {
            Seen = (ctx.Self, ctx.Program, ctx.Site, target);
            attackMod *= 1.5f;
            return target.Value != 0;
        }

        [ImpactNative("GenWrites")]
        public static void Writes(ref int s, out uint u, ref TimeSpan t, out Vector3 v)
        {
            s += 1;
            u = 7;
            t += TimeSpan.FromSeconds(1);
            v = new Vector3(1, 2, 3);
        }

        [ImpactNative("GenTimeAlias")]
        public static void TimeAlias(ref TimeSpan a, ref TimeSpan b) { a = TimeSpan.FromSeconds(2); b += a; }

        [ImpactNative("GenTimeThrow")]
        public static void TimeThrow(ref TimeSpan a) { a = TimeSpan.FromSeconds(2); throw new InvalidOperationException("time fault"); }

        public static ModContext? ThrowMod;
        [ImpactNative("GenThrows")]
        public static void Throws(ref int value)
        {
            ThrowMod = HostAccess.Current;
            value = 42;
            throw new InvalidOperationException("generated native fault");
        }

        [ImpactNative("GenMaybe")]
        public static byte Maybe(Optional<float> scale, [Raw] uint tag) => (byte)((scale.HasValue ? 100 : 0) + tag);
    }
}

namespace Kiln.Blocks.Tests
{
    /// <summary>The generated thunks of [ImpactNative] methods (Kiln.ImpactNatives, chapter 06c S4), driven as the core
    /// drives a sync native: decode by the parameters' types, live ref and out views, TimeSpan copied back, Optional and
    /// [Raw], the result byte, and a variable of another type stopped before the method runs.</summary>
    [Collection("raw gates")]
    public sealed unsafe class NativesGeneratedTests
    {
        const uint Sint32 = 0x9859f31e, Uint32 = 0x32940bec, Float32 = 0xe89f7410, Time = 0xdfe4e404, EntityIdType = 0x737c8f86, Float3 = 0x3d1f8582;

        // A call whose arguments are the given variables (type, size, first bytes), or a raw value where type is 0.
        sealed class Call : IDisposable
        {
            public readonly AbilitiesNative.NativeCall* C;
            readonly uint* _args;
            readonly byte* _layout, _data;
            readonly int[] _offsets;

            public Call(params (uint Type, int Size, byte[]? Init, uint Raw)[] args)
            {
                _args = (uint*)NativeMemory.AllocZeroed((nuint)(Math.Max(1, args.Length) * 4));
                _layout = (byte*)NativeMemory.AllocZeroed((nuint)(Math.Max(1, args.Length) * 24));
                var total = args.Sum(a => a.Type == 0 ? 0 : a.Size);
                _data = (byte*)NativeMemory.AllocZeroed((nuint)Math.Max(1, total));
                _offsets = new int[args.Length];
                int at = 0, var = 0;
                for (var i = 0; i < args.Length; i++)
                {
                    var a = args[i];
                    if (a.Type == 0) { _args[i] = a.Raw; continue; }
                    _args[i] = 0xFFFF0000u | (uint)var;
                    var e = _layout + var * 24;
                    *(uint*)(e + 8) = a.Type;
                    *(ushort*)(e + 12) = (ushort)a.Size;
                    *(ushort*)(e + 14) = (ushort)at;
                    a.Init?.CopyTo(new Span<byte>(_data + at, a.Size));
                    _offsets[i] = at;
                    at += a.Size;
                    var++;
                }
                C = (AbilitiesNative.NativeCall*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesNative.NativeCall));
                C->Size = (uint)sizeof(AbilitiesNative.NativeCall);
                C->Argc = (uint)args.Length;
                C->Args = _args;
                C->Layout = _layout;
                C->LayoutCount = (ulong)var;
                C->Data = _data;
                C->DataSize = (ulong)total;
                C->Self = 77;
                C->ProgramId = 0xABCD;
                C->Site = 0x1234;
                C->OutError = 0xFF;
            }

            public Span<byte> Bytes(int arg, int size) => new(_data + _offsets[arg], size);

            public void Dispose()
            {
                NativeMemory.Free(C);
                NativeMemory.Free(_data);
                NativeMemory.Free(_layout);
                NativeMemory.Free(_args);
            }
        }

        static NativeSyncHandler Thunk(string name) => (NativeSyncHandler)Delegate.CreateDelegate(typeof(NativeSyncHandler),
            typeof(Kiln.ImpactNatives.Generated.Registration).GetMethod("Thunk_" + name, BindingFlags.NonPublic | BindingFlags.Static)!);

        static byte Invoke(string name, Call call)
        {
            var c = new NativeCall(call.C, Plain, raw: false, sync: true);
            return Thunk(name)(ref c);
        }

        [Fact]
        public void ValuesAndTheContextAreReadAndARefIsWrittenInPlace()
        {
            using var call = new Call((EntityIdType, 4, BitConverter.GetBytes(42u), 0), (Float32, 4, BitConverter.GetBytes(2f), 0));
            Assert.Equal(1, Invoke("GenOnHit", call));
            Assert.Equal((new EntityId(77), 0xABCDu, 0x1234u, new EntityId(42)), GeneratedNatives.Methods.Seen);
            Assert.Equal(3f, BitConverter.ToSingle(call.Bytes(1, 4)));  // attackMod *= 1.5 in the program's variable
        }

        [Fact]
        public void OutAndRefOfEveryKindGoBackToTheProgram()
        {
            using var call = new Call((Sint32, 4, BitConverter.GetBytes(41), 0), (Uint32, 4, null, 0),
                (Time, 8, BitConverter.GetBytes(2_000_000_000L), 0), (Float3, 12, null, 0));
            Assert.Equal(1, Invoke("GenWrites", call));  // void answers 1
            Assert.Equal(42, BitConverter.ToInt32(call.Bytes(0, 4)));
            Assert.Equal(7u, BitConverter.ToUInt32(call.Bytes(1, 4)));
            Assert.Equal(3_000_000_000L, BitConverter.ToInt64(call.Bytes(2, 8)));  // 2 s + 1 s, as int64 ns
            Assert.Equal(new Vector3(1, 2, 3), MemoryMarshal.Read<Vector3>(call.Bytes(3, 12)));
        }

        [Fact]
        public void OptionalAndRawFollowWhatTheProgramPassed()
        {
            using (var call = new Call((Float32, 4, BitConverter.GetBytes(1f), 0), (0, 0, null, 5)))
                Assert.Equal(105, Invoke("GenMaybe", call));  // connected, raw 5
            using (var call = new Call((0, 0, null, 0), (0, 0, null, 6)))
                Assert.Equal(6, Invoke("GenMaybe", call));    // not connected, raw 6
            using (var call = new Call((0, 0, null, 0), (Uint32, 4, BitConverter.GetBytes(9u), 0)))
                Assert.Equal(9, Invoke("GenMaybe", call));    // [Raw] also takes a 4-byte variable
        }

        [Fact]
        public void AVariableOfAnotherTypeIsStoppedBeforeTheMethodRunsAndAnswersInternal()
        {
            GeneratedNatives.Methods.Seen = default;
            using var call = new Call((Sint32, 4, BitConverter.GetBytes(42), 0), (Float32, 4, BitConverter.GetBytes(2f), 0));
            Assert.Throws<InvalidOperationException>(() => Invoke("GenOnHit", call));
            Assert.Equal(default, GeneratedNatives.Methods.Seen);
            // through the sync dispatcher, as the core calls it: the exception answers 0x11, the program variable stays
            var e = AbilitiesDispatch.AddSync("plain-mod.GenOnHit", Thunk("GenOnHit"), Plain);
            try
            {
                var dispatch = (delegate* unmanaged[Cdecl]<AbilitiesNative.NativeCall*, void*, void>)typeof(AbilitiesDispatch)
                    .GetMethod("SyncDispatch", BindingFlags.NonPublic | BindingFlags.Static)!.MethodHandle.GetFunctionPointer();
                var t = new Thread(() => dispatch(call.C, (void*)e.Id));
                t.Start();
                t.Join();
                Assert.Equal(0x11, call.C->OutError);
                Assert.Equal(2f, BitConverter.ToSingle(call.Bytes(1, 4)));
            }
            finally { AbilitiesDispatch.RemoveSync(e); }
        }

        [Fact]
        public void AThrowingGeneratedMethodAnswersInternalAndKeepsItsWrites()
        {
            using var call = new Call((Sint32, 4, BitConverter.GetBytes(1), 0));
            var e = AbilitiesDispatch.AddSync("plain-mod.GenThrows", Thunk("GenThrows"), Plain);
            try
            {
                var dispatch = (delegate* unmanaged[Cdecl]<AbilitiesNative.NativeCall*, void*, void>)typeof(AbilitiesDispatch)
                    .GetMethod("SyncDispatch", BindingFlags.NonPublic | BindingFlags.Static)!.MethodHandle.GetFunctionPointer();
                var t = new Thread(() => dispatch(call.C, (void*)e.Id));
                t.Start(); t.Join();
                Assert.Equal(0x11, call.C->OutError);
                Assert.Equal(42, BitConverter.ToInt32(call.Bytes(0, 4)));
                Assert.Same(Plain, GeneratedNatives.Methods.ThrowMod);
                Assert.Equal(1, e.Faults);
            }
            finally { AbilitiesDispatch.RemoveSync(e); }
        }

        [Fact]
        public void TimeRefsAliasAndKeepWritesOnExceptions()
        {
            using var alias = new Call((Time, 8, BitConverter.GetBytes(1_000_000_000L), 0), (Time, 8, null, 0));
            alias.C->Args[1] = alias.C->Args[0];
            Invoke("GenTimeAlias", alias);
            Assert.Equal(4_000_000_000L, BitConverter.ToInt64(alias.Bytes(0, 8)));
            using var fault = new Call((Time, 8, BitConverter.GetBytes(1_000_000_000L), 0));
            Assert.Throws<InvalidOperationException>(() => Invoke("GenTimeThrow", fault));
            Assert.Equal(2_000_000_000L, BitConverter.ToInt64(fault.Bytes(0, 8)));
        }

        [Fact]
        public void TheNativeRefNamesTheParametersBlocksBindBy()
        {
            var r = GeneratedNatives.Natives.GenOnHit;
            Assert.Equal("GenOnHit", r.Name);
            Assert.Equal(["target", "attackMod"], r.Parameters.Select(p => p.Name));
            Assert.Equal(new NativeArg("float32", NativeArgDirection.InOut, false, false, 4), r.Parameters[1].Arg);
            Assert.Equal(AbilitiesNative.Fnv("frost-mark.GenOnHit"), r.HashFor("frost-mark"));
            Assert.Null(r.FullName);  // not registered in a test process
            var maybe = GeneratedNatives.Natives.GenMaybe.Parameters;
            Assert.True(maybe[0].Arg.Optional);
            Assert.True(maybe[1].Arg.Raw);
            Assert.Equal("any", maybe[1].Arg.Type);
        }

        [Fact]
        public void TheEntryAttributesCarryTheNativesJsonArgs()
        {
            var entries = typeof(NativesGeneratedTests).Assembly.GetCustomAttributes<ImpactNativeEntryAttribute>().ToDictionary(a => a.Name);
            Assert.Equal(["GenMaybe", "GenOnHit", "GenThrows", "GenTimeAlias", "GenTimeThrow", "GenWrites"], entries.Keys.Order());
            Assert.Contains("\"type\":\"Time\",\"dir\":\"inout\",\"size\":8", entries["GenWrites"].Args);
        }
    }
}
