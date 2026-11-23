# Eml (Kiln.Eml)

## ByteOrder — Byte order of a `DataBuffer`.
enum: Little, Big

## DataBuffer — A growable byte buffer with a read position (`DataBuffer.Head`) and a write position (`DataBuffer.Tail`): writes append at the tail, reads consume from the head, so...
- `DataBuffer(int initialCapacity = 64)` — Creates an empty buffer with room for `initialCapacity` bytes; it grows as needed.
- `Capacity { get; } -> int` — How many bytes the buffer holds room for before it has to grow.
- `Copy(DataBuffer source, int offset = 0, int? length = null) -> void` — Appends `length` bytes of `source` from its head + offset (all remaining by default).
- `Head { get; set; } -> int` — Read position.
- `Order { get; set; } -> ByteOrder` — The byte order of the multi-byte reads and writes (default `ByteOrder.Little`).
- `ReadBool() -> bool` — Reads one byte as a boolean (any non-zero value is true).
- `ReadBytes(int length) -> byte[]` — Reads `length` bytes from the head and moves the head past them.
- `ReadF16() -> Half` — Reads a 16-bit float in the buffer's `DataBuffer.Order`.
- `ReadF32() -> float` — Reads a 32-bit float in the buffer's `DataBuffer.Order`.
- `ReadF64() -> double` — Reads a 64-bit float in the buffer's `DataBuffer.Order`.
- `ReadI16() -> short` — Reads a signed 16-bit integer in the buffer's `DataBuffer.Order`.
- `ReadI32() -> int` — Reads a signed 32-bit integer in the buffer's `DataBuffer.Order`.
- `ReadI64() -> long` — Reads a signed 64-bit integer in the buffer's `DataBuffer.Order`.
- `ReadI8() -> sbyte` — Reads a signed 8-bit integer.
- `ReadString(int length) -> string` — `length` bytes as UTF-8 text.
- `ReadU16() -> ushort` — Reads an unsigned 16-bit integer in the buffer's `DataBuffer.Order`.
- `ReadU32() -> uint` — Reads an unsigned 32-bit integer in the buffer's `DataBuffer.Order`.
- `ReadU64() -> ulong` — Reads an unsigned 64-bit integer in the buffer's `DataBuffer.Order`.
- `ReadU8() -> byte` — Reads an unsigned 8-bit integer.
- `Remaining { get; } -> int` — Bytes between head and tail.
- `Reserve(int length) -> void` — Makes room for at least `length` more bytes after the tail.
- `Reset() -> void` — Head and tail back to 0 (the bytes stay allocated).
- `Skip(int length) -> void` — Moves the head forward by `length` bytes.
- `Tail { get; set; } -> int` — Write position (end of the data).
- `ToArray() -> byte[]` — The bytes between head and tail.
- `static Wrap(byte[] bytes) -> DataBuffer` — A buffer over a copy of `bytes`: head 0, tail at the end.
- `static Wrap(string text) -> DataBuffer` — A buffer over the UTF-8 bytes of `text`.
- `WriteBool(bool v) -> void` — Appends a boolean as one byte: 1 for true, 0 for false.
- `WriteBytes(ReadOnlySpan<byte> bytes) -> void` — Appends `bytes` at the tail.
- `WriteF16(Half v) -> void` — Appends a 16-bit float in the buffer's `DataBuffer.Order`.
- `WriteF32(float v) -> void` — Appends a 32-bit float in the buffer's `DataBuffer.Order`.
- `WriteF64(double v) -> void` — Appends a 64-bit float in the buffer's `DataBuffer.Order`.
- `WriteI16(short v) -> void` — Appends a signed 16-bit integer in the buffer's `DataBuffer.Order`.
- `WriteI32(int v) -> void` — Appends a signed 32-bit integer in the buffer's `DataBuffer.Order`.
- `WriteI64(long v) -> void` — Appends a signed 64-bit integer in the buffer's `DataBuffer.Order`.
- `WriteI8(sbyte v) -> void` — Appends a signed 8-bit integer.
- `WriteString(string text) -> void` — The UTF-8 bytes of `text` (no length prefix, no terminator).
- `WriteU16(ushort v) -> void` — Appends an unsigned 16-bit integer in the buffer's `DataBuffer.Order`.
- `WriteU32(uint v) -> void` — Appends an unsigned 32-bit integer in the buffer's `DataBuffer.Order`.
- `WriteU64(ulong v) -> void` — Appends an unsigned 64-bit integer in the buffer's `DataBuffer.Order`.
- `WriteU8(byte v) -> void` — Appends an unsigned 8-bit integer.

