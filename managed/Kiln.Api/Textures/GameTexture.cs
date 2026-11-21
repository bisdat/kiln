// SPDX-License-Identifier: MIT
// Adapted from ShroudForge by bonsaibauer, MIT (licenses/ShroudForge-LICENSE.txt): the C# form of Kiln's texture
// codecs for kiln mode's Lua functions image.decode_texture and image.encode_texture
// (crates/sfi/sfi-core/src/eml/texture.rs), same formats, same rules, same bytes. The block formats (BC1 to BC7) use
// BlockDecoder and BlockEncoder, the C# port of the block codec that file uses.
using System.Buffers.Binary;

namespace Kiln;

/// <summary>
/// The game's pixel formats (<c>keen::PixelFormat</c>), with the game's numbers: the value a texture resource stores is
/// the enum value. The names are the game's in .NET style (<c>R8G8B8A8_unorm</c> is <see cref="GamePixelFormat.R8G8B8A8Unorm"/>); each
/// member's summary gives the game's own name, the one kiln mode's image functions and the error messages use.
/// <see cref="GameTexture"/> decodes and encodes them.
/// </summary>
/// <remarks>Since 0.2.0-dev.30.3.</remarks>
public enum GamePixelFormat
{
    /// <summary>Game value 0, the game's `None`: no format (refused).</summary>
    None = 0,
    /// <summary>Game value 1, the game's `R4G4_unorm_pack8`: R4 G4 packed in an 8-bit word, unorm.</summary>
    R4G4UnormPack8 = 1,
    /// <summary>Game value 2, the game's `R4G4B4A4_unorm_pack16`: R4 G4 B4 A4 packed in a 16-bit word, unorm.</summary>
    R4G4B4A4UnormPack16 = 2,
    /// <summary>Game value 3, the game's `B4G4R4A4_unorm_pack16`: B4 G4 R4 A4 packed in a 16-bit word, unorm.</summary>
    B4G4R4A4UnormPack16 = 3,
    /// <summary>Game value 4, the game's `R5G6B5_unorm_pack16`: R5 G6 B5 packed in a 16-bit word, unorm.</summary>
    R5G6B5UnormPack16 = 4,
    /// <summary>Game value 5, the game's `B5G6R5_unorm_pack16`: B5 G6 R5 packed in a 16-bit word, unorm.</summary>
    B5G6R5UnormPack16 = 5,
    /// <summary>Game value 6, the game's `R5G5B5A1_unorm_pack16`: R5 G5 B5 A1 packed in a 16-bit word, unorm.</summary>
    R5G5B5A1UnormPack16 = 6,
    /// <summary>Game value 7, the game's `B5G5R5A1_unorm_pack16`: B5 G5 R5 A1 packed in a 16-bit word, unorm.</summary>
    B5G5R5A1UnormPack16 = 7,
    /// <summary>Game value 8, the game's `A1R5G5B5_unorm_pack16`: A1 R5 G5 B5 packed in a 16-bit word, unorm.</summary>
    A1R5G5B5UnormPack16 = 8,
    /// <summary>Game value 9, the game's `R8_unorm`: R8, unorm.</summary>
    R8Unorm = 9,
    /// <summary>Game value 10, the game's `R8_snorm`: R8, snorm.</summary>
    R8Snorm = 10,
    /// <summary>Game value 11, the game's `R8_uscaled`: R8, uscaled.</summary>
    R8Uscaled = 11,
    /// <summary>Game value 12, the game's `R8_sscaled`: R8, sscaled.</summary>
    R8Sscaled = 12,
    /// <summary>Game value 13, the game's `R8_uint`: R8, uint.</summary>
    R8Uint = 13,
    /// <summary>Game value 14, the game's `R8_sint`: R8, sint.</summary>
    R8Sint = 14,
    /// <summary>Game value 15, the game's `R8_srgb`: R8, srgb.</summary>
    R8Srgb = 15,
    /// <summary>Game value 16, the game's `R8G8_unorm`: R8 G8, unorm.</summary>
    R8G8Unorm = 16,
    /// <summary>Game value 17, the game's `R8G8_snorm`: R8 G8, snorm.</summary>
    R8G8Snorm = 17,
    /// <summary>Game value 18, the game's `R8G8_uscaled`: R8 G8, uscaled.</summary>
    R8G8Uscaled = 18,
    /// <summary>Game value 19, the game's `R8G8_sscaled`: R8 G8, sscaled.</summary>
    R8G8Sscaled = 19,
    /// <summary>Game value 20, the game's `R8G8_uint`: R8 G8, uint.</summary>
    R8G8Uint = 20,
    /// <summary>Game value 21, the game's `R8G8_sint`: R8 G8, sint.</summary>
    R8G8Sint = 21,
    /// <summary>Game value 22, the game's `R8G8_srgb`: R8 G8, srgb.</summary>
    R8G8Srgb = 22,
    /// <summary>Game value 23, the game's `R8G8B8_unorm`: R8 G8 B8, unorm.</summary>
    R8G8B8Unorm = 23,
    /// <summary>Game value 24, the game's `R8G8B8_snorm`: R8 G8 B8, snorm.</summary>
    R8G8B8Snorm = 24,
    /// <summary>Game value 25, the game's `R8G8B8_uscaled`: R8 G8 B8, uscaled.</summary>
    R8G8B8Uscaled = 25,
    /// <summary>Game value 26, the game's `R8G8B8_sscaled`: R8 G8 B8, sscaled.</summary>
    R8G8B8Sscaled = 26,
    /// <summary>Game value 27, the game's `R8G8B8_uint`: R8 G8 B8, uint.</summary>
    R8G8B8Uint = 27,
    /// <summary>Game value 28, the game's `R8G8B8_sint`: R8 G8 B8, sint.</summary>
    R8G8B8Sint = 28,
    /// <summary>Game value 29, the game's `R8G8B8_srgb`: R8 G8 B8, srgb.</summary>
    R8G8B8Srgb = 29,
    /// <summary>Game value 30, the game's `B8G8R8_unorm`: B8 G8 R8, unorm.</summary>
    B8G8R8Unorm = 30,
    /// <summary>Game value 31, the game's `B8G8R8_snorm`: B8 G8 R8, snorm.</summary>
    B8G8R8Snorm = 31,
    /// <summary>Game value 32, the game's `B8G8R8_uscaled`: B8 G8 R8, uscaled.</summary>
    B8G8R8Uscaled = 32,
    /// <summary>Game value 33, the game's `B8G8R8_sscaled`: B8 G8 R8, sscaled.</summary>
    B8G8R8Sscaled = 33,
    /// <summary>Game value 34, the game's `B8G8R8_uint`: B8 G8 R8, uint.</summary>
    B8G8R8Uint = 34,
    /// <summary>Game value 35, the game's `B8G8R8_sint`: B8 G8 R8, sint.</summary>
    B8G8R8Sint = 35,
    /// <summary>Game value 36, the game's `B8G8R8_srgb`: B8 G8 R8, srgb.</summary>
    B8G8R8Srgb = 36,
    /// <summary>Game value 37, the game's `R8G8B8A8_unorm`: R8 G8 B8 A8, unorm.</summary>
    R8G8B8A8Unorm = 37,
    /// <summary>Game value 38, the game's `R8G8B8A8_snorm`: R8 G8 B8 A8, snorm.</summary>
    R8G8B8A8Snorm = 38,
    /// <summary>Game value 39, the game's `R8G8B8A8_uscaled`: R8 G8 B8 A8, uscaled.</summary>
    R8G8B8A8Uscaled = 39,
    /// <summary>Game value 40, the game's `R8G8B8A8_sscaled`: R8 G8 B8 A8, sscaled.</summary>
    R8G8B8A8Sscaled = 40,
    /// <summary>Game value 41, the game's `R8G8B8A8_uint`: R8 G8 B8 A8, uint.</summary>
    R8G8B8A8Uint = 41,
    /// <summary>Game value 42, the game's `R8G8B8A8_sint`: R8 G8 B8 A8, sint.</summary>
    R8G8B8A8Sint = 42,
    /// <summary>Game value 43, the game's `R8G8B8A8_srgb`: R8 G8 B8 A8, srgb.</summary>
    R8G8B8A8Srgb = 43,
    /// <summary>Game value 44, the game's `B8G8R8A8_unorm`: B8 G8 R8 A8, unorm.</summary>
    B8G8R8A8Unorm = 44,
    /// <summary>Game value 45, the game's `B8G8R8A8_snorm`: B8 G8 R8 A8, snorm.</summary>
    B8G8R8A8Snorm = 45,
    /// <summary>Game value 46, the game's `B8G8R8A8_uscaled`: B8 G8 R8 A8, uscaled.</summary>
    B8G8R8A8Uscaled = 46,
    /// <summary>Game value 47, the game's `B8G8R8A8_sscaled`: B8 G8 R8 A8, sscaled.</summary>
    B8G8R8A8Sscaled = 47,
    /// <summary>Game value 48, the game's `B8G8R8A8_uint`: B8 G8 R8 A8, uint.</summary>
    B8G8R8A8Uint = 48,
    /// <summary>Game value 49, the game's `B8G8R8A8_sint`: B8 G8 R8 A8, sint.</summary>
    B8G8R8A8Sint = 49,
    /// <summary>Game value 50, the game's `B8G8R8A8_srgb`: B8 G8 R8 A8, srgb.</summary>
    B8G8R8A8Srgb = 50,
    /// <summary>Game value 51, the game's `A8B8G8R8_unorm_pack32`: A8 B8 G8 R8 packed in a 32-bit word, unorm.</summary>
    A8B8G8R8UnormPack32 = 51,
    /// <summary>Game value 52, the game's `A8B8G8R8_snorm_pack32`: A8 B8 G8 R8 packed in a 32-bit word, snorm.</summary>
    A8B8G8R8SnormPack32 = 52,
    /// <summary>Game value 53, the game's `A8B8G8R8_uscaled_pack32`: A8 B8 G8 R8 packed in a 32-bit word, uscaled.</summary>
    A8B8G8R8UscaledPack32 = 53,
    /// <summary>Game value 54, the game's `A8B8G8R8_sscaled_pack32`: A8 B8 G8 R8 packed in a 32-bit word, sscaled.</summary>
    A8B8G8R8SscaledPack32 = 54,
    /// <summary>Game value 55, the game's `A8B8G8R8_uint_pack32`: A8 B8 G8 R8 packed in a 32-bit word, uint.</summary>
    A8B8G8R8UintPack32 = 55,
    /// <summary>Game value 56, the game's `A8B8G8R8_sint_pack32`: A8 B8 G8 R8 packed in a 32-bit word, sint.</summary>
    A8B8G8R8SintPack32 = 56,
    /// <summary>Game value 57, the game's `A8B8G8R8_srgb_pack32`: A8 B8 G8 R8 packed in a 32-bit word, srgb.</summary>
    A8B8G8R8SrgbPack32 = 57,
    /// <summary>Game value 58, the game's `A2R10G10B10_unorm_pack32`: A2 R10 G10 B10 packed in a 32-bit word, unorm.</summary>
    A2R10G10B10UnormPack32 = 58,
    /// <summary>Game value 59, the game's `A2R10G10B10_snorm_pack32`: A2 R10 G10 B10 packed in a 32-bit word, snorm.</summary>
    A2R10G10B10SnormPack32 = 59,
    /// <summary>Game value 60, the game's `A2R10G10B10_uscaled_pack32`: A2 R10 G10 B10 packed in a 32-bit word, uscaled.</summary>
    A2R10G10B10UscaledPack32 = 60,
    /// <summary>Game value 61, the game's `A2R10G10B10_sscaled_pack32`: A2 R10 G10 B10 packed in a 32-bit word, sscaled.</summary>
    A2R10G10B10SscaledPack32 = 61,
    /// <summary>Game value 62, the game's `A2R10G10B10_uint_pack32`: A2 R10 G10 B10 packed in a 32-bit word, uint.</summary>
    A2R10G10B10UintPack32 = 62,
    /// <summary>Game value 63, the game's `A2R10G10B10_sint_pack32`: A2 R10 G10 B10 packed in a 32-bit word, sint.</summary>
    A2R10G10B10SintPack32 = 63,
    /// <summary>Game value 64, the game's `A2B10G10R10_unorm_pack32`: A2 B10 G10 R10 packed in a 32-bit word, unorm.</summary>
    A2B10G10R10UnormPack32 = 64,
    /// <summary>Game value 65, the game's `A2B10G10R10_snorm_pack32`: A2 B10 G10 R10 packed in a 32-bit word, snorm.</summary>
    A2B10G10R10SnormPack32 = 65,
    /// <summary>Game value 66, the game's `A2B10G10R10_uscaled_pack32`: A2 B10 G10 R10 packed in a 32-bit word, uscaled.</summary>
    A2B10G10R10UscaledPack32 = 66,
    /// <summary>Game value 67, the game's `A2B10G10R10_sscaled_pack32`: A2 B10 G10 R10 packed in a 32-bit word, sscaled.</summary>
    A2B10G10R10SscaledPack32 = 67,
    /// <summary>Game value 68, the game's `A2B10G10R10_uint_pack32`: A2 B10 G10 R10 packed in a 32-bit word, uint.</summary>
    A2B10G10R10UintPack32 = 68,
    /// <summary>Game value 69, the game's `A2B10G10R10_sint_pack32`: A2 B10 G10 R10 packed in a 32-bit word, sint.</summary>
    A2B10G10R10SintPack32 = 69,
    /// <summary>Game value 70, the game's `R16_unorm`: R16, unorm.</summary>
    R16Unorm = 70,
    /// <summary>Game value 71, the game's `R16_snorm`: R16, snorm.</summary>
    R16Snorm = 71,
    /// <summary>Game value 72, the game's `R16_uscaled`: R16, uscaled.</summary>
    R16Uscaled = 72,
    /// <summary>Game value 73, the game's `R16_sscaled`: R16, sscaled.</summary>
    R16Sscaled = 73,
    /// <summary>Game value 74, the game's `R16_uint`: R16, uint.</summary>
    R16Uint = 74,
    /// <summary>Game value 75, the game's `R16_sint`: R16, sint.</summary>
    R16Sint = 75,
    /// <summary>Game value 76, the game's `R16_sfloat`: R16, sfloat.</summary>
    R16Sfloat = 76,
    /// <summary>Game value 77, the game's `R16G16_unorm`: R16 G16, unorm.</summary>
    R16G16Unorm = 77,
    /// <summary>Game value 78, the game's `R16G16_snorm`: R16 G16, snorm.</summary>
    R16G16Snorm = 78,
    /// <summary>Game value 79, the game's `R16G16_uscaled`: R16 G16, uscaled.</summary>
    R16G16Uscaled = 79,
    /// <summary>Game value 80, the game's `R16G16_sscaled`: R16 G16, sscaled.</summary>
    R16G16Sscaled = 80,
    /// <summary>Game value 81, the game's `R16G16_uint`: R16 G16, uint.</summary>
    R16G16Uint = 81,
    /// <summary>Game value 82, the game's `R16G16_sint`: R16 G16, sint.</summary>
    R16G16Sint = 82,
    /// <summary>Game value 83, the game's `R16G16_sfloat`: R16 G16, sfloat.</summary>
    R16G16Sfloat = 83,
    /// <summary>Game value 84, the game's `R16G16B16_unorm`: R16 G16 B16, unorm.</summary>
    R16G16B16Unorm = 84,
    /// <summary>Game value 85, the game's `R16G16B16_snorm`: R16 G16 B16, snorm.</summary>
    R16G16B16Snorm = 85,
    /// <summary>Game value 86, the game's `R16G16B16_uscaled`: R16 G16 B16, uscaled.</summary>
    R16G16B16Uscaled = 86,
    /// <summary>Game value 87, the game's `R16G16B16_sscaled`: R16 G16 B16, sscaled.</summary>
    R16G16B16Sscaled = 87,
    /// <summary>Game value 88, the game's `R16G16B16_uint`: R16 G16 B16, uint.</summary>
    R16G16B16Uint = 88,
    /// <summary>Game value 89, the game's `R16G16B16_sint`: R16 G16 B16, sint.</summary>
    R16G16B16Sint = 89,
    /// <summary>Game value 90, the game's `R16G16B16_sfloat`: R16 G16 B16, sfloat.</summary>
    R16G16B16Sfloat = 90,
    /// <summary>Game value 91, the game's `R16G16B16A16_unorm`: R16 G16 B16 A16, unorm.</summary>
    R16G16B16A16Unorm = 91,
    /// <summary>Game value 92, the game's `R16G16B16A16_snorm`: R16 G16 B16 A16, snorm.</summary>
    R16G16B16A16Snorm = 92,
    /// <summary>Game value 93, the game's `R16G16B16A16_uscaled`: R16 G16 B16 A16, uscaled.</summary>
    R16G16B16A16Uscaled = 93,
    /// <summary>Game value 94, the game's `R16G16B16A16_sscaled`: R16 G16 B16 A16, sscaled.</summary>
    R16G16B16A16Sscaled = 94,
    /// <summary>Game value 95, the game's `R16G16B16A16_uint`: R16 G16 B16 A16, uint.</summary>
    R16G16B16A16Uint = 95,
    /// <summary>Game value 96, the game's `R16G16B16A16_sint`: R16 G16 B16 A16, sint.</summary>
    R16G16B16A16Sint = 96,
    /// <summary>Game value 97, the game's `R16G16B16A16_sfloat`: R16 G16 B16 A16, sfloat.</summary>
    R16G16B16A16Sfloat = 97,
    /// <summary>Game value 98, the game's `R32_uint`: R32, uint.</summary>
    R32Uint = 98,
    /// <summary>Game value 99, the game's `R32_sint`: R32, sint.</summary>
    R32Sint = 99,
    /// <summary>Game value 100, the game's `R32_sfloat`: R32, sfloat.</summary>
    R32Sfloat = 100,
    /// <summary>Game value 101, the game's `R32G32_uint`: R32 G32, uint.</summary>
    R32G32Uint = 101,
    /// <summary>Game value 102, the game's `R32G32_sint`: R32 G32, sint.</summary>
    R32G32Sint = 102,
    /// <summary>Game value 103, the game's `R32G32_sfloat`: R32 G32, sfloat.</summary>
    R32G32Sfloat = 103,
    /// <summary>Game value 104, the game's `R32G32B32_uint`: R32 G32 B32, uint.</summary>
    R32G32B32Uint = 104,
    /// <summary>Game value 105, the game's `R32G32B32_sint`: R32 G32 B32, sint.</summary>
    R32G32B32Sint = 105,
    /// <summary>Game value 106, the game's `R32G32B32_sfloat`: R32 G32 B32, sfloat.</summary>
    R32G32B32Sfloat = 106,
    /// <summary>Game value 107, the game's `R32G32B32A32_uint`: R32 G32 B32 A32, uint.</summary>
    R32G32B32A32Uint = 107,
    /// <summary>Game value 108, the game's `R32G32B32A32_sint`: R32 G32 B32 A32, sint.</summary>
    R32G32B32A32Sint = 108,
    /// <summary>Game value 109, the game's `R32G32B32A32_sfloat`: R32 G32 B32 A32, sfloat.</summary>
    R32G32B32A32Sfloat = 109,
    /// <summary>Game value 110, the game's `R64_uint`: R64, uint.</summary>
    R64Uint = 110,
    /// <summary>Game value 111, the game's `R64_sint`: R64, sint.</summary>
    R64Sint = 111,
    /// <summary>Game value 112, the game's `R64_sfloat`: R64, sfloat.</summary>
    R64Sfloat = 112,
    /// <summary>Game value 113, the game's `R64G64_uint`: R64 G64, uint.</summary>
    R64G64Uint = 113,
    /// <summary>Game value 114, the game's `R64G64_sint`: R64 G64, sint.</summary>
    R64G64Sint = 114,
    /// <summary>Game value 115, the game's `R64G64_sfloat`: R64 G64, sfloat.</summary>
    R64G64Sfloat = 115,
    /// <summary>Game value 116, the game's `R64G64B64_uint`: R64 G64 B64, uint.</summary>
    R64G64B64Uint = 116,
    /// <summary>Game value 117, the game's `R64G64B64_sint`: R64 G64 B64, sint.</summary>
    R64G64B64Sint = 117,
    /// <summary>Game value 118, the game's `R64G64B64_sfloat`: R64 G64 B64, sfloat.</summary>
    R64G64B64Sfloat = 118,
    /// <summary>Game value 119, the game's `R64G64B64A64_uint`: R64 G64 B64 A64, uint.</summary>
    R64G64B64A64Uint = 119,
    /// <summary>Game value 120, the game's `R64G64B64A64_sint`: R64 G64 B64 A64, sint.</summary>
    R64G64B64A64Sint = 120,
    /// <summary>Game value 121, the game's `R64G64B64A64_sfloat`: R64 G64 B64 A64, sfloat.</summary>
    R64G64B64A64Sfloat = 121,
    /// <summary>Game value 122, the game's `B10G11R11_ufloat_pack32`: B10 G11 R11 packed in a 32-bit word, ufloat.</summary>
    B10G11R11UfloatPack32 = 122,
    /// <summary>Game value 123, the game's `E5B9G9R9_ufloat_pack32`: E5 B9 G9 R9 packed in a 32-bit word, ufloat.</summary>
    E5B9G9R9UfloatPack32 = 123,
    /// <summary>Game value 124, the game's `D16_unorm`: a depth or stencil format (D16 unorm; refused).</summary>
    D16Unorm = 124,
    /// <summary>Game value 125, the game's `X8_D24_unorm_pack32`: a depth or stencil format (X8 D24 unorm pack32; refused).</summary>
    X8D24UnormPack32 = 125,
    /// <summary>Game value 126, the game's `D32_sfloat`: a depth or stencil format (D32 sfloat; refused).</summary>
    D32Sfloat = 126,
    /// <summary>Game value 127, the game's `S8_uint`: a depth or stencil format (S8 uint; refused).</summary>
    S8Uint = 127,
    /// <summary>Game value 128, the game's `D16_unorm_S8_uint`: a depth or stencil format (D16 unorm S8 uint; refused).</summary>
    D16UnormS8Uint = 128,
    /// <summary>Game value 129, the game's `D24_unorm_S8_uint`: a depth or stencil format (D24 unorm S8 uint; refused).</summary>
    D24UnormS8Uint = 129,
    /// <summary>Game value 130, the game's `D32_sfloat_S8_uint`: a depth or stencil format (D32 sfloat S8 uint; refused).</summary>
    D32SfloatS8Uint = 130,
    /// <summary>Game value 131, the game's `BC1_RGB_unorm_block`: BC1 RGB blocks, unorm, 8 bytes a 4x4 block.</summary>
    BC1RgbUnormBlock = 131,
    /// <summary>Game value 132, the game's `BC1_RGB_srgb_block`: BC1 RGB blocks, srgb, 8 bytes a 4x4 block.</summary>
    BC1RgbSrgbBlock = 132,
    /// <summary>Game value 133, the game's `BC1_RGBA_unorm_block`: BC1 RGBA blocks, unorm, 8 bytes a 4x4 block.</summary>
    BC1RgbaUnormBlock = 133,
    /// <summary>Game value 134, the game's `BC1_RGBA_srgb_block`: BC1 RGBA blocks, srgb, 8 bytes a 4x4 block.</summary>
    BC1RgbaSrgbBlock = 134,
    /// <summary>Game value 135, the game's `BC2_unorm_block`: BC2 blocks, unorm, 16 bytes a 4x4 block.</summary>
    BC2UnormBlock = 135,
    /// <summary>Game value 136, the game's `BC2_srgb_block`: BC2 blocks, srgb, 16 bytes a 4x4 block.</summary>
    BC2SrgbBlock = 136,
    /// <summary>Game value 137, the game's `BC3_unorm_block`: BC3 blocks, unorm, 16 bytes a 4x4 block.</summary>
    BC3UnormBlock = 137,
    /// <summary>Game value 138, the game's `BC3_srgb_block`: BC3 blocks, srgb, 16 bytes a 4x4 block.</summary>
    BC3SrgbBlock = 138,
    /// <summary>Game value 139, the game's `BC4_unorm_block`: BC4 blocks, unorm, 8 bytes a 4x4 block.</summary>
    BC4UnormBlock = 139,
    /// <summary>Game value 140, the game's `BC4_snorm_block`: BC4 blocks, snorm, 8 bytes a 4x4 block.</summary>
    BC4SnormBlock = 140,
    /// <summary>Game value 141, the game's `BC5_unorm_block`: BC5 blocks, unorm, 16 bytes a 4x4 block.</summary>
    BC5UnormBlock = 141,
    /// <summary>Game value 142, the game's `BC5_snorm_block`: BC5 blocks, snorm, 16 bytes a 4x4 block.</summary>
    BC5SnormBlock = 142,
    /// <summary>Game value 143, the game's `BC6H_ufloat_block`: BC6H blocks, ufloat, 16 bytes a 4x4 block.</summary>
    BC6HUfloatBlock = 143,
    /// <summary>Game value 144, the game's `BC6H_sfloat_block`: BC6H blocks, sfloat, 16 bytes a 4x4 block.</summary>
    BC6HSfloatBlock = 144,
    /// <summary>Game value 145, the game's `BC7_unorm_block`: BC7 blocks, unorm, 16 bytes a 4x4 block.</summary>
    BC7UnormBlock = 145,
    /// <summary>Game value 146, the game's `BC7_srgb_block`: BC7 blocks, srgb, 16 bytes a 4x4 block.</summary>
    BC7SrgbBlock = 146,
}

