/* SPDX-License-Identifier: MIT
 * kiln.items.storage v1 (abi/services/kiln_items_storage.h): every size, alignment, offset and value, compiled as C11
 * and C++20 by test-abi-headers.sh (also for Windows x64 when cc is clang) and by the native build (kiln-abi-headers,
 * MSVC on Windows). kiln_items.h comes first: the storage header's two kiln.items values must not collide with its own.
 * A change here is an ABI break, never a test update. */
#include <stddef.h>

#include "services/kiln_items.h"
#include "services/kiln_items_storage.h"

#ifdef __cplusplus
#define KILN_TEST_ASSERT static_assert
#define KILN_TEST_ALIGNOF alignof
#else
#define KILN_TEST_ASSERT _Static_assert
#define KILN_TEST_ALIGNOF _Alignof
#endif

KILN_TEST_ASSERT(sizeof(KilnStorageFilter) == 24 && KILN_TEST_ALIGNOF(KilnStorageFilter) == 8, "KilnStorageFilter");
KILN_TEST_ASSERT(offsetof(KilnStorageFilter, size) == 0, "KilnStorageFilter.size");
KILN_TEST_ASSERT(offsetof(KilnStorageFilter, categories) == 4, "KilnStorageFilter.categories");
KILN_TEST_ASSERT(offsetof(KilnStorageFilter, flags) == 8, "KilnStorageFilter.flags");
KILN_TEST_ASSERT(offsetof(KilnStorageFilter, id_count) == 12, "KilnStorageFilter.id_count");
KILN_TEST_ASSERT(offsetof(KilnStorageFilter, ids) == 16, "KilnStorageFilter.ids");

KILN_TEST_ASSERT(sizeof(KilnStorageDef) == 56 && KILN_TEST_ALIGNOF(KilnStorageDef) == 8, "KilnStorageDef");
KILN_TEST_ASSERT(offsetof(KilnStorageDef, size) == 0, "KilnStorageDef.size");
KILN_TEST_ASSERT(offsetof(KilnStorageDef, slots) == 4, "KilnStorageDef.slots");
KILN_TEST_ASSERT(offsetof(KilnStorageDef, key) == 8, "KilnStorageDef.key");
KILN_TEST_ASSERT(offsetof(KilnStorageDef, death) == 24, "KilnStorageDef.death");
KILN_TEST_ASSERT(offsetof(KilnStorageDef, flags) == 28, "KilnStorageDef.flags");
KILN_TEST_ASSERT(offsetof(KilnStorageDef, filter) == 32, "KilnStorageDef.filter");

KILN_TEST_ASSERT(sizeof(KilnStorageState) == 192 && KILN_TEST_ALIGNOF(KilnStorageState) == 8, "KilnStorageState");
KILN_TEST_ASSERT(offsetof(KilnStorageState, size) == 0, "KilnStorageState.size");
KILN_TEST_ASSERT(offsetof(KilnStorageState, status) == 4, "KilnStorageState.status");
KILN_TEST_ASSERT(offsetof(KilnStorageState, key) == 8, "KilnStorageState.key");
KILN_TEST_ASSERT(offsetof(KilnStorageState, handle) == 136, "KilnStorageState.handle");
KILN_TEST_ASSERT(offsetof(KilnStorageState, slots) == 140, "KilnStorageState.slots");
KILN_TEST_ASSERT(offsetof(KilnStorageState, pages) == 144, "KilnStorageState.pages");
KILN_TEST_ASSERT(offsetof(KilnStorageState, holder) == 148, "KilnStorageState.holder");
KILN_TEST_ASSERT(offsetof(KilnStorageState, stacks) == 152, "KilnStorageState.stacks");
KILN_TEST_ASSERT(offsetof(KilnStorageState, parked) == 156, "KilnStorageState.parked");
KILN_TEST_ASSERT(offsetof(KilnStorageState, quarantined) == 160, "KilnStorageState.quarantined");
KILN_TEST_ASSERT(offsetof(KilnStorageState, source) == 164, "KilnStorageState.source");
KILN_TEST_ASSERT(offsetof(KilnStorageState, paired) == 168, "KilnStorageState.paired");
KILN_TEST_ASSERT(offsetof(KilnStorageState, death) == 172, "KilnStorageState.death");
KILN_TEST_ASSERT(offsetof(KilnStorageState, refs) == 176, "KilnStorageState.refs");
KILN_TEST_ASSERT(offsetof(KilnStorageState, revision) == 184, "KilnStorageState.revision");

