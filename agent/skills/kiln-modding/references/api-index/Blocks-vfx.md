# Blocks (Kiln.Blocks): vfx

## Blend — How a particle blends with what is behind it.
enum: Alpha, Additive

## Curve — A number over a particle's life: a constant (`0.2f`), `Curve.Linear`, evenly spaced values (`Curve.Of`), or `Curve.Keys`.
- `static implicit operator Curve(float value) -> Curve` — A constant.
- `static Keys(params (float T, float Value)[] keys) -> Curve` — Values at times from 0 to 1, growing.
- `static Linear(float start, float end) -> Curve` — From `start` at the particle's birth to `end` at the end of its life.
- `static Of(params float[] values) -> Curve` — The values evenly spaced over the particle's life.

## DissolveBlock — How a material dissolves.
- `Color { get; init; } -> string?` — The edge's glow, `#rrggbb`.
- `static implicit operator DissolveBlock(bool on) -> DissolveBlock` — The short form: `true` is a dissolve with the default settings, `false` says there is none.
- `NoiseBlend { get; init; } -> float?` — 0 to 1: noise against distance from the centre.
- `NoiseTiling { get; init; } -> float?` — Default 0.5.
- `Width { get; init; } -> float?` — The edge's width.

## EmissiveBlock — What glows.
- `Color { get; init; } -> string?` — `#rrggbb`, sRGB: multiplies the map.
- `Exposure { get; init; } -> float?` — 0 to 1: how much it follows the camera's exposure correction.
- `static implicit operator EmissiveBlock(string mapOrColour) -> EmissiveBlock` — A map or a colour: a string that starts with `#` is the colour, any other is the map's path.
- `Map { get; init; } -> string?` — A PNG, sRGB.
- `Strength { get; init; } -> float?` — Default 1, 4 for `MaterialKind.Emissive`.

## Flipbook — The texture is a grid of frames.
- `Columns { get; init; } -> int?` — How many frames lie across the texture.
- `Fps { get; init; } -> float?` — Frames per second.
- `Rows { get; init; } -> int?` — How many frames lie down the texture.

## Gradient — A colour over a particle's life: one (`"#7fd4ff"`), evenly spaced ones (`Gradient.Of`), or `Gradient.Keys`.
- `static implicit operator Gradient(string colour) -> Gradient` — One colour for the whole life.
- `static Keys(params (float T, string Colour)[] keys) -> Gradient` — Colours at times from 0 to 1, growing.
- `static Of(params string[] colours) -> Gradient` — The colours (`#rrggbb`) evenly spaced over the particle's life.

## LightBlock — One point light for the life of the effect.
- `Brightness { get; init; } -> float?` — How bright the light is.
- `Color { get; init; } -> string?` — `#rrggbb`, sRGB.
- `Comment { init; }` — Data files may say it.
- `FadeIn { get; init; } -> float?` — Seconds it takes to come up at the start.
- `FadeOut { get; init; } -> float?` — Seconds, at the end of the lifetime or after the effect stops.
- `Flicker { get; init; } -> float?` — 0 to 1.
- `Lifetime { get; init; } -> float?` — Seconds.
- `Offset { get; init; } -> Vector3?` — Metres, in the effect's space.
- `Radius { get; init; } -> float?` — Metres.

## MaterialAlphaTest — `true`, `false`, or the alpha to cut at.
- `Equals(MaterialAlphaTest other) -> bool`
- `static implicit operator MaterialAlphaTest(bool on) -> MaterialAlphaTest` — `true` cuts where the albedo's alpha is under 0.5, `false` does not cut.
- `static implicit operator MaterialAlphaTest(float cutoff) -> MaterialAlphaTest` — Cuts where the albedo's alpha is under this value.
- `static operator !=(MaterialAlphaTest left, MaterialAlphaTest right) -> bool`
- `static operator ==(MaterialAlphaTest left, MaterialAlphaTest right) -> bool`