/// <summary>
/// Game texture pixels in code: decodes the bytes of a game texture (any <see cref="GamePixelFormat"/> kiln mode's
/// <c>image.decode_texture</c> reads) to RGBA8 pixels, and encodes RGBA8 pixels to a game format (what
/// <c>image.encode_texture</c> writes). The answers are the same as those Lua functions', byte for byte, failures
/// included. Pure functions on your own bytes: no game, service or capability is needed, any thread may call them, and
/// they work the same in unit tests.
/// </summary>
/// <remarks>
/// <para>Pixels are RGBA8: 4 bytes per pixel (R, G, B, A), row by row from the top, <c>width * height * 4</c> bytes.
/// <c>DataImage.FromRgba8</c> (Kiln.Eml) makes an image of them.</para>
/// <para>Decoding fills a channel the format does not have with 0 (alpha with 255). sRGB block formats decode like their
/// unorm forms (no gamma), BC4 and BC5 snorm like unorm; BC4, BC5, BC6H and <c>B5G6R5UnormPack16</c> give alpha 0.
/// Float formats give <c>value * 255</c>, cut to 0..255. Block formats need a width and height that are multiples of 4
/// (<see cref="DecodeMip"/> takes any size).</para>
/// <para>Encoding writes 8-bit and packed formats and BC1 to BC7 the way the game reads them. Formats wider than 8 bits
/// keep the Lua function's layout: unsigned ones store each channel's byte repeated (right for single-channel
/// textures such as <c>R16Unorm</c>), signed and float ones take one single-channel pixel only.</para>
/// <para>Refused formats: <c>None</c>, depth and stencil formats, the <c>A2R10G10B10</c> and <c>A2B10G10R10</c> packs,
/// sRGB forms of the 8-bit formats, <c>BC6HSfloatBlock</c>, and (decoding) the 32- and 64-bit integer formats;
/// encoding also refuses <c>B10G11R11UfloatPack32</c> and <c>E5B9G9R9UfloatPack32</c>. A refused format throws
/// <see cref="NotSupportedException"/>.</para>
/// <para>Since 0.2.0-dev.30.3.</para>
/// </remarks>
/// <example>
/// <code>
/// // a game icon's top level (BC7, 256x256, mip chain largest first) to pixels, tinted, and back
/// byte[] rgba = GameTexture.DecodeMip(blob, GamePixelFormat.BC7UnormBlock, 256, 256, mip: 0);
/// for (var i = 0; i &lt; rgba.Length; i += 4) rgba[i] = (byte)Math.Min(255, rgba[i] + 40);
/// byte[] bc7 = GameTexture.Encode(rgba, 256, 256, GamePixelFormat.BC7UnormBlock);
/// </code>
/// </example>
[AnyThread]
public static class GameTexture
{
    /// <summary>Decodes one image of <paramref name="width"/> x <paramref name="height"/> pixels in
    /// <paramref name="format"/> to RGBA8 (<c>width * height * 4</c> bytes). Bytes after the image are ignored. The
    /// same as kiln mode's <c>image.decode_texture(data, format, width, height)</c>.</summary>
    /// <exception cref="NotSupportedException">The format cannot be decoded.</exception>
    /// <exception cref="ArgumentException"><paramref name="data"/> is shorter than the image, or a block format's size is
    /// not a multiple of 4.</exception>
    /// <exception cref="ArgumentOutOfRangeException">A negative size, an image of more than 2 GiB of pixels, or a value
    /// that is not a <see cref="GamePixelFormat"/>.</exception>
    /// <remarks>Since 0.2.0-dev.30.3.</remarks>
    public static byte[] Decode(ReadOnlySpan<byte> data, GamePixelFormat format, int width, int height)
    {
        CheckSize(width, height);
        try
        {
            return PixelCodec.Decode(Codec(format), format, width, height, data);
        }
        catch (TextureError e)
        {
            throw e.ToException(nameof(data));
        }
    }

