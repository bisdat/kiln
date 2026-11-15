// This offline test deliberately drives the lab-only world lifecycle bridge.
#pragma warning disable KILNX001
using Kiln;
using Kiln.TestKit;
using System.Reflection;
using Xunit;
namespace TerrainTests;
public sealed class SavesWorldProofTests
{
    [Fact]
    public void Terrain_cannot_trust_sidecar_binding_without_actual_load_and_write_hooks()
    {
        var check=typeof(Saves).Assembly.GetType("Kiln.SavesRuntime")!.GetMethod("TerrainPersistenceAvailable",BindingFlags.NonPublic|BindingFlags.Static)!;
        Assert.False((bool)check.Invoke(null,[0u])!);
        Assert.False((bool)check.Invoke(null,[2u])!);
        Assert.False((bool)check.Invoke(null,[4u])!);
        Assert.True((bool)check.Invoke(null,[6u])!);
    }

    [Fact]
    public void New_world_proof_is_retired_on_close_and_is_not_inherited_by_an_existing_world()
    {
        using var h=new KilnHarness();
        var proof=typeof(Saves).Assembly.GetType("Kiln.SavesRuntime")!.GetProperty("NewWorldSession",BindingFlags.NonPublic|BindingFlags.Static)!;
        h.Run(()=> {
            Saves.LabWorldLoaded(null,0,0,true);
            Assert.True((bool)proof.GetValue(null)!);
            Saves.LabCloseWorld();
            Assert.False((bool)proof.GetValue(null)!);
            Saves.LabWorldLoaded("6f1c2a3b-0000-4000-8000-00000000000a",7,0,false);
            Assert.False((bool)proof.GetValue(null)!);
        });
    }
}
