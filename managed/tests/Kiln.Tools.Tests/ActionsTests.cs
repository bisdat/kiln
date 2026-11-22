// SPDX-License-Identifier: MIT
using Kiln;
using Xunit;
public sealed class ActionsTests {
    [Fact] public void ActionsAreDeclarativeAndTerrainOnlyByDefault() {
        var a = new TerrainToolAction { Mode = TerrainToolMode.Remove, RemoveMaterials = [8, 91] };
        Assert.True(a.TerrainOnly);
        Assert.Equal(TerrainToolMode.Remove, a.Mode);
        Assert.Equal(new byte[] { 8, 91 }, a.RemoveMaterials);
    }
    [Fact] public void EmptyMaterialSetDoesNotAuthorizeAnyTerrain() {
        Assert.Empty(new TerrainToolAction { Mode = TerrainToolMode.Add }.RemoveMaterials);
    }
}