## DataHash — Hashes game data uses and EML mods compute (EML `hasher.fnv1a32/crc32/crc64`).
- `static Crc32(ReadOnlySpan<byte> data) -> uint` — CRC-32/ISO-HDLC (zlib's): reflected polynomial 0xEDB88320, init and final XOR 0xFFFFFFFF.
- `static Crc32(string text) -> uint` — CRC-32/ISO-HDLC of the UTF-8 bytes of `text`.
- `static Crc64(ReadOnlySpan<byte> data) -> ulong` — CRC-64/ECMA-182: polynomial 0x42F0E1EBA9EA3693, not reflected, init 0, no final XOR.
- `static Crc64(string text) -> ulong` — CRC-64/ECMA-182 of the UTF-8 bytes of `text`.
- `static Fnv1a32(ReadOnlySpan<byte> data) -> uint` — FNV-1a, 32 bit (offset basis 0x811C9DC5, prime 0x01000193).
- `static Fnv1a32(string text) -> uint` — FNV-1a, 32 bit, of the UTF-8 bytes of `text`.

## DataImage — An image of 8-bit RGBA pixels, row by row from the top, 4 bytes per pixel (R, G, B, A).
- `DataImage(int width, int height)` — A transparent black image (`image.create`).
- `Clone() -> DataImage` — A copy of the image with its own pixel data.
- `static Decode(ReadOnlySpan<byte> data) -> DataImage` — Decodes an image file.
- `static DecodePng(ReadOnlySpan<byte> png) -> DataImage` — Decodes a PNG: every colour type and bit depth (grey, RGB, palette, grey+alpha, RGBA; 1, 2, 4, 8 and 16 bits), the...
- `EncodePng() -> byte[]` — The image as a PNG file: 8-bit RGBA, not interlaced, each row filtered with the best of the five PNG filters.
- `Fill(Rgba color) -> void` — Sets every pixel.
- `static FromRgba8(ReadOnlySpan<byte> rgba, int width, int height) -> DataImage` — An image over a copy of raw RGBA8 data (`width` x `height` x 4 bytes).
- `GetPixel(int x, int y) -> Rgba?` — The pixel at (x, y), or null outside the image (EML `get_pixel` gives nil).
- `GetPixelPacked(int x, int y) -> uint?` — The pixel as 0xRRGGBBAA, or null outside the image (EML `get_pixel_packed`).
- `Height { get; } -> int` — The height in pixels.
- `static IsPng(ReadOnlySpan<byte> data) -> bool` — True when the bytes start with the PNG signature.
- `const MaxPixels = 67108864 -> long` — The most pixels an image may have (64 Mpixel: 256 MiB as RGBA8).
- `Pixels { get; } -> Span<byte>` — The pixel bytes, shared with the image: row-major from the top row, R G B A per pixel.
- `SetPixel(int x, int y, Rgba color) -> void` — Sets a pixel from `color`.
- `SetPixel(int x, int y, byte r, byte g, byte b, byte a) -> void` — Sets a pixel.
- `SetPixelPacked(int x, int y, uint rrggbbaa) -> void` — Sets a pixel from 0xRRGGBBAA (EML `set_pixel_packed`).
- `ToRgba8() -> byte[]` — The raw RGBA8 bytes (a copy).
- `Width { get; } -> int` — The width in pixels.

## Eml — The EML mods of this start and what kiln-eml did with them (docs/kiln/eml.md).
- `static DefaultStateFile { get; } -> string` — `<game>/kiln/state/eml.json`, found next to Kiln.Api (`<game>/kiln/host/`).
- `static IsLoaded(string id) -> bool` — True when the EML mod `id` ran this start (EML `loader.has_mod`).
- `static Load(string path, bool currentProcess = false) -> EmlStatus?` — Reads a state file; null when it is missing or not format 1.
- `static Status { get; } -> EmlStatus?` — This start's result, or null when kiln-eml did not run (not installed, an older Kiln).

## EmlModStatus — One EML mod folder kiln-eml looked at.
- `Capabilities { get; init; } -> IReadOnlyList<string>` — The capabilities the mod declares in its `mod.json` (`patch`, `export`, `runtime`, `runtime-register-dll`).
- `Id { get; init; } -> string?` — The mod's manifest id; null when it has none.
- `Name { get; init; } -> string?` — The mod's display name; null when it has none.
- `Path { get; init; } -> string` — The mod's folder.
- `Reason { get; init; } -> string?` — Why the mod failed or was skipped, when it was; null otherwise.
- `Source { get; init; } -> string` — `profile`, `kiln-dropin` (kiln/eml/mods), `game-mods` (EML's mods folder), or the engine plugin that added it.
- `State { get; init; } -> string` — `loaded`, `failed`, `skipped` (not an EML mod), `disabled`, `duplicate`.
- `Version { get; init; } -> string?` — The mod's version; null when it has none.

## EmlStatus — kiln-eml's run (state/eml.json).
- `Aside { get; init; } -> string?` — With phase `aside`: what made kiln-eml stand aside (EML's own loader ran instead); null otherwise.
- `Cached { get; init; } -> bool` — True when the result of an earlier start was reused and nothing was patched again.
- `EmlVersion { get; init; } -> string?` — The EML release the plugin carries.
- `Error { get; init; } -> string?` — Why the run failed; null when it did not.
- `File { get; init; } -> string?` — The game's data file: `enshrouded` or `enshrouded_server`.
- `Format { get; init; } -> int` — The state file's format number (1).
- `KilnEmlVersion { get; init; } -> string?` — The version of the kiln-eml plugin that wrote the file.
- `Mods { get; init; } -> IReadOnlyList<EmlModStatus>` — The EML mod folders kiln-eml looked at, with what it did with each.
- `Ms { get; init; } -> double` — How long the run took, in milliseconds.
- `Patched { get; init; } -> int` — How many game resources the run patched.
- `Phase { get; init; } -> string` — `done` (patched or unchanged since the last run), `failed` (game data restored to vanilla), `aside` (EML's own...
- `Pid { get; init; } -> int` — The id of the game process this run belongs to (the Windows pid under Wine and Proton).
- `Restored { get; init; } -> bool` — True when the run put the game data back to vanilla.

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
- `A { get; init; } -> byte` — The alpha channel, 0 (transparent) to 255 (opaque).
- `B { get; init; } -> byte` — The blue channel, 0 to 255.
- `static FromPacked(uint rrggbbaa) -> Rgba` — Splits a packed `0xRRGGBBAA` value into a pixel.
- `G { get; init; } -> byte` — The green channel, 0 to 255.
- `Packed { get; } -> uint` — 0xRRGGBBAA.
- `R { get; init; } -> byte` — The red channel, 0 to 255.
