# Locks with a required item: the press's lock check and the interaction's payment (GitHub #147)

Builds: game client Steam 26430625 / svn 1287369, `enshrouded.exe` sha256 `5d9ef2f1…0c77` (the pinned client), and the
dedicated server Steam 26430253 / svn 1287369, `enshrouded_server.exe` sha256 `f0d824be…aeb2`. Image base `0x140000000`;
addresses are RVAs, client `C`, server `S`. Read from the exe files only (never run): program descriptors through
`tools/typegen/catalog.py`'s reader (update pointer at descriptor +0x10, cross-checked by the qword in `.rdata`),
capstone disassembly, the offline resolver (`tools/sigport/sigdb_check.py`), the reflection dump in
`sigdb/types/<side>/<sha>.json`. Tags as in the other RE pages: **[desc]** descriptor, **[dis]** disassembly, **[refl]**
reflection, **[sig]** the sigdb rows `sigdb/symbols/server.d/lock-sources.json` and `client.d/lock-sources.json`, qualified
by `native/services/items/tests/lock_sources_sigdb_test.py` (38 checks on both exes). No runtime evidence yet **[rt: none]**.

## 1. The component
`keen::ecs::InteractionLock` (hash `0xFBA1F222`, 256 bytes) **[refl]**: `requiredItemRef` +0 (ItemReference),
`requiredItem` u32 **+0x10**, `requiredPermission` +0x14, `requiredItemAmount` u16 **+0x16**, `lockMask` +0x18
(`InteractionLockMask` = Bitmask8 of `InteractionLockFlag {Consume = 0, DestroyLock = 1}`), `enableWaterLock` +0x19 /
`waterLock` +0x1c, `enableToggleLock` +0x3c / `toggleLock` +0x40, `enableBuffLock` +0x64 / `buffLock` +0x68,
`enableDaytimeLock` +0xa0 / `daytimeLock` +0xa8, `successfulAttempt` +0xe0, `failedAttempt` +0xf0 (ActorSequenceReference).
The issue's offsets (+16, +22, +24) are right on both builds; the sigdb patterns capture them as `locks/lock.item` 0x10,
`locks/lock.amount` 0x16, `locks/lock.mask` 0x18 **[sig]**. The runtime side: `ClientInteractionLock {itemUsed}` (1 byte)
and `DynamicInteractionLock {destroy +0, itemUsed +1}`.

## 2. Programs **[desc]**
| Program | C update | S update | Lock role |
|---|---|---|---|
| `player_control_action_sequence` | `0x2dcca0` | `0xc8c40` | the press: calls the lock check (below) at C `0x2dd34c`, S `0xc92ec`; sends `AcceptOfferedInteractionEvent` only for an unlocked press |
| `interaction_attempt` | `0x2a7a10` | `0xa4e80` | events-in `AcceptOfferedInteractionEvent`; other-tpl `InteractionLock`; writes `Inventory`, `DynamicInteractionLock`, `ClientInteractionLock`; the payment (below) |
| `player_ui` | `0x2b8430` | (client program, no update in S) | the locked prompt: counts the item the same way (below) |
| `interaction_query_new` | `0x2aa360` | `0xa77d0` | target selection; reads InteractionLock (not a count) |

`player_control_action_sequence` runs in every world: the server world (dedicated, or a hosting client's own) and, on a
client, the client and prediction worlds. A client exe has one copy of the code for all three.

## 3. The press's lock check: C `0x2c5350`, S `0xb08a0` **[dis]**
Called with (u32* sequenceOut, …, ProgramContext-holder) from the press. `rsi` = the program's per-call block (`[rsi+0x448]`
the InteractionLock template slot, `[rsi+0x58]` the presser's InventorySetup, `[rsi+0x4f8]`, `[rsi+0x508]`,
`[rsi+0x410]` registries and accessors); `[rsp+0x58]` holds the ProgramContext. With `r14` = the host's InteractionLock
(template component lookup C `0x9bfca0`) and `rdi` = its DynamicInteractionLock:
- `requiredItem != 0` and `DynamicInteractionLock.itemUsed == 0` → **InventoryItemCount** (C `0xe93160`, S `0x93a2a0`)
  `(out = rbp-0x68, ctx = [rsp+0x58], InventorySetup*, Inventory accessor, …, u32 item = [r14+0x10], ItemRegistry*)`;
  the return address is **`locks/LockCountRet`** = C `0x2c5547`, S `0xb0a97`;
- `cmp [rbp-0x68], 0` (error), `cmovz edi, [rbp-0x60]` (count), `movzx ebx, word [r14+0x16]` (amount), `cmp edi, ebx;
  … setb r15b`: **locked when count < amount**; then the other lock kinds (toggle, buff via C `0x2a6430`, permission via
  `stations/PlayerPermissions`, daytime via C `0x2a6500`, water via C `0x2a64d0`);
- locked → the sequence id written to `*sequenceOut` (C `0x2c570e`) is the lock's `failedAttempt` (+0xf0, through
  `items/GuidToItemId`) instead of the `successfulAttempt` (+0xe0) / offer's sequence. The press then plays the failed
  attempt; the `AcceptOfferedInteractionEvent` that `interaction_attempt` needs comes from the accepted sequence, so a
  locked press reaches no payment. That the event is not sent at all for the failed sequence is read from the code path,
  not seen at runtime **[dis]** (the issue's point 1, and Kiln's own furniture docs: "a player's game shows it locked").
