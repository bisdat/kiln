<!-- SPDX-License-Identifier: MIT -->
# kiln.eml.values v1

**Kiln-internal:** the typed-value seam of Kiln's interpreter for ShroudForge mods. The header is not in the SDK and is
no mod API.

The header is declarations only. GPL kiln-eml implements it: the typed graph and its packets in Rust
(`gpl/kiln-eml/kiln-eml/src/archive_values/`), and the C table, its registration and the protected reader adapter
(`archive_values/abi.rs` and `reader.rs`; see "The provider" below). MIT kiln-sfi consumes it through neutral records,
and the startup pass uses the same graph in-process. Nothing here claims ShroudForge API parity. The immutable archive
reads and the stateless codecs stay in [`kiln.eml.archive`](eml-archive.md); this service holds only the mutable,
VM-local graph and the protected Lua-object reader.

Header: `native/core/abi/services/kiln_eml_values.h`. It is the reviewed design draft (sha256 `a4ebd80d…`) with two
edits only: the Kiln-internal note and a sibling `#include "kiln_eml_archive.h"`. The test
`native/core/tests/abi_headers/eml_values_identity.py` reverses those two edits and checks the draft hash, so any
other change to the header is a reviewed change of that pin.

## Table and discovery

`KilnEmlValuesApiV1` is a separate service, `KILN_EML_VALUES_SERVICE` = `"kiln.eml.values"`, version 1: an 8-byte
header plus **17 pointers = 144 bytes** on x64. It has normal service lookup only; there is no early-boot values
export. Category D, worker-local: graph operations run on the creator interpreter worker.

| Offset | Member | Offset | Member |
|---|---|---|---|
| 8 | arena_create | 80 | value_export |
| 16 | arena_close | 88 | value_close |
| 24 | arena_collect | 96 | value_result_copy |
| 32 | value_open | 104 | value_array_push |
| 40 | value_info | 112 | value_array_insert |
| 48 | value_read | 120 | value_array_pop |
| 56 | value_child | 128 | value_array_remove |
| 64 | value_set | 136 | value_array_clear |
| 72 | value_clone | | |

A consumer checks header.version, the member's extent (`header.size >= offset + 8`) and non-null before each call.
The header defines no capability rows. kiln-eml reports `eml.values` and one row per member (`eml.values.arena_create`
... `eml.values.value_array_clear`), all AVAILABLE, once the table is registered: static support only (an arena still
needs an authorized generation that keeps its registry; see below).

## Records

