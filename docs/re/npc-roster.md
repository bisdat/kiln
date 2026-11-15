# The NPC roster's unlock cache: every consumer, both executables

Package 17c's consumer audit (chapter 17.3 §2, §3, §8), the gate of the roster's second page. Static reading of the two
executables as files (Capstone over the PE image, the program descriptors as `tools/typegen/catalog.py` reads them);
nothing was run for it. Builds: dedicated server Steam 26430253 (`enshrouded_server.exe` sha256 `f0d824be…`), client
Steam 26430625 (`enshrouded.exe` `5d9ef2f1…`). Addresses are RVAs of those two files. Signatures:
`sigdb/symbols/server.d/dev29-17c-roster-cap.json` (the client file extends it unchanged).

## The object

`NpcRegistry` is a world singleton (registered by name, once per world: the server's world setup at `0x233ee0`; on the
client the client world's setup `0x3a60b0` and the hosted server world's `0x49be10`). It is not a reflected type
(`keen::NpcRegistry` is unknown to the reflection), so no reflection-driven save or replication path touches it. Its
loader (server `0x96c3a0`, called from the world's system loader `0x23b000` with the systems aggregate's `+0x200`)
builds it from the NpcCollection resource:

| Offset | What |
|---|---|
| +0x18..+0x30 | the NpcCollection resource handle; +0x20 the resource (a BlobArray of 136-byte NpcResource at +0) |
| +0x38 | id map: NpcId (the entry's u32 at +0) to entry index (u64) |
| +0x90 | u16: the knowledge version the cache was computed at |
| +0x98 | the cache, a 16-byte result: u8 status at +0x98 (0 valid), u64 payload at +0xa0 (bit i = entry i unlocked) |
| +0xa8 | item map: the summon tool's ItemId (entry +0x24) to NpcId |

The loader sets the version to 0 and the cache to status 0x10 ("not computed"). Nothing else writes +0x90/+0x98/+0xa0
but `NpcRegistrySetUnlocks` (server `0x96c6a0`, client `0xec4800`), which stores the version and {status 0, payload}.

## The three functions

- `NpcEvalUnlocks` (server `0xa2ef90`, client `0xf0c510`): `Result64* (Result64* out, knowledge context, u16* seen,
  {NpcResource* p, u64 n})`. If `*seen` equals the knowledge state's version: status 6 ("unchanged"), payload 0, `*seen`
  untouched. Else for each entry i: an empty requirement (query id 0) counts as unlocked; otherwise the game's query
  evaluator decides (status 0x1f "unknown" counts as locked; any other error marks the result failed but the loop
  goes on); unlocked entries set bit i with `bts rbx, rsi`, which takes i modulo 64. At the end: failed: status 1,
  payload 0, `*seen` untouched; else `*seen` = the knowledge version, status 0, payload the mask.
- `NpcRegistrySetUnlocks` (above): `void (NpcRegistry*, u64 mask, u16 version)`.
- `NpcIsUnlocked` (server `0x94bf00`, client `0xea4cb0`): `ResultBool* (ResultBool* out, NpcRegistry*, u32 NpcId)`.
  Cache status not 0: status 1. Id not in the map: status 0x1f. Else status 0, bool = `(payload >> index) & 1` with
  `shr rax, cl`, which takes the index modulo 64.

So an entry at index 64 or more both is produced onto bit `index & 63` and is read from it: entry 64 shares entry 0's
state.

## Every consumer

From the program descriptors, every program whose access lists name `NpcRegistry` (nine on both builds), plus every
direct, RIP-relative and data reference to the functions above, plus the holders of the pointer outside programs.

| Consumer | Exe | Where | What it does with the registry | At index 64 and above |
|---|---|---|---|---|
| `npc_unlock_state_server` (write) | server | update `0x1fa330`: version getter `0x1fa349`, span `0x1fa360`, Eval `0x1fa384`, SetUnlocks `0x1fa3a3` | computes the cache, stores it when Eval says status 0 | entry i lands on bit i & 63 |
| `npc_unlock_state_server` / `npc_unlock_state_client` (write) | client | `0x45fb10` / `0x45fa90`, byte-identical bodies; Eval `0x45fb64`/`0x45fae4`, SetUnlocks `0x45fb83`/`0x45fb03` | the same, for the hosted server world's and the client world's own registry | the same |
| `unique_npc_spawns` (read) | server, client | `0x1fb0f0` (body `0x1fb124`), IsUnlocked `0x1fb279`; client `0x4627d0` (body `0x462804`), IsUnlocked `0x462959` | for each entity with ActiveNpcStateSaved: NpcId = FNV-1a of its template GUID; IsUnlocked; status 0 and locked (and no debug override) destroys it; status not 0 is treated as unlocked (kept, subject to the base's duplicate check) | reads bit index & 63: a page-2 NPC takes a page-1 NPC's state (an unlocked one can be destroyed) |
| `player_assign_npc` (read) | server, client | `0xc4170`; client `0x2d81d0` | item map only (`0x92ff80`, client `0xe88db0`: ItemId to NpcId) | no index involved |
| `actor_spawn_entity` (read) | server, client | `0x454f0`, `NpcFind` at `0x447cd`; client `0x228130`, `NpcFind` at `0x22740d` | entry by id (map +0x38, index × 0x88) | u64 index, no 64-limit |
| `server_cursor_no_prediction` (read) | server, client | `0x9cbd0` (`NpcFind` through `0x1ee6e0`, `0x1ee530`); client `0x297840` (`0x450e30`, `0x450c80`) | entry by id | no 64-limit |
| `client_cursor` (read) | client only (no update in the server exe) | `0x294620`, `NpcFind` through `0x450e30` | entry by id | no 64-limit |
| `fill_ui_npc_directory` (read; the Survivors directory) | client only | `0x45d160`: the roster span `0x45d19a`; per entry the entry's own requirement (entry +0x10) through the knowledge evaluator `0xf0aa20` in two modes | does not read the cache; writes the directory map in G38UiPlayData (a hash map that grows when full) | evaluates every entry itself: no 64-limit in the unlock state |
| `unlocked_recipes_ui` (read) | client only | `0x31b730`: the roster span `0x31bd7b` | entries' assistant flag and workshop id | no index |
| UI code outside programs | client only | 24 functions read the client world's registry pointer that the client world's setup stores in G38UiPlayData+0x4667e8 (`0x3af165`) | `NpcFind` (`0xb32cbe`) and the roster span only; none reads +0x90/+0x98/+0xa0 on it | no index |

Whole-image census (both exes), for an inlined or indirect reader the table could miss:

- references to the six typed functions: only the call sites above; no RIP-relative operand and no data pointer to any of
  them (no function-pointer use).
- a variable-index bit taken from a u64 at +0xa0 (`mov r, [X+0xa0]` followed by `shr r, cl`, `bt r, ...`, `shrx`,
  `bextr` within 8 instructions): exactly one function per exe, `NpcIsUnlocked` itself. No memory bit test (`bt [X+0xa0]`).
- fixed-bit tests at +0xa0..+0xa7 (4 server, 8 client), 16-byte loads of +0x98 (19 server, 41 client) and functions
  reading a byte at +0x98 and +0xa0 of one base (7 server, 14 client): each is another struct (none touches +0x20, +0x38
  and +0x90 of the same base the way the registry is laid out; most are 16-byte-stride copies), and none is reachable
  from the nine programs.

## Verdict

**Safe on both executables for a second page kept beside the registry**, with these hooks: `NpcEvalUnlocks` (page the
span into runs of at most 64 entries, call the game's evaluator once per page), `NpcRegistrySetUnlocks` (attach the
extra pages to the registry it writes, same thread), `NpcIsUnlocked` (answer an id whose index is 64 or more from the
registry's extra pages, or fail closed with the game's own "cache invalid" status 1, which `unique_npc_spawns` treats
like an unlocked NPC: kept). The cache's only reader in the game is `unique_npc_spawns`, a server-world program; the
client world's cache has no reader in the game at all (the directory evaluates knowledge itself). The cache is derived:
the loader resets it, only the producers write it, only the query reads it, and the registry is not reflected, so
nothing found saves or sends it. What only a run can show: the evaluator's unchanged/error behaviour per page and the
pairing on worker threads (lab), a save and restart (lab: knowledge is what is saved), and on a client the directory,
summon and beds with entries 0 and 64 (guided check `17-roster-client`).

## How to repeat it

`tools/typegen/catalog.py` finds the programs; the audit scripts of the package (outside the tree) did the reference
census, the reachability walk (calls, jumps, fall-through chunks and function-pointer LEAs from each program's update to
depth 4) and the shape census. A new game build needs the census again before the second page is trusted: the
signatures (feature `quests.npc_pages`) only say that the four functions are still there.
