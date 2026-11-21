<!-- SPDX-License-Identifier: MIT -->
# kiln.eml.archive v1

**Kiln-internal:** the seam of Kiln's interpreter for ShroudForge mods into kiln-eml's startup pass. The header is not in
the SDK and is no mod API.

This is a **declaration-only contract**, not a claim that an archive provider or any ShroudForge API has parity. GPL kiln-eml implements it (its archive provider). Native and MIT consumers use neutral caller-owned records through the table; they do not link GPL code. The mutable graph and protected Lua-object reader have a different service, `kiln.eml.values` ([eml-values.md](eml-values.md)). Neither service exports physical archive publication authority.

## Discovery and required subsets

The early export is `const KilnEmlArchiveApiV1* KILN_CALL kiln_eml_archive_service(uint32_t min_version)`. Resolve it from the already loaded kiln-eml module. Versions 0/1 return the process-lifetime immutable table; versions above 1 return null. Later `KilnApi.service("kiln.eml.archive", 1)` returns **that same address**. Getter/export presence alone grants no view. kiln-eml's provider tests test the actual export and registration; the header's host test uses a fake getter only.

The table is **18 pointers / 152 bytes** on x64. The first four offsets still occupy 40 bytes, but the error-channel amendment changes `resource_json` and twelve other function typedefs. **There is no binary-call compatibility claim with the earlier 16-slot draft.** Regenerate/recompile every early getter, provider, runtime fake and consumer against this header in the same change. Only acquire/identity/release call shapes remain unchanged; the existing real `kiln.eml` v1/v2 interface is untouched. A registry-only consumer needs only acquire, identity, release and registry_metadata, through byte **48**; it must not require resource_json, the rest of the table, codecs, attributes or the values service. Before every pointer access, check header.version, member extent, non-null, group and member capability facts. `KILN_EML_ARCHIVE_HAS(api, member)` implements **only the structural checks**, short-circuiting before reading the tail. Use a stable table pointer expression. Early consumers use the support facts kiln-eml's startup pass supplies before normal caps publication, never a guessed full-service guarantee.

Five independent capability groups each guarantee their **entire named subset**, not every slot in the table:

| Group row | Required members |
|---|---|
| `eml.archive.metadata` | acquire, identity, release, registry_metadata |
| `eml.archive.read` | resource_json, find_resource, list_resources, read_resource, content_read_range, list_contents |
| `eml.archive.codec` | value_decode, value_encode, loaded_field |
| `eml.archive.attributes` | attribute_catalog, attribute_resolve, attribute_evaluate |
| `eml.archive.errors` | error_copy, error_release (retained immutable diagnostic channel) |

A group is AVAILABLE only when **every** member it names is implemented and can succeed for supported inputs. A provider may publish `eml.archive.metadata` and its four member rows while `eml.archive.read` stays UNAVAILABLE until all six resource/content members work. Partial codec and attribute implementations likewise keep their group UNAVAILABLE. A provider must not advertise a partial group's availability using descriptive text or per-member rows. Every implemented member additionally reports `<group>.<member>`; those facts supplement, never replace or weaken, the complete-group guarantee. Consumers retain group/member, extent, version and non-null checks. Group policy holds and missing member rows refuse use; a full table size does not prove support. An available group does not waive input, generation or operation-specific refusal checks.

For example, a native EMF consumer reading reflection checks `eml.archive.metadata`, its four member rows and `KILN_EML_ARCHIVE_HAS` for each required member. It acquires using the pins kiln-eml's startup pass provides, requests registry pages into its own buffers, and releases the token. It works with the 48-byte metadata subset even if resource_json and all later resource/codec/attribute slots are null. A resource consumer additionally checks `eml.archive.read` and the relevant resource member rows. Neither needs a Lua object or mutable graph.

## Pins, records, lifetimes and errors

