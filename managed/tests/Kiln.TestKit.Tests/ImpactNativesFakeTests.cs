// SPDX-License-Identifier: MIT
// The kit's fake of kiln.abilities' natives (Fakes/Abilities/, chapter 06c S7) under the real Kiln.Abilities: same-step
// natives, typed declarations, attachments and the ECS bridge offline, with the native service's rules
// (native/services/abilities/natives_core.h; its own tests are native/services/abilities/tests).
using System.Collections.Concurrent;
using Xunit;

#pragma warning disable KILNX001  // Abilities.Natives.LabCounters: the thunks' counters

namespace Kiln.TestKit.Tests;

public sealed class ImpactNativesFakeTests
{
    private sealed class NativesMod : Mod;

    private const uint Explosion = 0x7a120ee8, DamageSite = 0x10, TransformSite = 0x20;

    private static (KilnHarness Kiln, ModHandle Mod) Start()
    {
        var kiln = new KilnHarness();
        var mod = kiln.Load<NativesMod>(new ModManifest("tk-natives"));
        kiln.Step(1);  // the mods load; the fake server world comes in at the next step
        return (kiln, mod);
    }

    // a sint32 native that doubles its argument in place and pushes 7
    private static NativeResult Twice(KilnHarness kiln, ModHandle mod, ConcurrentQueue<bool>? tick = null) =>
        kiln.As(mod, () => Abilities.Natives.RegisterSync("Twice", 1, (ref NativeCall c) =>
        {
            tick?.Enqueue(Game.IsTickThread);
            c.SetU32(0, c.GetU32(0) * 2);
            return 7;
        }));

    [Fact]
    public void A_sync_native_waits_for_the_world_then_answers_inside_the_call_off_the_tick_thread()
    {
        var (kiln, mod) = Start();
        using var owned = kiln;
        var tick = new ConcurrentQueue<bool>();
        var r = Twice(kiln, mod, tick);
        Assert.True(r.Ok, r.Reason);
        Assert.Equal(NativeState.Waiting, kiln.Run(() => Abilities.Natives.StateOf(r.Hash)));
        // a row Kiln.Abilities reports itself: Kiln.Capabilities, as a mod reads it
        Assert.True(kiln.Run(() => Capabilities.Is("abilities.natives.sync")), kiln.Run(() => Capabilities.Why("abilities.natives.sync")));
        kiln.Step(1);
        Assert.Equal(NativeState.Live, kiln.Run(() => Abilities.Natives.StateOf(r.Hash)));
        Assert.Contains(kiln.Run(() => Abilities.Natives.All), n => n.Name == "tk-natives.Twice" && n.Kind == ImpactNativeKind.Sync);

        var call = kiln.ImpactNatives.Call(new FakeImpactCall("tk-natives.Twice", FakeImpactArg.Int(21)));
        Assert.Equal((0, 7, 42), (call.Error, call.Value, call.Arg<int>(0)));
        Assert.Equal([false], tick);  // impact_simulation's thread, never the tick thread
        Assert.Equal(1, Assert.Single(kiln.ImpactNatives.Calls).Handlers);
    }

    [Fact]
    public void A_typed_declaration_refuses_another_type_and_a_missing_argument_before_the_handler()
    {
        var (kiln, mod) = Start();
        using var owned = kiln;
        var tick = new ConcurrentQueue<bool>();
        Twice(kiln, mod, tick);
        kiln.Run(() => Abilities.Natives.DeclareTyped("tk-natives.Twice", new NativeArg("sint32", NativeArgDirection.InOut)));
        kiln.Step(1);
        Assert.Equal(0x2C, kiln.ImpactNatives.Call(new FakeImpactCall("tk-natives.Twice", FakeImpactArg.Float(2f))).Error);
        Assert.Equal(0x2C, kiln.ImpactNatives.Call(new FakeImpactCall("tk-natives.Twice", FakeImpactArg.Literal(5))).Error);
        Assert.Equal(0x18, kiln.ImpactNatives.Call(new FakeImpactCall("tk-natives.Twice", FakeImpactArg.NotConnected)).Error);
        Assert.Empty(tick);
        Assert.Equal(0, kiln.ImpactNatives.Call(new FakeImpactCall("tk-natives.Twice", FakeImpactArg.Int(4))).Error);
        Assert.Single(tick);
        var counters = kiln.Run(() => Abilities.Natives.LabCounters!.Value);
        Assert.Equal((1ul, 2ul, 1ul), (counters.SyncCalls, counters.TypedRefused, counters.TypedUnconnected));
    }

