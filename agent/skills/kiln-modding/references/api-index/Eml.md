# Eml (Kiln.Eml)

## ByteOrder — Byte order of a `DataBuffer`.
enum: Little, Big

## DataBuffer — A growable byte buffer with a read position (`DataBuffer.Head`) and a write position (`DataBuffer.Tail`): writes append at the tail, reads consume from the head, so...
- `DataBuffer(int initialCapacity = 64)`
- `Capacity { get; } -> int`
- `Copy(DataBuffer source, int offset = 0, int? length = null) -> void` — Appends `length` bytes of `source` from its head + offset (all remaining by default).
- `Head { get; set; } -> int` — Read position.
- `Order { get; set; } -> ByteOrder`
- `ReadBool() -> bool`
- `ReadBytes(int length) -> byte[]`
- `ReadF16() -> Half`
- `ReadF32() -> float`
- `ReadF64() -> double`
- `ReadI16() -> short`
- `ReadI32() -> int`
- `ReadI64() -> long`
- `ReadI8() -> sbyte`
- `ReadString(int length) -> string` — `length` bytes as UTF-8 text.
- `ReadU16() -> ushort`
- `ReadU32() -> uint`
- `ReadU64() -> ulong`
- `ReadU8() -> byte`
- `Remaining { get; } -> int` — Bytes between head and tail.
- `Reserve(int length) -> void` — Makes room for at least `length` more bytes after the tail.
- `Reset() -> void` — Head and tail back to 0 (the bytes stay allocated).
- `Skip(int length) -> void` — Moves the head forward by `length` bytes.
- `Tail { get; set; } -> int` — Write position (end of the data).
- `ToArray() -> byte[]` — The bytes between head and tail.
- `static Wrap(byte[] bytes) -> DataBuffer` — A buffer over a copy of `bytes`: head 0, tail at the end.
- `static Wrap(string text) -> DataBuffer` — A buffer over the UTF-8 bytes of `text`.
- `WriteBool(bool v) -> void`
- `WriteBytes(ReadOnlySpan<byte> bytes) -> void`
- `WriteF16(Half v) -> void`
- `WriteF32(float v) -> void`
- `WriteF64(double v) -> void`
- `WriteI16(short v) -> void`
- `WriteI32(int v) -> void`
- `WriteI64(long v) -> void`
- `WriteI8(sbyte v) -> void`
- `WriteString(string text) -> void` — The UTF-8 bytes of `text` (no length prefix, no terminator).
- `WriteU16(ushort v) -> void`
- `WriteU32(uint v) -> void`
- `WriteU64(ulong v) -> void`
- `WriteU8(byte v) -> void`

