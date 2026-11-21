// SPDX-License-Identifier: MIT
// A fake engine laid out from a real ported profile's offsets (host tests): the thread registry with its "GameServer"
// row, the registered Scene (thread object, ready-entry list, Scene word, state byte), the world context, execution
// owners with their component registry and entity manager, and on a client the root/client/controller owner chain.
// add_content_worker() also registers the resource content worker: its service object with the worker thread
// inside it, a reader and a bulk payload; without it the registry is the gate tests' (MainThread, GameServer).
// Every region is anonymous mmap memory, so untouched pages cost nothing (an execution owner spans 14 MB).
#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "include/kiln_engine_layout.h"

namespace fake {

struct Region {
    std::uint8_t* base = nullptr;
    std::size_t size = 0;
    explicit Region(std::size_t bytes) : size((bytes + 4095) & ~std::size_t{4095}) {
#ifdef _WIN32
        void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) throw std::runtime_error("VirtualAlloc");
#else
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) throw std::runtime_error("mmap");
#endif
        base = static_cast<std::uint8_t*>(p);
    }
#ifdef _WIN32
    ~Region() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
#else
    ~Region() { if (base) munmap(base, size); }
#endif
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;
    std::uintptr_t at(std::size_t offset = 0) const { return reinterpret_cast<std::uintptr_t>(base) + offset; }
};

template <class T> void put(std::uintptr_t address, T value) { std::memcpy(reinterpret_cast<void*>(address), &value, sizeof value); }
template <class T> T get(std::uintptr_t address) { T v; std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof v); return v; }
inline std::int32_t* word(std::uintptr_t address) { return reinterpret_cast<std::int32_t*>(address); }

// An unreadable address: a reserved PROT_NONE page kept for the process (an unmapped one could be reused by the
// sanitizers' allocators).
inline std::uintptr_t unmapped() {
    static std::uintptr_t address = [] {
#ifdef _WIN32
        void* p = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
#else
        void* p = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        return reinterpret_cast<std::uintptr_t>(p);
    }();
    return address;
}

struct Engine {
    const kiln::engine::Layout& L;
    Region image, scene, entries, context, exec, table{4096}, names{4096};
    Region client{L.dedicated_server ? 4096 : L.controller_offset + 4096}, controller{4096}, main_world{4096},
        main_exec{L.dedicated_server ? 4096 : L.entity_manager_offset + 4096}, main_table{4096};
    // the resource content worker: the service (worker thread at resource_write.service_thread), its reader, its payload
    Region content{content_bytes(L)}, reader_object{4096}, payload{65536};
    std::uintptr_t thread = 0, handle = 0x1234, content_handle = 0x5678;
    int content_rows = 0;  // registry rows of the content worker that set_rows writes: 0 (not registered), 1, or 2

    static std::size_t content_bytes(const kiln::engine::Layout& layout) {
        const auto& r = layout.resource_write;
        const auto& w = layout.write;
        std::size_t end = r.service_thread + w.inline_name + 64;
        for (std::size_t field : {r.initialized + 4, r.reader + 8, r.payload_pointer + 8, r.payload_size + 8,
                                  r.service_thread + w.name_pointer + 8, r.service_thread + w.name_length + 8,
                                  r.service_thread + w.name_capacity + 8, r.service_thread + w.thread_handle + 8,
                                  r.service_thread + w.thread_context + 8, r.service_thread + w.thread_stop + 4,
                                  r.service_thread + w.thread_callback + 8})
            end = field > end ? field : end;
        return end;
    }

    explicit Engine(const kiln::engine::Layout& layout)
        : L(layout), image(layout.image_size), scene(8192), entries(layout.entry_stride * 4), context(4096),
          exec(layout.entity_manager_offset + layout.write.tracking_context + layout.write.tracking_manager + 4096) {
        const auto& w = L.write;
        // registry: a decoy row, then GameServer (thread object inside the Scene, scene_thread bytes in)
        thread = scene.at(w.scene_thread);
        set_rows(true);
        // thread object
        put<std::uint64_t>(thread + w.name_pointer, thread + w.inline_name);
        put<std::uint64_t>(thread + w.name_length, w.name_size);
        put<std::uint64_t>(thread + w.name_capacity, w.name_capacity_value);
        std::memcpy(reinterpret_cast<void*>(thread + w.inline_name), w.name.data(), w.name_size);
        put<std::uint64_t>(thread + w.thread_handle, handle);
        put<std::uint64_t>(thread + w.thread_context, scene.at(w.scene_context));
        put<std::uint32_t>(thread + w.thread_stop, 0);
        put<std::uint64_t>(thread + w.thread_callback, image.at(w.callback));
        put<std::uint8_t>(scene.at(w.scene_state), w.running_state);
        // the Scene's ready-entry list: one ready entry whose context is the world context
        put<std::uint64_t>(scene.at(L.scene_entries_offset), entries.at());
        put<std::uint64_t>(scene.at(L.scene_count_offset), 1);
        put<std::uint64_t>(scene.at(L.scene_capacity_offset), 4);
        set_entry(0, L.entry_ready_state, context.at(), {0x1111, 0x2222});
        // execution owners: authority (from the world context) and, on a client, the main world's
        wire_exec(context, exec, table);
        // the change tracker: its guard bytes at its RVA, and its context's manager back pointer
        std::memcpy(reinterpret_cast<void*>(image.at(w.tracker)), w.tracker_bytes.data(), w.tracker_bytes.size());
        put<std::uint64_t>(manager() + w.tracking_context + w.tracking_manager, manager());
        if (!L.dedicated_server) {
            put<std::uint64_t>(image.at(L.root_rva), image.at(L.root_vtable));
            put<std::uint64_t>(image.at(L.root_rva) + L.client_offset, client.at());
            put<std::uint64_t>(client.at(), image.at(L.client_vtable));
            put<std::uint64_t>(client.at(L.controller_offset), controller.at());
            put<std::uint64_t>(controller.at(), image.at(L.controller_vtable));
            put<std::uint64_t>(controller.at(L.controller_back_pointer), client.at());
            put<std::uint64_t>(controller.at(L.main_owner_offset), main_world.at());
            put<std::uint64_t>(client.at(L.client_scene_offset), scene.at());
            wire_exec(main_world, main_exec, main_table);
        }
    }