    /// <summary>Decodes level <paramref name="mip"/> of a mip chain: the levels one after another, largest first, as
    /// a game texture's data holds them. Level <c>n</c> is <c>max(1, width &gt;&gt; n)</c> x <c>max(1, height &gt;&gt; n)</c>
    /// pixels and starts at <see cref="MipOffset"/>. Block-format levels of any size decode (the whole blocks are
    /// decoded and cut to the level); otherwise the rules are <see cref="Decode"/>'s.</summary>
    /// <param name="mipChain">The texture's data, from the start of level 0.</param>
    /// <param name="format">The texture's pixel format.</param>
    /// <param name="width">Level 0's width (at least 1).</param>
    /// <param name="height">Level 0's height (at least 1).</param>
    /// <param name="mip">The level: 0 is the full size.</param>
    /// <returns>RGBA8 pixels of the level.</returns>
    /// <remarks>Since 0.2.0-dev.30.3.</remarks>
    public static byte[] DecodeMip(ReadOnlySpan<byte> mipChain, GamePixelFormat format, int width, int height, int mip)
    {
        var offset = MipOffset(format, width, height, mip);
        var (w, h) = (Math.Max(1, width >> mip), Math.Max(1, height >> mip));
        var codec = Codec(format);
        try
        {
            if (offset > mipChain.Length)
                throw new TextureError(TextureFault.Data, $"texture data too short: level {mip} starts at byte {offset}, got {mipChain.Length}");
            var level = mipChain[offset..];
            if (codec.Kind != CodecKind.Block || (w % 4 == 0 && h % 4 == 0))
                return PixelCodec.Decode(codec, format, w, h, level);
            // whole blocks, in long: a level within 3 pixels of int.MaxValue would wrap
            var (wide, tall) = ((w + 3L) & ~3L, (h + 3L) & ~3L);
            if (wide > int.MaxValue || tall > int.MaxValue) throw new TextureError(TextureFault.Size, "texture dimensions overflow");
            var (pw, ph) = ((int)wide, (int)tall);
            var padded = PixelCodec.Decode(codec, format, pw, ph, level);
            var rgba = new byte[w * h * 4];
            for (var y = 0; y < h; y++)
                padded.AsSpan(y * pw * 4, w * 4).CopyTo(rgba.AsSpan(y * w * 4));
            return rgba;
        }
        catch (TextureError e)
        {
            throw e.ToException(nameof(mipChain));
        }
    }