`KilnEmlArchiveIdentityV1` is the **single canonical 160-byte record**. The old 120-byte design was deliberately extended with nonzero process-unique, non-pointer registry_id at 120 and registry_sha256 at 128: a view pins its linked immutable registry, not just an archive digest. Values and every other consumer include this header rather than redeclaring that record. Its embedding makes the current values identity 208 bytes and value info 248 bytes; the historical standalone 120/168/208 fixture is incompatible. The public-header duplicate-record regression rejects a second definition or forward copy. The launch/plan ids are 16-byte RFC 4122 order UUIDs; asset GUIDs remain 16 game-order bytes. The acquire owner and every expected pin must match an authorized immutable generation. No caller-supplied path or wildcard identity is accepted. Runtime grants select PUBLISHED; a PREPARED grant needs the startup pass's authorization. Registry pages, resources, codec selections and attribute caches all refer to that same generation, not the latest process-global registry.

Other new records are caller-sized `KilnArchiveQuery` (48), `KilnArchiveResourceInfo` (64) and `KilnFieldSelection` (64). Each has schema_version 1. Larger caller records retain their unknown suffix untouched; shorter records, unknown schema/flags/enums and nonzero reserved fields refuse. Existing `KilnAssetKey` and `KilnContentHash` retain their original fixed strides (24 and 16). Their size-like fields are not new record headers.

All calls are category D, both sides, any-thread immutable-data work; IO occurs on the caller, not by entering Lua or waiting on a world thread. This is not permission to perform blocking archive IO in an engine callback. No game pointer is accepted or returned. A call in flight pins backing storage. Release/revoke prevents new calls but does not invalidate an already pinned copy. Tokens are never reused; owner revocation closes all its grants. Release is idempotent for issued tokens (even revoked ones), unknown tokens are invalid, stale reads are revoked. Prefix calls use possession of the issued owner's token as read/release authority; owner-taking tail calls additionally require the matching owner. Release never waits on the interpreter.

Buffers are caller-owned. Byte/string replies count bytes without a NUL; list replies count elements at the declared fixed stride. A short buffer writes **nothing** except the full required count, consumes nothing and returns BUFFER_TOO_SMALL. Attribute evaluation also leaves the entire output_bits array untouched if its trace does not fit. Other failures leave data outputs unchanged; the new valid ErrorOut is the explicit exception and is initialized on every return. Inputs, data/error outputs and required pointers must not overlap. Zero-length data may be null. There is no new global size budget silently truncating legitimate data; `contents.read`'s Lua 32 MiB/four-job policy belongs to its wrapper, not this raw range API.

Missing resources/contents/types return NOT_FOUND; inaccessible phase/policy/provider, unsupported types and ambiguous selectors return UNAVAILABLE; malformed queries/records/trees, mismatched typed values and checked-arithmetic failures return INVALID_ARGUMENT; retired grants return REVOKED; contained provider faults return INTERNAL. Existing KilnResult numbers are unchanged. Those codes are **not detailed Source error text**; never synthesize precise IO/codec reasons from a generic code. Source-specific result/raise/prefix mapping stays in the translator, while the original provider cause is preserved through the per-call channel below.

### Per-call immutable diagnostics

All thirteen fallible data methods have a nullable trailing `KilnArchiveErrorV1*`: resource_json, registry_metadata, find/list/read_resource, content_read_range, list_contents, value_decode/encode, loaded_field, and all three attribute methods. Its 32-byte layout is size@0, result@4, token@8, message_bytes@16, flags@24, reserved@28. Validate its size/flags/reserved and buffer overlap **before any lookup or IO**, initialize the complete valid v1 output every path, and leave its larger unknown suffix alone. An invalid short record cannot trigger work. NULL is an explicit native opt-out, not precise-error parity.

A real failure captures its original complete counted UTF-8 message/cause once and returns a fresh token plus length, including raw output capacity zero and source count-zero reads. Embedded NUL and arbitrarily long diagnostics survive; no terminator, truncation, global/TLS last-error or second lookup/IO/getter/codec to recover text. Success-zero and ordinary capacity-only refusal have token/length zero. `error_copy(owner, token, out, capacity, required)` is pure immutable probe/copy: short copies report full required, write nothing, consume nothing and do not overwrite the original result. `error_release(owner, token)` is idempotent for that owner's issued tokens (also after retirement); unknown/zero is INVALID_ARGUMENT and foreign owner REVOKED.

