// SPDX-License-Identifier: MIT
// repl-test, package 15 stream A: sidecars (Replication.Extension) on the loopback harness (DESIGN "What each lab proves",
// part "sidecar"). Two fakes stand for two kiln.net lab peers (tools/nettest, BIND_MACHINE): "with" advertised
// bigcan:water@1 in its handshake (repl.ext), "without" did not. The game's watering can holds 2,000 charges here (its
// template's FuelSetup.maxFuelCharges, set by this test); the extension carries a can's water (units, max) while it holds
// more than 1,023 charges, which the game's own 16-bit NetworkFuel cannot show.
//   UPDATE      96000/128000 reaches the "with" peer within 1 step of the water change, with the can's createTick for it
//   without     the "without" peer gets no kiln:repl frame at all (and an unbound fake, a vanilla peer, none either)
//   CLEAR       at 1,023 charges the value is cleared on "with" (the game's own value is exact again)
//   destroy     the can taken out of the chest (a destroy list's list A): CLEAR on "with"
//   hot reload  the extension disposed and registered again: DEFS sent again with the new id, the value again within 2 steps
// The lab script checks the nettest side (what each lab peer decoded). Part "sidecar" of kiln/config/repl-test.json.
using System.Runtime.InteropServices;
using Kiln;

namespace ReplTest;

/// <summary>The value of bigcan:water@1 (as mods/samples/BigCan): a can's water in units (charges x 64) and its maximum.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct CanWater
{
    public uint Units;
    public uint Max;
}

/// <summary>The game's Fuel attribute as a component name for the provider (the value is read through Items.Fuel).</summary>
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct FuelAttribute : IGameComponent<FuelAttribute>
{
    public int Current, Max;
    public static uint Hash => 0x05b0bf75;
    public static string Name => "Fuel";
    public static string QualifiedName => "keen::ecs::Fuel";
    public static int Size => 8;
    public static ulong Layout => 0;
}

public sealed partial class ReplTestMod
{
    private const string Water = "bigcan:water";
    private const string CanTemplate = "Pide_Fuel_Watering_Can";
    private const int BigCharges = 2000, ExactCharges = 1023;
    private ReplicatedExtension<CanWater>? _water;
    private IDisposable? _waterProvider;

    // In OnLoad: kiln:repl and the extension must be there before the lab peers' handshakes.
    private void RegisterSidecar()
    {
        if (!Part("sidecar")) return;
        try
        {
            _water = Replication.Extension<CanWater>(Water, 1);
            Info($"sidecar: {Water} registered, advertised as {ExtensionAdvert.Format(_water.Name, _water.Version, _water.Size, _water.LayoutHash)}");
        }
        catch (Exception e)
        {
            Log.Warn($"{Me}: sidecar extension not registered: {e.Message}");
        }
    }

    private static bool Provide(EntityId e, ref CanWater v)
    {
        if (Items.Fuel.Read(e) is not { } f || f.Units <= ExactCharges * Items.Fuel.UnitsPerCharge) return false;
        v = new CanWater { Units = (uint)f.Units, Max = (uint)f.Max };
        return true;
    }