| Record | Bytes | Notes |
|---|---|---|
| `KilnEmlValueIdentityV1` | 208 | Embeds the canonical 160-byte `KilnEmlArchiveIdentityV1` at 48, included from `kiln_eml_archive.h`, never redeclared |
| `KilnEmlValueInfoV1` | 248 | `requested` bits gate `logical_len`, `source_len` and the DIRTY flag; `requested = 0` is identity/class only, never false-clean evidence |
| `KilnEmlValueSelectorV1` | 32 | FIELD (KilnStr) or INDEX (u64, Source's usize conversion already done) |
| `KilnEmlValueResultV1` | 48 | Fixed reply: disposition, MUTATED flag, Lua arity, item/payload/error leases, receiver revision |
| `KilnEmlValueOpenV1` | 88 | RESOURCE, BYTES, LUA_INPUT, LOADED_SNAPSHOT or DEFAULT origin; bytes are copied |
| `KilnEmlLuaAccessV1` | 56 | One protected reader request |
| `KilnEmlLuaValueV1` | 56 | One protected reader reply: Lua kind, object token, bits, length, typed value, error, adapter class, lease disposition |
| `KilnEmlLuaReaderV1` | 56 | Host callback table: access, copy, retain, release, wake |

Every enum value in the header is ABI (value kinds, selector kinds, dispositions, info request bits and flags, read
queries, open origins, reader operations, Lua kinds, adapter classes, lease dispositions). The native probe pins each
one.

## Lifetimes and authority

- **Tokens** are unforgeable process-unique ids, never pointers and never reused. A node or lease belongs to the
  VM/phase arena, not to a mod owner. `owner` and `creator_owner` are attribution only: a package's retirement does not
  affect any token, and another mod's first access through a Lua reference it already holds still succeeds. There is
  no grant or transfer API; possession in the same VM is the authority, and the MIT frontend never carries a token to
  another VM.
- `arena_create` returns a private creator-host **control** token, distinct from `identity.arena`. Only `arena_close`
  revokes all leases; `arena_collect` drains on the worker. VM teardown closes and drains the arena first.
- `value_close` closes any lease kind (value, iterator, result/payload, error). It is idempotent for an issued token;
  a never-issued token is NOT_FOUND. A returned item lease belongs to the caller; payload and error leases are
  separate and caller-owned.
- **Threads.** create/open/info/read/child/set/clone/export, the array operations and collect run on the creator
  worker only; no world, render or window thread waits for them. close and arena_close are any-thread and
  non-blocking, and never run Lua or free worker-owned graph off the worker.
- **PATCH.** Local edits succeed with PATCH=false. Resource.data assignment, create_resource/content and the startup pass's
  writer keep their own gates; no mode enrolls a stage, patches, writes the archive or swaps a live resource.
- No Lua longjmp, C++ exception or Rust panic crosses the C boundary.

## The protected reader

Before **every** callback, the provider checks `reader.size >= offsetof(member) + sizeof(member)`, `version == 1`
and `member != NULL`. It never reads a pointer past a truncated table, not even one used only for failure cleanup.
It snapshots the admitted table and keeps the host `user` context alive until the arena's pending count reaches 0.
`wake` is an any-thread signal and runs no Lua.

`access` executes each get/length/stringify exactly once per request on the creator worker. MAKE_TYPED_PROXY passes a
provisional lease plus the neutral arena/node identity. A new proxy ADOPTS that lease (its `__gc` closes it once); a
reused proxy returns REUSED and GPL closes the provisional lease. Frontend interning is a weak table. A GC during any
reader callback may re-enter `value_close` for another proxy; that close is queued and drained once after the outer
call, never applied under it.

## The provider (kiln-eml)

kiln-eml registers the table as `kiln.eml.values` in its plugin start, right after `kiln.eml.archive`, through the
core's `register_service` (normal lookup only), then reports the rows above. The table is one process-lifetime static.

**Records.** Every record a member reads or writes must be non-null with `size` at least its v1 size (open 88, info 248,
selector 32, result 48); a smaller one is INVALID_ARGUMENT before any reader call or effect. A larger one is accepted:
its `size` is kept and the bytes past v1 are untouched. A valid result record is cleared (every v1 field 0) before any
work, so a refused call (one refused inside `wake` included) never leaves a token in it; `arena_create` writes 0 to its
token output first. A `KilnStr` or byte range with a null pointer is valid only with length 0, and any count above
`isize::MAX` is INVALID_ARGUMENT. `value_result_copy` needs `required`; a null buffer only with capacity 0; buffer and
`required` must not overlap; a short buffer is BUFFER_TOO_SMALL with `required` set and nothing run again. `pending`
outputs may be null.
Owner arguments are attribution only, except that `arena_create`'s host owner must be nonzero.

**arena_create** resolves the archive view (unknown INVALID_ARGUMENT, released or retired REVOKED) to its authorized
generation. kiln-eml's startup pass authorizes a generation together with the kfc registry its snapshot was built from
(`archive_service::authorize_with_registry`); `arena_create` copies that registry once for the arena, on the calling
thread (the arena's worker), and checks the copy before anything else exists: its registry digest (kiln.eml.archive's
`registry_sha256` definition) must equal the view's `registry_sha256`. The view's `registry_id` is bound by
construction rather than compared: it is the id of the snapshot kept in the same generation entry as the registry, so
the content digest is the one check that can fail. A mismatch, or a generation authorized without a registry, is
UNAVAILABLE: no arena, no reader call. No other registry is ever used (not `kiln.types`, never a
rebuild from the JSON snapshot). `identity.layout_revision` is `registry_id`. The arena keeps its copy after the view or
the generation goes; `value_open` needs a live owner view of the same generation (another generation's view is
INVALID_ARGUMENT, a released one REVOKED). RESOURCE roots read the generation's archive through the archive's read
group (the transaction first, then the reference catalog, as `find_resource`/`read_resource`); a missing resource's root
is nil. Over a generation the startup pass authorized without its archive, `value_open` RESOURCE answers UNAVAILABLE.

**Reader admission.** `arena_create` reads the reader's `size` first and never a byte past it, and keeps a snapshot of
the covered prefix (`min(size, 56)`), so the host may reuse its table's memory afterwards. Admitted: `version == 1`,
`user` covered, and `access`, `copy`, `retain` and `release` covered and non-null; otherwise INVALID_ARGUMENT. `wake`
is optional: without it nothing signals the host, which then calls `arena_collect` on its own schedule. Every callback
is checked against the snapshot again before it is called. Every callback returns: a Lua error is caught by a
protected call inside it and answered as `error_object`, and a C++ exception or panic is caught there too; a longjmp or
unwind out of a callback crosses the provider's frames and is undefined behaviour. Request pointers (`key`) and the
request and reply records are valid only during the call. A non-OK KilnResult from a callback, or a reply whose kind,
adapter class or lease disposition is not a header value, is a transport failure (the root `internal` class). `copy`
is called with a small buffer first and once more at the exact size it asked for; a size that changes between the two
is refused.

**wake** is `wake(user, control)`: the control token `arena_create` returned (the one `arena_close`/`arena_collect`
take). It runs on the thread that closed something off the worker, under the arena's internal lock, so it may only
signal (set an event, post to the worker): no Lua, no kiln.eml.values call, and no waiting for the worker or for a lock
the worker may hold while it calls a member (that deadlocks against `arena_collect`). Every member called from inside
`wake` answers WRONG_THREAD instead of deadlocking, with its outputs cleared. Once a closed arena is torn down and pending is
0, nothing calls `wake` (or any callback) again.

**Host duties.**
- Close and drain every arena (`arena_close`, then `arena_collect` on the worker until pending is 0) before destroying
  the VM and before the worker thread exits, and keep the reader's `user` valid until then. A worker that exits with an
  arena still open revokes it as its thread-local state goes: every token is revoked (later closes are idempotent,
  `arena_close`/`arena_collect` answer REVOKED), the waker is dropped, and the graph is dropped without a single host
  call (what the host retained for the graph dies with its VM).
- **LOADED_SNAPSHOT roots are held weakly, with no pin.** Source keeps a loaded root's Lua value on that ResourceInfo
  instance (its value cache), so the frontend keeps the root's userdata (the proxy owning the item lease) on its own
  ResourceInfo object. While that ResourceInfo lives, re-opening the same instance returns the same node with its edits;
  once it is collected, the root and everything pinned under it are freed, and a later open decodes the bytes afresh.
  RESOURCE roots stay pinned by the arena (the shared ResourceInfo of the phase).
- SOURCE_TO_STRING answers mlua 0.11.6's `Value::to_string` of the object (see "What the reader copies and answers").

## Payloads and errors (frozen, schema_version 1)

The packets `value_result_copy` returns, and what each operation's fixed `KilnEmlValueResultV1` means, are frozen by the
schemas in `schema/sfi/eml-values/` (`$id` `eml-values/<file>`; fixtures in `fixtures/`, checked by
`schema/tests/sfi_eml_values.mjs`). Every JSON object is closed and every packet carries `schema_version: 1` and a
`packet` discriminator; any new field, class, kind or tag is version 2, and a reader refuses versions it does not know.
The GPL graph (`gpl/kiln-eml/kiln-eml/src/archive_values/`) writes them; its test `m6_value_graph.rs` compares its
output with the fixtures.

### Replies per operation

| Operation | disposition | arity | item | payload | MUTATED |
|---|---|---|---|---|---|
| value_open | HANDLED | 1 | the root (a scalar root: a scalar node) | VIEW of a scalar root, else 0 | no |
| value_child | HANDLED | 1 | a typed child; **0 for a scalar** | VIEW of a scalar child (nil included), else 0 | no |
| value_set | HANDLED | 0 | 0 | 0 | yes |
| value_clone | HANDLED | 1 | the clone (a scalar node for a scalar) | VIEW of a scalar, else 0 | no |
| value_export | HANDLED | 1 | 0 | raw bytes | no |
| value_read VIEW / LENGTH / DIRTY / STRINGIFY | HANDLED | 1 | 0 | the packet (STRINGIFY: raw bytes) | no |
| value_read SOURCE_BYTES | HANDLED | 1 | 0 | raw bytes; 0 for Source's None | no |
| value_read ITER_BEGIN | HANDLED | 3 | the iterator | 0 | no |
| value_read ITER_NEXT (iterator lease) | HANDLED | 2 | the value when typed, else 0 | ITER_NEXT | no |
| push, insert, clear | HANDLED | 1 (nil) | 0 | 0 | yes (clear also on an empty array) |
| pop, remove that removed | HANDLED | 1 | the removed node (a nonzero **nil node** for a removed nil) | VIEW of a scalar | yes |
| pop, remove that removed nothing (0, len + 1, empty pop) | HANDLED | 1 (nil) | 0 | 0 | no |
| an array operation on a live non-array value | UNSUPPORTED | 0 | 0 | 0 | no |
| any Source error | SEMANTIC_ERROR | 0 | 0 | 0 (`error` is the error lease) | **never** |

- Every returned lease is issued before the operation's own edit, and Source never errors after its edit, so a
  SEMANTIC_ERROR leaves the receiver as a plain read would (a mapped child that the read cached may stay). An edit that a
  callback made during validation shows only in `receiver_revision` (the receiver's edit count).
- **Transport refusals** (KilnResult, never a packet): a never-issued token (NOT_FOUND), a closed lease or arena
  (REVOKED), a worker-only call off the creator worker (WRONG_THREAD), and INVALID_ARGUMENT for a wrong-kind token, a
  FIELD selector on an array, an INDEX selector on a struct or variant, a variant field other than `type`/`value` (only
  `value` has a setter), LENGTH/STRINGIFY/ITER_BEGIN on a variant, scalar, iterator or packet lease, ITER_NEXT on
  anything but an iterator, DIRTY/SOURCE_BYTES on an iterator or packet, and `value_result_copy` of a value or iterator
  lease. A refused request makes no reader call and has no effect.
- **Raw packets** (STRINGIFY, SOURCE_BYTES, value_export) are counted bytes with no envelope, known by the query that
  made them; embedded NULs and non-UTF-8 bytes are kept. STRINGIFY is Source's `__tostring` text: each value through
  mlua 0.11.6's `Value::to_string` (a number in Rust's `Display`, such as `1` or `0.30000000000000004`, not Lua's
  `%.14g`; a string that is not UTF-8 is mlua's conversion error, a SEMANTIC_ERROR of class `runtime`). SOURCE_BYTES
  payload 0 is Source's None; a zero-length packet is `Some(empty)`.
- **Iteration:** owned structs walk the type's fields (inherited first), mapped structs the mapped keys, owned arrays the
  current length, mapped arrays the ORIGINAL length through the live cache, so after a removal they yield `(k, nil)`,
  which is not the end. The end is key nil, value nil. ITER_BEGIN (`__pairs`) borrows the value shared; an ITER_NEXT
  step borrows nothing, because Source's iterator holds the value's cache and not its userdata, so a step works even
  while an array operation on that value is validating its argument.
- **Scalars from value_child** come back as item 0 plus a VIEW payload: Source scalars have no identity, so no lease is
  spent on them. ITER_NEXT carries scalar values inline and `{"tag":"item"}` for a typed value (the reply's item).

### JSON packets

A Lua scalar (`lua_value`) is the tagged value of the interpreter's packets restricted to Lua's scalars, with no `type` key:
`{"tag":"nil"}`, `{"tag":"bool","value":true}`, `{"tag":"i64","value":"i64:<16 hex, two's complement>"}`,
`{"tag":"f64","value":"f64:<16 hex of the IEEE bits>"}` (NaN payloads and -0 kept), `{"tag":"string","value":"string:<hex>"}`
(the Lua string's bytes). A type is `{"index":"u64:..","qualified_name":".."}`: the registry type's own qualified name.

| Packet | Shape |
|---|---|
| `view` | `value`: a lua_value; or `{"tag":"typed","class":"struct"/"array"/"variant","storage":"owned"/"mapped","type":<type>}` with `"variant_type":<type>` (the selected type) for a variant only; or `{"tag":"iterator"}` |
| `length` | `value`: `i64:` (Source's `#`; a mapped array's is its current length as `i32`) |
| `dirty` | `value`: boolean (Source's recursive `is_dirty`) |
| `iter_next` | `key`: lua_value; `value`: lua_value or `{"tag":"item"}` |
| `error` | `error`: the error tree's root |

### The error tree

Each level is `{"class":..,"message":"bytes:..",<class fields>}`. `message` is the complete Source Display text of that
level (any bytes). Lua-derived or arbitrary texts are `bytes:` (`message`, `found`, `invalid_property.key`,
`invalid_type.name`, `invalid_guid.guid`); registry- or Rust-formatted names are plain strings (`expected`, `field`,
`type`, `path`: `.` at the root, dotted field names and zero-based indices).

| class | Fields | Raised again by the frontend as |
|---|---|---|
| `runtime` | none | `Error::external(message)` |
| `position_out_of_bounds` | `index`, `start`, `end` (`u64:`) | `Error::external(message)` |
| `static_array` | none | `Error::external(message)` |
| `invalid_property` | `key` (bytes), `type` | `Error::external(message)` |
| `borrow` | `mode`: `shared` / `mutable` | mlua's `UserDataBorrowError` / `UserDataBorrowMutError` |
| `validation` | `path`, `kind` and its fields (below) | `Error::external(message)` |
| `conversion` | `path`, `kind`: `incompatible_type`, `invalid_guid`, `lua` or `mapping` | `Error::external(message)` |
| `lua` | `host_error` (optional, nonzero `u64:`) | the retained original error (`Error::external(message)` when `host_error` is absent) |
| `internal` | none; **root only** | Kiln's own error, logged, never presented as Source text |

Validation kinds: `incompatible_type` (`expected`, and `found` or `found_userdata`), `missing_field` (`field`, `type`),
`invalid_type` (`name`), `invalid_guid` (`guid`), `lua` (then `cause`: the next level, any class but `internal`) and
`mapping`. The class list and the kind lists are closed.

- **`found_userdata`** replaces `found` where Source prints a userdata's Rust TypeId (`userdata \`TypeId(..)\``):
  `{"lua_kind":"typed_value"|"known_userdata"|"other_userdata","adapter":"none"|"guid"|"content"|"resource"}`, plus
  `value_kind` (`struct_owned` ... `variant_mapped`) for a typed value. GPL invents no TypeId text: its `message` has
  the placeholder `userdata \`<frontend TypeId>\`` there, and `<frontend type name>` where Source's
  `guid expected, got ..` prints a userdata, function or thread; the frontend renders both (see "The frontend").
- **`lua`:** `message` is exactly `format!("{}", err)` (mlua 0.11.6 Display) of the `mlua::Error` the protected call
  returned, which is the host's `copy` of that error object: for a thrown string it includes `runtime error: ` and any
  traceback mlua added. GPL retains the error object (`retain`) before the outermost service call ends; if that fails
  `host_error` is absent and the error is still reported. The token is valid only while the error lease is open: the
  frontend takes its own reference first. The release happens exactly once, on the creator worker: at `value_close` of
  the error lease there, otherwise when `arena_collect` (or the next call's end) drains it; `arena_close` plus collect
  releases every outstanding token before VM teardown. `value_result_copy` never retains or releases. A struct field
  get that Source turns into `missing_field` is not retained.
- **`internal`:** a contained Kiln fault (a panic in Kiln code, a reader transport failure: a non-OK KilnResult with no
  error object) aborts the operation and is only ever the root. If even the error lease cannot be issued, the operation
  is refused with a KilnResult and every lease it had reserved is withdrawn. ShroudForge panic paths that Kiln turns into
  errors keep class `runtime` with the text kiln-eml's startup value wrappers raise (`sf_startup/resource_values`) (`runtime error: <panic text>`).

### What the reader copies and answers

`copy` returns: a STRING object's bytes; an error object's text (above); a KNOWN_USERDATA's neutral record by adapter
(GUID: 16 game-order bytes, CONTENT: a 16-byte `KilnContentHash`, little endian; RESOURCE: a 24-byte `KilnAssetKey`).
Scalars come in `bits` (BOOLEAN 0/1, INTEGER two's complement, NUMBER IEEE bits); every reply carries an object token,
scalars included. TYPED_VALUE carries `typed_value`, a value lease of the same arena (another arena's is a Kiln fault,
never a fallback). SOURCE_TO_STRING answers a STRING object with Source's `to_string` text, which is mlua 0.11.6's
`Value::to_string` (value.rs:154-187): numbers in Rust's `Display`, strings and `__tostring` results only when they are
UTF-8 (else the conversion error, as an error object); GPL formats the values it holds itself the same way.
GET_FIELD_STRING answers a STRING after Source's own string conversion (or the error it raised).

## The frontend (kiln-sfi)

`crates/sfi/kiln-sfi/src/runtime/values/` is the MIT consumer: the reader (`lua_reader.rs`), one arena per VM
(`arena.rs`), the packets (`packets.rs`), the userdata Lua sees (`proxies.rs`) and the typed table port (`tables.rs`).
It reaches the table only through the runtime's service adapter and links no GPL code.

- **Userdata.** One per node, interned in a weak table by node id; each owns one lease (the adopted provisional or a
  caller-owned item) and closes it once when Lua collects it. They carry ShroudForge's names and registrations
  (`StructValue`, `MappedArrayValue`, ...; struct keys cast to `String`, array keys to `usize` with sfi-core's
  argument casts; variants have only the fields `type` and `value`; `pairs` returns a fresh function, an iterator
  state and nil), so every text Lua sees before the provider is asked is Source's. The lease is also the userdata's
  user value, so DESCRIBE names it without borrowing a userdata an operation holds.
- **Tokens.** Temporaries live until the frontend's outermost call returns; retained tokens until released.
- **Errors.** Transport refusals and unreadable replies are Kiln's own error (`kiln: kiln.eml.values...`, reported to
  the host). Error packets are raised as the table above says; for a root `lua` level the frontend takes its own
  reference to the retained error before it closes the error lease.
- **The two frontend texts.** The reader remembers the last userdata, function or thread a reply described during the
  current outermost call (the value Source's text names; it is dropped when the call returns). Source's TypeId label is
  the Debug form of that userdata's own Rust TypeId in this process (`userdata \`TypeId(0x..)\``; without one, the
  TypeId of the frontend's userdata for `value_kind`); Source's `guid expected, got ..` type name is `{value:#?}` of
  a userdata (`<__name>: 0x<address>`) or mlua's `type_name()` of anything else. Both are exact in form; the TypeId
  value and the address differ per build and process, as they do in ShroudForge.
- **Lifetime.** The VM's host owns the `ValuesArena` outside Lua; it holds the VM, so the VM outlives it. Dropping it
  (or `close`) runs `arena_close` and `arena_collect` until pending is 0 while the VM lives; if pending never reaches 0
  (or `arena_create` admits the reader but returns no control token) the reader's user context is leaked, never freed
  (a drain that stops short is reported to the host's `on_fault`). A panic inside a callback answers INTERNAL and never
  unwinds into the provider; a userdata that cannot be made never takes a provisional lease. A wake sets a flag and
  calls the host's signal; the worker collects after its next outermost call or from its loop (`collect_if_woken`).
- **Production wiring.** The lifecycle host gives every Lua VM, runtime-create's and each live reload's new
  one, its own arena (`runtime/values/host.rs`, `VmValues`): a view of the archive generation granted to the start (the
  kiln.caps rows `eml.archive.metadata` and its members AVAILABLE, `acquire` under kiln-sfi's owner, checked with
  `identity` in every pin), then `arena_create` over it at `KILN_VALUE_RUNTIME`, after the VM's providers are attached
  (with the typed table port) and before any package is registered; a refusal is that VM's setup failure. Families reach
  the arena through the VM's runtime context; the host keeps it beside the VM, never in Lua. A wake is collected at the
  end of the worker's turn (its park ignores unparks, so no `notify` is set). The arena is closed and drained while its
  VM lives: at the end of the orderly leave (after every on_unload), for a reload's old VM right after the swap (that VM
  then closes), for a new VM that did not become the runtime when the reload returns, and when a VM's setup fails after
  the arena was made. The view is released only after the drain; an arena that does not drain is logged and keeps its
  view and reader for good, and so does one the provider may have admitted although its create failed (OK without a
  control token). The fast leave and any cancellation close nothing: the arena and its view are forgotten with the VM,
  and the provider's worker-exit revocation frees the graph without calling the reader. `game.types.of` answers a typed
  value with the registry index of an identity-only `value_info` (Source's `type_of`, with its shared borrow of the
  value).
- **Tests.** `tests/values_frontend.rs` drives a scripted provider built from kiln-abi types (the C contract); the
  example `values_consumer` (a cdylib) runs inside `m6_value_abi`'s `c_consumer_drives_the_real_table` through
  `M6_VALUES_CONSUMERS` against the real provider, including the differential with kiln-eml's startup value wrappers
  (`tests/fixtures/values/`). The production wiring's lifecycle paths (create, the turn's collect, the orderly stop, the
  fast leaves, an interpreter stop, setup failures including a withheld archive row and an arena admitted without a
  control, a reload's success, refusals, abandoned VM, and the watchdog stopping a call that overran inside a reader
  callback whose error the provider swallowed) are in `tests/shared/reload.rs` (`tests/reload.rs`) over
  `tests/support/fake_archive.rs`'s arena lifecycle fake, and through the production plan source in
  `tests/plan_source.rs` and `tests/reload_plan.rs`. The frontend's and the types' bodies
  (`tests/shared/runtime_values.rs`, `tests/shared/runtime_types.rs`) are linked by the aggregate's
  `kiln_sfi_runtime_values` module (Partial claims).

## Checks

Run each command with its own output, temp and target folders:

- `bash native/core/tests/test-eml-values.sh` (needs `KILN_OUT`) runs:
  - the C11 and C++20 layout and contract probes, with every offset, size and enum value, and every member signature
    pinned by assignment;
  - the probes again under ASan/UBSan (truncated readers and short allocations);
  - the identity test (draft hash, canonical include, no name collision with another ABI header);
  - the archive duplicate-record test;
  - four Windows x64 compile-only layout checks in both include orders.
- `python3 -I tools/sfi/abigen/generate.py --check` and `test_abigen.py`: regenerate-and-diff.
- `cargo test --manifest-path crates/sfi/Cargo.toml -p kiln-abi --locked --offline --test eml_values`: the generated
  Rust layouts, constants and callback types.
- `node schema/tests/sfi_eml_values.mjs`: the packet schemas and fixtures (closed objects, the tagged scalar patterns,
  every class and kind, invalid packets refused).
- `cargo test --manifest-path gpl/kiln-eml/Cargo.toml --release --locked --offline -p kiln-eml --test m6_value_graph`:
  the GPL graph over a real mlua host (proxies, finalizers, re-entry, the typed views and item steps equal to the
  fixtures, texts equal to kiln-eml's startup value wrappers).
- `cargo test --manifest-path gpl/kiln-eml/Cargo.toml --release --locked --offline -p kiln-eml --lib archive_values`:
  every fixture that needs no registry written by the serializer, byte for byte, plus the C boundary's panic frame.
- `cargo test --manifest-path gpl/kiln-eml/Cargo.toml --release --locked --offline -p kiln-eml --test m6_value_abi`:
  the C table over a real mlua host behind the C reader ABI (registration and rows, view to registry resolution and
  mismatch refusals, reader admission with every truncation behind a page guard on Linux and Windows, counts past
  `isize::MAX`, Source behaviour through the
  C round trip compared with kiln-eml's startup value wrappers, weak LOADED roots, worker exit, wake discipline, multi-thread closes), and
  the C consumer `tests/c/m6_values_consumer.c` built as C11 and as C++20 (on Linux with `$CC`/`$CXX`; elsewhere
  `M6_VALUES_CONSUMERS` names the prebuilt libraries).

The header probes use placeholders, not the provider, and prove no graph, GC, lifetime, reader or Source behaviour.
`m6_value_graph` proves the GPL graph over a test host and `m6_value_abi` the C table and reader adapter; the MIT
frontend's own suites are in "The frontend" above.