The issued view and **transport-root owner**, not a creating package, live until capture/copy/release completes. Calls already pinned survive close; view/owner retirement rejects new operations and cleans retained transport state. Kiln's interpreter copies the diagnostic into owned Rust done state and releases the native token before transport close, so another mod's latent same-VM job reference survives its creator's unload. Providers must prove copy/release/view/owner overlap safety.

Known-length raw/content capacity probes obtain required from index metadata without actual IO. Capacity zero with required zero is a **real operation**, including Source open/seek/validation, and can fail. A read-only view does not freeze files another DLL or Steam may modify. Unknown-size resource_json/attribute replies must use owned materialization cached for that pinned generation before sizing/copy, with actual preparation/pinning proof; stateless codec probes use the same owned input snapshot. Never replay live Lua getters; that path uses the separate values reader. Count-zero and size-probe rules are distinct, not an `out==NULL` shortcut.

Precise SF callers require `eml.archive.errors`, both error-method extent/non-null checks, their family/group/member facts **and actual source-cause emission/roundtrip proof**. The channel row alone never means all family errors are precise. Native NULL opt-out consumers may use family rows without claiming original causes.

List query flags conjunctively select GUID/type/part. Catalog order preserves source enumeration; part order requires GUID+type and sorts by part. `first` indexes the filtered sequence; a nonzero query limit selects at most that many elements, while zero selects the full suffix. Content listing returns the full suffix. Capacity never silently becomes a page limit. `first==total` succeeds empty; `first>total`, first+limit overflow and element-byte-count overflow refuse. Metadata limit zero requests header metadata only. Content ranges read exactly count bytes with start<=content.size and count<=content.size-start, including zero at EOF. Find/read include finalized transaction entries; list and content membership use the reference catalog, preserving their distinct source visibility.

`resource_json` accepts kiln.boot's resource/item/template/skill/material query forms, but its result is the **new versioned lossless envelope**, not the old untagged boot snapshot JSON. kiln.boot's reader translates it explicitly. Missing named indexes refuse rather than silently falling back to another generation.

kiln.content is the second consumer of that translation: in a kiln-backend start the resource patches collector checks a boot generation's patches before the entry point against the same committed generation, through its own `CommittedView` with kiln.boot's binding rules (`native/services/content/gamedata_bootview_eml.h`; the identity is `kiln.eml.startup`'s session-free `published_identity`, through kiln.boot's own call; without it the start checks the patches when they are written, as a hosted start does). It reads with kiln.content's owner, checks the same six kiln.caps rows, binds at the first prepare and releases the view when the game's resources are up. It refuses nothing yet: a refusal needs the source to say its view of a resource is final for this start (nothing else in the start creates or overrides it), and this source gives no such answer, so the patches are checked when they are written.

## JSON and binary schemas

Every JSON argument/reply has schema_version 1 and a checked-in schema:

| File under `schema/sfi/eml-archive/` | Payload |
|---|---|
| `common.schema.json` | Shared identity pins, tagged values, complete ordered reflected Type/field/attribute metadata |
| `registry.schema.json` | registry_metadata: selected game version, registry version/count, page start and ordered types |
| `resource.schema.json` | resource_json: pins, resourceId/typeHash/part/type_index, neutral value |
| `attribute-catalog.schema.json` | attribute_catalog: pins, ordered entries and neutral layout descriptors |
| `attribute-selector.schema.json` | attribute_resolve input: name or stored id, exclusively |
| `attribute-resolve.schema.json` | attribute_resolve output: pins and a complete entry |
| `attribute-evaluate.schema.json` | attribute_evaluate: pins, root id, exact u32 input/output bits, ordered trace |