KILN_TEST_ASSERT(sizeof(KilnStorageRecord) == 80 && KILN_TEST_ALIGNOF(KilnStorageRecord) == 4, "KilnStorageRecord");
KILN_TEST_ASSERT(offsetof(KilnStorageRecord, slot) == 0, "KilnStorageRecord.slot");
KILN_TEST_ASSERT(offsetof(KilnStorageRecord, length) == 2, "KilnStorageRecord.length");
KILN_TEST_ASSERT(offsetof(KilnStorageRecord, flags) == 3, "KilnStorageRecord.flags");
KILN_TEST_ASSERT(offsetof(KilnStorageRecord, item) == 4, "KilnStorageRecord.item");
KILN_TEST_ASSERT(offsetof(KilnStorageRecord, count) == 8, "KilnStorageRecord.count");
KILN_TEST_ASSERT(offsetof(KilnStorageRecord, level) == 12, "KilnStorageRecord.level");
KILN_TEST_ASSERT(offsetof(KilnStorageRecord, reason) == 13, "KilnStorageRecord.reason");
KILN_TEST_ASSERT(offsetof(KilnStorageRecord, reserved) == 14, "KilnStorageRecord.reserved");
KILN_TEST_ASSERT(offsetof(KilnStorageRecord, data) == 16, "KilnStorageRecord.data");

KILN_TEST_ASSERT(sizeof(KilnStorageEntry) == 160 && KILN_TEST_ALIGNOF(KilnStorageEntry) == 4, "KilnStorageEntry");
KILN_TEST_ASSERT(offsetof(KilnStorageEntry, size) == 0, "KilnStorageEntry.size");
KILN_TEST_ASSERT(offsetof(KilnStorageEntry, kind) == 4, "KilnStorageEntry.kind");
KILN_TEST_ASSERT(offsetof(KilnStorageEntry, key) == 8, "KilnStorageEntry.key");
KILN_TEST_ASSERT(offsetof(KilnStorageEntry, slots) == 136, "KilnStorageEntry.slots");
KILN_TEST_ASSERT(offsetof(KilnStorageEntry, first) == 140, "KilnStorageEntry.first");
KILN_TEST_ASSERT(offsetof(KilnStorageEntry, count) == 144, "KilnStorageEntry.count");
KILN_TEST_ASSERT(offsetof(KilnStorageEntry, reason) == 148, "KilnStorageEntry.reason");
KILN_TEST_ASSERT(offsetof(KilnStorageEntry, refs) == 152, "KilnStorageEntry.refs");

KILN_TEST_ASSERT(sizeof(KilnStorageDoc) == 80 && KILN_TEST_ALIGNOF(KilnStorageDoc) == 8, "KilnStorageDoc");
KILN_TEST_ASSERT(offsetof(KilnStorageDoc, size) == 0, "KilnStorageDoc.size");
KILN_TEST_ASSERT(offsetof(KilnStorageDoc, flags) == 4, "KilnStorageDoc.flags");
KILN_TEST_ASSERT(offsetof(KilnStorageDoc, source) == 8, "KilnStorageDoc.source");
KILN_TEST_ASSERT(offsetof(KilnStorageDoc, entry_count) == 12, "KilnStorageDoc.entry_count");
KILN_TEST_ASSERT(offsetof(KilnStorageDoc, record_count) == 16, "KilnStorageDoc.record_count");
KILN_TEST_ASSERT(offsetof(KilnStorageDoc, reserved) == 20, "KilnStorageDoc.reserved");
KILN_TEST_ASSERT(offsetof(KilnStorageDoc, revision) == 24, "KilnStorageDoc.revision");
KILN_TEST_ASSERT(offsetof(KilnStorageDoc, witness) == 32, "KilnStorageDoc.witness");
KILN_TEST_ASSERT(offsetof(KilnStorageDoc, account) == 40, "KilnStorageDoc.account");
KILN_TEST_ASSERT(offsetof(KilnStorageDoc, character) == 48, "KilnStorageDoc.character");
KILN_TEST_ASSERT(offsetof(KilnStorageDoc, entries) == 64, "KilnStorageDoc.entries");
KILN_TEST_ASSERT(offsetof(KilnStorageDoc, records) == 72, "KilnStorageDoc.records");