    private async Task<IReadOnlyList<(int Machine, int Slot)>> SidecarPart(EntityId chest, IReadOnlyList<(int Machine, int Slot)> fakes)
    {
        if (!Part("sidecar")) return fakes;
        Info($"sidecar: kiln.repl.net capabilities 0x{ReplNetNative.Capabilities():x}; repl.sidecar {Capabilities.Get("repl.sidecar")}");
        var labs = Network.Peers.Where(p => p.IsLab).ToList();
        var with = labs.FirstOrDefault(p => p.Properties.TryGetValue("repl.ext", out var v) && v.Contains(Water + "@1", StringComparison.Ordinal));
        var without = labs.FirstOrDefault(p => !p.Properties.ContainsKey("repl.ext"));
        if (!Result(_water is not null && with is not null && without is not null, "sidecar setup",
                $"extension {_water?.ToString() ?? "none"}; lab peers {labs.Count} ({string.Join(", ", labs.Select(p => $"0x{p.Id:x} {p.State} ext={p.Properties.GetValueOrDefault("repl.ext") ?? "-"}"))})"))
            return fakes;
        try
        {
            _waterProvider = _water!.On<FuelAttribute>(Provide);
        }
        catch (Exception e)
        {
            Result(false, "sidecar setup", $"On<Fuel> threw {e.GetType().Name}: {e.Message}");
            return fakes;
        }

        // a 2,000-charge can: the game's can template holds 2,000 charges from now on (a can made after this)
        var template = Content.Templates.Get(CanTemplate);
        try
        {
            template?.Set("FuelSetup.maxFuelCharges", BigCharges);
        }
        catch (Exception e)
        {
            Info($"sidecar: the can's template could not be set ({e.Message})");
        }
        var (can, max) = await NewCan(chest);
        if (!Result(!can.IsNone && max >= BigCharges * Items.Fuel.UnitsPerCharge, "sidecar setup",
                $"a can {can} with water max {max} (template {CanTemplate}: {template?.Get("FuelSetup.maxFuelCharges").Integer.ToString() ?? "none"})"))
            return fakes;

        // fresh fakes: machine A for "with", B for "without", C unbound (vanilla)
        ReplicationLab.RemoveFakePeers();
        await Steps(3);
        fakes = ReplicationLab.AddFakePeers(3);
        var (a, b, c) = (fakes[0].Machine, fakes[1].Machine, fakes[2].Machine);
        var boundA = ReplicationLabInternals.BindMachine(with!.Id, a);
        var boundB = ReplicationLabInternals.BindMachine(without!.Id, b);
        foreach (var f in fakes)
        {
            ReplicationLab.ForceRelevant(f.Machine, chest);
            ReplicationLab.ForceRelevant(f.Machine, can);
        }
        var seen = new List<(ulong Step, ReplExtensionChanged<CanWater> E)>();
        ulong step = 0;
        void OnChanged(ReplExtensionChanged<CanWater> e) => seen.Add((step, e));
        var ext = _water!;
        ext.Changed += OnChanged;
        async Task Step1()
        {
            await Steps(1);
            step++;
        }
        async Task<(bool Ok, ulong At, ReplExtensionChanged<CanWater>? E)> WaitFor(Func<ReplExtensionChanged<CanWater>, bool> match, int limit)
        {
            for (var i = 0; i <= limit; i++)
            {
                var hit = seen.FirstOrDefault(s => s.E.Peer?.Id == with.Id && s.E.Entity == can && match(s.E));
                if (hit.E is not null) return (true, hit.Step, hit.E);
                await Step1();
            }
            return (false, 0, null);
        }
        long Frames(NetPeer p) => SidecarServer.Core?.PeerStates.TryGetValue(p.Id, out var s) == true ? s.FramesSent : -1;

        // the first value: after the fake's create of the can
        await Items.Fuel.Set(can, BigCharges * Items.Fuel.UnitsPerCharge);
        var first = await WaitFor(e => e.Value is { } v && v.Units == BigCharges * 64, 120);
        var created = SidecarServer.Core?.PeerStates.TryGetValue(with.Id, out var ps) == true && ps.Tracked.TryGetValue(can.Value, out var ct) ? ct : 0u;
        Info($"sidecar: bound 0x{boundA:x} to fake {a} (with), 0x{boundB:x} to fake {b} (without), fake {c} unbound; first value " +
             $"{(first.Ok ? $"{first.E!.Value!.Value.Units}/{first.E.Value.Value.Max} createTick {first.E.CreateTick}" : "none")}, tracked createTick {created}");

        // UPDATE within 1 step of the water change
        var before = step;
        await Items.Fuel.Set(can, 1500 * 64);
        var changedAt = step;
        var upd = await WaitFor(e => e.Value is { } v && v.Units == 96000, 20);
        var withinSteps = upd.Ok ? (long)(upd.At - changedAt) : -1;
        Result(first.Ok && upd.Ok && upd.E!.Value!.Value.Max == (uint)max && upd.E.CreateTick == created && created != 0 && withinSteps <= 1, "sidecar",
            $"UPDATE {(upd.Ok ? $"{upd.E!.Value!.Value.Units}/{upd.E.Value.Value.Max}" : "none")} to peer with, createTick {(upd.Ok ? upd.E!.CreateTick : 0)}, " +
            $"within {withinSteps} step(s) (the water set in step {changedAt - before} of the wait)");

        // nothing to the peer without, nor to the unbound fake
        Result(Frames(with) > 0 && Frames(without) <= 0 && !seen.Any(s => s.E.Peer?.Id == without.Id), "sidecar without",
            $"nothing to peer without (kiln:repl frames {Math.Max(0, Frames(without))}; to peer with {Frames(with)}); the unbound fake {c} has no kiln.net peer to send to; " +
            $"peer without's channel-12 frames in all {without.FramesOut} (handshake and pings)");

        // CLEAR at 1,023 charges
        await Items.Fuel.Set(can, ExactCharges * Items.Fuel.UnitsPerCharge);
        var clr = await WaitFor(e => e.Value is null, 20);
        Result(clr.Ok, "sidecar clear", $"CLEAR at 1,023 charges: {(clr.Ok ? $"cleared on peer with, createTick {clr.E!.CreateTick}" : "no CLEAR")}");

        // CLEAR after destroy (list A)
        seen.Clear();
        await Items.Fuel.Set(can, 1500 * 64);
        await WaitFor(e => e.Value is { } v && v.Units == 96000, 20);
        seen.Clear();
        var slot = Items.Read(chest)?.Slots.FirstOrDefault(s => s.ItemEntity == can);
        var take = slot is null ? null : await Items.TakeSlot(chest, slot);
        var gone = await WaitFor(e => e.Value is null, 60);
        Result(take is { Done: > 0 } && gone.Ok, "sidecar destroy",
            $"CLEAR after destroy (list A): took the can ({take?.Status}); {(gone.Ok ? $"cleared on peer with, createTick {gone.E!.CreateTick}" : "no CLEAR")}");

        // hot reload: the extension goes and comes back (a new id), its provider with it
        var (can2, _) = await NewCan(chest);
        foreach (var f in fakes) ReplicationLab.ForceRelevant(f.Machine, can2);
        can = can2;
        seen.Clear();
        await Items.Fuel.Set(can, 1500 * 64);
        var pre = await WaitFor(e => e.Value is { } v && v.Units == 96000, 120);
        var oldId = ext.Id;
        var defsBefore = SidecarServer.Core?.PeerStates.TryGetValue(with.Id, out var p1) == true ? p1.DefsVersion : -1;
        ext.Changed -= OnChanged;
        _waterProvider?.Dispose();
        ext.Dispose();
        _water = ext = Replication.Extension<CanWater>(Water, 1);
        _waterProvider = ext.On<FuelAttribute>(Provide);
        ext.Changed += OnChanged;
        seen.Clear();
        var reloadAt = step;
        var back = await WaitFor(e => e.Value is { } v && v.Units == 96000, 10);
        var defsAfter = SidecarServer.Core?.PeerStates.TryGetValue(with.Id, out var p2) == true ? p2.DefsVersion : -1;
        var reloadSteps = back.Ok ? (long)(back.At - reloadAt) : -1;
        Result(pre.Ok && back.Ok && ext.Id != oldId && defsAfter != defsBefore && reloadSteps <= 2, "sidecar reload",
            $"DEFS resent (version {defsBefore} -> {defsAfter}), ids remapped ({oldId} -> {ext.Id}), values resent within {reloadSteps} step(s)");

        ext.Changed -= OnChanged;
        ReplicationLabInternals.BindMachine(with.Id, -1);
        ReplicationLabInternals.BindMachine(without.Id, -1);
        try
        {
            template?.Set("FuelSetup.maxFuelCharges", 100);
        }
        catch (Exception)
        {
            // the lab's template is only this session's
        }
        Info($"sidecar: done; peer with got {Frames(with)} kiln:repl frame(s), peer without {Math.Max(0, Frames(without))}");
        return fakes;
    }

    // A watering can made now in the chest (its template's capacity at this moment), and its water max.
    private async Task<(EntityId Can, int Max)> NewCan(EntityId chest)
    {
        var give = await Items.Give(chest, Items.Lookup(Can)!.Id, 1, options: GiveOptions.NoMerge);
        await Steps(2);
        var slot = give.Changes.Count > 0 ? Items.Read(chest)?.Slots.FirstOrDefault(s => s.Slot == give.Changes[0].Slot) : null;
        var can = slot?.ItemEntity ?? EntityId.None;
        return (can, can.IsNone ? 0 : Items.Fuel.Read(can)?.Max ?? 0);
    }
}