    /// <summary>Encodes RGBA8 pixels (<c>width * height * 4</c> bytes) to <paramref name="format"/>. The same bytes as
    /// kiln mode's <c>image.encode_texture(image, format)</c>; BC1 to BC7 match the game's block layout.</summary>
    /// <exception cref="NotSupportedException">The format cannot be encoded, or a signed or float format wider than 8
    /// bits got more than one single-channel pixel.</exception>
    /// <exception cref="ArgumentException"><paramref name="rgba"/> is not <c>width * height * 4</c> bytes, or a block
    /// format's size is not a multiple of 4.</exception>
    /// <exception cref="ArgumentOutOfRangeException">A negative size or a value that is not a
    /// <see cref="GamePixelFormat"/>.</exception>
    /// <remarks>Since 0.2.0-dev.30.3.</remarks>
    public static byte[] Encode(ReadOnlySpan<byte> rgba, int width, int height, GamePixelFormat format)
    {
        CheckSize(width, height);
        var codec = Codec(format);
        if (rgba.Length != (long)width * height * 4)
            throw new ArgumentException($"{width}x{height} RGBA8 is {(long)width * height * 4} bytes, not {rgba.Length}", nameof(rgba));
        try
        {
            return PixelCodec.Encode(codec, format, width, height, rgba);
        }
        catch (TextureError e)
        {
            throw e.ToException(nameof(rgba));
        }
    }

