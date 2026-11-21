# Engine-call recipes: format and review checklist (Kiln-internal)

For Kiln's own services that make an engine call inside a held write gate (`ecs_gate_invoke`,
kiln.internal.engine; [engine.md](engine.md)). Ported profiles carry no recipe. Each recipe lands in the same change as the
service that calls it (prop spawn and destroy, the Impact operation), and that change's review works through the
checklist below. A recipe is never added on its own.

## Where a recipe lives

A recipe sits in the profile of its exact build, `sigdb/engine/<side>/<build>.json`, under the top-level
`engineCalls` object. That section is Kiln's own: it is not ShroudForge data, it is left out of the generated leaves,
and the rest of the file stays byte for byte the copied profile. `tools/engine/generate.py` types each recipe into
`Layout::calls` (`kiln_engine_layout.h` `CallRecipe`). A profile without the section, or with an empty one, generates
`CallRecipes{}`, exactly as before.

```json
"engineCalls": {
  "PropSpawnSubmit": {
    "id": 1,
    "argBytes": [160, 0],
    "resultBits": 8,
    "ranges": [
      {"rva": 1234560, "size": 412, "original_sha256": "<sha256 of the original bytes of this range>"},
      {"rva": 2345670, "size": 96,  "original_sha256": "..."}
    ],
    "review": {
      "sfSource": "<ShroudForge file:lines that calls it inside its Scene hold>",
      "sfInHold": true,
      "locks": ["<every engine lock the graph takes, and how>"],
      "allocators": ["<every allocator the graph reaches>"],
      "threadState": {"mxcsr": "<what the graph assumes or changes>", "engineTls": "<engine TLS slots it reads or writes>"},
      "stackBuffersMovedToHeap": ["<each ShroudForge stack-local buffer the Kiln member prepares on the heap before the gate>"],
      "boundedLoops": ["<each loop in the graph and its fixed bound>"]
    }
  }
}
```

The generator refuses the profile, and so the build, unless all of these hold:

| Rule | Why |
|---|---|
| The key is a `CallRecipeId` name and `id` is its value (`PropSpawnSubmit` 1, `PropDestroySubmit` 2, `ImpactSameEntity` 3). The recipe lands in slot `id - 1`. | The id is frozen and names the same kind of call on every build. New ids are appended to the enum and to `CALL_RECIPE_IDS` together; `test_generate.py` compares the two. |
| Exactly the keys above, at every level, with no duplicate key | A misspelt field must not be silently dropped. |
| `argBytes`: at most 8 entries (the argument count), each 0 for an integer or the byte size of the caller buffer a pointer argument points to | `ecs_gate_invoke` checks every pointer buffer against the stack, the image and the capture |
| `resultBits` is 0, 8, 16, 32 or 64 | the low bits of rax the caller receives |
| 1 to 32 ranges. `ranges[0]` starts at the called entry; the others cover every procedure the graph reaches. Each range lies inside SizeOfImage, is at most 64 KiB, does not overlap another range, and has a 64-hex lowercase `original_sha256`. All ranges together come to at most 64 KiB (`kCallCodeBytes`). | the run-time compare covers the whole graph. The 64 KiB cap matches `read_original` / `code_state`. |
| `review.sfInHold` is `true`, `review.sfSource` is non-empty, `threadState` has both entries, and the lists hold non-empty strings | only graphs ShroudForge itself calls inside its Scene hold on that exact build are admitted |
| The profile's ECS write gate is enabled | the call runs only inside that gate's hold |

`generate.py --check` also checks every range's `original_sha256` against a read-only copy of the exact executable,
when one is available: `--binaries <folder>` or `KILN_ENGINE_BINARIES`. It uses the file named like the target whose
sha256 is the profile's, maps each range through the PE section table, and fails on a different digest, on a range
that is not wholly inside `.text`'s file data, and on a range that holds a base-relocation field. Those are the bytes
`kiln.hooks.read_original` can return with one digest: it reads only the exe's `.text`, and it relocates the file's
bytes to where the exe is loaded, so a fixup field inside a range would change the digest at every other load address.
A build with no copy in the folder is reported `NOT CHECKED`, and without `--binaries` a profile that carries recipes
prints `engine call originals: NOT CHECKED`; neither fails the run (CI has no copies), so neither counts as a check.
The copies are only read, never run.

