# Terrain-only tool carving

Since 0.2.0-dev.30.5. Static, read-only qualification on client Steam 26430625
(`5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77`) and dedicated server Steam 26430253
(`f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2`). Image base `0x140000000`.
The functions were disassembled and individually decompiled with Ghidra 12.1.2, without executing either target.
No runtime/client verification is implied.

## Two separate branches

| Function | Client RVA | Server RVA | Observed contract |
|---|---:|---:|---|
| `gameplay/TerraformImpact` | `0x3ece10` | `0x18e0b0` | Impact VM callback; the generic remove path calls terrain removal, then building removal. |
| Terrain removal helper | `0x490f80` | `0x2284a0` | Builds material coefficients for cell ids 1 through 127 and invokes the terrain brush wrapper twice (preview and commit). |
| `tools/TerrainCarveKernel` | `0xa8f880` | `0xa3a180` | `bool(void* brush, void* cell_span, void* third, const int32_t* origin, const uint32_t* dims)`; third argument is passed through unchanged. |
| `tools/RemoveBuildingVoxels` | `0x4906a0` | `0x227bc0` | `uint8_t(void* operation, void* brush, uint32_t item)`; iterates cells 128 through 255 and runs a separate building kernel. |

The terrain kernel itself only changes cells with material 1..127 and nonzero fill. Its coefficient lookup is bounded
by the span count. A non-positive coefficient suppresses removal of that material. Air and all building materials are
outside the kernel's mutation branch. Building removal is not suppressed by zeroing terrain coefficients: the separate
building helper must be skipped. It returns a success byte to the enclosing operation, so the scoped suppression must
return 1 without calling its continuation.

The terrain brush descriptor occupies 0x90 bytes. Its coefficient pointer is at +0x70, its u64 coefficient count at
+0x78, its native report pointer at +0x80, and its final flag at +0x88. The native wrapper constructs the descriptor
on its stack; the kernel reads it synchronously. Geometry, radius, origin, rotation, direction, flags and report pointer
are not replaced by Kiln. Kiln copies the descriptor and at most 256 coefficient floats to owned stack storage,
preserves allowed terrain coefficients exactly, and zeroes every other coefficient. Neither the borrowed descriptor
nor its source table is modified. This applies to every native brush size, including fractional-fill boundaries,
without implementing an approximation to the game's geometry.

## Mining yields remain native

On the client the terrain helper writes removed volume into a per-material float report. The impact passes that report
to `0x492980`, together with the coefficient-derived rates from `0x491ee0`. The yield helper calls `0x4a6f40` using
per-material state at building context +0x40 (stride 12), additional state at +0x800 and the native material exchange
mapping. It then resolves the resulting item and appends the item/count to the game's transaction. This is not merely
a count of changed cells; a standalone cell writer plus item grants would bypass the native yield accounting.

Kiln therefore leaves the original terrain report pointer, impact completion, yield calculation, transaction and history
path in place. Only the coefficient span is filtered and the building branch skipped. `Build.Dig` and `Build.Write`
are not called by this feature and retain their existing semantics. Props/entities are not inspected or destroyed by
these two hooks. The enclosing tool action must still ensure that only the qualified generic terrain impact is run.

## Qualification gates

The tools service installs both hooks through `kiln.hooks` and reports them separately to its policy gate. Installation
requires one of the two exact executable identities, an enabled verified-build hook policy, both resolved symbols,
and matching entry bytes for both functions. A failed second installation revokes the first link. The action service
must not activate Remove until both hooks and its required access checks are ready. A malformed/unreadable brush under
an active Remove policy refuses rather than continuing an unrestricted kernel. No-policy and Add invocations call their
original continuations with their original arguments and return values.

Unit/native fixtures validate filtering and continuation behaviour; they do not prove real game brush geometry,
multiplayer authority, inventory yield, saved history or entity behaviour. Those need the owner-guided client check.