    /// <summary>The bytes of one image of <paramref name="width"/> x <paramref name="height"/> in
    /// <paramref name="format"/> (block formats: whole 4x4 blocks).</summary>
    /// <exception cref="NotSupportedException"><c>None</c> and the depth and stencil formats, whose layout is the GPU's.</exception>
    /// <remarks>Since 0.2.0-dev.30.3.</remarks>
    public static int ByteSize(GamePixelFormat format, int width, int height)
    {
        CheckSize(width, height);
        var codec = Codec(format);
        // units * bytes, compared before multiplying: (2^31 - 1)^2 pixels of 32 bytes overflow a long
        var (units, bytes) = codec.Kind == CodecKind.Block || codec.Refusal == Refusal.Bc6hSigned
            ? ((width + 3L) / 4 * ((height + 3L) / 4), codec.BlockBytes)
            : codec.TexelBytes > 0 ? ((long)width * height, codec.TexelBytes)
            : throw new NotSupportedException($"{PixelCodec.GameName(format)} has no fixed size: its layout is the GPU's");
        return units <= Array.MaxLength / bytes ? (int)(units * bytes) : throw new ArgumentOutOfRangeException(nameof(width), "texture dimensions overflow");
    }

    /// <summary>Where level <paramref name="mip"/> of a mip chain starts: the sum of <see cref="ByteSize"/> of the
    /// larger levels.</summary>
    /// <exception cref="ArgumentOutOfRangeException">A size below 1, or a level past the 1x1 one.</exception>
    /// <remarks>Since 0.2.0-dev.30.3.</remarks>
    public static int MipOffset(GamePixelFormat format, int width, int height, int mip)
    {
        if (width < 1 || height < 1) throw new ArgumentOutOfRangeException(width < 1 ? nameof(width) : nameof(height), "a mip chain is at least 1x1");
        var last = 31 - int.LeadingZeroCount(Math.Max(width, height));
        if (mip < 0 || mip > last) throw new ArgumentOutOfRangeException(nameof(mip), $"a {width}x{height} chain has levels 0 to {last}");
        long offset = 0;
        for (var i = 0; i < mip; i++)
            offset += ByteSize(format, Math.Max(1, width >> i), Math.Max(1, height >> i));
        return offset <= Array.MaxLength ? (int)offset : throw new ArgumentOutOfRangeException(nameof(mip), "texture dimensions overflow");
    }

    private static void CheckSize(int width, int height)
    {
        if (width < 0) throw new ArgumentOutOfRangeException(nameof(width), "a size is not negative");
        if (height < 0) throw new ArgumentOutOfRangeException(nameof(height), "a size is not negative");
    }

    private static PixelCodec.Info Codec(GamePixelFormat format) =>
        (uint)format < (uint)PixelCodec.Table.Length
            ? PixelCodec.Table[(int)format]
            : throw new ArgumentOutOfRangeException(nameof(format), $"{(int)format} is not a GamePixelFormat");
}

internal enum TextureFault
{
    /// <summary>The format is refused, or this use of it is.</summary>
    Unsupported,
    /// <summary>The data is too short or the size does not fit the format.</summary>
    Data,
    /// <summary>The size is too large.</summary>
    Size,
}

/// <summary>A codec failure with the Lua functions' cause text; turned into the .NET exception at the API.</summary>
internal sealed class TextureError(TextureFault fault, string message) : Exception(message)
{
    public TextureFault Fault { get; } = fault;

    public Exception ToException(string param) => Fault switch
    {
        TextureFault.Unsupported => new NotSupportedException(Message),
        TextureFault.Size => new ArgumentOutOfRangeException(param, Message),
        _ => new ArgumentException(Message, param),
    };
}

internal enum CodecKind
{
    /// <summary>Refused before any size or data check, even for an empty image.</summary>
    Refused,
    /// <summary>A little-endian packed word of <c>TexelBytes</c> bytes with (channel, bits, shift) fields.</summary>
    Packed,
    /// <summary>Byte i of each component group holds RGBA channel <c>Order[i]</c>; <c>Size</c> bytes per component.</summary>
    Plain,
    B10G11R11,
    E5B9G9R9,
    /// <summary>BC1 to BC7.</summary>
    Block,
}

internal enum Refusal { None, Srgb, Packed10, Opaque, Bc6hSigned }

internal enum Signedness { Unsigned, Signed, Float }

internal enum BlockKind { Bc1, Bc2, Bc3, Bc4, Bc5, Bc6h, Bc7 }