    [Fact]
    public void A_handler_that_throws_answers_0x11_and_a_native_of_an_unloaded_mod_0x1F()
    {
        var (kiln, mod) = Start();
        using var owned = kiln;
        var r = kiln.As(mod, () => Abilities.Natives.RegisterSync("Boom", 0, (ref NativeCall c) => throw new InvalidOperationException("boom")));
        kiln.Step(1);
        Assert.Equal(0x11, kiln.ImpactNatives.Call(new FakeImpactCall("tk-natives.Boom")).Error);
        Assert.Contains(kiln.LogsOf("tk-natives"), l => l.Text.Contains("sync native tk-natives.Boom threw", StringComparison.Ordinal));
        kiln.Unload(mod);
        Assert.Equal(NativeState.Dead, kiln.Run(() => Abilities.Natives.StateOf(r.Hash)));
        Assert.Equal(0x1F, kiln.ImpactNatives.Call(new FakeImpactCall("tk-natives.Boom")).Error);
        Assert.Equal(0x1F, kiln.ImpactNatives.Call(new FakeImpactCall("tk-natives.Nobody")).Error);
    }

    [Fact]
    public void The_world_takes_19_mod_natives_and_refuses_the_next_one()
    {
        var (kiln, mod) = Start();
        using var owned = kiln;
        var hashes = kiln.As(mod, () => Enumerable.Range(0, 20)
            .Select(i => Abilities.Natives.RegisterSync($"N{i}", 0, (ref NativeCall c) => 1).Hash).ToList());
        kiln.Step(1);
        var states = kiln.Run(() => hashes.Select(Abilities.Natives.StateOf).ToList());
        Assert.Equal(19, states.Count(s => s == NativeState.Live));
        Assert.Equal(NativeState.Refused, states[19]);
        Assert.Equal(0x1F, kiln.ImpactNatives.Call(new FakeImpactCall("tk-natives.N19")).Error);
        // once the world is in, one more is refused at once
        var late = kiln.As(mod, () => Abilities.Natives.RegisterSync("Late", 0, (ref NativeCall c) => 1));
        Assert.False(late.Ok);
        Assert.StartsWith("native registry full", late.Reason);
    }

    [Fact]
    public void Attachments_run_in_the_services_order_only_for_their_program_and_site()
    {
        var (kiln, mod) = Start();
        using var owned = kiln;
        kiln.ImpactNatives.AddProgram(Explosion, new FakeEcall("applyDamage", DamageSite), new FakeEcall("getTransform", TransformSite));
        var order = new List<string>();
        kiln.ImpactNatives.SetGameNative("applyDamage", g =>
        {
            order.Add($"game {g.Get<float>(0)}");
            return 1;
        });
        kiln.Step(1);
        kiln.As(mod, () =>
        {
            Assert.True(Abilities.Natives.Attach(Explosion, "applyDamage", NativeStage.After, (ref NativeCall c) => order.Add("after"), DamageSite).Ok);
            Assert.True(Abilities.Natives.Attach(Explosion, "applyDamage", NativeStage.Before, (ref NativeCall c) =>
            {
                order.Add("before 1");
                c.SetFloat(0, c.GetFloat(0) * 2);
            }, DamageSite, priority: 1).Ok);
            Assert.True(Abilities.Natives.Attach(Explosion, "applyDamage", NativeStage.Before, (ref NativeCall c) => order.Add("before 5"), DamageSite, priority: 5).Ok);
            Assert.True(Abilities.Natives.Attach(Explosion, "applyDamage", NativeStage.Instead, (ref NativeCall c) => order.Add("instead")).Ok);
        });
        var hit = kiln.ImpactNatives.Call(new FakeImpactCall("applyDamage", FakeImpactArg.Float(3f)) { Program = Explosion, Site = DamageSite });
        Assert.Equal(["before 5", "before 1", "instead", "game 6", "after"], order);
        Assert.Equal((0, 1, true, 4), (hit.Error, hit.Value, hit.OriginalCalled, hit.Handlers));
        Assert.Equal(6f, hit.Arg<float>(0));

        // another program's call enters none of them (the Instead names the program, any site); the game's native runs
        order.Clear();
        var other = kiln.ImpactNatives.Call(new FakeImpactCall("applyDamage", FakeImpactArg.Float(3f)) { Program = 0x1234, Site = DamageSite });
        Assert.Equal(["game 3"], order);
        Assert.Equal((0, true), (other.Handlers, other.OriginalCalled));
        Assert.Equal(4ul, kiln.Run(() => Abilities.Natives.LabCounters!.Value.AttachSkipped));

        // an answer in a Before decides the call: no Instead, no game native; After still runs
        order.Clear();
        kiln.As(mod, () => Abilities.Natives.Attach(Explosion, "applyDamage", NativeStage.Before, (ref NativeCall c) => c.Cancel(), DamageSite, priority: -1));
        var canceled = kiln.ImpactNatives.Call(new FakeImpactCall("applyDamage", FakeImpactArg.Float(3f)) { Program = Explosion, Site = DamageSite });
        Assert.Equal(["before 5", "before 1", "after"], order);
        Assert.Equal((0, 0, false), (canceled.Error, canceled.Value, canceled.OriginalCalled));
    }

