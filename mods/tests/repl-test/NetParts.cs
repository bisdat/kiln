// SPDX-License-Identifier: MIT
// kiln.repl.net on the loopback harness (package 15, DESIGN "What each lab proves"): per-peer filters, touch and the
// item remap, on the fakes the inspector parts leave (a fake without a bound kiln.net lab peer is a vanilla peer).
//   filter      a NetworkFuel filter for vanilla peers: those fakes decode the filter's bytes, a bound fake the live
//               ones; after Dispose every fake decodes the live bytes again within a few steps (the touch on removal);
//               a filter that throws is removed and its fakes decode the live bytes
//   touch       Replication.Touch: queued for the can, "entity gone" for a free id; a filter with Sources = ["Fuel"]
//               re-sends the can when only Fuel changes (NetworkFuel's filter output depends on it)
//   remap       two Kiln-only items (clones of the watering can, one with VanillaLook = the can, one without) in the
//               chest: a vanilla fake decodes the can's id in place of the first, 0 in place of the second, and every
//               other item id unchanged, in Inventory and in the items' own entities (ItemState, UsedItem); a bound fake
//               (Kiln, content acknowledged) decodes the real ids
// The parts to run: kiln/config/repl-test.json {"parts": ["filter", "touch", "remap"]}; without the file, all.
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Kiln;

namespace ReplTest;

/// <summary>The game's NetworkFuel as this test edits it: what a client draws as the fuel gauge (two 16-bit values).</summary>
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct NetworkFuelView : IGameComponent<NetworkFuelView>
{
    public ushort Fuel, Max;
    public static uint Hash => ReplNetTest.FuelHash;
    public static string Name => "NetworkFuel";
    public static string QualifiedName => "keen::ecs::NetworkFuel";
    public static int Size => 4;
    public static ulong Layout => 0;
}

/// <summary>The game's ServerInfoMessage as raw bytes (0x108 bytes on f0d824be): the codec's round trip.</summary>
[StructLayout(LayoutKind.Sequential, Size = 0x108)]
public struct ServerInfoRaw : IGameMessage
{
    private byte _first;
    public static string SchemaName => "ServerInfoMessage";
}

internal static class ReplNetTest
{
    internal static readonly uint FuelHash = Fnv("keen::ecs::NetworkFuel");
    internal static readonly Guid LookGuid = new("6f0e5a51-1f0b-4c55-9c1e-15a0e0a1d001");
    internal static readonly Guid NoLookGuid = new("6f0e5a51-1f0b-4c55-9c1e-15a0e0a1d002");

    private static uint Fnv(string s)
    {
        var h = 2166136261u;
        foreach (var c in System.Text.Encoding.UTF8.GetBytes(s)) h = (h ^ c) * 16777619u;
        return h;
    }
}

public sealed partial class ReplTestMod
{
    private HashSet<string>? _parts;

