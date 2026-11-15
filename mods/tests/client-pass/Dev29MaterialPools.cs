// SPDX-License-Identifier: MIT
using System.Text.Json;
using Kiln;
using Kiln.Testing;
using MaterialPoolsTest;
using static Kiln.Testing.ClientCheckTags;
namespace ClientPass;

public sealed partial class ClientPassMod
{
    [ClientCheck("17-material-pools","Custom material pools on new shapes",Area="Material limits",Since=Dev29,
        Tags=[Visual,TwoPlayers,RequiredExpectations],Needs=ClientCheckNeeds.Any,Order=5500)]
    public async Task MaterialPools(CheckContext c)
    {
        c.Expect(c.Session!=ClientCheckSession.Server && c.Player!=EntityId.None,"a real hosting or joined player is present");
        c.Expect(Mods.Get("mp-fixture")?.IsLoaded==true,"the mp-fixture mod is loaded");
        c.Expect(Capabilities.Is("worldtables.material_pools"),"this executable resolves custom pool inputs",Capabilities.Why("worldtables.material_pools"));
        try {
            foreach(var (name,ok,detail) in PoolProof.Inspect())c.Expect(ok,name,detail);
        } catch(Exception e){c.Expect(false,"served pools, shape rows and actual item references can be inspected",e.Message);}
        try {
            var dir=Mods.Get("mp-fixture")?.Directory??throw new InvalidOperationException("mp-fixture is absent");
            using var proof=JsonDocument.Parse(File.ReadAllText(Path.Combine(dir,"lab-proof.json")));
            var p=proof.RootElement;var ran=p.GetProperty("passed").EnumerateArray().Select(v=>v.GetString()).ToHashSet(StringComparer.Ordinal);
            string[] required=["served","restart","absent","back","late-material-room","handshake-order-before-stream","handshake-binding-before-stream","late","replay","torn-refused","restored-main-hash","building-seed","building-retained","terrain-boundary"];
            c.Expect(p.GetProperty("format").GetInt32()==1 && p.GetProperty("complete").GetBoolean() && required.All(ran.Contains),"all coordinator lab cases passed, including refusals before streaming");
            c.Expect(p.GetProperty("hash").GetString()==WorldTables.Hash,"the lab proof is for this exact material/pool/binding profile",WorldTables.Hash);
        } catch(Exception e){c.Expect(false,"the coordinator's complete lab-proof.json is installed in mp-fixture",e.Message);}
        c.Instruct("Use disposable characters and a disposable world. Do this on a hosting client AND a client joined to the coordinator's server (record each session). " +
            "Craft the three Pool blocks from rough stone (one rough-stone block makes ten); prepare Rough Wood and Mycelium blocks too. " +
            "In the build menu choose Pool Four Wall: it offers exactly Ice, Frost, Slate, Rough Wood in that order, never a fifth entry. " +
            "Cycle first and last, confirm the selected block's name and inventory count, preview the three-cell L, and place it in open space. Each placement consumes exactly three chosen blocks; " +
            "the three filled cells match the preview and chosen surface. Mine the custom cells: each gives one block of its own material, with no loss or duplicate loot. " +
            "Pool Shared Wall offers only Ice and Rough Stone, using the same Ice inventory and appearance as the first pool. Pool Full Wall lists all 64 distinct choices without truncation; " +
            "select and place its first (Mycelium) and last (Slate) choices. Pool Roof offers only Plant Fibre and places the roof version of the same L. " +
            "Ordinary vanilla wall, roof and terrain shapes retain their original lists; none lists Ice, Frost or Slate. " +
            "Leave and reload, rejoin the server, then host a second world in the SAME process: selections, previews, costs, placed surfaces and mined loot still work, with no old-world reference or crash. " +
            "Also try joining with the fixture's four-material order changed, and with Four Wall bound to Shared: each mismatching client is refused before any blocks or templates stream. " +
            "Leaving a world ends this running check: reopen it in the final world/session after the reload exercises. Pass only if EVERY visual and lifecycle requirement above happened; " +
            "otherwise Fail with the first mismatch. All failed automatic requirements force a Fail even if you press Pass.");
        await c.Verdict();
    }
}