`registry_metadata.game_version` is the original selected archive version text, and `registry_version` is the registry's original version text. Both are unrestricted JSON strings, not tagged integers or executable build numbers. Preserve them independently without parsing, normalization or requiring equality (both pins: `lua/bindings/game/mod.rs:13-19`, `types.rs:42-44`, `reflection/load_type_registry.rs:71-111`). Counts, page starts, indices and generations remain tagged integer quantities.

A catalog entry's neutral `descriptor` carries **all** source descriptor members, including calculation words, definition ownership, root indices and support/refusal metadata. `layouts` and trace use the same ordered tagged tree, not parser structures. Wrappers must retain original descriptor keys/order rather than reconstructing them from names or hashes. The explicit identity fields in the entry are convenient indexes, not replacements for the complete descriptor. `scalar_type` mirrors the descriptor's resolved scalar type: a qualified name or null when no unique supported scalar is resolved. Catalog and descriptor lookup still succeed with null and retain the original scalar hash and support/refusal metadata; only an operation actually requiring a resolved scalar may refuse (both pins: `reflection/build_attribute_catalog.rs:427-442,522-550,576-587`, `lua/ingame/ecs/attributes/read_attribute_catalog.rs:10-18`, `read_attribute_descriptor.rs:9-13`). `metadata.json` supplies synthetic unresolved/ambiguous entries for both catalog and resolve validation, plus distinct nonnumeric and numeric-looking version strings. Semantic consistency (pin equality, page counts/indices, descriptor identity and input/output lengths) is validated by the provider, not expressible by these shape schemas alone.

64-bit quantities use exactly 16 lowercase hexadecimal digits after `u64:` or `i64:` (the latter is a two's-complement bit pattern). Floats use raw `f32:` + 8 or `f64:` + 16 hexadecimal bits, preserving NaNs and signed zero. GUID strings use `guid:` + 32 hex digits in game byte order; bytes use `bytes:` + even-length hex, including empty; Lua strings use `string:` + even-length hex, so NUL/non-UTF8 data survives. Ordinary 32-bit integers remain bounded JSON integers. Metadata's original index and four distinct hash domains are never collapsed; absent default_bytes is null, not an inferred all-zero default. Arrays preserve registry/map/field/enum/attribute order and duplicate-name/hash ambiguity.

The **binary** value_decode/value_encode transport is specified in `KilnArchiveValueRecordV1` in the header: a 48-byte little-endian size/version/tag/length prefix, optional qualified type bytes, payload and recursively counted children. It has no native pointers, implicit padding or parser types. It covers nil, bool, i64/u64, f32/f64 bits, GUID, bytes, binary strings, arrays, ordered maps, variant discriminants, optional presence and references by GUID. Strict arity/length checks reject malformed nodes; the root consumes the entire packet. Binary serialization is not a C struct memcpy on arbitrary alignment. `trees.json` contains independent synthetic positive packets for every tag; the test mutates every prefix/version/length field and truncates packets. JSON schema fixtures also include all value tags and precision edge cases.

These stateless trees do not establish persistent aliases, metatable traversal, dirty-state or live engine-write parity. Those require the separate values service and actual GPL/runtime integration.

## Source-backed successful cases, not implementation claims

The following paths are under `modules/mod-runtime/` in **both** SF pins d8a394bc (1.7.9) and 38ef448d (1.8.0), except the named Kiln source. These are direct source evidence that the members have useful successful cases on supported builds, not oracle or engine execution. None of the original sixteen data slots is failure-only by design. The two new error transport methods have ordinary successful capture/copy/release use cases; their token protocol is Kiln-owned, not another game operation.

| Members | Successful path / evidence |
|---|---|
| acquire, identity, release | `session/create_mod_session.rs:91-163`: selected immutable archive/registry session; the scoped token protocol is Kiln-owned, not a copied SF ABI |
| registry_metadata | `lua/bindings/game/types.rs:42-44,245-322,374-505`: versions, original indices, defaults and ordered metadata; `lua/bindings/game/mod.rs:13-22`: selected archive game version |
| resource_json | neutral tree from the read/codec paths below; existing boot envelope/query consumer: `native/core/src/services/boot_gamedata.h:262-316` (Kiln) |
| find_resource, list_resources, read_resource | `lua/bindings/game/assets/access_file_resources.rs:2-121`; `session/asset_resources.rs` resource lookup/read; part sorting follows the frozen documented-order correction, not incidental catalog order |
| content_read_range, list_contents | `lua/bindings/game/assets/access_file_content.rs:2-28`; `lua/bindings/game/assets/content.rs:22-54`: original content membership/read |
| value_decode, value_encode | `lua/values/decode_owned_value.rs:10-37`, `encode_owned_value.rs:9-25`: scalar and structured copied-value success, no engine allocation needed for supported types |
| loaded_field | `lua/values/resolve_loaded_field.rs:7-47`: scalar selection from an owned complete snapshot; no live pointer traversal |
| attribute_catalog, attribute_resolve | `reflection/build_attribute_catalog.rs:270-278,321-350,550-566`: load actual metadata and resolve an unambiguous stored id/name/root |
| attribute_evaluate | `reflection/build_attribute_catalog.rs:571-603`: complete verified root copied into results, with an ordered trace; transport does not add an engine dependency |

The authority is FINAL `eml-assets-types` (202 inventory rows, not test count), with delegated runtime attributes supplied by the attribute provider. Header tests promote **no** API parity rows. GPL/runtime providers must test their actual shape, semantic, error, owner/revoke and concurrent-copy/retirement implementations; source-derived claims are not differential-oracle observations.

## kiln-eml's attribute group

kiln-eml's provider implements the metadata, read, attribute and error groups; the codec slots are null and
`eml.archive.codec` is reported unavailable. Its attribute group answers from ShroudForge's attribute catalog of the
generation's **reference** archive (the same catalog the startup pass's `runtime.ecs.get_attributes` reads), built once
per generation on the first attribute call and kept with it, its failure too:

- `attribute_catalog`: every entry in attribute-ID order, then the reflected component layouts. An entry's
  `descriptor` is the whole source descriptor as a tagged tree (its keys in source order); `definition_guid` is the
  definition GUID of the entry's one associated component, null without exactly one.
- `attribute_resolve`: one entry for `{"schema_version":1,"name":...}` or `{"schema_version":1,"id":...}`. A missing ID
  or name is NOT_FOUND, an ambiguous name UNAVAILABLE, each with the source's text; any other selector is
  INVALID_ARGUMENT before any lookup.
- `attribute_evaluate`: the source's recalculation of one complete root (`count` must be the root's length): the output
  bits and the ordered trace. An incomplete root is NOT_FOUND, another count INVALID_ARGUMENT, an unverified operation
  enum or unsupported scalar UNAVAILABLE, a program that fails on the given bits INVALID_ARGUMENT, each with the
  source's text.

A catalog that cannot be built answers UNAVAILABLE with its source text to all three members.

## Checks

Run each command with its own output, temp and target folders:

- `bash native/core/tests/test-eml-archive.sh`: rebuilt C11/C++20 caller/early-getter probes, Windows x64 C/C++ compile-only layouts, three canonical-record tests, and separate ASan/UBSan + TSan reference-provider error probes (including copy in flight versus close/view retirement). These reference probes are NOT the GPL IO provider or Source message parity; kiln-eml's provider tests repeat the scenarios on the actual implementation.
- `node schema/tests/sfi_eml_archive.mjs`: schemas, precision/shape negatives and binary transport packets; uses installed launcher AJV, no download.
- `python3 -I tools/sfi/abigen/generate.py --check` and `test_abigen.py`: independent header parsing and regenerate-and-diff.
- `cargo test --manifest-path crates/sfi/Cargo.toml -p kiln-abi --locked --offline`: all generated Rust layout assertions and consumer/callback tests.

Actual early-export identity, capability registration, generation authorization and revoke/copy races belong to kiln-eml's provider tests. The header itself creates no engine or concurrent service implementation and needs no running-game assumption. kiln.boot's same-generation OnBoot checks and the backend-switch checks still apply.