KILN_TEST_ASSERT(sizeof(KilnStorageCapture) == 104 && KILN_TEST_ALIGNOF(KilnStorageCapture) == 8, "KilnStorageCapture");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, size) == 0, "KilnStorageCapture.size");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, flags) == 4, "KilnStorageCapture.flags");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, seq) == 8, "KilnStorageCapture.seq");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, join) == 16, "KilnStorageCapture.join");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, step) == 24, "KilnStorageCapture.step");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, witness) == 32, "KilnStorageCapture.witness");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, revision) == 40, "KilnStorageCapture.revision");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, changes) == 48, "KilnStorageCapture.changes");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, account) == 56, "KilnStorageCapture.account");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, character) == 64, "KilnStorageCapture.character");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, player) == 80, "KilnStorageCapture.player");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, slot) == 84, "KilnStorageCapture.slot");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, first_entry) == 88, "KilnStorageCapture.first_entry");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, entry_count) == 92, "KilnStorageCapture.entry_count");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, reason) == 96, "KilnStorageCapture.reason");
KILN_TEST_ASSERT(offsetof(KilnStorageCapture, reserved) == 100, "KilnStorageCapture.reserved");

KILN_TEST_ASSERT(sizeof(KilnStorageFrozenBatch) == 80 && KILN_TEST_ALIGNOF(KilnStorageFrozenBatch) == 8, "KilnStorageFrozenBatch");
KILN_TEST_ASSERT(offsetof(KilnStorageFrozenBatch, size) == 0, "KilnStorageFrozenBatch.size");
KILN_TEST_ASSERT(offsetof(KilnStorageFrozenBatch, flags) == 4, "KilnStorageFrozenBatch.flags");
KILN_TEST_ASSERT(offsetof(KilnStorageFrozenBatch, tag) == 8, "KilnStorageFrozenBatch.tag");
KILN_TEST_ASSERT(offsetof(KilnStorageFrozenBatch, saves_seq) == 16, "KilnStorageFrozenBatch.saves_seq");
KILN_TEST_ASSERT(offsetof(KilnStorageFrozenBatch, revision) == 24, "KilnStorageFrozenBatch.revision");
KILN_TEST_ASSERT(offsetof(KilnStorageFrozenBatch, capture_count) == 28, "KilnStorageFrozenBatch.capture_count");
KILN_TEST_ASSERT(offsetof(KilnStorageFrozenBatch, entry_count) == 32, "KilnStorageFrozenBatch.entry_count");
KILN_TEST_ASSERT(offsetof(KilnStorageFrozenBatch, record_count) == 36, "KilnStorageFrozenBatch.record_count");
KILN_TEST_ASSERT(offsetof(KilnStorageFrozenBatch, first_step) == 40, "KilnStorageFrozenBatch.first_step");
KILN_TEST_ASSERT(offsetof(KilnStorageFrozenBatch, last_step) == 48, "KilnStorageFrozenBatch.last_step");
KILN_TEST_ASSERT(offsetof(KilnStorageFrozenBatch, unix_ms) == 56, "KilnStorageFrozenBatch.unix_ms");
KILN_TEST_ASSERT(offsetof(KilnStorageFrozenBatch, world_guid) == 64, "KilnStorageFrozenBatch.world_guid");