## Review checklist for a change that adds a recipe

The reviewer of the change ticks each item with evidence (file:line in ShroudForge at the fork pin, or a disassembly
excerpt of the exact build):

1. **Admission.** ShroudForge calls this graph inside its Scene hold on this exact build (`sfSource`), and the Kiln
   member calls it from the same kind of hold. If either is missing, the graph is not admitted.
2. **The whole graph.** `ranges[0]` is the entry ShroudForge calls. Every procedure the graph reaches (direct calls,
   tail jumps, called thunks, the root's linkage when the review keeps it) has a range. Nothing is reached through a
   data-dependent indirect call that the ranges do not cover.
3. **Original bytes.** Every `original_sha256` was taken from the read-only copy, and `generate.py --check` with the
   binaries passes on the change's head (paste the `engine call originals: N checked, 0 failed, 0 profiles NOT CHECKED`
   line; N is every range of the change's recipes).
4. **Locks.** `locks` lists every engine lock the graph takes. For each one, the engine-lock assumption ("the
   GameServer thread never holds this lock while it acquires the Scene word") is a check of this graph that only the
   real game can prove, done there before the member ships switched on.
5. **Allocators.** `allocators` lists every allocator the graph reaches. The engine allocates, never Kiln; and no
   allocator waits on something the Scene word's holder blocks.
6. **Thread state.** `threadState` says what the graph assumes about MXCSR (rounding, denormals) and which engine TLS
   slots it reads or writes. A graph that needs a TLS value only the game's own threads have is not admitted.
7. **Buffers.** Each stack-local buffer ShroudForge passes becomes a heap buffer that the member prepares before the gate
   (`stackBuffersMovedToHeap`). The member passes it through `argBytes` or `KilnEngineCall.buffers`.
   `ecs_gate_invoke` refuses any buffer on the calling thread's stack.
8. **Loops.** Every loop in the graph has a fixed bound (`boundedLoops`). A graph with a data-dependent unbounded loop is
   not admitted.
9. **Result.** `resultBits` and `argBytes` match the function's signature in the exact build, and the member treats
   anything but Returned as not done.
10. **Credit.** The ported recipe and the member's port carry the MIT notice (bonsaibauer) and the ShroudForge file:lines.
11. **Protection.** The member protects every range of the recipe through `kiln.internal.hooks` (step 6 below) with its
    own owner and a reason that names the member, after the capture matched the original bytes and before the gate, and
    it does not take the gate unless that call answered `KILN_OK`. A test shows a Kiln hook placed first makes the
    member unavailable, and one asked for after its first use is refused. The reviewer also lists every hook Kiln itself
    places (the core, its services, Kiln's own plugins) and checks that none lands in the graph's ranges: one placed
    before the first call makes the member unavailable for the session, and one a service places on demand afterwards
    is refused, so that service's feature stops working. The same list covers Kiln code that writes game code without
    kiln.hooks, which the protection does not stop: `patch_code_byte` in `native/services/content/game.cpp` (the stack
    clamp byte) and the compatibility plugins' own code patchers (`native/plugins/shroudtopia/src/context.cpp`,
    `native/plugins/greeneye/include/greeneye/jumppatch.h`). One of those writing into a range during a call is caught
    only by the in-hold compare, which then answers Uncertain and latches every engine call.

## The calling member's order (before, inside and after the gate)

Before the gate, in this order:

1. kiln.hooks offers both members: `header.size` covers `read_original` (offset 296) and `code_state` (304), neither is
   null, and the capability rows `hooks.read_original` and `hooks.code_state` are available (they are off when Kiln
   only has the live code, which is not the original). Otherwise the member is unavailable.
2. `kiln.hooks.read_original` for every range: the exe file's bytes relocated to where the exe is loaded, never live
   memory. It accepts only a range inside the exe's `.text` of at most 64 KiB (`KILN_HOOKS_CODE_RANGE_MAX`), which is
   what the generator and `--check` enforce.
3. The recipe's code shape, and each range's `original_sha256`, checked on those original bytes. On a mismatch the
   member refuses and does not take the gate.
4. `kiln.hooks.code_state` for every range answers `KILN_CODE_ORIGINAL`. `MANAGED`, `FOREIGN` or `UNKNOWN` refuses
   with `engine-code-patched`.
5. Capture the live bytes of every range into the member's own heap buffer (`KilnEngineCall.code`), then compare the
   capture with the `read_original` bytes of step 2, byte for byte; any difference refuses with `engine-code-patched`.
   `code_state` only describes the moment it looked: a patch written between it and the capture would be captured, and
   the in-hold compare (live bytes against the capture) would then pass it. The capture equal to the file's bytes is what
   proves the code called is the original. `code_state`'s generation is no substitute: it is one counter for the whole
   process and moves on every call for another range, the member's own next range included.
6. Protect the ranges with `kiln.internal.hooks` `protect_code_ranges(owner, ranges, n, reason)`
   (`native/core/src/hooks_protect_internal.h`, [hooks.md](hooks.md#protected-code-ranges-kilninternalhooks-kiln-internal)).
   Resolve it through the service's own `KilnApi` (`kiln.internal.hooks`, version 1, `header.size` at least 16); a
   missing table makes the member unavailable. Pass every range of the recipe in one call, as live addresses (image +
   rva), with the member's own owner and a reason a mod author can read in kiln.log. Under kiln.hooks' code lock it
   checks every range is still `ORIGINAL`, then protects all of them or none:
   - `KILN_OK`: protected until the member's owner retires. From then on kiln.hooks refuses every Kiln detour, patch
     and write over those bytes. Calling it again at every pre-gate check is harmless (nothing is added, the
     `ORIGINAL` check runs again); calling it once and remembering the answer is fine too.
   - `KILN_ERR_UNAVAILABLE`: a Kiln hook or patch, or another module, changed the code since step 4, a Kiln patch is
     recorded over it (one that put the file's bytes back over another still counts), or the process-wide bounds (256
     ranges, 512 KiB) are full. Refuse with `engine-code-patched` and do not take the gate.
   - `KILN_ERR_INVALID_ARGUMENT` or `KILN_ERR_REVOKED`: a wrong range or reason, or that owner is gone. Unavailable.

   This happens lazily, at the first check that got this far: never at load, and never after a `MANAGED` or `FOREIGN`
   answer. First come wins: a Kiln mod that hooked a recipe function earlier keeps its hook and the engine call stays
   unavailable, and a hook
   asked for after the member's first use is refused with the member's reason in kiln.log. Steps 4 to 6 must stay in
   this order: step 5 catches a capture taken while an install was in place, and step 6 refuses when an install landed
   after step 5 and stops every later one. The protection binds kiln.hooks' writers only (item 11 lists the Kiln code
   that writes game code without it), so the in-hold compare (live bytes against the capture) stays.
7. Prepare every argument buffer on the heap. Then take the gate (`EcsGateScope`) and check `held()`.

Inside the hold, the member makes one `ecs_gate_invoke` per step, plus its own `validate()` calls. It does no
allocation, takes no lock, does not wait, does not log and does not call the KilnApi. kiln.engine refuses with nothing
called on: a gate that is not held, a gate that has closed, the latch, a missing recipe, `engine-call-target` (the
gate's image is not the loaded game image), a wrong argument or capture, a buffer on the stack, a gate that no longer
validates, or code that changed since the capture.

After the release come the events, the logs and the caller's out-buffers. An Uncertain answer means kiln.engine
latched every engine call for the session; the member also sets its own write latch, as ShroudForge's
`MarkSessionWriteUncertain` does.

## What a change that adds a recipe runs

- `python3 -I tools/engine/generate.py --check --binaries <read-only copies>` and `python3 -I tools/engine/test_generate.py`
  (with `KILN_ENGINE_BINARIES` set as well).
- The engine host suite under ASan and TSan, the world suite, Wine, and a native Windows run of the member's real call
  path.
- A run of the graph in the real game before the member ships switched on.