    [Fact]
    public void Attach_refuses_as_the_service_does()
    {
        var (kiln, mod) = Start();
        using var owned = kiln;
        kiln.ImpactNatives.AddProgram(Explosion, new FakeEcall("applyDamage", DamageSite));
        kiln.Step(1);
        kiln.As(mod, () =>
        {
            Assert.StartsWith("program not loaded", Abilities.Natives.Attach(0x5555, "applyDamage", NativeStage.Before, (ref NativeCall c) => { }).Reason);
            Assert.StartsWith("site not found", Abilities.Natives.Attach(Explosion, "applyDamage", NativeStage.Before, (ref NativeCall c) => { }, 0x99).Reason);
            Assert.StartsWith("not a native the game registered", Abilities.Natives.Attach(0, "spawnEntity", NativeStage.Before, (ref NativeCall c) => { }).Reason);
            Assert.True(Abilities.Natives.Attach(Explosion, "applyDamage", NativeStage.Instead, (ref NativeCall c) => { }, DamageSite).Ok);
            Assert.StartsWith("a second Instead", Abilities.Natives.Attach(Explosion, "applyDamage", NativeStage.Instead, (ref NativeCall c) => { }, DamageSite).Reason);
        });
    }

    [Fact]
    public void An_attachment_made_before_the_world_is_pending_then_off_then_active_when_its_program_comes()
    {
        var (kiln, mod) = Start();
        using var owned = kiln;
        kiln.ImpactNatives.SetGameNative("applyDamage");
        var a = kiln.As(mod, () => Abilities.Natives.Attach(Explosion, "applyDamage", NativeStage.Before, (ref NativeCall c) => { }));
        Assert.True(a.Ok, a.Reason);
        Assert.Equal(NativeAttachmentState.Pending, kiln.Run(() => a.Handle!.State));
        kiln.Step(1);
        Assert.Equal(NativeAttachmentState.Off, kiln.Run(() => a.Handle!.State));
        kiln.ImpactNatives.AddProgram(Explosion, new FakeEcall("applyDamage", DamageSite));
        Assert.Equal(NativeAttachmentState.Active, kiln.Run(() => a.Handle!.State));
        Assert.True(kiln.HasLog("is on: program 0x7a120ee8 is in now"));
        Assert.Equal(1, kiln.ImpactNatives.Call(new FakeImpactCall("applyDamage") { Program = Explosion, Site = DamageSite }).Handlers);
        kiln.Run(() => a.Handle!.Dispose());
        kiln.Step(1);
        Assert.Equal(NativeAttachmentState.Gone, kiln.Run(() => a.Handle!.State));
        Assert.Equal(0, kiln.ImpactNatives.Call(new FakeImpactCall("applyDamage") { Program = Explosion, Site = DamageSite }).Handlers);
    }

