// SPDX-License-Identifier: MIT
// GameTexture (Kiln.Api, 0.2.0-dev.30.3) in a KilnHarness: Kiln's own code, offline as in the game, here on a mip chain a
// mod stores and reads back through the kit's kiln.assets.
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed class GameTextureKitTests
{
    private sealed class TextureMod : Mod;

    private static byte[] Solid(int w, int h, byte r, byte g, byte b)
    {
        var rgba = new byte[w * h * 4];
        for (var i = 0; i < rgba.Length; i += 4) (rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]) = (r, g, b, 255);
        return rgba;
    }

    [Fact]
    public void A_mod_encodes_a_mip_chain_stores_it_as_a_blob_and_decodes_every_level()
    {
        Assert.Contains(Coverage.All, r => r.Area == "Game textures (GameTexture)" && r.Kind == CoverageKind.Real && r.Capabilities.Count == 0);
        using var kiln = new KilnHarness();
        var mod = kiln.Load<TextureMod>(new ModManifest("texture-kit"));
        kiln.Step(2);

        // BC1, 8x8 with 4 levels: 8x8 (4 blocks), then 4x4, 2x2 and 1x1 (a block each, the small ones padded)
        var chain = kiln.As(mod, () =>
        {
            var level0 = GameTexture.Encode(Solid(8, 8, 255, 0, 0), 8, 8, GamePixelFormat.BC1RgbaUnormBlock);
            var block = GameTexture.Encode(Solid(4, 4, 0, 0, 255), 4, 4, GamePixelFormat.BC1RgbaUnormBlock);
            return level0.Concat(block).Concat(block).Concat(block).ToArray();
        });
        Assert.Equal(32 + 8 + 8 + 8, chain.Length);
        Assert.Equal(48, GameTexture.MipOffset(GamePixelFormat.BC1RgbaUnormBlock, 8, 8, 3));

        var hash = kiln.As(mod, () => Assets.AddBlob(chain));
        var levels = kiln.As(mod, () =>
        {
            var data = Assets.ReadBlob(hash)!;
            return Enumerable.Range(0, 4).Select(mip => GameTexture.DecodeMip(data, GamePixelFormat.BC1RgbaUnormBlock, 8, 8, mip)).ToArray();
        });
        Assert.Equal(Solid(8, 8, 255, 0, 0), levels[0]);
        Assert.Equal(Solid(4, 4, 0, 0, 255), levels[1]);
        Assert.Equal(Solid(2, 2, 0, 0, 255), levels[2]);
        Assert.Equal(Solid(1, 1, 0, 0, 255), levels[3]);
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.DecodeMip(chain, GamePixelFormat.BC1RgbaUnormBlock, 8, 8, 4));
    }
}