/// <summary>The decode and encode rules of each format (texture.rs's <c>Codec</c>), byte for byte.</summary>
internal static class PixelCodec
{
    internal sealed record Info(CodecKind Kind, Refusal Refusal = Refusal.None, int TexelBytes = 0, (int Channel, int Bits, int Shift)[]? Fields = null,
        int[]? Order = null, int Size = 0, Signedness Sign = Signedness.Unsigned, BlockKind Block = BlockKind.Bc1)
    {
        public int BlockBytes => Block is BlockKind.Bc1 or BlockKind.Bc4 ? 8 : 16;
    }

    // Before Table: static fields initialise in order.
    private static readonly int[] R = [0], RG = [0, 1], RGB = [0, 1, 2], BGR = [2, 1, 0], RGBA = [0, 1, 2, 3], BGRA = [2, 1, 0, 3], ABGR = [3, 2, 1, 0];

    /// <summary>The game's own name of each format, by value (the names kiln mode's image functions use, and the ones
    /// the error messages give, as the Lua functions' do).</summary>
    private static readonly string[] GameNames =
    [
        "None", "R4G4_unorm_pack8", "R4G4B4A4_unorm_pack16", "B4G4R4A4_unorm_pack16", "R5G6B5_unorm_pack16",
        "B5G6R5_unorm_pack16", "R5G5B5A1_unorm_pack16", "B5G5R5A1_unorm_pack16", "A1R5G5B5_unorm_pack16", "R8_unorm",
        "R8_snorm", "R8_uscaled", "R8_sscaled", "R8_uint", "R8_sint", "R8_srgb", "R8G8_unorm", "R8G8_snorm",
        "R8G8_uscaled", "R8G8_sscaled", "R8G8_uint", "R8G8_sint", "R8G8_srgb", "R8G8B8_unorm", "R8G8B8_snorm",
        "R8G8B8_uscaled", "R8G8B8_sscaled", "R8G8B8_uint", "R8G8B8_sint", "R8G8B8_srgb", "B8G8R8_unorm",
        "B8G8R8_snorm", "B8G8R8_uscaled", "B8G8R8_sscaled", "B8G8R8_uint", "B8G8R8_sint", "B8G8R8_srgb",
        "R8G8B8A8_unorm", "R8G8B8A8_snorm", "R8G8B8A8_uscaled", "R8G8B8A8_sscaled", "R8G8B8A8_uint", "R8G8B8A8_sint",
        "R8G8B8A8_srgb", "B8G8R8A8_unorm", "B8G8R8A8_snorm", "B8G8R8A8_uscaled", "B8G8R8A8_sscaled", "B8G8R8A8_uint",
        "B8G8R8A8_sint", "B8G8R8A8_srgb", "A8B8G8R8_unorm_pack32", "A8B8G8R8_snorm_pack32", "A8B8G8R8_uscaled_pack32",
        "A8B8G8R8_sscaled_pack32", "A8B8G8R8_uint_pack32", "A8B8G8R8_sint_pack32", "A8B8G8R8_srgb_pack32",
        "A2R10G10B10_unorm_pack32", "A2R10G10B10_snorm_pack32", "A2R10G10B10_uscaled_pack32",
        "A2R10G10B10_sscaled_pack32", "A2R10G10B10_uint_pack32", "A2R10G10B10_sint_pack32",
        "A2B10G10R10_unorm_pack32", "A2B10G10R10_snorm_pack32", "A2B10G10R10_uscaled_pack32",
        "A2B10G10R10_sscaled_pack32", "A2B10G10R10_uint_pack32", "A2B10G10R10_sint_pack32", "R16_unorm", "R16_snorm",
        "R16_uscaled", "R16_sscaled", "R16_uint", "R16_sint", "R16_sfloat", "R16G16_unorm", "R16G16_snorm",
        "R16G16_uscaled", "R16G16_sscaled", "R16G16_uint", "R16G16_sint", "R16G16_sfloat", "R16G16B16_unorm",
        "R16G16B16_snorm", "R16G16B16_uscaled", "R16G16B16_sscaled", "R16G16B16_uint", "R16G16B16_sint",
        "R16G16B16_sfloat", "R16G16B16A16_unorm", "R16G16B16A16_snorm", "R16G16B16A16_uscaled",
        "R16G16B16A16_sscaled", "R16G16B16A16_uint", "R16G16B16A16_sint", "R16G16B16A16_sfloat", "R32_uint",
        "R32_sint", "R32_sfloat", "R32G32_uint", "R32G32_sint", "R32G32_sfloat", "R32G32B32_uint", "R32G32B32_sint",
        "R32G32B32_sfloat", "R32G32B32A32_uint", "R32G32B32A32_sint", "R32G32B32A32_sfloat", "R64_uint", "R64_sint",
        "R64_sfloat", "R64G64_uint", "R64G64_sint", "R64G64_sfloat", "R64G64B64_uint", "R64G64B64_sint",
        "R64G64B64_sfloat", "R64G64B64A64_uint", "R64G64B64A64_sint", "R64G64B64A64_sfloat",
        "B10G11R11_ufloat_pack32", "E5B9G9R9_ufloat_pack32", "D16_unorm", "X8_D24_unorm_pack32", "D32_sfloat",
        "S8_uint", "D16_unorm_S8_uint", "D24_unorm_S8_uint", "D32_sfloat_S8_uint", "BC1_RGB_unorm_block",
        "BC1_RGB_srgb_block", "BC1_RGBA_unorm_block", "BC1_RGBA_srgb_block", "BC2_unorm_block", "BC2_srgb_block",
        "BC3_unorm_block", "BC3_srgb_block", "BC4_unorm_block", "BC4_snorm_block", "BC5_unorm_block",
        "BC5_snorm_block", "BC6H_ufloat_block", "BC6H_sfloat_block", "BC7_unorm_block", "BC7_srgb_block"
    ];

    internal static string GameName(GamePixelFormat format) => GameNames[(int)format];

    internal static readonly Info[] Table = Enum.GetValues<GamePixelFormat>().Select(Classify).ToArray();

