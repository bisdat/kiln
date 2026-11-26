# Locks with a required item: the press's lock check and the interaction's payment (GitHub #147)

Builds: game client Steam 26430625 / svn 1287369, `enshrouded.exe` sha256 `5d9ef2f1…0c77` (the pinned client), and the
dedicated server Steam 26430253 / svn 1287369, `enshrouded_server.exe` sha256 `f0d824be…aeb2`. Image base `0x140000000`;
addresses are RVAs, client `C`, server `S`. Read from the exe files only (never run): program descriptors through
`tools/typegen/catalog.py`'s reader (update pointer at descriptor +0x10, cross-checked by the qword in `.rdata`),
capstone disassembly, the offline resolver (`tools/sigport/sigdb_check.py`), the reflection dump in
`sigdb/types/<side>/<sha>.json`. Tags as in the other RE pages: **[desc]** descriptor, **[dis]** disassembly, **[refl]**
reflection, **[sig]** the sigdb rows `sigdb/symbols/server.d/lock-sources.json` and `client.d/lock-sources.json`, qualified
by `native/services/items/tests/lock_sources_sigdb_test.py` (46 checks on both exes). Runtime: Fuzen777's hosted world on
0.2.0-dev.31.13 (GitHub #147) opened a lockpick lock from a linked chest and took the pick there, while the prompt's
requirement line stayed red (section 4a) **[rt: player report]**.

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
| (HUD, not a program) | `0xbf5d80` | (none) | draws the prompt; its requirement line counts the item once more (section 4a) |
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

## 4a. The prompt's requirement line: the HUD's own count, C `0xbf7283`–`0xbf7469` **[dis]**
The red "Required: Lockpick" is not decided by player_ui's count. The HUD's prompt draw (C `0xbf5d80`, `r15` =
G38UiPlayData, `r14` = `r15+0x2bc0`, so `+0xd8` = `+0x2c98` and `+0xdc` = `+0x2c9c`) reads only the item and amount
player_ui wrote and **counts the item again itself**:
- `mov ecx,[r14+0xd8]; test ecx,ecx; je` (no item: no line); the item's name and icon through C `0xe94470`;
- `call 0xd40fe0` = the local player's UI inventory mirror (G38UiPlayData's player list `+0x3060`, stride `0x8f70`,
  matched by the local player's id), then `mov r11d,[r14+0xd8]; mov r8d,edi` (0) and a loop over the mirror's slot
  list (`[rax+8]`, count `[rax+0x10]`) into its stacks (`[rax+0x2d8]`, stride `0x48`): `cmp [stack+8],r11d; jne; add
  r8d,[stack+0x10]` (C `0xbf73a2`–`0xbf73bc`). The bag only.
- at **`locks/client.LockRowCount`** = C `0xbf73be`: `movzx ecx,word [r14+0xdc]` (the amount), `ecx = max(amount, 1)`,
  `[rbp+0x244] = r8d` (the count the line shows), `cmp r8d,ecx; setae [rbp+0x248]` (**met**), then the row goes to the
  row draw C `0xbf1ea0` (call at `0xbf7469`), which picks the line's colour (theme `+0xf060` / `+0xf068`, C `0xbf2141`)
  and text style (`+0xba28` / `+0xba58`, C `0xbf2455`) by that met byte (row `+0x30`).
- `+0x2ca0` / `+0x2ca4` (player_ui's locked flags) only feed the prompt's own locked look (C `0xbf71be`–`0xbf71db`); no
  other code reads `+0x2c98`–`+0x2ca4` by displacement (a scan of the client's `.text`).

So a hook at player_ui's count alone (section 6 before 0.2.0-dev.31.15) makes the prompt unlocked but leaves the line
red whenever the bag is short. Client-only HUD code: the dedicated server has no such site **[sig]**.

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

## 6. What kiln-items does there (since 0.2.0-dev.31.12, mod API 0.2.21)
- **Mid hook at `locks/LockCountRet`** (both sides): when error is 0 and the bag's count is below the amount, the
  context's world decides: the server world (storage kind 0; the presser from `world/CurrentEntityId(ctx)`) adds the
  server's own count of that player's links registered with Locks (read on the tick thread each 30 steps); a client or
  prediction world adds the server's lock snapshot (`set_lock_stock`). Never more than the game's count when the bag
  covers the lock.
- **Mid hook at `locks/client.UiLockCountRet`** (client): the same for the prompt, from the snapshot.
- **Mid hook at `locks/client.LockRowCount`** (client, since 0.2.0-dev.31.15): the requirement line (section 4a). When
  the HUD's count `r8d` is below `max(amount, 1)`, `r8d` becomes bag + the snapshot's row, so the line shows met and its
  count the same as the press. kiln.log logs the line's state once per change (the HUD runs every frame):
  `locks: the lock prompt's requirement line counts the linked sources: item 0x..., bag a + linked b, needs n (shown
  met)` or `... counts the bag only: item 0x..., <check> (bag a, linked b, needs n)`.
- **A hosted world:** the host's process has one kiln-items for its server and client worlds. The server half's
  `LockStock` hands the host's own player its message in process (`Client.IsHosting && Client.LocalPlayer == player`,
  no network; the host's asks go straight to the server half's queue), so the host's client world gets
  `set_lock_stock` and its Pre (`lock_client_pre`) puts the client hooks in, as a joined Kiln client's does on the
  channel. A prompt shown before its row arrives asks for it (the HUD and player_ui both note the item); the HUD and
  player_ui count again every frame, so the line turns met once the row is in, without looking away.
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

## 8. Who decides a remote player's press **[desc] [dis]**
The server's own check decides. `player_control_action_sequence` has flags `0x200` (the server world and a client's
prediction world, docs/re/rules.md 1.2) and reads `PlayerInput` read-only (descriptor lists, both builds); no program in
either exe writes `PlayerInput` (it is the client's raw input, `fromClient.digitalInput`, copied in by the network
layer), so a client's predicted outcome of the press never reaches the server. The server world runs the same press
from that input (docs/re/player-state.md: on a spawned character with no client the game's own control programs act on
`fromClient.digitalInput`), calls its own lock check (S `0xc92ec` → `0xb08a0`, where Kiln's `locks/LockCountRet` hook
adds the links) and sends `AcceptOfferedInteractionEvent` itself; `interaction_attempt` (flags `0x4`) takes it in the
server world. `interaction_query_new`, the other reader of InteractionLock, writes `ClientInteractionQuery` on both
sides and counts no item (section 2). So with a Kiln server a **vanilla client's** press on a lock its bag cannot open still opens it
and pays from the opted-in links; its own prompt (player_ui, client only) shows the lock locked and its prediction world
may first play the lock's failed attempt, until the server's state reaches it. A Kiln client's snapshot makes its
prompt and prediction agree with the server; it is not what lets the press through.

## 9. Not seen yet
No lab or client run of our own. Fuzen777's hosted run (0.2.0-dev.31.13) saw the opening and the payment from a linked
chest; the requirement line's fix (section 4a, 0.2.0-dev.31.15) is read from the code only. Open questions only a game
answers: what a vanilla client shows when the server opens a lock its prediction refused (section 8: the decision itself
is read from the code, not seen); the line's look in the moment before the snapshot row arrives; a joined client's line
on a dedicated server.
