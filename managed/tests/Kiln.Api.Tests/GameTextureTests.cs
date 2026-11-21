// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.Api.Tests;

/// <summary>GameTexture beyond the Lua fixtures: the game's format numbers, mip chains, sizes and the exceptions.</summary>
public sealed class GameTextureTests
{
    [Fact]
    public void Format_values_are_the_games_pixel_format_numbers()
    {
        // keen::PixelFormat values of the game's reflection (crates/kiln-manage/src/gameart/texture.rs builtin_name)
        Assert.Equal(9, (int)GamePixelFormat.R8Unorm);
        Assert.Equal(22, (int)GamePixelFormat.R8G8Srgb);
        Assert.Equal(37, (int)GamePixelFormat.R8G8B8A8Unorm);
        Assert.Equal(50, (int)GamePixelFormat.B8G8R8A8Srgb);
        Assert.Equal(70, (int)GamePixelFormat.R16Unorm);
        Assert.Equal(131, (int)GamePixelFormat.BC1RgbUnormBlock);
        Assert.Equal(143, (int)GamePixelFormat.BC6HUfloatBlock);
        Assert.Equal(146, (int)GamePixelFormat.BC7SrgbBlock);
        Assert.Equal(147, Enum.GetValues<GamePixelFormat>().Length);
    }

    [Fact]
    public void Member_names_are_the_games_names_in_dotnet_style()
    {
        // R8G8B8A8_unorm -> R8G8B8A8Unorm, BC1_RGB_unorm_block -> BC1RgbUnormBlock, D16_unorm_S8_uint -> D16UnormS8Uint
        static string DotNet(string game)
        {
            var parts = game.Split('_');
            return parts[0] + string.Concat(parts.Skip(1).Select(p =>
                char.IsLower(p[0]) ? char.ToUpperInvariant(p[0]) + p[1..] : p.All(char.IsLetter) ? p[0] + p[1..].ToLowerInvariant() : p));
        }
        foreach (var f in Enum.GetValues<GamePixelFormat>())
            Assert.Equal(DotNet(PixelCodec.GameName(f)), f.ToString());
        Assert.Equal("R8G8B8A8_unorm", PixelCodec.GameName(GamePixelFormat.R8G8B8A8Unorm));
        Assert.Equal("BC7_srgb_block", PixelCodec.GameName(GamePixelFormat.BC7SrgbBlock));
    }