    void wire_exec(const Region& owner, const Region& execution, const Region& entities) {
        put<std::uint64_t>(owner.at(L.execution_owner_offset), execution.at());
        const std::uintptr_t registry = execution.at(L.registry_owner_offset);
        const std::uintptr_t manager = execution.at(L.entity_manager_offset);
        put<std::uint64_t>(registry + 16, 128);  // registered components
        put<std::uint64_t>(manager + L.manager_owner_offset, registry);
        put<std::uint64_t>(manager + L.entities.manager_count, 3);
        put<std::uint64_t>(manager + L.entities.manager_table, entities.at());
    }

    void set_rows(bool server_row, bool twice = false) {
        const auto& w = L.write;
        std::uint16_t count = 0;
        const auto row = [&](std::uintptr_t name, std::uint64_t length, std::uint64_t h) {
            const std::uintptr_t r = image.at(w.registry_entries) + count * w.stride;
            put<std::uint64_t>(r + w.row_handle, h);
            put<std::uint64_t>(r + w.row_name, name);
            put<std::uint64_t>(r + w.row_length, length);
            ++count;
        };
        std::memcpy(names.base, "MainThread", 10);
        row(names.at(), 10, 0x99);
        if (server_row) row(thread + w.inline_name, w.name_size, handle);
        if (twice) row(thread + w.inline_name, w.name_size, handle);
        for (int i = 0; i < content_rows; ++i)
            row(content_thread() + w.inline_name, L.resource_write.name_size, content_handle);
        put<std::uint16_t>(image.at(w.registry_count), count);
    }

    void set_entry(std::size_t index, std::uint8_t state, std::uintptr_t ctx, std::array<std::uint64_t, 2> uuid) {
        const std::uintptr_t e = entries.at(index * L.entry_stride);
        put<std::uint64_t>(e + L.entry_context_offset, ctx);
        put<std::array<std::uint64_t, 2>>(e + L.entry_uuid_offset, uuid);
        put<std::uint8_t>(e + L.entry_state_offset, state);
    }

    // Registers the content worker (`rows` registry rows; 2 makes it ambiguous) with a valid thread and service.
    void add_content_worker(int rows = 1) {
        const auto& w = L.write;
        const auto& r = L.resource_write;
        const std::uintptr_t t = content_thread();
        put<std::uint64_t>(t + w.name_pointer, t + w.inline_name);
        put<std::uint64_t>(t + w.name_length, r.name_size);
        put<std::uint64_t>(t + w.name_capacity, w.name_capacity_value);
        std::memcpy(reinterpret_cast<void*>(t + w.inline_name), r.name.data(), r.name_size);
        put<std::uint64_t>(t + w.thread_handle, content_handle);
        put<std::uint64_t>(t + w.thread_context, t);  // the content worker's context is its own thread object
        put<std::uint32_t>(t + w.thread_stop, 0);
        put<std::uint64_t>(t + w.thread_callback, image.at(r.callback));
        put<std::uint32_t>(service() + r.initialized, r.initialized_value);
        put<std::uint64_t>(service() + r.reader, reader());
        put<std::uint64_t>(service() + r.payload_pointer, payload.at());
        put<std::uint64_t>(service() + r.payload_size, payload.size);
        content_rows = rows;
        set_rows(true);
    }
    std::uintptr_t service() const { return content.at(); }
    std::uintptr_t content_thread() const { return content.at(L.resource_write.service_thread); }
    std::uintptr_t reader() const { return reader_object.at(); }
    std::uintptr_t row(std::size_t index) const { return image.at(L.write.registry_entries) + index * L.write.stride; }
    std::uintptr_t registry_word() const { return image.at(L.write.registry_lock); }
    std::uintptr_t scene_word() const { return scene.at(L.write.scene_lock); }
    std::uintptr_t registry_owner() const { return exec.at(L.registry_owner_offset); }
    std::uintptr_t manager() const { return exec.at(L.entity_manager_offset); }
};

}  // namespace fake