## MaterialBlock — A surface on the game's own shaders.
- `Albedo { get; init; } -> string?` — Colour: a PNG in your mod's folder, sRGB, a power of two on both sides, 4 to 4096.
- `AlphaTest { get; init; } -> MaterialAlphaTest?` — Cut where the albedo's alpha is under this: true is 0.5.
- `Comment { init; }` — Data files may say it.
- `ContactShadows { get; init; } -> bool?` — Default true.
- `Dissolve { get; init; } -> DissolveBlock?` — The game can fade it in and out with an edge glow, as it does to creatures and placed objects: true, or a...
- `DoubleSided { get; init; } -> bool?` — Draw both faces, with the normal turned on the back.
- `Emissive { get; init; } -> EmissiveBlock?` — What glows: a PNG, `#rrggbb`, or an `EmissiveBlock`.
- `Frost { get; init; } -> bool?` — Takes frost in the cold.
- `Kind { get; init; } -> MaterialKind?` — What it is, and where every other field starts from.
- `Metallic { get; init; } -> float?` — 0 to 1: the value without a map, a factor with one.
- `MetallicRoughness { get; init; } -> string?` — A PNG in glTF's layout, linear: G roughness, B metallic.
- `Normal { get; init; } -> NormalMap?` — A tangent-space normal map, green up as in glTF and Blender.
- `Occlusion { get; init; } -> string?` — A PNG, linear, in R.
- `Params { get; init; } -> string?` — A PNG in the game's own layout, linear: R metallic, G roughness, B occlusion, and A reflectance if...
- `Reflectance { get; init; } -> MaterialReflectance?` — Non-metal specular, 0 to 1, or `MaterialReflectance.FromParams`.
- `Replaces { get; init; } -> string?` — The GUID of one of the game's materials: this block draws in its place on every model that uses it, items, props and...
- `Roughness { get; init; } -> float?` — 0 to 1: the value without a map, a factor with one.
- `Shader { get; init; } -> string?` — Reserved for surface shaders, which aren't built yet.
- `Shadows { get; init; } -> bool?` — Casts a shadow.
- `Snow { get; init; } -> bool?` — Takes snow.
- `Tint { get; init; } -> string?` — `#rrggbb`, sRGB: multiplies the albedo.
- `Translucent { get; init; } -> float?` — 0 to 1: how much light comes through from behind.
- `UvScale { get; init; } -> MaterialUvScale?` — Repeats of every map: a number, or `(u, v)`.
- `VertexColor { get; init; } -> bool?` — Multiply the albedo by the model's vertex colour.
- `Was { get; init; } -> string?` — The name this block had before a rename.
- `Wetness { get; init; } -> bool?` — Darkens in the rain.

## MaterialBlockHandle — What `Blocks.Define` gives back.
- `Status { get; } -> VfxBlockStatus` — Whether this game has it now.

## MaterialKind — What a material is.
enum: Opaque, Metal, Cutout, Foliage, Cloth, Emissive, Dithered

## MaterialRef — A material of this mod: its name, or the handle from `Blocks.Define`.
- `MaterialRef()`
- `MaterialRef(string Name)`
- `static implicit operator MaterialRef(BlockHandle<MaterialBlock> handle) -> MaterialRef` — A material by the handle `Blocks.Define` gave back.
- `static implicit operator MaterialRef(string name) -> MaterialRef` — A material of this mod by its name.
- `Name { get; init; } -> string` — The material's block name in this mod.

## MaterialReflectance — A number, or `MaterialReflectance.FromParams`.
- `Equals(MaterialReflectance other) -> bool`
- `static readonly FromParams -> MaterialReflectance` — Read it from the alpha of `MaterialBlock.Params`.
- `static implicit operator MaterialReflectance(float value) -> MaterialReflectance` — A reflectance of this value, 0 to 1.
- `static operator !=(MaterialReflectance left, MaterialReflectance right) -> bool`
- `static operator ==(MaterialReflectance left, MaterialReflectance right) -> bool`

## MaterialUvScale — A number, or `(u, v)`.
- `MaterialUvScale()`
- `MaterialUvScale(float U, float V)`
- `static implicit operator MaterialUvScale((float U, float V) scale) -> MaterialUvScale` — Repeats along u and v, as a tuple.
- `static implicit operator MaterialUvScale(float value) -> MaterialUvScale` — The same number of repeats along both axes.
- `U { get; init; } -> float` — Repeats of every map along u.
- `V { get; init; } -> float` — Repeats of every map along v.

