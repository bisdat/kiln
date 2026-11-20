// SPDX-License-Identifier: MIT
// kiln.engine identity and generated profiles: exact selection, every identity component, and the generated layout
// checked field by field against the verbatim profile leaves (catches an emit-order slip in the generator).
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "identity.h"
#include "profiles.g.h"

namespace {
using namespace kiln::engine;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}
constexpr const Profile* kAll = generated::kProfiles;
constexpr std::size_t kCount = sizeof generated::kProfiles / sizeof generated::kProfiles[0];

const Profile& by_target(std::string_view target) {
    for (std::size_t i = 0; i < kCount; ++i)
        if (target == kAll[i].target) return kAll[i];
    std::fprintf(stderr, "no profile for %.*s\n", static_cast<int>(target.size()), target.data());
    std::abort();
}
const Leaf* leaf(const Profile& p, std::string_view key) {
    std::size_t lo = 0, hi = p.leaf_count;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        const int c = std::string_view(p.leaves[mid].key).compare(key);
        if (c == 0) return &p.leaves[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}
std::uint64_t u(const Profile& p, std::string_view key) {
    const Leaf* l = leaf(p, key);
    check(l && l->kind == LeafKind::U64, std::string(p.id) + ": leaf " + std::string(key));
    return l ? l->value : ~0ull;
}
ImageFacts exact(const Profile& p) {
    ImageFacts f;
    f.headers = true; f.target = p.target; f.sha256 = p.sha256; f.timestamp = p.timestamp; f.size_of_image = p.size_of_image;
    f.archive_format = 4;
    return f;
}

void identity_components() {
    for (std::size_t i = 0; i < kCount; ++i) {
        const Profile& p = kAll[i];
        const std::string id = p.id;
        auto s = select(exact(p), kAll, kCount);
        check(s.status == KILN_ENGINE_EXACT && s.profile == &p, id + ": exact image selects its profile");
        auto f = exact(p); f.sha256[0] = f.sha256[0] == '0' ? '1' : '0';
        check(select(f, kAll, kCount).status == KILN_ENGINE_UNKNOWN_BUILD, id + ": another sha256 is an unknown build");
        f = exact(p); for (auto& c : f.sha256) c = static_cast<char>(std::toupper(c));
        check(!select(f, kAll, kCount).profile, id + ": the hash is compared as lower-case hex only");
        f = exact(p); f.sha256.clear();
        check(select(f, kAll, kCount).status == KILN_ENGINE_UNKNOWN_BUILD, id + ": no file hash never matches");
        f = exact(p); ++f.timestamp;
        check(!select(f, kAll, kCount).profile, id + ": TimeDateStamp must match");
        f = exact(p); f.size_of_image += 0x1000;
        check(!select(f, kAll, kCount).profile, id + ": SizeOfImage must match (not the file length)");
        f = exact(p); f.target = std::string(p.target) == "enshrouded.exe" ? "enshrouded_server.exe" : "enshrouded.exe";
        check(!select(f, kAll, kCount).profile, id + ": the other side's exe name never matches");
        f = exact(p); f.target = "Enshrouded.exe";
        check(!select(f, kAll, kCount).profile, id + ": the target is the profile's exact file name");
        for (std::uint32_t kfc : {0u, 3u}) {
            f = exact(p); f.archive_format = kfc;
            check(!select(f, kAll, kCount).profile, id + ": a KFC" + std::to_string(kfc) + " process archive never matches");
        }
        f = exact(p); f.headers = false;
        check(select(f, kAll, kCount).status == KILN_ENGINE_UNIDENTIFIED, id + ": unreadable headers are unidentified");
    }
    // Two identical profiles: ShroudForge refuses an ambiguous exact match (select_resource_profile.cpp:56-60).
    const Profile twice[] = {kAll[0], kAll[0]};
    const auto s = select(exact(kAll[0]), twice, 2);
    check(s.status == KILN_ENGINE_AMBIGUOUS && !s.profile, "two exact matches are refused");
    unsigned char k4[] = {'K', 'F', 'C', '4'}, k3[] = {'K', 'F', 'C', '3'}, bad[] = {'K', 'F', 'C', '5'};
    check(archive_format_of(k4, 4) == 4 && archive_format_of(k3, 4) == 3 && !archive_format_of(bad, 4) && !archive_format_of(k4, 3),
          "KFC header formats");
}

void layouts_match_leaves() {
    for (std::size_t i = 0; i < kCount; ++i) {
        const Profile& p = kAll[i];
        const Layout& l = p.layout;
        const std::string id = p.id;
        const auto eq = [&](std::uint64_t got, std::string_view key) {
            check(got == u(p, key), id + ": layout field from " + std::string(key));
        };
        check(l.image_size == p.size_of_image && p.size_of_image == u(p, "image.size"), id + ": image size");
        check(p.timestamp == u(p, "image.timestamp") && std::string_view(p.sha256) == leaf(p, "image.sha256")->text, id + ": image");
        check(l.dedicated_server == (std::string_view(p.target) == "enshrouded_server.exe"), id + ": dedicated flag");
        eq(l.root_rva, "ownerLayout.rootObjectRva"); eq(l.root_vtable, "ownerLayout.rootVtableRva");
        eq(l.client_vtable, "ownerLayout.clientVtableRva"); eq(l.controller_vtable, "ownerLayout.controllerVtableRva");
        eq(l.client_offset, "ownerLayout.clientPointerOffset"); eq(l.controller_offset, "ownerLayout.controllerPointerOffset");
        eq(l.controller_back_pointer, "ownerLayout.controllerClientBackPointerOffset");
        eq(l.main_owner_offset, "ownerLayout.controllerMainOwnerOffset");
        check(l.ecs && l.authority, id + ": ECS read and authority layouts");
        eq(l.execution_owner_offset, "ecsReadLayout.mainExecutionOwnerOffset");
        eq(l.registry_owner_offset, "ecsReadLayout.executionRegistryOwnerOffset");
        eq(l.entity_manager_offset, "ecsReadLayout.executionEntityManagerOffset");
        eq(l.manager_owner_offset, "ecsReadLayout.entityManagerOwnerOffset");
        eq(l.client_scene_offset, "ecsReadLayout.clientSceneOwnerOffset");
        eq(l.scene_entries_offset, "ecsReadLayout.sceneEntriesPointerOffset");
        eq(l.scene_count_offset, "ecsReadLayout.sceneEntriesCountOffset");
        eq(l.scene_capacity_offset, "ecsReadLayout.sceneEntriesCapacityOffset");
        eq(l.entry_stride, "ecsReadLayout.sceneEntryStride"); eq(l.entry_context_offset, "ecsReadLayout.sceneEntryContextOffset");
        eq(l.entry_uuid_offset, "ecsReadLayout.sceneEntryUuidOffset"); eq(l.entry_state_offset, "ecsReadLayout.sceneEntryStateOffset");
        eq(l.maximum_scenes, "ecsReadLayout.maximumSceneEntries"); eq(l.entry_ready_state, "ecsReadLayout.sceneEntryReadyState");
        const EcsLayout& e = l.entities;
        eq(e.manager_count, "ecsReadLayout.entityManagerCount"); eq(e.manager_table, "ecsReadLayout.entityManagerTable");
        eq(e.manager_occupancy, "ecsReadLayout.entityManagerOccupancy"); eq(e.manager_keys, "ecsReadLayout.entityManagerKeys");
        eq(e.entity_id, "ecsReadLayout.entityId"); eq(e.entity_generation, "ecsReadLayout.entityGeneration");
        eq(e.entity_layout, "ecsReadLayout.entityLayout"); eq(e.entity_storage, "ecsReadLayout.entityStorage");
        eq(e.entity_definition, "ecsReadLayout.entityDefinitionPointer"); eq(e.entity_row, "ecsReadLayout.entityRow");
        check(e.entity_definition_uuid_validated, id + ": definition uuid offset present");
        eq(e.entity_definition_uuid, "ecsReadLayout.entityDefinitionUuidOffset");
        eq(e.component_bits, "ecsReadLayout.componentBits"); eq(e.component_offsets, "ecsReadLayout.componentOffsets");
        eq(e.component_strides, "ecsReadLayout.componentStrides"); eq(e.maximum_entities, "ecsReadLayout.maximumEntities");
        const WriteLayout& w = l.write;
        check(w.enabled, id + ": write gate enabled");
        eq(w.registry_lock, "ecsWriteLayout.threadRegistryLockRva"); eq(w.registry_entries, "ecsWriteLayout.threadRegistryEntriesRva");
        eq(w.registry_count, "ecsWriteLayout.threadRegistryCountRva"); eq(w.callback, "ecsWriteLayout.threadCallbackRva");
        eq(w.capacity, "ecsWriteLayout.threadRegistryCapacity"); eq(w.stride, "ecsWriteLayout.threadRegistryEntryStride");
        eq(w.row_handle, "ecsWriteLayout.registryEntryHandleOffset"); eq(w.row_name, "ecsWriteLayout.registryEntryNamePointerOffset");
        eq(w.row_length, "ecsWriteLayout.registryEntryNameLengthOffset"); eq(w.inline_name, "ecsWriteLayout.threadInlineNameOffset");
        eq(w.scene_thread, "ecsWriteLayout.sceneThreadOffset"); eq(w.name_pointer, "ecsWriteLayout.threadNamePointerOffset");
        eq(w.name_length, "ecsWriteLayout.threadNameLengthOffset"); eq(w.name_capacity, "ecsWriteLayout.threadNameCapacityOffset");
        eq(w.name_capacity_value, "ecsWriteLayout.threadNameCapacity"); eq(w.thread_handle, "ecsWriteLayout.threadHandleOffset");
        eq(w.thread_context, "ecsWriteLayout.threadContextOffset"); eq(w.thread_stop, "ecsWriteLayout.threadStopOffset");
        eq(w.thread_callback, "ecsWriteLayout.threadCallbackOffset"); eq(w.scene_context, "ecsWriteLayout.sceneContextOffset");
        eq(w.scene_lock, "ecsWriteLayout.sceneLockOffset"); eq(w.scene_state, "ecsWriteLayout.sceneStateOffset");
        eq(w.execution_mode, "ecsWriteLayout.executionModeOffset"); eq(w.maximum_bytes, "ecsWriteLayout.maximumWriteBytes");
        eq(w.running_state, "ecsWriteLayout.sceneRunningState"); eq(w.authority_mode, "ecsWriteLayout.executionAuthorityMode");
        eq(w.tracker, "ecsWriteLayout.changeTrackerRva"); eq(w.tracking_context, "ecsWriteLayout.changeTrackerContextOffset");
        eq(w.tracking_manager, "ecsWriteLayout.changeTrackerManagerOffset"); eq(w.tracking_queue_b, "ecsWriteLayout.changeTrackerQueueBOffset");
        eq(w.maximum_tracking_capacity, "ecsWriteLayout.maximumTrackingQueueCapacity");
        eq(w.tracking_registry_a, "ecsWriteLayout.changeTrackerRegistryAOffset");
        eq(w.tracking_registry_b, "ecsWriteLayout.changeTrackerRegistryBOffset");
        const Leaf* name = leaf(p, "ecsWriteLayout.threadName");
        check(name && w.name_size == std::strlen(name->text) && std::memcmp(w.name.data(), name->text, w.name_size) == 0 &&
                  w.name[w.name_size] == 0, id + ": thread name");
        const Leaf* bytes = leaf(p, "ecsWriteLayout.changeTrackerBytes");
        std::string hex;
        for (auto b : w.tracker_bytes) { char t[3]; std::snprintf(t, sizeof t, "%02x", b); hex += t; }
        check(bytes && hex == bytes->text, id + ": change tracker guard bytes");
        const QueueLayout& q = l.queue;
        check(q.present, id + ": command queue layout");
        eq(q.task_state, "entityCommandQueueLayout.taskStateOffset"); eq(q.task_records, "entityCommandQueueLayout.taskRecordsOffset");
        eq(q.task_count, "entityCommandQueueLayout.taskCountOffset"); eq(q.queue_records, "entityCommandQueueLayout.queueRecordsOffset");
        eq(q.queue_count, "entityCommandQueueLayout.queueCountOffset"); eq(q.task_record_stride, "entityCommandQueueLayout.taskRecordStride");
        eq(q.entity_id_counter, "entityCommandQueueLayout.entityIdCounterOffset");
        eq(q.active_tracking_contexts, "entityCommandQueueLayout.activeTrackingContextsOffset");
        eq(q.buffer_capacity, "entityCommandQueueLayout.bufferCapacity");
        check(q.shared_tracker == w.tracking_context && q.manager_owner == l.manager_owner_offset &&
                  q.execution_mode == w.execution_mode && q.authority_mode == w.authority_mode,
              id + ": queue fields derived from the write layout (configure_engine_layout.cpp:224-227)");
        check(l.spawn.present, id + ": spawn layout");
        eq(l.spawn.source_entity_ids, "buildingPropSpawnLayout.sourceEntityIdsOffset");
        eq(l.spawn.allocation_failure_marker, "buildingPropSpawnLayout.allocationFailureMarkerValue");
        const ResourceWriteLayout& r = l.resource_write;
        check(r.enabled, id + ": resource write enabled");
        eq(r.service_thread, "resourceWriteLayout.serviceThreadOffset"); eq(r.callback, "resourceWriteLayout.threadCallbackRva");
        eq(r.initialized, "resourceWriteLayout.serviceInitializedOffset");
        eq(r.initialized_value, "resourceWriteLayout.serviceInitializedValue");
        eq(r.reader, "resourceLayout.resourceServiceReaderOffset");
        eq(r.payload_pointer, "resourceWriteLayout.servicePayloadPointerOffset");
        eq(r.payload_size, "resourceWriteLayout.servicePayloadSizeOffset");
        const Leaf* rname = leaf(p, "resourceWriteLayout.threadName");
        check(rname && r.name_size == std::strlen(rname->text) && std::memcmp(r.name.data(), rname->text, r.name_size) == 0,
              id + ": resource worker name");
        for (std::size_t k = 1; k < p.leaf_count; ++k)
            check(std::string_view(p.leaves[k - 1].key) < p.leaves[k].key, id + ": leaves sorted and unique");
    }
}

void describe_strings() {
    const Profile& p = by_target("enshrouded.exe");
    auto f = exact(p); f.target = std::string(60, 'x');
    KilnEngineIdentity id{};
    describe(f, select(exact(p), kAll, kCount), id);
    check(id.size == sizeof id && id.status == KILN_ENGINE_EXACT && std::string_view(id.profile_id) == p.id, "identity record");
    check(std::strlen(id.target) == sizeof id.target - 1 && std::string_view(id.sha256) == p.sha256, "identity strings cut and terminated");
    check(id.sections == p.sections && id.schema_version == p.schema_version && id.archive_format == 4, "identity profile facts");
}
}  // namespace

int main() {
    identity_components();
    layouts_match_leaves();
    describe_strings();
    std::printf("engine identity: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