    private static Info Classify(GamePixelFormat format)
    {
        var name = GameName(format);
        var sign = name.Contains("_sfloat", StringComparison.Ordinal) ? Signedness.Float
            : name.Contains("_snorm", StringComparison.Ordinal) || name.Contains("_sscaled", StringComparison.Ordinal) || name.Contains("_sint", StringComparison.Ordinal) ? Signedness.Signed
            : Signedness.Unsigned;
        static Info Packed(int bytes, params (int, int, int)[] fields) => new(CodecKind.Packed, TexelBytes: bytes, Fields: fields);
        static Info Block(BlockKind b) => new(CodecKind.Block, Block: b);
        switch (format)
        {
            case GamePixelFormat.None: return new Info(CodecKind.Refused, Refusal.None);
            case GamePixelFormat.R4G4UnormPack8: return Packed(1, (0, 4, 4), (1, 4, 0));
            case GamePixelFormat.R4G4B4A4UnormPack16: return Packed(2, (0, 4, 12), (1, 4, 8), (2, 4, 4), (3, 4, 0));
            case GamePixelFormat.B4G4R4A4UnormPack16: return Packed(2, (2, 4, 12), (1, 4, 8), (0, 4, 4), (3, 4, 0));
            case GamePixelFormat.R5G6B5UnormPack16: return Packed(2, (0, 5, 11), (1, 6, 5), (2, 5, 0));
            case GamePixelFormat.B5G6R5UnormPack16: return Packed(2, (2, 5, 11), (1, 6, 5), (0, 5, 0));
            case GamePixelFormat.R5G5B5A1UnormPack16: return Packed(2, (0, 5, 11), (1, 5, 6), (2, 5, 1), (3, 1, 0));
            case GamePixelFormat.B5G5R5A1UnormPack16: return Packed(2, (2, 5, 11), (1, 5, 6), (0, 5, 1), (3, 1, 0));
            case GamePixelFormat.A1R5G5B5UnormPack16: return Packed(2, (3, 1, 15), (0, 5, 10), (1, 5, 5), (2, 5, 0));
            case GamePixelFormat.B10G11R11UfloatPack32: return new Info(CodecKind.B10G11R11, TexelBytes: 4);
            case GamePixelFormat.E5B9G9R9UfloatPack32: return new Info(CodecKind.E5B9G9R9, TexelBytes: 4);
            case GamePixelFormat.BC1RgbUnormBlock or GamePixelFormat.BC1RgbSrgbBlock or GamePixelFormat.BC1RgbaUnormBlock or GamePixelFormat.BC1RgbaSrgbBlock:
                return Block(BlockKind.Bc1);
            case GamePixelFormat.BC2UnormBlock or GamePixelFormat.BC2SrgbBlock: return Block(BlockKind.Bc2);
            case GamePixelFormat.BC3UnormBlock or GamePixelFormat.BC3SrgbBlock: return Block(BlockKind.Bc3);
            case GamePixelFormat.BC4UnormBlock or GamePixelFormat.BC4SnormBlock: return Block(BlockKind.Bc4);
            case GamePixelFormat.BC5UnormBlock or GamePixelFormat.BC5SnormBlock: return Block(BlockKind.Bc5);
            case GamePixelFormat.BC6HUfloatBlock: return Block(BlockKind.Bc6h);
            // Refused, but its layout is known (ByteSize).
            case GamePixelFormat.BC6HSfloatBlock: return new Info(CodecKind.Refused, Refusal.Bc6hSigned, Block: BlockKind.Bc6h);
            case GamePixelFormat.BC7UnormBlock or GamePixelFormat.BC7SrgbBlock: return Block(BlockKind.Bc7);
        }
        if (name.StartsWith("A2", StringComparison.Ordinal)) return new Info(CodecKind.Refused, Refusal.Packed10, TexelBytes: 4);
        if (name.StartsWith('D') || name.StartsWith("S8", StringComparison.Ordinal) || name.StartsWith("X8", StringComparison.Ordinal))
            return new Info(CodecKind.Refused, Refusal.Opaque);
        (string Prefix, int[] Order)[] layouts =
        [
            ("A8B8G8R8_", ABGR), ("R8G8B8A8_", RGBA), ("B8G8R8A8_", BGRA), ("R16G16B16A16_", RGBA), ("R32G32B32A32_", RGBA), ("R64G64B64A64_", RGBA),
            ("R8G8B8_", RGB), ("B8G8R8_", BGR), ("R16G16B16_", RGB), ("R32G32B32_", RGB), ("R64G64B64_", RGB),
            ("R8G8_", RG), ("R16G16_", RG), ("R32G32_", RG), ("R64G64_", RG), ("R8_", R), ("R16_", R), ("R32_", R), ("R64_", R),
        ];
        var (prefix, order) = layouts.First(l => name.StartsWith(l.Prefix, StringComparison.Ordinal));
        var size = prefix.Contains("64", StringComparison.Ordinal) ? 8 : prefix.Contains("32", StringComparison.Ordinal) ? 4 : prefix.Contains("16", StringComparison.Ordinal) ? 2 : 1;
        if (name.Contains("_srgb", StringComparison.Ordinal))
            return new Info(CodecKind.Refused, Refusal.Srgb, TexelBytes: order.Length * size);
        return new Info(CodecKind.Plain, TexelBytes: order.Length * size, Order: order, Size: size, Sign: sign);
    }

    private static string RefusalText(GamePixelFormat format, Refusal refusal, string operation) => refusal switch
    {
        Refusal.None => "Unsupported pixel format: None",
        Refusal.Opaque => $"Opaque pixel format: {GameName(format)}",
        Refusal.Packed10 => $"A2B10G10R10_*_pack32 {operation} not implemented",
        _ => $"{GameName(format)} {operation} not implemented",
    };

    private static TextureError Overflow() => new(TextureFault.Size, "texture dimensions overflow");

    private static long Need(ReadOnlySpan<byte> data, long required) =>
        data.Length >= required ? required : throw new TextureError(TextureFault.Data, $"texture data too short: expected {required} bytes, got {data.Length}");

    /// <summary>Bit replication of an n-bit field to 8 bits (packed decode).</summary>
    private static byte Expand(uint value, int bits)
    {
        if (bits == 1) return value != 0 ? (byte)255 : (byte)0;
        var output = value << (8 - bits);
        for (var filled = bits; filled < 8; filled *= 2) output |= output >> filled;
        return (byte)output;
    }

    /// <summary>8-bit to n-bit: 4-bit truncates, 5, 6 and 1-bit round to nearest (packed encode).</summary>
    private static uint Quantize(byte value, int bits) => bits == 4 ? (uint)value >> 4 : ((uint)value * ((1u << bits) - 1) + 127) / 255;

    private static float UnsignedFloat(uint field, int mantissaBits, int exponentBits, bool special)
    {
        var exponent = field >> mantissaBits;
        var mantissa = field & ((1u << mantissaBits) - 1);
        var scale = (float)(1u << mantissaBits);
        if (special && exponent == (1u << exponentBits) - 1) return mantissa == 0 ? float.PositiveInfinity : float.NaN;
        return exponent == 0 ? mantissa / scale * Pow2(-14) : (1f + mantissa / scale) * Pow2((int)exponent - 15);
    }

    /// <summary>2^e as an f32, exactly (Rust's powi of 2 for these small exponents).</summary>
    private static float Pow2(int e) => BitConverter.Int32BitsToSingle((e + 127) << 23);

    internal static byte UnitF32(float value) => Sat.ToByte(value * 255f);

