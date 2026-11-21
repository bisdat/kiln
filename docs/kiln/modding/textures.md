# Textures in code

Since 0.2.0-dev.30.3, a C# mod can read and write the game's texture data in code: `GameTexture` (Kiln.Api,
namespace `Kiln`) decodes the bytes of a game texture to RGBA8 pixels and encodes RGBA8 pixels to a game pixel format,
BC1 to BC7 included. It gives the same answers as kiln mode's Lua functions `image.decode_texture` and
`image.encode_texture`, byte for byte, failures included, so a ShroudForge-format mod's texture code ports to C# line
by line.

```csharp
// a game texture's data (a content blob: its mip chain, largest level first) to pixels, tinted, and back
byte[] blob = Assets.ReadBlob(hash)!;
byte[] rgba = GameTexture.DecodeMip(blob, GamePixelFormat.BC7_unorm_block, 256, 256, mip: 0);
for (var i = 0; i < rgba.Length; i += 4) rgba[i] = (byte)Math.Min(255, rgba[i] + 40);
byte[] bc7 = GameTexture.Encode(rgba, 256, 256, GamePixelFormat.BC7_unorm_block);

var icon = DataImage.FromRgba8(rgba, 256, 256);   // Kiln.Eml: pixels, PNG
```

## The API

| Member | What |
|---|---|
| `enum GamePixelFormat` | The game's pixel formats (`keen::PixelFormat`) with the game's names and numbers: `R8G8B8A8_unorm = 37`, `BC1_RGB_unorm_block = 131`, `BC7_srgb_block = 146`. A texture resource's format value casts to it. |
| `GameTexture.Decode(data, format, width, height)` | One image to RGBA8 (`width * height * 4` bytes). Bytes after the image are ignored. The same as `image.decode_texture(data, format, width, height)`. |
| `GameTexture.DecodeMip(mipChain, format, width, height, mip)` | Level `mip` of a mip chain (the levels one after another, largest first, as the game stores them). Level `n` is `max(1, width >> n)` x `max(1, height >> n)`. Block-format levels of any size decode: the whole blocks are decoded and cut to the level. |
| `GameTexture.Encode(rgba, width, height, format)` | RGBA8 to the format. The same bytes as `image.encode_texture(image, format)`. |
| `GameTexture.ByteSize(format, width, height)` | The bytes of one image (block formats: whole 4x4 blocks). |
| `GameTexture.MipOffset(format, width, height, mip)` | Where level `mip` starts in a chain: the sum of the larger levels' sizes. |

Pixels are RGBA8: 4 bytes a pixel (R, G, B, A), row by row from the top. Everything is plain C# on your own bytes: no
game, service or capability, any thread (`[AnyThread]`), and the same in a unit test (Kiln.TestKit's coverage table
lists it as real). Encoding BC6H and BC7 searches many block modes and is slow
on large textures: run it in a task (`Context.Tasks`, [tasks.md](tasks.md)), not on the tick.

## Formats

| Formats | Decode | Encode |
|---|---|---|
| 8-bit: `R8`, `R8G8`, `R8G8B8`, `B8G8R8`, `R8G8B8A8`, `B8G8R8A8`, `A8B8G8R8_*_pack32` (unorm, snorm, uscaled, sscaled, uint, sint) | yes | yes |
| Packed: `R4G4_unorm_pack8`, `R4G4B4A4`, `B4G4R4A4`, `R5G6B5`, `B5G6R5`, `R5G5B5A1`, `B5G5R5A1`, `A1R5G5B5` (`_unorm_pack16`) | yes | yes |
| 16-bit (`R16` to `R16G16B16A16`), all kinds; `_sfloat` as halves | yes | unsigned: each channel's byte repeated (right for one channel: `R16_unorm` gets `x * 257`); signed and float: one pixel of a single-channel format only |
| 32 and 64-bit floats (`R32_sfloat` ... `R64G64B64A64_sfloat`) | yes | one pixel of a single-channel format only |
| 32 and 64-bit integers (`R32_uint` ... `R64G64B64A64_sint`) | no (an empty image yes) | unsigned: each channel's byte repeated; signed: one single-channel pixel |
| `B10G11R11_ufloat_pack32`, `E5B9G9R9_ufloat_pack32` | yes | no |
| `BC1` to `BC5`, `BC6H_ufloat_block`, `BC7` (unorm and sRGB) | yes, sizes in multiples of 4 (`DecodeMip`: any) | yes, sizes in multiples of 4 |
| `BC6H_sfloat_block`, `A2R10G10B10` and `A2B10G10R10` packs, sRGB forms of the 8-bit formats, depth and stencil, `None` | no | no |

What decoding gives:
- a channel the format does not have is 0, alpha 255; `B5G6R5_unorm_pack16`, BC4, BC5 and BC6H give alpha 0;
- sRGB block formats decode like their unorm forms (no gamma), BC4 and BC5 snorm like unorm;
- signed 8-bit values are biased (`x ^ 0x80`), 16-bit values keep their high byte, floats give `value * 255` cut
  to 0..255 (NaN is 0); BC6H is made sRGB-like (`linear ^ (1 / 2.4)`) and rounded.

What encoding gives:
- 4-bit fields truncate, 5, 6 and 1-bit fields round;
- `A8B8G8R8_*_pack32` stores the bytes A, B, G, R;
- BC1 to BC7 are the game's block layout. BC6H takes the 8-bit channels as sRGB and stores them linear. BC7 uses
  every mode but mode 2.

## Errors

| Exception | When |
|---|---|
| `NotSupportedException` | The format cannot be decoded or encoded (the table above), or a signed or float format wider than 8 bits got more than one single-channel pixel. The message is the Lua function's (`R8_srgb encoding not implemented`). |
| `ArgumentException` | The data is shorter than the image (or the level), `rgba` is not `width * height * 4` bytes, or a block format's size is not a multiple of 4 (`Decode`, `Encode`). |
| `ArgumentOutOfRangeException` | A negative size, more than 2 GiB of pixels, a value that is not a `GamePixelFormat`, a mip level past the 1x1 one. |

Unlike the Lua function, `Decode` has no mip argument (the Lua function ignores its own): `DecodeMip` takes a level.

## Where it runs and how it is checked

`GameTexture` is managed code in Kiln.Api: no native call, no new ABI. Its rules are those of Kiln's texture code for
kiln mode (`crates/sfi/sfi-core/src/eml/texture.rs`); its BC1 to BC7 code is the C# form of the block codec that
code uses, block_compression 0.6.0, operation for operation in single-precision floats, so both write the same bytes.

The checks run kiln mode's texture fixtures (`crates/sfi/sfi-core/tests/conformance/texture`) both ways against the
same goldens: `sfi-core`'s `texture_conformance` test runs the Lua fixtures, and `Kiln.Api.Tests`'
`GameTextureConformanceTests` rebuilds every case of `decode.lua` and `encode.lua` in C# (the same inputs and
digests) and checks every record of their goldens. `Kiln.TestKit.Tests`' `GameTextureKitTests` stores a BC1 mip
chain as a blob in a `KilnHarness` and decodes each level.

Credits: the format rules are adapted from ShroudForge by bonsaibauer, MIT
([licence](../../../licenses/ShroudForge-LICENSE.txt)); the block codec is ported from block_compression 0.6.0 by Nils
Hasenbanck and Intel Corporation, MIT ([licence](../../../licenses/block_compression-LICENSE.txt)), whose decoders come
from bcdec by Sergii Kudlai (public domain).