KILN_TEST_ASSERT(sizeof(KilnStorageWitness) == 80 && KILN_TEST_ALIGNOF(KilnStorageWitness) == 8, "KilnStorageWitness");
KILN_TEST_ASSERT(offsetof(KilnStorageWitness, size) == 0, "KilnStorageWitness.size");
KILN_TEST_ASSERT(offsetof(KilnStorageWitness, flags) == 4, "KilnStorageWitness.flags");
KILN_TEST_ASSERT(offsetof(KilnStorageWitness, join) == 8, "KilnStorageWitness.join");
KILN_TEST_ASSERT(offsetof(KilnStorageWitness, witness) == 16, "KilnStorageWitness.witness");
KILN_TEST_ASSERT(offsetof(KilnStorageWitness, step) == 24, "KilnStorageWitness.step");
KILN_TEST_ASSERT(offsetof(KilnStorageWitness, captured) == 32, "KilnStorageWitness.captured");
KILN_TEST_ASSERT(offsetof(KilnStorageWitness, capture_seq) == 40, "KilnStorageWitness.capture_seq");
KILN_TEST_ASSERT(offsetof(KilnStorageWitness, account) == 48, "KilnStorageWitness.account");
KILN_TEST_ASSERT(offsetof(KilnStorageWitness, character) == 56, "KilnStorageWitness.character");
KILN_TEST_ASSERT(offsetof(KilnStorageWitness, player) == 72, "KilnStorageWitness.player");
KILN_TEST_ASSERT(offsetof(KilnStorageWitness, slot) == 76, "KilnStorageWitness.slot");

KILN_TEST_ASSERT(sizeof(KilnStorageEvent) == 240 && KILN_TEST_ALIGNOF(KilnStorageEvent) == 8, "KilnStorageEvent");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, size) == 0, "KilnStorageEvent.size");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, kind) == 4, "KilnStorageEvent.kind");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, seq) == 8, "KilnStorageEvent.seq");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, step) == 16, "KilnStorageEvent.step");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, player) == 24, "KilnStorageEvent.player");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, handle) == 28, "KilnStorageEvent.handle");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, flags) == 32, "KilnStorageEvent.flags");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, reason) == 36, "KilnStorageEvent.reason");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, stacks) == 40, "KilnStorageEvent.stacks");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, items) == 44, "KilnStorageEvent.items");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, target) == 48, "KilnStorageEvent.target");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, source) == 52, "KilnStorageEvent.source");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, slot) == 56, "KilnStorageEvent.slot");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, reserved) == 60, "KilnStorageEvent.reserved");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, revision) == 64, "KilnStorageEvent.revision");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, join) == 72, "KilnStorageEvent.join");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, tag) == 80, "KilnStorageEvent.tag");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, account) == 88, "KilnStorageEvent.account");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, character) == 96, "KilnStorageEvent.character");
KILN_TEST_ASSERT(offsetof(KilnStorageEvent, key) == 112, "KilnStorageEvent.key");

KILN_TEST_ASSERT(sizeof(KilnItemsStorageStatus) == 208 && KILN_TEST_ALIGNOF(KilnItemsStorageStatus) == 8, "KilnItemsStorageStatus");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, size) == 0, "KilnItemsStorageStatus.size");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, storage) == 4, "KilnItemsStorageStatus.storage");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, persist) == 5, "KilnItemsStorageStatus.persist");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, session) == 6, "KilnItemsStorageStatus.session");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, stage_point) == 7, "KilnItemsStorageStatus.stage_point");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, reason) == 8, "KilnItemsStorageStatus.reason");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, registrations) == 12, "KilnItemsStorageStatus.registrations");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, holders) == 16, "KilnItemsStorageStatus.holders");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, parked) == 20, "KilnItemsStorageStatus.parked");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, quarantined) == 24, "KilnItemsStorageStatus.quarantined");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, batches) == 28, "KilnItemsStorageStatus.batches");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, captures) == 32, "KilnItemsStorageStatus.captures");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, restores) == 40, "KilnItemsStorageStatus.restores");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageStatus, detail) == 48, "KilnItemsStorageStatus.detail");