## NormalMap — A normal map: a PNG (`"n.png"`), or with a `NormalMap.Strength`.
- `NormalMap(string Map, float? Strength = null, bool? YDown = null)`
- `static implicit operator NormalMap(string map) -> NormalMap` — A normal map from a PNG: the same as `new NormalMap(map)`.
- `Map { get; init; } -> string` — The PNG, tangent space, in the mod's folder.
- `Strength { get; init; } -> float?` — How strongly the map bends the surface (glTF's normal `scale`).
- `YDown { get; init; } -> bool?` — True for a map whose green points down (a DirectX map).

## ParticleAlign — Which way a camera-facing quad turns.
enum: Camera, Velocity

## ParticleBlock — What spawns and how it looks.
- `Acceleration { get; init; } -> Vector3?` — Metres per second squared in world space, added to `ParticleBlock.Gravity`.
- `Align { get; init; } -> ParticleAlign?` — `ParticleAlign.Velocity`: a camera-facing quad turns to point the way it moves (sparks, rain, streaks).
- `Alpha { get; init; } -> Curve?` — 0 to 1 over the particle's life.
- `Blend { get; init; } -> Blend?` — Default `Blend.Alpha`.
- `Burst { get; init; } -> int?` — Particles at the start, and at each loop.
- `Color { get; init; } -> Gradient?` — sRGB over the particle's life.
- `Comment { init; }` — Data files may say it.
- `Delay { get; init; } -> float?` — Seconds before the first spawn.
- `DepthFade { get; init; } -> float?` — Metres of soft fade against the scene.
- `Drag { get; init; } -> float?` — Per second: velocity *= exp(-drag * dt).
- `Duration { get; init; } -> float?` — Seconds of emission.
- `Emissive { get; init; } -> float?` — 0 to 1: how much the colour follows the camera's exposure correction.
- `FaceCamera { get; init; } -> bool?` — Turn the mesh to the camera.
- `Flipbook { get; init; } -> Flipbook?` — The texture is a grid of frames.
- `Gravity { get; init; } -> float?` — Metres per second squared on world y: negative falls.
- `Intensity { get; init; } -> float?` — Multiplies the colour, which may then exceed 1.
- `Lifetime { get; init; } -> VfxRange?` — Seconds a particle lives: a number, or `(min, max)`.
- `Lit { get; init; } -> bool?` — Take ambient and sun light through the fog volume.
- `Loop { get; init; } -> bool?` — Start again after the `ParticleBlock.Duration`, which must be above 0.
- `Material { get; init; } -> MaterialRef?` — A material of this mod: its albedo as the `ParticleBlock.Texture`, its tint in the `ParticleBlock.Color`, and...
- `MaxCount { get; init; } -> int?` — Particles alive at once, 1 to 4096.
- `Mesh { get; init; } -> ParticleMesh?` — Default `ParticleMesh.Quad`.
- `Rate { get; init; } -> float?` — Particles per second.
- `Roll { get; init; } -> VfxRange?` — Degrees at spawn: a number, or `(min, max)`.
- `Sampler { get; init; } -> Sampler?` — How the texture repeats.
- `Shader { get; init; } -> ParticleShader?` — Advanced: this mod's own particle hooks, a hook module built from HLSL with `kiln-vfxcook shader build` and linked...
- `Shape { get; init; } -> SpawnShape?` — Where particles start, in the effect's space: `SpawnShape.Sphere`, `SpawnShape.Shell`, `SpawnShape.Hemisphere`...
- `Size { get; init; } -> Curve?` — Metres over the particle's life.
- `SpawnRadius { get; init; } -> float?` — Metres: start inside a sphere this big.
- `Speed { get; init; } -> VfxRange?` — Metres per second at spawn: a number, or `(min, max)`.
- `Spin { get; init; } -> VfxRange?` — Degrees per second: a number, or `(min, max)`.
- `Spread { get; init; } -> Shape?` — Direction at spawn, around the effect's up.
- `Stretch { get; init; } -> float?` — With `ParticleAlign.Velocity`: extra length in metres per metre per second of speed, 0 to 100.
- `Texture { get; init; } -> VfxTexture?` — A PNG in your mod's folder, power of two on both sides, 4 to 2048.

## ParticleMesh — What a particle is drawn as.
enum: Quad, QuadXz

## ParticleShader — Advanced: a mod's own particle hooks (a hook module).
- `ParticleShader(string File, IReadOnlyList<float>? Constants = null)`
- `Constants { get; init; } -> IReadOnlyList<float>?` — Up to 8 finite numbers the hooks read as `c.user0` and `c.user1`.
- `File { get; init; } -> string` — The hook module in the mod's folder, built with `kiln-vfxcook shader build` (`"shaders/build/swirl.spv"`).
- `static implicit operator ParticleShader(string file) -> ParticleShader` — A hook module by its file.

## Sampler — How a particle's texture repeats beyond its edges.
enum: Wrap, Mirror, Clamp, ClampToZero

## ScreenEffectBlock — A full-screen effect: colour grading, a tint, a vignette, a fade or a flash.
- `Comment { init; }` — A note in a data file, ignored.
- `Range { get; init; } -> float?` — Metres: when an effect plays it at a place, the players this near see it.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## ScreenRef — A screen effect: inline, from `Blocks.Define`, or by name.
- `static implicit operator ScreenRef(BlockHandle<ScreenEffectBlock> handle) -> ScreenRef` — An effect from the handle `Blocks.Define` gave back, by its block and its name.
- `static implicit operator ScreenRef(ScreenEffectBlock block) -> ScreenRef` — An effect written in place.
- `static implicit operator ScreenRef(string name) -> ScreenRef` — An effect by its block name.
- `Inline { get; } -> ScreenEffectBlock?` — The effect itself: written in place, or taken from a handle.
- `Name { get; } -> string?` — The effect's block name (`name`, or `other-mod:name`): set for a name and for a handle.

## SpawnShape — Where particles start, in the effect's space (its scale and rotation apply).
- `static Box(Vector3 size) -> SpawnShape` — Inside a box this big, centred.
- `static Box(float x, float y, float z) -> SpawnShape` — Inside a box this big, centred.
- `static Disc(float radius, float? inner = null) -> SpawnShape` — On a flat disc in the effect's horizontal plane; with `inner`, outside that radius too.
- `static Hemisphere(float radius, float? inner = null) -> SpawnShape` — Inside the upper half of a sphere.
- `static Line(float length) -> SpawnShape` — On a line this long along the effect's x, centred.
- `static readonly Point -> SpawnShape` — At the effect's position (the default).
- `static Ring(float radius) -> SpawnShape` — On a circle in the effect's horizontal plane.
- `static Shell(float radius) -> SpawnShape` — On a sphere's surface.
- `static Sphere(float radius, float? inner = null) -> SpawnShape` — Inside a sphere; with `inner`, outside that radius too.

## VfxBlock — One effect: particles, a light.
- `Comment { init; }` — Data files may say it.
- `CullDistance { get; init; } -> VfxRange?` — Metres beyond which nothing is simulated or drawn: far, or `(near, far)`.
- `GameSpriteModule { init; }` — Data files may say it.
- `Light { get; init; } -> LightBlock?` — A point light for the life of the effect.
- `Particles { get; init; } -> IReadOnlyList<ParticleBlock>?` — What spawns and how it looks.
- `Sound { get; init; } -> string?` — A sound's name.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## VfxBlockHandle — What `Blocks.Define` gives back: a block's handle, which `Vfx`'s calls also take.
- `static implicit operator VfxId(VfxBlockHandle handle) -> VfxId` — The effect's id, so a handle goes where `VfxId` does.
- `Status { get; } -> VfxBlockStatus` — Whether this game has it now.

## VfxBlockStatus — Whether a `VfxBlock` is in this game.
enum: Registered, Pending, NotHere

## VfxParameterBlock — The per-play parameters every Kiln VfxBlock has (KX_* in native/services/vfx/shaders/kiln_vfx_params.h), all factors on what the block says: `color` (a tint...
- `static Defaults() -> uint[]` — The defaults: white, all factors 1, and the word that says the block is Kiln's.
- `static readonly Names -> IReadOnlyList<string>` — The names, in the block's order.
- `static Of(IReadOnlyDictionary<string, VfxValue> values, Func<string, string, Exception> error) -> uint[]` — The block with these values over the defaults.

## VfxRange — A number, or a range to pick from: `0.5f`, `(0.4f, 0.9f)`.
- `VfxRange()`
- `VfxRange(float Min, float Max)`
- `static implicit operator VfxRange((float Min, float Max) range) -> VfxRange` — A range to pick from, as a tuple `(min, max)`.
- `static implicit operator VfxRange(float value) -> VfxRange` — A fixed value: the same minimum and maximum.
- `Max { get; init; } -> float` — The highest value.
- `Min { get; init; } -> float` — The lowest value.

## VfxTexture — A PNG in the mod's folder.
- `VfxTexture(string File, bool? Linear = null)`
- `File { get; init; } -> string` — The PNG's path in the mod's folder.
- `static implicit operator VfxTexture(string file) -> VfxTexture` — A PNG by its path in the mod's folder: the same as `new VfxTexture(file)`.
- `Linear { get; init; } -> bool?` — True when the image holds data, not colour.

## VfxValue — The value of a named VFX parameter: a number (`3f`, a float), a colour (`"#ff0000"`, `"#ff000080"`), a `Vector3`, or `VfxValue.Int`.
- `Equals(VfxValue other) -> bool`
- `static implicit operator VfxValue(Color value) -> VfxValue` — A colour, written as `#rrggbbaa`.
- `static implicit operator VfxValue(Vector3 value) -> VfxValue` — A vector.
- `static implicit operator VfxValue(float value) -> VfxValue` — A number, as a float.
- `static implicit operator VfxValue(string colour) -> VfxValue` — A colour as a string: `#rrggbb` or `#rrggbbaa`.
- `static Int(int value) -> VfxValue` — A whole-number parameter, as the game's `count`.
- `static operator !=(VfxValue left, VfxValue right) -> bool`
- `static operator ==(VfxValue left, VfxValue right) -> bool`