- InventoryItemCount writes `{u8 error; u32 0; u32 count}`: error `0x15` (with the count 0) when the InventorySetup is not
  initialized (`[r8+0x53] == 0`), else 0 and the count of the item over the setup's inventories (stacks whose item the
  registry knows). S `0x93a2d0`–`0x93a2da`, `0x93a4ff`–`0x93a507` **[dis]**.

The 70 bytes from `lea rcx,[rbp-0x68]` to `movzx ebx,word [r14+0x16]` are identical on both builds (`48 8D 4D 98 … 41 0F
B7 5E 16`). `locks/LockCountRet`'s pattern spans them, so the mid hook's operands (rbp-0x68, [rsp+0x58], r14) are pinned
by the match **[sig]**.

## 4. The locked prompt: C `0x2bc43a` in `player_ui` **[dis]**
`mov ebx,[rdi+0x10]` (rdi = the InteractionLock); unless `DynamicInteractionLock.destroy`, InventoryItemCount(out =
rbp+0x620, ctx = [rbp+0x2d5e0], …, item = ebx); return **`locks/client.UiLockCountRet`** = C `0x2bc483`; then
G38UiPlayData `+0x2c98` = item, `+0x2c9c` = amount, and when error != 0 or count `[rbp+0x628]` < amount, `+0x2ca0` =
item and `+0x2ca4` = 1 (the prompt shows locked). Client-only UI code: the server exe has no such site **[sig]**.

## 5. The interaction's payment: inside `interaction_attempt` **[dis]**
For an accepted offer whose host has an InteractionLock with `requiredItem != 0` and `DynamicInteractionLock.itemUsed ==
0` (S `0xa5228`–`0xa5251`): an inventory transaction on the stack (`items/InventoryTxCtor` + `items/InventoryTxInit` with
the program's InventorySetup, Inventory, SlotSelection, ItemState, OwnerRelationship, InventoryEmptyState accessors; no
Fuel accessor), then
```
movzx eax, word [rsi+0x16]       ; requiredItemAmount
lea   rdx, [rbp+0x230]           ; the transaction
mov   r9d, [rsi+0x10]            ; requiredItem
lea   rcx, [rsp+0x58]            ; PayMaterial's result {u8 error; u32; u32 left}
mov   r8d, [r14]                 ; the guest (the event's +8)
call  gems/PayMaterial           ; C 0x411330, S 0x1b0d60
cmp   byte [rsp+0x58], 0 ; jne fail
cmp   dword [rsp+0x60], 0 ; jne fail
test  byte [rsi+0x18], 1         ; lockMask Consume
je    rollback ; call items/InventoryCommit
```
The return address is **`locks/LockPaid`** = C `0x2a7efe`, S `0xa536e`. A lock without Consume is a presence test: the
payment runs and is rolled back. `fail` (S `0xa57f8`) rolls back (`items/InventoryRollback` S `0xa57ff`) and sends
`DeclinedInteraction` (EmitEvent hash `0x13f49577`, 12 bytes, S `0xa581f`). `gems/PayMaterial` pays only from the entity it is given (the guest's bag): it takes what it holds
and answers the rest in `left`.

## 6. What kiln-items does there (0.2.0-dev.31.12)
- **Mid hook at `locks/LockCountRet`** (both sides): when error is 0 and the bag's count is below the amount, the
  context's world decides: the server world (storage kind 0; the presser from `world/CurrentEntityId(ctx)`) adds the
  server's own count of that player's links registered with Locks (read on the tick thread each 30 steps); a client or
  prediction world adds the server's lock snapshot (`set_lock_stock`). Never more than the game's count when the bag
  covers the lock.
- **Mid hook at `locks/client.UiLockCountRet`** (client): the same for the prompt, from the snapshot.
- **Mid hook at `locks/LockPaid`** (server world): when PayMaterial answered error 0 with `left > 0`, the rest is paid from
  the opted-in links in link order through PayMaterial **in the same transaction**, after a recount of each link through
  the transaction's own InventorySetup/Inventory accessors; on success `left` is set to 0 and the game commits (Consume)
  or rolls back (presence). Short, refused or faulting: `left` stays, so the game declines and rolls back everything, the
  bag's take included.
- Before any hook goes in, `code_state` is asked for each call site's pattern range, InventoryItemCount's entry and
  PayMaterial's entry; a FOREIGN byte names the module (`describe_code_conflict`) in kiln.log and the feature stands down.

## 7. The issue's item ids (not checked here)
The issue names `Lockpick_Scrapy_T0-T4` (item 2897843384) on 73 templates and `Lockpick_Ancient_T7` (1528331711) on 11.
Kiln does not depend on them: the hooks read the lock's own `requiredItem`. They were not re-counted for this page.

## 8. Not seen yet
No lab or client run. Open questions only a game answers: whether the dedicated server's own press check (S `0xb08a0`)
is what decides a remote player's press, or whether the client's predicted failed attempt also stops it (then a Kiln
client is required, as designed); the prompt's look while the snapshot row is on its way; a hosting client's three worlds
in one process.