KILN_TEST_ASSERT(sizeof(KilnStorageReturnResult) == 208 && KILN_TEST_ALIGNOF(KilnStorageReturnResult) == 8, "KilnStorageReturnResult");
KILN_TEST_ASSERT(offsetof(KilnStorageReturnResult, size) == 0, "KilnStorageReturnResult.size");
KILN_TEST_ASSERT(offsetof(KilnStorageReturnResult, status) == 4, "KilnStorageReturnResult.status");
KILN_TEST_ASSERT(offsetof(KilnStorageReturnResult, ticket) == 8, "KilnStorageReturnResult.ticket");
KILN_TEST_ASSERT(offsetof(KilnStorageReturnResult, player) == 16, "KilnStorageReturnResult.player");
KILN_TEST_ASSERT(offsetof(KilnStorageReturnResult, target) == 20, "KilnStorageReturnResult.target");
KILN_TEST_ASSERT(offsetof(KilnStorageReturnResult, requested) == 24, "KilnStorageReturnResult.requested");
KILN_TEST_ASSERT(offsetof(KilnStorageReturnResult, done) == 28, "KilnStorageReturnResult.done");
KILN_TEST_ASSERT(offsetof(KilnStorageReturnResult, stacks) == 32, "KilnStorageReturnResult.stacks");
KILN_TEST_ASSERT(offsetof(KilnStorageReturnResult, kept) == 36, "KilnStorageReturnResult.kept");
KILN_TEST_ASSERT(offsetof(KilnStorageReturnResult, steps) == 40, "KilnStorageReturnResult.steps");
KILN_TEST_ASSERT(offsetof(KilnStorageReturnResult, reserved) == 44, "KilnStorageReturnResult.reserved");
KILN_TEST_ASSERT(offsetof(KilnStorageReturnResult, message) == 48, "KilnStorageReturnResult.message");

KILN_TEST_ASSERT(sizeof(KilnItemsStorageApi) == 128 && KILN_TEST_ALIGNOF(KilnItemsStorageApi) == 8, "KilnItemsStorageApi");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, header) == 0, "KilnItemsStorageApi.header");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, register_storage) == 8, "KilnItemsStorageApi.register_storage");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, holder) == 16, "KilnItemsStorageApi.holder");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, state) == 24, "KilnItemsStorageApi.state");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, set_ref) == 32, "KilnItemsStorageApi.set_ref");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, status) == 40, "KilnItemsStorageApi.status");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, list) == 48, "KilnItemsStorageApi.list");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, next_event) == 56, "KilnItemsStorageApi.next_event");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, return_parked) == 64, "KilnItemsStorageApi.return_parked");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, persist_bind) == 72, "KilnItemsStorageApi.persist_bind");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, take_frozen) == 80, "KilnItemsStorageApi.take_frozen");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, release_frozen) == 88, "KilnItemsStorageApi.release_frozen");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, witness_of) == 96, "KilnItemsStorageApi.witness_of");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, restore) == 104, "KilnItemsStorageApi.restore");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, park) == 112, "KilnItemsStorageApi.park");
KILN_TEST_ASSERT(offsetof(KilnItemsStorageApi, unpark) == 120, "KilnItemsStorageApi.unpark");

KILN_TEST_ASSERT(KILN_ITEMS_STORAGE_VERSION == 1u, "kiln.items.storage v1");
KILN_TEST_ASSERT(sizeof(((KilnStorageState*)0)->key) == KIS_KEY_MAX && sizeof(((KilnStorageEntry*)0)->key) == KIS_KEY_MAX &&
                     sizeof(((KilnStorageEvent*)0)->key) == KIS_KEY_MAX && KIS_KEY_MAX == 128u, "key buffers");
KILN_TEST_ASSERT(sizeof(((KilnStorageRecord*)0)->data) == KIS_RECORD_MAX && KIS_RECORD_MAX == 64u, "record bytes");
KILN_TEST_ASSERT(KIS_MAX_SLOTS == 64u && KIS_MAX_PER_MOD == 8u && KIS_MAX_SLOTS_PLAYER == 256u && KIS_MAX_FILTER_IDS == 1024u &&
                     KIS_MAX_STORAGES == 32u && KIS_MAX_ENTRIES == 128u && KIS_MAX_RECORDS == 2048u, "limits");
KILN_TEST_ASSERT(KIS_DEATH_KEEP == 0 && KIS_DEATH_GRAVE == 1 && KIS_DEATH_FOLLOW_GAME == 2, "death rules");
KILN_TEST_ASSERT(KIS_FILTER_AMMO_ARROW == 1u && KIS_FILTER_AMMO_SPELL == 2u && KIS_FILTER_NO_PIDE == 4u, "filter flags");
KILN_TEST_ASSERT(KIS_STATE_NONE == 0 && KIS_STATE_WAITING == 1 && KIS_STATE_READY == 2 && KIS_STATE_PARKED == 3 &&
                     KIS_STATE_LOCKED == 4, "states");