    internal static byte[] Decode(Info codec, GamePixelFormat format, int width, int height, ReadOnlySpan<byte> data)
    {
        if (codec.Kind == CodecKind.Refused) throw new TextureError(TextureFault.Unsupported, RefusalText(format, codec.Refusal, "decoding"));
        var count = (long)width * height;
        // The output must stay below 4 GiB (u32 arithmetic in the Lua function) and fit a .NET array (the smaller
        // bound). Compared before multiplying: (2^31 - 1)^2 pixels times 4 overflow a long.
        if (count > Array.MaxLength / 4) throw Overflow();
        var outLen = count * 4;
        // Every input check runs before the output exists.
        switch (codec.Kind)
        {
            case CodecKind.Packed:
            case CodecKind.B10G11R11:
            case CodecKind.E5B9G9R9:
                data = data[..(int)Need(data, count * codec.TexelBytes)];
                break;
            case CodecKind.Plain:
                data = data[..(int)Need(data, count * codec.Order!.Length * codec.Size)];
                if (count > 0 && codec.Size >= 4 && codec.Sign != Signedness.Float)
                    throw new TextureError(TextureFault.Unsupported,
                        $"{GameName(format)} decoding is not supported: 32- and 64-bit integer formats do not decode (an empty image does)");
                break;
            case CodecKind.Block:
                if (count > 0 && (width % 4 != 0 || height % 4 != 0))
                    throw new TextureError(TextureFault.Data, $"{GameName(format)} decoding requires dimensions that are multiples of 4, got {width}x{height}");
                data = data[..(int)Need(data, (long)((width + 3) / 4) * ((height + 3) / 4) * codec.BlockBytes)];
                break;
        }
        var output = new byte[outLen];
        switch (codec.Kind)
        {
            case CodecKind.Packed:
            {
                var bytes = codec.TexelBytes;
                for (int t = 0, o = 0; o < output.Length; t += bytes, o += 4)
                {
                    uint word = 0;
                    for (var b = bytes - 1; b >= 0; b--) word = (word << 8) | data[t + b];
                    output[o + 3] = format == GamePixelFormat.B5G6R5UnormPack16 ? (byte)0 : (byte)255;
                    foreach (var (channel, bits, shift) in codec.Fields!)
                        output[o + channel] = Expand((word >> shift) & ((1u << bits) - 1), bits);
                }
                break;
            }
            case CodecKind.Plain:
            {
                var order = codec.Order!;
                var size = codec.Size;
                var group = order.Length * size;
                for (int t = 0, o = 0; o < output.Length; t += group, o += 4)
                {
                    output[o] = 0; output[o + 1] = 0; output[o + 2] = 0; output[o + 3] = 255;
                    for (var i = 0; i < order.Length; i++)
                    {
                        var c = data.Slice(t + i * size, size);
                        output[o + order[i]] = (codec.Sign, size) switch
                        {
                            (Signedness.Float, 2) => UnitF32((float)BinaryPrimitives.ReadHalfLittleEndian(c)),
                            (Signedness.Float, 4) => UnitF32(BinaryPrimitives.ReadSingleLittleEndian(c)),
                            (Signedness.Float, _) => Sat.ToByte(BinaryPrimitives.ReadDoubleLittleEndian(c) * 255.0),
                            (Signedness.Signed, _) => (byte)(c[size - 1] ^ 0x80),
                            _ => c[size - 1],
                        };
                    }
                }
                break;
            }
            case CodecKind.B10G11R11:
            case CodecKind.E5B9G9R9:
                for (int t = 0, o = 0; o < output.Length; t += 4, o += 4)
                {
                    var v = BinaryPrimitives.ReadUInt32LittleEndian(data[t..]);
                    float r, g, b;
                    if (codec.Kind == CodecKind.B10G11R11)
                    {
                        r = UnsignedFloat(v & 0x3ff, 5, 5, true);
                        g = UnsignedFloat((v >> 11) & 0x7ff, 6, 5, true);
                        b = UnsignedFloat(v >> 22, 6, 4, false);
                    }
                    else
                    {
                        var exponent = v >> 27;
                        float Channel(int shift)
                        {
                            var mantissa = (v >> shift) & 0x1ff;
                            if (exponent == 31) return mantissa == 0 ? float.PositiveInfinity : float.NaN;
                            return (1f + mantissa / 512f) * Pow2((int)exponent - 15);
                        }
                        (r, g, b) = (Channel(0), Channel(9), Channel(18));
                    }
                    output[o] = UnitF32(r); output[o + 1] = UnitF32(g); output[o + 2] = UnitF32(b); output[o + 3] = 255;
                }
                break;
            case CodecKind.Block:
                if (count > 0) BlockDecoder.Decode(codec.Block, width, height, data, output);
                break;
        }
        return output;
    }

    internal static byte[] Encode(Info codec, GamePixelFormat format, int width, int height, ReadOnlySpan<byte> rgba)
    {
        switch (codec.Kind)
        {
            case CodecKind.Refused:
                throw new TextureError(TextureFault.Unsupported, RefusalText(format, codec.Refusal, "encoding"));
            case CodecKind.B10G11R11:
            case CodecKind.E5B9G9R9:
                // The Lua function's text for both formats.
                throw new TextureError(TextureFault.Unsupported, "E5B9G9R9_ufloat_pack32 encoding not implemented");
        }
        var count = (long)width * height;
        if (codec.Kind == CodecKind.Plain && codec.Size > 1 && codec.Sign != Signedness.Unsigned && count > 0 && (codec.Order!.Length > 1 || count > 1))
            throw new TextureError(TextureFault.Unsupported,
                $"{GameName(format)} encoding takes one pixel of a single-channel format only, got {width}x{height}");
        if (codec.Kind == CodecKind.Block && (width % 4 != 0 || height % 4 != 0))
            throw new TextureError(TextureFault.Data, $"{GameName(format)} encoding requires dimensions that are multiples of 4, got {width}x{height}");
        var length = codec.Kind == CodecKind.Block
            ? (long)((width + 3) / 4) * ((height + 3) / 4) * codec.BlockBytes
            : count * codec.TexelBytes;
        if (length > Array.MaxLength) throw Overflow();
        var output = new byte[length];
        switch (codec.Kind)
        {
            case CodecKind.Packed:
            {
                var bytes = codec.TexelBytes;
                for (int p = 0, t = 0; t < output.Length; p += 4, t += bytes)
                {
                    uint word = 0;
                    foreach (var (channel, bits, shift) in codec.Fields!) word |= Quantize(rgba[p + channel], bits) << shift;
                    for (var b = 0; b < bytes; b++) output[t + b] = (byte)(word >> (8 * b));
                }
                break;
            }
            case CodecKind.Plain when codec.Size == 1:
            {
                var order = codec.Order!;
                var bias = codec.Sign == Signedness.Signed ? 0x80 : 0;
                for (int p = 0, t = 0; t < output.Length; p += 4, t += order.Length)
                    for (var i = 0; i < order.Length; i++)
                        output[t + i] = (byte)(rgba[p + order[i]] ^ bias);
                break;
            }
            case CodecKind.Plain when codec.Sign == Signedness.Unsigned:
            {
                // Each channel at byte offset i (not i * size), its byte repeated; later channels overwrite earlier ones.
                var order = codec.Order!;
                var size = codec.Size;
                var group = order.Length * size;
                for (int p = 0, t = 0; t < output.Length; p += 4, t += group)
                    for (var i = 0; i < order.Length; i++)
                        output.AsSpan(t + i, size).Fill(rgba[p + order[i]]);
                break;
            }
            case CodecKind.Plain:
                if (count > 0)
                {
                    var x = rgba[0];
                    var size = codec.Size;
                    if (codec.Sign == Signedness.Signed)
                    {
                        output[0] = x;
                        output[size - 1] |= 0x80;
                    }
                    else if (size == 2) BinaryPrimitives.WriteHalfLittleEndian(output, (Half)(x / 255f));
                    else if (size == 4) BinaryPrimitives.WriteSingleLittleEndian(output, x / 255f);
                    else BinaryPrimitives.WriteDoubleLittleEndian(output, x / 255.0);
                }
                break;
            case CodecKind.Block:
                if (count > 0) BlockEncoder.Encode(codec.Block, rgba, output, width, height);
                break;
        }
        return output;
    }
}

/// <summary>Rust's saturating float-to-integer casts (NaN is 0), which C#'s casts do not promise.</summary>
internal static class Sat
{
    public static byte ToByte(float f) => float.IsNaN(f) || f <= 0f ? (byte)0 : f >= 255f ? (byte)255 : (byte)f;

    public static byte ToByte(double d) => double.IsNaN(d) || d <= 0.0 ? (byte)0 : d >= 255.0 ? (byte)255 : (byte)d;

    public static int ToInt(float f) => float.IsNaN(f) ? 0 : f >= 2147483648f ? int.MaxValue : f <= -2147483648f ? int.MinValue : (int)f;

    public static uint ToUInt(float f) => float.IsNaN(f) || f <= 0f ? 0u : f >= 4294967296f ? uint.MaxValue : (uint)f;

    /// <summary>Rust's f32::min: a NaN argument gives the other one.</summary>
    public static float Min(float a, float b) => float.IsNaN(a) ? b : float.IsNaN(b) ? a : a < b ? a : b;

    /// <summary>Rust's f32::max: a NaN argument gives the other one.</summary>
    public static float Max(float a, float b) => float.IsNaN(a) ? b : float.IsNaN(b) ? a : a > b ? a : b;

    /// <summary>f32 powf, correctly rounded (through double), as the platform's powf gives it.</summary>
    public static float Pow(float x, float y) => (float)Math.Pow(x, y);
}
