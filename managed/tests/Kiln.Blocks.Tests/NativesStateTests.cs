// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Blocks.Tests;

/// <summary>Review P1 item 4 (dev.29): a sync native accepted before a world existed and refused by the world's setup
/// is reported: <see cref="Abilities.Natives.StateOf"/> says Refused, and one NativeRegistrationRefusedEvent is posted
/// (once, also when the check runs again). The core's native_state is a fake here (kiln-abilities.dll's own mapping is
/// tested by native/services/abilities/tests/registration_test.cpp).</summary>
[Collection("raw gates")]
public sealed unsafe class NativesStateTests
{
    static readonly Dictionary<uint, uint> States = new();

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int FakeState(uint hash, uint* state)
    {
        lock (States) *state = States.GetValueOrDefault(hash);
        return AbilitiesNative.Ok;
    }

    static ModContext NewMod(string id) => HostAccess.CreateContext(new ModInfo(id, id, "1.0.0", "no-such-folder"), 0, "no-such-folder");

    [Fact]
    public void ANativeTheWorldsSetupRefusedIsReportedOnce()
    {
        var table = (AbilitiesNative.TableV2*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesNative.TableV2));
        table->V1.Header.Size = (uint)sizeof(AbilitiesNative.TableV2);
        table->NativeStateFn = &FakeState;
        var field = typeof(AbilitiesNative).GetField("_t", BindingFlags.Static | BindingFlags.NonPublic)!;
        var previous = field.GetValue(null);
        var check = typeof(AbilitiesDispatch).GetMethod("CheckSetupRefusals", BindingFlags.Static | BindingFlags.NonPublic)!;
        var mod = NewMod("state-mod-1");
        var refusedNow = AbilitiesDispatch.AddSync("state-mod-1.Late", (ref NativeCall c) => 1, mod);
        var liveNow = AbilitiesDispatch.AddSync("state-mod-1.InTime", (ref NativeCall c) => 1, mod);
        refusedNow.Hash = 0x5717e001;
        liveNow.Hash = 0x5717e002;
        refusedNow.Watched = liveNow.Watched = true;
        var events = new List<NativeRegistrationRefusedEvent>();
        using var sub = Events.On<NativeRegistrationRefusedEvent>(events.Add);
        try
        {
            field.SetValue(null, Pointer.Box(table, typeof(AbilitiesNative.Table*)));
            lock (States)
            {
                States[refusedNow.Hash] = 1;  // KILN_NATIVE_STATE_WAITING: no world yet
                States[liveNow.Hash] = 1;
            }
            Assert.Equal(NativeState.Waiting, Abilities.Natives.StateOf(refusedNow.Hash));
            check.Invoke(null, null);
            Assert.Empty(events);  // still waiting: nothing to say
            lock (States)
            {
                States[refusedNow.Hash] = 3;  // the world's setup: no room for this one
                States[liveNow.Hash] = 2;     // this one went in
            }
            check.Invoke(null, null);
            check.Invoke(null, null);  // checked again: said once
            var e = Assert.Single(events);
            Assert.Equal(("state-mod-1.Late", 0x5717e001u), (e.Name, e.Hash));
            Assert.Contains("registry full", e.Reason);
            Assert.Equal(NativeState.Refused, Abilities.Natives.StateOf(refusedNow.Hash));
            Assert.Equal(NativeState.Live, Abilities.Natives.StateOf(liveNow.Hash));
            Assert.False(refusedNow.Watched || liveNow.Watched);
        }
        finally
        {
            field.SetValue(null, previous);
            AbilitiesDispatch.RemoveSync(refusedNow);
            AbilitiesDispatch.RemoveSync(liveNow);
            NativeMemory.Free(table);
        }
    }
}