KILN_TEST_ASSERT(KIS_SOURCE_NONE == 0 && KIS_SOURCE_NEW == 1 && KIS_SOURCE_SERVER == 2 && KIS_SOURCE_CLIENT == 3, "sources");
KILN_TEST_ASSERT(KIS_ENTRY_LIVE == 1 && KIS_ENTRY_PARKED == 2 && KIS_ENTRY_QUARANTINE == 3, "entry kinds");
KILN_TEST_ASSERT(KIS_PARK_UNREGISTERED == 1 && KIS_PARK_OVERFLOW == 2 && KIS_PARK_FILTER == 3 && KIS_PARK_HELD == 16 &&
                     KIS_PARK_REQUESTED == 16 && KIS_PARK_STALE == 17 && KIS_PARK_LOCKED == 18 && KIS_PARK_RETURNED == 19,
                 "park reasons");
KILN_TEST_ASSERT(KIS_QUARANTINE_UNKNOWN_ITEM == 1 && KIS_QUARANTINE_COUNT == 2 && KIS_QUARANTINE_SLOT == 3 &&
                     KIS_QUARANTINE_DECODE == 4 && KIS_QUARANTINE_REBUILD == 5, "quarantine reasons");
KILN_TEST_ASSERT(KIS_REF_RANGED == 0 && KIS_REF_ARROW == 1 && KIS_REF_SPELL == 2 && KIS_REF_BAIT == 3 && KIS_REF_COUNT == 4 &&
                     KIS_REF_NONE == 0xFFFFu && KIS_REF_CLEAR == 0xFFFFFFFFu, "references");
KILN_TEST_ASSERT(KIS_RECORD_LEVEL == 0x01u && KIS_RECORD_DURABILITY == 0x02u && KIS_RECORD_LIQUID == 0x04u &&
                     KIS_RECORD_DYE == 0x08u && KIS_RECORD_UPGRADES == 0x10u && KIS_RECORD_GEM == 0x20u, "record flags");
KILN_TEST_ASSERT(KIS_DOC_PAIRED == 1u && KIS_DOC_LOCKED == 2u, "document flags");
KILN_TEST_ASSERT(KIS_CAPTURE_UNRESTORED == 1u && KIS_CAPTURE_LOCKED == 2u && KIS_CAPTURE_REFUSED == 4u &&
                     KIS_CAPTURE_LEFT == 8u, "capture flags");
KILN_TEST_ASSERT(KIS_BATCH_COMPLETE == 1u && KIS_BATCH_REFUSED == 2u, "batch flags");
KILN_TEST_ASSERT(KIS_WITNESS_READ == 1u && KIS_WITNESS_APPLIED == 2u && KIS_WITNESS_RESTORED == 4u && KIS_WITNESS_LEFT == 8u &&
                     KIS_WITNESS_EXTRA_MAIN_PAGES == 0x10u, "witness flags");
KILN_TEST_ASSERT(KIS_EVENT_JOINED == 1 && KIS_EVENT_CAPTURED == 2 && KIS_EVENT_RESTORED == 3 && KIS_EVENT_PARKED == 4 &&
                     KIS_EVENT_QUARANTINED == 5 && KIS_EVENT_DEATH == 6 && KIS_EVENT_LEFT == 7 && KIS_EVENT_RETURNED == 8 &&
                     KIS_EVENT_BATCH == 9, "event kinds");
KILN_TEST_ASSERT(KIS_EVENT_PAIRED == 1u && KIS_EVENT_LIVE == 2u, "event flags");
/* the two kiln.items values: past kiln.items' own (find flags 0x01-0x08, result status 0-9) */
KILN_TEST_ASSERT(KILN_ITEMS_FIND_PLAYER_STORAGE == 0x10u &&
                     (KILN_ITEMS_FIND_PLAYER_STORAGE & (KILN_ITEMS_FIND_CONTAINERS | KILN_ITEMS_FIND_PLAYERS |
                                                        KILN_ITEMS_FIND_NPCS | KILN_ITEMS_FIND_NONEMPTY)) == 0,
                 "find flag");
KILN_TEST_ASSERT(KILN_ITEMS_FILTERED == 10 && KILN_ITEMS_FILTERED > KILN_ITEMS_DROPPED, "filtered status");

int kiln_items_storage_abi_test_anchor;