    private bool Part(string name)
    {
        if (_parts == null)
        {
            _parts = [];
            try
            {
                var file = Path.Combine(Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "..")), "config", "repl-test.json");
                if (File.Exists(file) && JsonNode.Parse(File.ReadAllText(file))?["parts"] is JsonArray a)
                    foreach (var p in a) if ((string?)p is { } s) _parts.Add(s);
            }
            catch (Exception e)
            {
                Info($"parts: {e.Message}; running all");
            }
        }
        return _parts.Count == 0 || _parts.Contains(name);
    }

    // Two Kiln-only items for the remap part (registered in OnLoad, before the world loads).
    private void RegisterRemapItems()
    {
        try
        {
            Content.Items.Register(new ItemDefinition
            {
                Guid = ReplNetTest.LookGuid, Name = "repl_test_lookalike", BaseOn = Can, DisplayName = "Repl test can (look-alike)",
                VanillaLook = Can,
            });
            Content.Items.Register(new ItemDefinition
            {
                Guid = ReplNetTest.NoLookGuid, Name = "repl_test_nolook", BaseOn = Can, DisplayName = "Repl test can (no look-alike)",
            });
        }
        catch (Exception e)
        {
            Log.Warn($"{Me}: remap items not registered: {e.Message}");
        }
    }

    private (uint Peer, byte Flags)[] Machines()
    {
        Span<uint> ids = stackalloc uint[17];
        Span<byte> flags = stackalloc byte[17];
        ReplNetNative.MachinePeers(ids, flags);
        var r = new (uint, byte)[17];
        for (var i = 0; i < 17; i++) r[i] = (ids[i], flags[i]);
        return r;
    }

    // a fake is vanilla when no kiln.net lab peer is bound to it (Peers.Vanilla)
    private bool IsVanilla(int machine) => FilterHost.PeersOf(Peers.Vanilla).Contains(Machines()[machine].Peer);

    private async Task NetParts(EntityId chest, EntityId can, IReadOnlyList<(int Machine, int Slot)> fakes, int max)
    {
        // the inspector's destroy part took the can out: a fresh one in the chest
        if (World.ReadBytes(can, "NetworkFuel") is null)
        {
            var give = await Items.Give(chest, Items.Lookup(Can)!.Id, 1, options: GiveOptions.NoMerge);
            await Steps(2);
            var slot = give.Changes.Count > 0 ? Items.Read(chest)?.Slots.FirstOrDefault(s => s.Slot == give.Changes[0].Slot) : null;
            can = slot?.ItemEntity ?? EntityId.None;
            max = can.IsNone ? 0 : Items.Fuel.Read(can)?.Max ?? 0;
            if (!Result(!can.IsNone && max > 0, "net setup", $"a fresh watering can {can} in chest {chest}, water max {max}")) return;
        }
        Info($"kiln.repl.net capabilities 0x{ReplNetNative.Capabilities():x}; rows {Capabilities.Get("repl.filter").State} filter, " +
             $"{Capabilities.Get("repl.touch").State} touch, {Capabilities.Get("repl.itemremap").State} itemremap; NetworkFuel " +
             $"{Replication.Component("NetworkFuel")}");
        foreach (var f in fakes)
        {
            ReplicationLab.ForceRelevant(f.Machine, chest);
            ReplicationLab.ForceRelevant(f.Machine, can);
        }
        var vanilla = fakes.Where(f => IsVanilla(f.Machine)).Select(f => f.Machine).ToList();
        var kiln = fakes.Select(f => f.Machine).Except(vanilla).ToList();
        Info($"net parts: vanilla fakes {string.Join(",", vanilla)}, Kiln fakes {(kiln.Count == 0 ? "none" : string.Join(",", kiln))}");
        if (Part("filter")) await FilterPart(can, fakes, vanilla, max);
        if (Part("touch")) await TouchPart(can, fakes, vanilla, max);
        if (Part("remap")) await RemapPart(chest, fakes, vanilla);
        if (Part("messages")) await MessagesPart(fakes);
    }

    // The game's codec and the message hooks: a lab has no game session, so no real traffic: the codec's round trip, the
    // hooks going in and out, an inject to a peer without a game session refused cleanly.
    private async Task MessagesPart(IReadOnlyList<(int Machine, int Slot)> fakes)
    {
        var info = Replication.Schema("ServerInfoMessage");
        var chat = Replication.Schema("ChatMessageServerMessage") ?? Replication.Schema("ChatMessageClientMessage");
        Info($"messages: schemas ServerInfoMessage {(info == null ? "none" : $"{info.Size} B, {info.Fields.Count} fields")}, chat " +
             $"{(chat == null ? "none" : $"{chat.Name} {chat.Size} B: {string.Join(", ", chat.Fields.Select(f => $"{f.Name}:{f.Type}/{f.Shape}@{f.Offset}"))}")}");
        try
        {
            var zero = default(ServerInfoRaw);
            var bytes = Replication.Encode(zero);
            var back = Replication.Decode<ServerInfoRaw>(bytes);
            var again = Replication.Encode(back);
            Result(info != null && bytes.Length > 0 && bytes.AsSpan().SequenceEqual(again), "messages codec",
                $"ServerInfoMessage ({info?.Size} B) encoded to {bytes.Length} B, decoded and encoded again to the same {again.Length} B");
        }
        catch (Exception e)
        {
            Result(false, "messages codec", $"{e.GetType().Name}: {e.Message}");
        }
        try
        {
            Replication.Decode<ServerInfoRaw>(new byte[] { 0xff, 0xff, 0xff });
            Result(false, "messages codec refusal", "3 bytes of 0xff decoded as a ServerInfoMessage");
        }
        catch (FormatException e)
        {
            Result(true, "messages codec refusal", $"3 bytes of 0xff refused: {e.Message}");
        }
        catch (Exception e)
        {
            Result(false, "messages codec refusal", $"{e.GetType().Name}: {e.Message}");
        }
        var seen = 0;
        IDisposable? hook = null;
        try
        {
            hook = Replication.OnMessage(GameChannel.ChatMessages, MessageDirection.Send | MessageDirection.Receive, (NetPeer? p, ReadOnlySpan<byte> d, ref MessageRewrite r) =>
            {
                seen++;
                return MessageAction.Pass;
            });
            await Steps(30);
            hook.Dispose();
            await Steps(3);
            var refused = Network.Peers.FirstOrDefault() is { } lab ? Replication.Inject(lab, GameChannel.ChatMessages, [1, 2, 3, 4]) : null;
            Result(refused is null || !refused.Ok, "messages hooks",
                $"a chat hook (send and receive) went in and out ({seen} messages seen: a lab has no game session); inject to a lab peer: " +
                $"{(refused == null ? "no lab peer" : $"Ok {refused.Ok}, {refused.Reason}")}");
        }
        catch (Exception e)
        {
            hook?.Dispose();
            Result(false, "messages hooks", $"{e.GetType().Name}: {e.Message}");
        }
    }

    // Steps until every fake's newest decode of the can's NetworkFuel matches (vanilla: want, else live); the steps used.
    private async Task<(bool Ok, int Steps, string Detail)> WaitFuel(EntityId can, IReadOnlyList<(int Machine, int Slot)> fakes,
        IReadOnlyCollection<int> filtered, byte[]? want, int limit)
    {
        string detail = "";
        for (var step = 0; step <= limit; step++)
        {
            var live = World.ReadBytes(can, "NetworkFuel");
            var all = live != null;
            var parts = new List<string>();
            foreach (var f in fakes)
            {
                var d = ReplicationLab.Decoded(f.Machine, can, "NetworkFuel");
                var expect = want != null && filtered.Contains(f.Machine) ? want : live;
                var ok = d != null && expect != null && d.Bytes.Span.SequenceEqual(expect);
                all &= ok;
                parts.Add($"{f.Machine}:{(d == null ? "none" : Convert.ToHexString(d.Bytes.Span))}{(ok ? "" : "!")}");
            }
            detail = $"live {(live == null ? "none" : Convert.ToHexString(live))}; fakes {string.Join(" ", parts)}";
            if (all) return (true, step, detail);
            await Steps(1);
        }
        return (false, limit, detail);
    }

    private async Task FilterPart(EntityId can, IReadOnlyList<(int Machine, int Slot)> fakes, List<int> vanilla, int max)
    {
        await Items.Fuel.Set(can, max * 3 / 4);
        await Steps(5);
        byte[] want = [0xC0, 0xBF, 0xC0, 0xFF];
        IDisposable? filter = null;
        try
        {
            filter = Replication.Filter<NetworkFuelView>(Peers.Vanilla, (ref NetworkFuelView v, in FilterContext c) =>
            {
                v.Fuel = 0xBFC0;
                v.Max = 0xFFC0;
            });
        }
        catch (Exception e)
        {
            Result(false, "filter", $"Replication.Filter threw {e.GetType().Name}: {e.Message}");
            return;
        }
        // the touch on add re-sends the can: no fuel change needed
        var on = await WaitFuel(can, fakes, vanilla, want, 10);
        Result(on.Ok, "filter", $"vanilla fakes decode C0BFC0FF, the others the live bytes, {on.Steps} step(s) after Filter: {on.Detail}");
        filter.Dispose();
        var off = await WaitFuel(can, fakes, vanilla, null, 10);
        Result(off.Ok, "filter dispose", $"every fake decodes the live bytes {off.Steps} step(s) after Dispose: {off.Detail}");
        // a filter that throws: removed, the game's bytes go out
        var threw = Replication.Filter<NetworkFuelView>(Peers.All, (ref NetworkFuelView v, in FilterContext c) =>
        {
            v.Fuel = 1;
            throw new InvalidOperationException("repl-test: a filter that throws");
        });
        await Items.Fuel.Set(can, max / 2);
        var raw = await WaitFuel(can, fakes, [], null, 10);
        await Steps(3);
        var removed = threw is FilterHost.Box b && b.Disposed;
        Result(raw.Ok && removed, "filter fault", $"the throwing filter was removed ({removed}) and every fake decodes the live bytes: {raw.Detail}");
        threw.Dispose();
    }

    private async Task TouchPart(EntityId can, IReadOnlyList<(int Machine, int Slot)> fakes, List<int> vanilla, int max)
    {
        var t = Replication.Touch(can);
        var gone = Replication.Touch(new EntityId(0x7ffff000));
        Result(t.Ok && !gone.Ok && gone.Reason == "entity gone", "touch", $"can: {t}; a free id: {gone}");
        // Sources: the filter's output is the can's Fuel (not NetworkFuel): a Fuel change alone must re-send it
        var value = (ushort)0;
        IDisposable? f = null;
        try
        {
            f = Replication.Filter<NetworkFuelView>(Peers.Vanilla, (ref NetworkFuelView v, in FilterContext c) =>
            {
                v.Fuel = value;
                v.Max = 0xFFC0;
            }, new FilterOptions { Sources = ["Fuel"] });
        }
        catch (Exception e)
        {
            Result(false, "touch sources", $"Filter with Sources threw {e.GetType().Name}: {e.Message}");
            return;
        }
        value = 0x1000;
        await Steps(3);
        await Items.Fuel.Set(can, max / 4);   // Fuel and NetworkFuel change: the next send carries 0x1000
        var a = await WaitFuel(can, fakes, vanilla, [0x00, 0x10, 0xC0, 0xFF], 10);
        value = 0x2000;                       // the output changes, NetworkFuel does not: only a touch re-sends it
        var fuel = World.ReadBytes(can, "Fuel");
        await Items.Fuel.Set(can, max / 4 + 1);
        var b = await WaitFuel(can, fakes, vanilla, [0x00, 0x20, 0xC0, 0xFF], 20);
        Result(a.Ok && b.Ok, "touch sources", $"re-snapshot after a Sources change: first {a.Steps} step(s) ({a.Detail}); after a Fuel change " +
            $"{b.Steps} step(s) ({b.Detail}); Fuel was {(fuel == null ? "none" : Convert.ToHexString(fuel))}");
        f.Dispose();
        await Steps(5);
    }

    private async Task RemapPart(EntityId chest, IReadOnlyList<(int Machine, int Slot)> fakes, List<int> vanilla)
    {
        var look = Items.Lookup("repl_test_lookalike");
        var none = Items.Lookup("repl_test_nolook");
        var can = Items.Lookup(Can);
        if (!Result(look != null && none != null && can != null, "remap setup", $"Kiln-only items {look?.Id} (look-alike) {none?.Id} (none), can {can?.Id}")) return;
        // a bound fake is a Kiln peer with the server's content for this test (the nettest peer does not ack content)
        var was = Peers.ContentCurrent;
        Peers.ContentCurrent = _ => true;
        try
        {
            var g1 = await Items.Give(chest, look!.Id, 1, options: GiveOptions.NoMerge);
            var g2 = await Items.Give(chest, none!.Id, 1, options: GiveOptions.NoMerge);
            await Steps(5);
            var inv = Items.Read(chest);
            var entities = inv?.Slots.Where(s => s.Item == look.Id || s.Item == none.Id).Select(s => s.ItemEntity).Where(e => !e.IsNone).ToList() ?? [];
            foreach (var f in fakes)
                foreach (var e in entities)
                    ReplicationLab.ForceRelevant(f.Machine, e);
            // wait for the remap table (Kiln checks its content every 2 s)
            for (var i = 0; i < 300 && !Replication.RemapStats().Active; i++) await Steps(1);
            var stats = Replication.RemapStats();
            Info($"remap stats before: {stats}");
            await Steps(10);
            var sites = ItemRemap.BuildSites();
            var byHash = sites.GroupBy(s => s.ComponentHash).ToDictionary(g => g.Key, g => g.ToList());
            var checkedSites = 0;
            var problems = new List<string>();
            var seenLook = 0;
            var seenNone = 0;
            void Compare(EntityId e, string component)
            {
                var hash = Replication.Component(component)?.Hash ?? 0;
                if (!byHash.TryGetValue(hash, out var list)) return;
                var live = World.ReadBytes(e, component);
                if (live == null) return;
                foreach (var f in fakes)
                {
                    var d = ReplicationLab.Decoded(f.Machine, e, component);
                    if (d == null) continue;
                    var isVanilla = vanilla.Contains(f.Machine);
                    foreach (var s in list)
                        for (var k = 0; k < Math.Max(1, (int)s.Count); k++)
                        {
                            var off = s.Offset + k * s.Stride;
                            if (off + 4 > live.Length || off + 4 > d.Bytes.Length) break;
                            var l = BitConverter.ToUInt32(live, off);
                            var v = BitConverter.ToUInt32(d.Bytes.Span[off..]);
                            var expect = !isVanilla ? l : l == look.Id.Value ? can!.Id.Value : l == none.Id.Value ? 0u : l;
                            checkedSites++;
                            if (l == look.Id.Value) seenLook++;
                            if (l == none.Id.Value) seenNone++;
                            if (v != expect)
                                problems.Add($"{component} of {e} fake {f.Machine}{(isVanilla ? " (vanilla)" : "")} +{off}: live 0x{l:x8} decoded 0x{v:x8} want 0x{expect:x8}");
                        }
                }
            }
            Compare(chest, "Inventory");
            foreach (var e in entities)
                foreach (var c in new[] { "ItemState", "UsedItem" }) Compare(e, c);
            var after = Replication.RemapStats();
            Result(problems.Count == 0 && seenLook > 0 && seenNone > 0 && after.Active, "remap",
                $"{checkedSites} site values checked on {fakes.Count} fakes ({vanilla.Count} vanilla), the look-alike item seen {seenLook}x, " +
                $"the one without {seenNone}x; {problems.Count} wrong{(problems.Count > 0 ? ": " + string.Join("; ", problems.Take(6)) : "")}; stats {after}");
            Result(after.Remapped > 0 && after.Zeroed > 0 && after.Faults == 0, "remap-zero",
                $"remapped {after.Remapped}, zeroed {after.Zeroed} (the item without a look-alike), faults {after.Faults}; give {g1.Changes.Count}+{g2.Changes.Count} slots");
        }
        finally
        {
            Peers.ContentCurrent = was;
        }
    }
}