## DataHash — Hashes game data uses and EML mods compute (EML `hasher.fnv1a32/crc32/crc64`).
- `static Crc32(ReadOnlySpan<byte> data) -> uint` — CRC-32/ISO-HDLC (zlib's): reflected polynomial 0xEDB88320, init and final XOR 0xFFFFFFFF.
- `static Crc32(string text) -> uint` — CRC-32/ISO-HDLC (zlib's): reflected polynomial 0xEDB88320, init and final XOR 0xFFFFFFFF.
- `static Crc64(ReadOnlySpan<byte> data) -> ulong` — CRC-64/ECMA-182: polynomial 0x42F0E1EBA9EA3693, not reflected, init 0, no final XOR.
- `static Crc64(string text) -> ulong` — CRC-64/ECMA-182: polynomial 0x42F0E1EBA9EA3693, not reflected, init 0, no final XOR.
- `static Fnv1a32(ReadOnlySpan<byte> data) -> uint` — FNV-1a, 32 bit (offset basis 0x811C9DC5, prime 0x01000193).
- `static Fnv1a32(string text) -> uint` — FNV-1a, 32 bit (offset basis 0x811C9DC5, prime 0x01000193).

## DataImage — An image of 8-bit RGBA pixels, row by row from the top, 4 bytes per pixel (R, G, B, A).
- `DataImage(int width, int height)` — A transparent black image (`image.create`).
- `Clone() -> DataImage`
- `static Decode(ReadOnlySpan<byte> data) -> DataImage` — Decodes an image file.
- `static DecodePng(ReadOnlySpan<byte> png) -> DataImage` — Decodes a PNG: every colour type and bit depth (grey, RGB, palette, grey+alpha, RGBA; 1, 2, 4, 8 and 16 bits), the...
- `EncodePng() -> byte[]` — The image as a PNG file: 8-bit RGBA, not interlaced, each row filtered with the best of the five PNG filters.
- `Fill(Rgba color) -> void` — Sets every pixel.
- `static FromRgba8(ReadOnlySpan<byte> rgba, int width, int height) -> DataImage` — An image over a copy of raw RGBA8 data (`width` x `height` x 4 bytes).
- `GetPixel(int x, int y) -> Rgba?` — The pixel at (x, y), or null outside the image (EML `get_pixel` gives nil).
- `GetPixelPacked(int x, int y) -> uint?` — The pixel as 0xRRGGBBAA, or null outside the image (EML `get_pixel_packed`).
- `Height { get; } -> int`
- `static IsPng(ReadOnlySpan<byte> data) -> bool` — True when the bytes start with the PNG signature.
- `const MaxPixels = 67108864 -> long` — The most pixels an image may have (64 Mpixel: 256 MiB as RGBA8).
- `Pixels { get; } -> Span<byte>` — The pixel bytes, shared with the image: row-major from the top row, R G B A per pixel.
- `SetPixel(int x, int y, Rgba color) -> void` — Sets a pixel.
- `SetPixel(int x, int y, byte r, byte g, byte b, byte a) -> void` — Sets a pixel.
- `SetPixelPacked(int x, int y, uint rrggbbaa) -> void` — Sets a pixel from 0xRRGGBBAA (EML `set_pixel_packed`).
- `ToRgba8() -> byte[]` — The raw RGBA8 bytes (a copy).
- `Width { get; } -> int`

## Eml — The EML mods of this start and what kiln-eml did with them (docs/kiln/eml.md).
- `static DefaultStateFile { get; } -> string` — `<game>/kiln/state/eml.json`, found next to Kiln.Api (`<game>/kiln/host/`).
- `static IsLoaded(string id) -> bool` — True when the EML mod `id` ran this start (EML `loader.has_mod`).
- `static Load(string path, bool currentProcess = false) -> EmlStatus?` — Reads a state file; null when it is missing or not format 1.
- `static Status { get; } -> EmlStatus?` — This start's result, or null when kiln-eml did not run (not installed, an older Kiln).

## EmlModStatus — One EML mod folder kiln-eml looked at.
- `Capabilities { get; init; } -> IReadOnlyList<string>`
- `Id { get; init; } -> string?`
- `Name { get; init; } -> string?`
- `Path { get; init; } -> string`
- `Reason { get; init; } -> string?`
- `Source { get; init; } -> string` — `profile`, `kiln-dropin` (kiln/eml/mods), `game-mods` (EML's mods folder), or the engine plugin that added it.
- `State { get; init; } -> string` — `loaded`, `failed`, `skipped` (not an EML mod), `disabled`, `duplicate`.
- `Version { get; init; } -> string?`

## EmlStatus — kiln-eml's run (state/eml.json).
- `Aside { get; init; } -> string?`
- `Cached { get; init; } -> bool`
- `EmlVersion { get; init; } -> string?`
- `Error { get; init; } -> string?`
- `File { get; init; } -> string?` — The game's data file: `enshrouded` or `enshrouded_server`.
- `Format { get; init; } -> int`
- `KilnEmlVersion { get; init; } -> string?`
- `Mods { get; init; } -> IReadOnlyList<EmlModStatus>`
- `Ms { get; init; } -> double`
- `Patched { get; init; } -> int`
- `Phase { get; init; } -> string` — `done` (patched or unchanged since the last run), `failed` (game data restored to vanilla), `aside` (EML's own...
- `Pid { get; init; } -> int`
- `Restored { get; init; } -> bool`

## GuidHash — The hash of a GUID.
- `static FromBytes(ReadOnlySpan<byte> guidBytes) -> Guid` — The GUID the game's 16 bytes mean (its order is `System.Guid`'s).
- `static IdOf(Guid guid) -> uint` — The id the game derives from a GUID (item id, buff type id, node id): 0 for the zero GUID (it means "none")...
- `static IdOf(ReadOnlySpan<byte> guidBytes) -> uint` — The id the game derives from 16 GUID bytes in its order; 0 for all zeros.
- `static Of(Guid guid) -> uint` — FNV-1a-32 of the GUID's bytes in the game's order.
- `static Of(ReadOnlySpan<byte> guidBytes) -> uint` — FNV-1a-32 of the 16 bytes (EML `game.guid.hash`).
- `static ToBytes(Guid guid) -> byte[]` — The 16 bytes of a GUID in the game's order.

## Rgba — One pixel: 8 bits per channel, not premultiplied.
- `Rgba()`
- `Rgba(byte R, byte G, byte B, byte A)`
- `A { get; init; } -> byte`
- `B { get; init; } -> byte`
- `static FromPacked(uint rrggbbaa) -> Rgba`
- `G { get; init; } -> byte`
- `Packed { get; } -> uint` — 0xRRGGBBAA.
- `R { get; init; } -> byte`