    [Fact]
    public void A_mip_chain_is_its_levels_one_after_another()
    {
        // a 256x256 BC7 icon with 9 levels is 65536 + 16384 + ... + 16 + 16 + 16 bytes (the game's own icons)
        var last = GameTexture.MipOffset(GamePixelFormat.BC7UnormBlock, 256, 256, 8);
        Assert.Equal(87_408, last + GameTexture.ByteSize(GamePixelFormat.BC7UnormBlock, 1, 1));
        Assert.Equal(65_536, GameTexture.MipOffset(GamePixelFormat.BC7UnormBlock, 256, 256, 1));
        Assert.Equal(2 * 4 * 2 + 1 * 2 * 2, GameTexture.MipOffset(GamePixelFormat.R8G8Unorm, 2, 4, 2)); // 2x4, 1x2, then 1x1
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.MipOffset(GamePixelFormat.R8Unorm, 2, 4, 3));
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.MipOffset(GamePixelFormat.R8Unorm, 0, 4, 0));
    }

    [Fact]
    public void A_block_level_of_any_size_is_its_whole_blocks_cut_to_the_level()
    {
        var rgba = new byte[8 * 8 * 4];
        for (var i = 0; i < rgba.Length; i++) rgba[i] = (byte)(i * 7);
        var blocks = GameTexture.Encode(rgba, 8, 8, GamePixelFormat.BC3UnormBlock);
        var whole = GameTexture.Decode(blocks, GamePixelFormat.BC3UnormBlock, 8, 8);
        // a 6x6 level is stored as 8x8 of blocks
        var level = GameTexture.DecodeMip(blocks, GamePixelFormat.BC3UnormBlock, 6, 6, 0);
        Assert.Equal(6 * 6 * 4, level.Length);
        for (var y = 0; y < 6; y++)
            Assert.Equal(whole.AsSpan(y * 8 * 4, 6 * 4).ToArray(), level.AsSpan(y * 6 * 4, 6 * 4).ToArray());
        // Decode keeps the Lua function's rule
        Assert.Throws<ArgumentException>(() => GameTexture.Decode(blocks, GamePixelFormat.BC3UnormBlock, 6, 6));
        Assert.Throws<ArgumentException>(() => GameTexture.DecodeMip(blocks.AsSpan(0, 63), GamePixelFormat.BC3UnormBlock, 6, 6, 0));
    }

    [Fact]
    public void Failures_are_dotnet_exceptions_with_the_lua_texts()
    {
        var e = Assert.Throws<NotSupportedException>(() => GameTexture.Decode(new byte[4], GamePixelFormat.R8Srgb, 1, 1));
        Assert.Equal("R8_srgb decoding not implemented", e.Message);
        Assert.Throws<NotSupportedException>(() => GameTexture.Decode(new byte[8], GamePixelFormat.R32Uint, 1, 1));
        Assert.Empty(GameTexture.Decode([], GamePixelFormat.R32Uint, 0, 1));
        Assert.Throws<NotSupportedException>(() => GameTexture.Encode(new byte[8], 2, 1, GamePixelFormat.R16Sfloat));
        Assert.Throws<NotSupportedException>(() => GameTexture.ByteSize(GamePixelFormat.D24UnormS8Uint, 4, 4));
        Assert.Equal(16, GameTexture.ByteSize(GamePixelFormat.BC6HSfloatBlock, 4, 4));
        Assert.Equal(16, GameTexture.ByteSize(GamePixelFormat.A2B10G10R10UnormPack32, 2, 2));
        var short_ = Assert.Throws<ArgumentException>(() => GameTexture.Decode(new byte[3], GamePixelFormat.R8G8B8A8Unorm, 1, 1));
        Assert.Equal("data", short_.ParamName);
        Assert.Throws<ArgumentException>(() => GameTexture.Encode(new byte[3], 1, 1, GamePixelFormat.R8Unorm));
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.Decode([], GamePixelFormat.R8Unorm, -1, 1));
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.Decode([], (GamePixelFormat)200, 0, 0));
    }

    [Fact]
    public void Hostile_sizes_throw_the_documented_exception_and_never_wrap()
    {
        // (2^31 - 1)^2 pixels: their RGBA8 bytes (and 32 bytes a pixel) overflow a long, which must not wrap past the checks
        const int max = int.MaxValue;
        var data = new byte[64];
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.Decode(data, GamePixelFormat.R8G8B8A8Unorm, max, max));
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.Decode(data, GamePixelFormat.R8Unorm, max, max));
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.Decode(data, GamePixelFormat.BC1RgbaUnormBlock, max - 3, max - 3));
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.ByteSize(GamePixelFormat.R64G64B64A64Uint, max, max));
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.ByteSize(GamePixelFormat.R8Unorm, max, max));
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.MipOffset(GamePixelFormat.R64G64B64A64Sfloat, max, max, 1));
        // a block level whose whole blocks pass int.MaxValue pixels wide
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.DecodeMip(data, GamePixelFormat.BC1RgbaUnormBlock, max, 4, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.DecodeMip(data, GamePixelFormat.BC7UnormBlock, max, max, 0));
        // the largest sizes that still fit are answered, not refused
        Assert.Equal(Array.MaxLength / 32 * 32, GameTexture.ByteSize(GamePixelFormat.R64G64B64A64Uint, Array.MaxLength / 32, 1));
        Assert.Equal(16, GameTexture.ByteSize(GamePixelFormat.BC7UnormBlock, 1, 1));
        Assert.Throws<ArgumentException>(() => GameTexture.Decode(data, GamePixelFormat.R8Unorm, Array.MaxLength / 4, 1));  // short data, not a size
    }

    [Fact]
    public void Eight_bit_and_packed_formats_round_trip()
    {
        byte[] rgba = [10, 20, 30, 40, 250, 128, 0, 255];
        foreach (var f in new[] { GamePixelFormat.R8G8B8A8Unorm, GamePixelFormat.B8G8R8A8Unorm, GamePixelFormat.A8B8G8R8UnormPack32, GamePixelFormat.R8G8B8A8Snorm })
            Assert.Equal(rgba, GameTexture.Decode(GameTexture.Encode(rgba, 2, 1, f), f, 2, 1));
        // A8B8G8R8 stores the bytes A, B, G, R
        Assert.Equal(new byte[] { 40, 30, 20, 10 }, GameTexture.Encode(rgba.AsSpan(0, 4), 1, 1, GamePixelFormat.A8B8G8R8UnormPack32));
        // 5-bit fields round to nearest and decode by bit replication
        var p = GameTexture.Decode(GameTexture.Encode([255, 0, 132, 255], 1, 1, GamePixelFormat.R5G6B5UnormPack16), GamePixelFormat.R5G6B5UnormPack16, 1, 1);
        Assert.Equal(new byte[] { 255, 0, 132, 255 }, p);
    }
}
