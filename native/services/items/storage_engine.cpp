// SPDX-License-Identifier: MIT
// Game-memory adapters. Structural calls require the no-jobs Post gate; inventory writes use program accessors.
#include "storage_engine.h"
#include <windows.h>
namespace kitems::storage {
EngineStorage S;
namespace {
template <class T> T read(const void* p, size_t at) {
    T v{};
    std::memcpy(&v, static_cast<const uint8_t*>(p) + at, sizeof v);
    return v;
}
template <class T> void put(void* p, size_t at, T v) { std::memcpy(static_cast<uint8_t*>(p) + at, &v, sizeof v); }
bool copy(void* out, const void* in, size_t n) {
    __try {
        if (!in)
            return false;
        std::memcpy(out, in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void* component(uintptr_t world, uint32_t hash, uint32_t entity, size_t size) {
    PtrSize p{};
    __try {
        E.get_component(&p, world, hash, entity);
        return p.size >= size ? p.ptr : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
uint32_t make(void* storage, const CreateDesc* d) {
    __try {
        return S.create(storage, d);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
bool remove(void* storage, uint32_t entity) {
    __try {
        return S.destroy(storage, entity, 1, 0) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool write_record(const uint8_t record[56], uint8_t* out, uint8_t* length) {
    __try {
        uint64_t buffer[]{reinterpret_cast<uint64_t>(out), KIS_RECORD_MAX, 0};
        uint64_t ptr = reinterpret_cast<uint64_t>(buffer);
        S.write(&ptr, record);
        if (buffer[2] < 8 || buffer[2] > KIS_RECORD_MAX)
            return false;
        *length = static_cast<uint8_t>(buffer[2]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool convert_record(uint8_t record[56], const void* stack, uint8_t slot, void* attrs, void* bindings, EntityRef* ref) {
    __try {
        Result8 out{};
        S.convert(&out, record, stack, slot, attrs, bindings, ref);
        return !out.error;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool read_record(const uint8_t* data, size_t length, uint8_t out[56]) {
    __try {
        uint64_t buffer[]{reinterpret_cast<uint64_t>(data), length, 0, 0, 0, 0, 0};
        uint64_t ptr = reinterpret_cast<uint64_t>(buffer);
        std::memset(out, 0, 56);
        S.read(out, &ptr);
        return buffer[3] == length && !(buffer[4] & 255u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint32_t rebuild_record(const uint8_t record[56], uint32_t owner, const void* info, const void* ctx) {
    __try {
        return S.rebuild(record, owner, info, ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
void* item_info(uint32_t item, void* registry) {
    __try {
        return E.item_find(item, registry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
} // namespace
bool item_traits(void* registry, uint32_t item, ItemTraits* out) {
    auto info = static_cast<const uint8_t*>(item_info(item, registry));
    if (!info)
        return false;
    uint32_t flags = 0;
    if (!copy(&out->max_stack, info + E.item_max_stack, 2) || !copy(&flags, info + E.item_flags, 4) ||
        !copy(&out->category, info + 0xcc, 1) || !copy(&out->equip, info + 0x104, 1))
        return false;
    out->pide = flags & 2u;
    return true;
}
bool component_copy(uintptr_t world, uint32_t hash, uint32_t e, void* out, size_t size) {
    return copy(out, component(world, hash, e, size), size);
}
bool record_from_memory(const uint8_t* p, Record* out) {
    uint8_t bytes[56]{};
    if (!copy(bytes, p, sizeof bytes))
        return false;
    *out = {};
    out->count = read<uint16_t>(bytes, 0);
    out->slot = bytes[4];
    out->item = read<uint32_t>(bytes, 8);
    const unsigned present[]{3, 20, 32, 40, 7, 52}, offset[]{2, 12, 24, 36, 5, 44};
    for (unsigned i = 0; i < 6; ++i) {
        out->present[i] = bytes[present[i]] != 0;
        if (out->present[i])
            out->options[i].assign(bytes + offset[i], bytes + offset[i] + option_sizes[i]);
    }
    return true;
}
bool memory_from_record(const Record& r, uint8_t* out) {
    std::memset(out, 0, 56);
    put(out, 0, r.count);
    put(out, 4, r.slot);
    put(out, 8, r.item);
    const unsigned present[]{3, 20, 32, 40, 7, 52}, offset[]{2, 12, 24, 36, 5, 44};
    for (unsigned i = 0; i < 6; ++i)
        if (r.present[i]) {
            if (r.options[i].size() != option_sizes[i])
                return false;
            out[present[i]] = 1;
            std::memcpy(out + offset[i], r.options[i].data(), option_sizes[i]);
        }
    return true;
}
bool inventory_from_character(const void* character, Inventory* out) {
    if (!character)
        return false;
    const auto& l = S.layout;
    std::vector<uint8_t> b(static_cast<size_t>(l.list) + 24);
    if (l.list <= 0 || !copy(b.data(), character, b.size()))
        return false;
    *out = {};
    for (unsigned i = 0; i < 16; ++i) {
        auto p = b.data() + l.equip0 + i * l.equip_stride;
        if (p[56]) {
            Record r;
            if (!record_from_memory(p, &r))
                return false;
            out->equipment[i] = std::move(r);
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        auto p = b.data() + l.refs + i * 4;
        if (p[2])
            out->refs[i] = read<uint16_t>(p, 0);
    }
    auto main = b.data() + l.main_pages, extra = b.data() + l.extra_pages;
    out->main_pages = main[0];
    out->extra_pages = extra[0];
    uint32_t first[2]{read<uint32_t>(main, 4), read<uint32_t>(extra, 4)},
        count[2]{read<uint32_t>(main, 8), read<uint32_t>(extra, 8)};
    uint64_t n = read<uint64_t>(b.data(), l.list + 8), cap = read<uint64_t>(b.data(), l.list + 16);
    auto ptr = read<const uint8_t*>(b.data(), l.list);
    if (out->main_pages > 16 || out->extra_pages > 16 || n > 248 || cap < n || uint64_t(count[0]) + count[1] != n ||
        first[0] != 0 || first[1] != count[0] || (n && !ptr))
        return false;
    for (unsigned k = 0; k < 2; ++k) {
        if (first[k] > n || count[k] > n - first[k])
            return false;
        for (uint32_t i = 0; i < count[k]; ++i) {
            Record r;
            if (!record_from_memory(ptr + (first[k] + i) * l.record_stride, &r))
                return false;
            (k ? out->extra : out->main).push_back(std::move(r));
        }
    }
    return true;
}
bool character_witness(const void* p, uint64_t* out, uint32_t baseline, uint32_t* flags) {
    Inventory inv;
    std::vector<uint8_t> bytes;
    if (!out || !inventory_from_character(p, &inv) || !encode(inv, &bytes))
        return false;
    *out = witness(bytes);
    if (flags)
        *flags = KIS_WITNESS_READ | (inv.main_pages > baseline ? KIS_WITNESS_EXTRA_MAIN_PAGES : 0u);
    return true;
}
bool create_holder(const Access& a, uint32_t player, uint32_t slots, Holder* out) {
    *out = {};
    if (!S.structural_gate || !S.structural_gate() || !S.create || !S.destroy || !player || !slots ||
        slots > KIS_MAX_SLOTS)
        return false;
    uint8_t setup[84]{};
    setup[81] = static_cast<uint8_t>((slots + 7) / 8 * 8);
    setup[82] = setup[81];
    setup[83] = 1;
    uint32_t owner = player;
    ComponentDesc cs[]{{kHashInventorySetup, 0, setup, 84}, {0xd7361788u, 0, &owner, 4}};
    CreateDesc d{};
    std::memcpy(d.guid, default_inventory_guid, 16);
    d.flags = 4;
    d.components = cs;
    d.count = 2;
    uint32_t root = make(a.storage, &d);
    if (!root || root <= kMaxPlayer || root == UINT32_MAX)
        return false;
    out->entity = root;
    out->slots = slots;
    owner = root;
    for (uint32_t i = 0; i < (slots + 7) / 8; ++i) {
        uint8_t inv[96]{};
        cs[0] = {kHashInventory, 0, inv, 96};
        uint32_t id = make(a.storage, &d);
        if (!id || id <= kMaxPlayer || id == UINT32_MAX) {
            destroy_holder(a, out);
            *out = {};
            return false;
        }
        out->pages.push_back(id);
        put(setup, i * 4, id);
        setup[64 + i] = 4;
    }
    const auto world = reinterpret_cast<uintptr_t>(a.storage) - E.world_storage;
    auto p = component(world, kHashInventorySetup, root, 84);
    if (!p || !copy(p, setup, 84)) {
        destroy_holder(a, out);
        *out = {};
        return false;
    }
    return true;
}
bool destroy_holder(const Access& a, const Holder* h) {
    if (!S.structural_gate || !S.structural_gate() || !S.destroy)
        return false;
    // Cleanup is only for exact native holders, never a player or a player's own Generic pages.
    if (!h || h->entity <= kMaxPlayer || h->entity == UINT32_MAX)
        return false;
    bool ok = true;
    uintptr_t world = reinterpret_cast<uintptr_t>(a.storage) - E.world_storage;
    uint8_t setup[84]{};
    const bool root_present = component_copy(world, kHashInventorySetup, h->entity, setup, sizeof setup);
    if (root_present && setup[kSetupFlags] != 0)
        return false;
    std::vector<uint32_t> present_pages;
    for (uint32_t page : h->pages) {
        if (page <= kMaxPlayer || page == UINT32_MAX)
            return false;
        uint32_t owner = 0;
        if (!component_copy(world, 0xd7361788u, page, &owner, sizeof owner)) {
            uint8_t inventory[96]{};
            if (component_copy(world, kHashInventory, page, inventory, sizeof inventory))
                return false;
            continue; // The engine may already have removed a leaving player's owned entities.
        }
        if (owner != h->entity)
            return false;
        present_pages.push_back(page);
    }
    for (uint32_t page : present_pages) {
        uint32_t inv[24]{};
        if (component_copy(world, kHashInventory, page, inv, 96))
            for (unsigned i = 0; i < 8; ++i)
                if (inv[3 * i + 2] && inv[3 * i + 2] != UINT32_MAX)
                    ok = remove(a.storage, inv[3 * i + 2]) && ok;
        ok = remove(a.storage, page) && ok;
    }
    if (root_present)
        ok = remove(a.storage, h->entity) && ok;
    return ok;
}
bool insert_record(const Access& a, const Holder& h, const KilnStorageRecord& input, uint32_t* reason) {
    *reason = KIS_QUARANTINE_DECODE;
    if (!S.structural_gate || !S.structural_gate())
        return false;
    Record record;
    if (input.length > KIS_RECORD_MAX || !decode_record(input.data, input.length, &record))
        return false;
    if (record.slot != input.slot || record.item != input.item || record.count != input.count)
        return false;
    uint8_t memory[56]{};
    if (!S.read || !read_record(input.data, input.length, memory))
        return false;
    if (record.slot >= h.slots || record.slot >= h.pages.size() * 8) {
        *reason = KIS_QUARANTINE_SLOT;
        return false;
    }
    auto info = item_info(record.item, a.items);
    if (!info) {
        *reason = KIS_QUARANTINE_UNKNOWN_ITEM;
        return false;
    }
    uint16_t max = 0;
    uint32_t flags = 0;
    if (!copy(&max, static_cast<uint8_t*>(info) + E.item_max_stack, 2) ||
        !copy(&flags, static_cast<uint8_t*>(info) + E.item_flags, 4)) {
        *reason = KIS_QUARANTINE_REBUILD;
        return false;
    }
    if (!record.count || record.count > max || ((flags & 2) && record.count != 1)) {
        *reason = KIS_QUARANTINE_COUNT;
        return false;
    }
    uint32_t page = h.pages[record.slot / 8];
    uintptr_t world = reinterpret_cast<uintptr_t>(a.storage) - E.world_storage;
    auto inv = static_cast<uint8_t*>(component(world, kHashInventory, page, 96));
    uint32_t old[3]{};
    if (!inv || !copy(old, inv + (record.slot % 8) * 12, 12)) {
        *reason = KIS_QUARANTINE_REBUILD;
        return false;
    }
    if (old[0] || old[1] || old[2]) {
        *reason = KIS_QUARANTINE_SLOT;
        return false;
    }
    uint32_t pide = 0;
    if (flags & 2) {
        uint8_t template_guid[16]{};
        if (!copy(template_guid, static_cast<const uint8_t*>(info) + 0x54c, sizeof template_guid) ||
            std::all_of(std::begin(template_guid), std::end(template_guid), [](uint8_t b) { return b == 0; })) {
            *reason = KIS_QUARANTINE_REBUILD;
            return false;
        }
        uint8_t ctx[80]{};
        put(ctx, 0, a.storage);
        put(ctx, 64, a.items);
        put(ctx, 72, a.attributes);
        pide = rebuild_record(memory, h.entity, info, ctx);
        if (!pide) {
            *reason = KIS_QUARANTINE_REBUILD;
            return false;
        }
    }
    // Rebuilding can create entities (and a gem), invalidating earlier component views.
    inv = static_cast<uint8_t*>(component(world, kHashInventory, page, 96));
    if (!inv || !copy(old, inv + (record.slot % 8) * 12, 12) || old[0] || old[1] || old[2]) {
        if (pide)
            remove(a.storage, pide);
        *reason = KIS_QUARANTINE_REBUILD;
        return false;
    }
    uint32_t stack[]{record.item, record.count, pide};
    if (!copy(inv + (record.slot % 8) * 12, stack, 12)) {
        if (pide)
            remove(a.storage, pide);
        *reason = KIS_QUARANTINE_REBUILD;
        return false;
    }
    if (pide) {
        auto is = static_cast<uint8_t*>(component(world, kHashItemState, pide, 20));
        if (!is) {
            std::memset(inv + (record.slot % 8) * 12, 0, 12);
            remove(a.storage, pide);
            *reason = KIS_QUARANTINE_REBUILD;
            return false;
        }
        uint64_t cell = KILN_SLOT_ID(page, record.slot % 8);
        auto owner = component(world, 0xd7361788u, pide, 4);
        if (!copy(is, &cell, 8) || !owner || !copy(owner, &h.entity, 4)) {
            std::memset(inv + (record.slot % 8) * 12, 0, 12);
            remove(a.storage, pide);
            *reason = KIS_QUARANTINE_REBUILD;
            return false;
        }
    }
    *reason = 0;
    return true;
}
bool freeze_holder(const Holder& h, void* attrs, void* bindings, EntityRef* ref, Entry* out) {
    if (!S.convert || !S.write || !ref || !ref->storage)
        return false;
    const auto world = reinterpret_cast<uintptr_t>(ref->storage) - E.world_storage;
    out->records.clear();
    for (size_t p = 0; p < h.pages.size(); ++p) {
        uint32_t inv[24]{};
        if (!component_copy(world, kHashInventory, h.pages[p], inv, 96))
            return false;
        for (unsigned i = 0; i < 8; ++i) {
            auto st = inv + i * 3;
            if (!st[0] || !st[1])
                continue;
            if (st[1] > 65535)
                return false;
            uint8_t memory[56]{};
            KilnStorageRecord r{};
            r.slot = static_cast<uint16_t>(p * 8 + i);
            if (!convert_record(memory, st, static_cast<uint8_t>(r.slot), attrs, bindings, ref) ||
                !write_record(memory, r.data, &r.length))
                return false;
            Record record;
            if (!decode_record(r.data, r.length, &record))
                return false;
            r.item = record.item;
            r.count = record.count;
            r.flags = r.data[7];
            r.level = record.present[0] ? record.options[0][0] : 0;
            out->records.push_back(r);
        }
    }
    out->row.count = static_cast<uint32_t>(out->records.size());
    std::copy(h.refs.begin(), h.refs.end(), out->row.refs);
    uint64_t selection[20]{};
    if (component_copy(world, kHashSlotSelection, ref->id, selection, sizeof selection)) {
        static constexpr unsigned equip[]{12, 14, 15, 16};
        for (unsigned r = 0; r < 4; ++r) {
            out->row.refs[r] = KIS_REF_NONE;
            auto cell = selection[equip[r]];
            for (size_t p = 0; p < h.pages.size(); ++p)
                if (KILN_SLOT_INVENTORY(cell) == h.pages[p] && KILN_SLOT_INDEX(cell) < 8)
                    out->row.refs[r] = static_cast<uint16_t>(p * 8 + KILN_SLOT_INDEX(cell));
        }
    }
    return true;
}
bool clear_holder_references(uintptr_t world, uint32_t player, const Holder& h) {
    if (!S.structural_gate || !S.structural_gate() || !S.tick_write)
        return false;
    __try {
        PtrSize ps{};
        E.get_component(&ps, world, kHashSlotSelection, player);
        if (!ps.ptr)
            return true;
        if (ps.size < 216)
            return false;
        bool needed = false;
        constexpr uint32_t equip[]{12, 14, 15, 16};
        for (auto slot : equip) {
            auto cell = read<uint64_t>(ps.ptr, slot * 8);
            if (std::find(h.pages.begin(), h.pages.end(), KILN_SLOT_INVENTORY(cell)) != h.pages.end())
                needed = true;
        }
        if (!needed)
            return true;
        S.tick_write(&ps, world, kHashSlotSelection, player);
        if (!ps.ptr || ps.size < 216)
            return false;
        for (auto slot : equip) {
            auto cell = read<uint64_t>(ps.ptr, slot * 8);
            if (std::find(h.pages.begin(), h.pages.end(), KILN_SLOT_INVENTORY(cell)) != h.pages.end())
                put(ps.ptr, slot * 8, uint64_t(0));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool reference_write(const Access& a, uint32_t player, uint32_t equip, uint64_t slot, bool only_empty) {
    if (equip != 12 && equip != 14 && equip != 15 && equip != 16)
        return false;
    __try {
        PtrSize p{};
        E.write_other(&p, a.storage, E.job_tracker(a.job), a.acc[A_SLOTSEL], player, E.job_write_flag(a.ctx));
        if (!p.ptr || p.size < 216)
            return false;
        if (only_empty && read<uint64_t>(p.ptr, equip * 8) != 0)
            return true;
        std::memcpy(static_cast<uint8_t*>(p.ptr) + equip * 8, &slot, 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace kitems::storage