    [Fact]
    public void NativeContext_reads_and_writes_the_fake_worlds_components_of_impact_simulations_lists()
    {
        var (kiln, mod) = Start();
        using var owned = kiln;
        var target = kiln.Entities.Create(new WorldPosition(1, 2, 3));
        var health = default(Ecs.Health);
        health.Values[0] = 80;
        health.Values[7] = 100;
        kiln.Entities.Set(target, health);
        var read = new ConcurrentQueue<string>();
        kiln.As(mod, () => Abilities.Natives.RegisterSync("Bite", 1, (ref NativeCall c) =>
        {
            var ctx = ImpactNativeAccess.Context(ref c);
            var who = c.GetEntity(0);
            read.Enqueue($"fuel {ctx.TryRead<Ecs.ServerToggleState>(who, out _)}");  // not in other_read or other_write
            if (!ctx.TryRead<Ecs.Health>(who, out var h)) return 0;
            read.Enqueue($"health {h.Values[0]}");
            h.Values[0] -= 30;
            return (byte)(ctx.TryWrite(who, in h) ? 1 : 0);
        }));
        var written = new ConcurrentQueue<ComponentWrittenEvent>();
        kiln.As(mod, () => Events.On<ComponentWrittenEvent>(e => written.Enqueue(e)));
        kiln.Step(1);

        var r = kiln.ImpactNatives.Call(new FakeImpactCall("tk-natives.Bite", FakeImpactArg.Entity(target)));
        Assert.Equal((0, 1), (r.Error, r.Value));
        Assert.Equal(["fuel False", "health 80"], read);
        Assert.Equal(50, kiln.Entities.Get<Ecs.Health>(target).Values[0]);
        kiln.Step(2);
        var e = Assert.Single(written);
        Assert.Equal(("Health", "tk-natives/native:tk-natives.Bite"), (e.Component, e.Writer));
        kiln.Step(25);  // Kiln.Abilities refreshes its rows every 25 steps: pending until the first call, then on
        Assert.True(kiln.Run(() => Capabilities.Is("abilities.natives.ecs")), kiln.Run(() => Capabilities.Why("abilities.natives.ecs")));

        // the row switched off by the test: no access
        kiln.Capabilities.Override("abilities.natives.ecs", false, "an unpinned world");
        read.Clear();
        Assert.Equal(0, kiln.ImpactNatives.Call(new FakeImpactCall("tk-natives.Bite", FakeImpactArg.Entity(target))).Value);
        Assert.Equal(["fuel False"], read);
    }

    [Fact]
    public void FrostMark_bites_a_badly_hurt_enemy_through_its_generated_native()
    {
        using var kiln = new KilnHarness();
        kiln.LoadFolder(TestMods.Folder("FrostMark"));
        kiln.ImpactNatives.AddProgram(Explosion, new FakeEcall("applyDamage", DamageSite));
        kiln.Step(3);  // the generated registration runs in a service tick under the mod; the world takes it
        Assert.NotEmpty(kiln.Lines("frostmark", "frostmark: no sword here"));  // no game resources offline: the sword is not defined
        Assert.Equal(NativeState.Live, kiln.Run(() => Abilities.Natives.StateOf(AbilitiesNativeHash("frostmark.OnHit"))));

        static EntityId Enemy(KilnHarness kiln, int current)
        {
            var e = kiln.Entities.Create(new WorldPosition(5, 0, 5));
            var h = default(Ecs.Health);
            h.Values[0] = current;
            h.Values[7] = 100;
            kiln.Entities.Set(e, h);
            return e;
        }
        float Hit(EntityId target) =>
            kiln.ImpactNatives.Call(new FakeImpactCall("frostmark.OnHit", FakeImpactArg.Entity(target), FakeImpactArg.Float(1f))).Arg<float>(1);
        Assert.Equal(1f, Hit(Enemy(kiln, 100)));
        Assert.Equal(1f, Hit(Enemy(kiln, 25)));
        Assert.Equal(1.5f, Hit(Enemy(kiln, 20)));

        // its attachment on the Fireball explosion's applyDamage counts that program's hits only
        kiln.ImpactNatives.Call(new FakeImpactCall("applyDamage") { Program = Explosion, Site = DamageSite });
        kiln.ImpactNatives.Call(new FakeImpactCall("applyDamage") { Program = 0x1234, Site = DamageSite });
        Assert.Equal("Fireball explosion hits: 1", Assert.Single(kiln.RunCommand("frostmark").Output));
    }

    private static uint AbilitiesNativeHash(string name)
    {
        uint h = 0x811c9dc5;
        foreach (var b in System.Text.Encoding.UTF8.GetBytes(name)) h = (h ^ b) * 0x01000193;
        return h;
    }
}
