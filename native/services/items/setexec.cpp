// SPDX-License-Identifier: MIT
// kiln.items v6: the game's side of the set executor (setexec.h). Every call into the game runs under its own
// structured-exception guard, in a function with nothing to unwind; a fault returns false and the state machine stops
// there (no engine call repairs anything after it). Reads go only through the program's declared accessors (Inventory,
// InventorySetup, ItemState, OwnerRelationship, SlotSelection) and kiln.items' stamp table, which is thread-safe: this
// runs inside kiln_items_equip, on a worker once a player is in the world.
#include <windows.h>

#include <cstring>
#include <vector>

#include "engine.h"
#include "equip.h"
#include "instances.h"
#include "setexec.h"
#include "setsvc.h"
#include "xfer.h"

namespace kitems {
namespace {

using namespace sets;

// ---- the program's reader ----
class ProgramReader final : public SnapReader {
public:
    ProgramReader(const Access& a, const SetLayout& l) : a_(a), l_(l) {}
    const uint8_t* get(Part part, uint32_t e) override {
        Acc which = A_SETUP;
        uint32_t size = 0;
        switch (part) {
            case Part::Setup: which = A_SETUP, size = l_.setup_size; break;
            case Part::Inventory: which = A_INVENTORY, size = l_.inv_size; break;
            case Part::Selection: which = A_SLOTSEL, size = l_.sel_size; break;
            case Part::ItemState: which = A_ITEMSTATE, size = l_.state_size; break;
            case Part::Owner: which = A_OWNER, size = l_.owner_size; break;
        }
        PtrSize ps{};
        E.read_other(&ps, a_.storage, a_.acc[which], e);
        return ps.ptr && ps.size >= size ? static_cast<const uint8_t*>(ps.ptr) : nullptr;
    }
    uint32_t stamp(uint32_t pide, uint32_t item) override { return stamp_for(pide, item); }

private:
    const Access& a_;
    const SetLayout& l_;
};

// ---- guarded calls (no object with a destructor in these frames) ----
bool g_read(SnapReader* r, const SetLayout* l, uint32_t player, SetModel* m, SnapStatus* st) {
    __try {
        *st = read_model(*r, *l, player, m);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool g_open(uint8_t* tx, const uint8_t* setup) {
    __try {
        E.tx_ctor(tx);
        E.tx_init(tx, setup);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// EquipResult8 ({u8 error; pad; u32 zero}); kiln.items backs it with the oversized buffer its single equip uses
bool g_equip(uint8_t* tx, uint64_t source, uint8_t slot, bool unequip, uint8_t* error) {
    __try {
        alignas(8) Result12 out[3] = {};
        if (unequip) E.unequip_item(&out[0], tx, source, slot);
        else E.equip_item(&out[0], tx, source, slot);
        *error = out[0].error;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool g_pages(const Access* a, KilnEntity player, PageSet* ps) {
    __try {
        return read_pages(*a, player, ps, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool g_guards(const uint8_t* tx, bool* would) {
    __try {
        uint64_t empty_state = 0, touched = 0;
        std::memcpy(&empty_state, tx + E.tx_empty_state, 8);
        std::memcpy(&touched, tx + E.tx_touched_count, 8);
        *would = (tx[E.tx_guard_flags] & 0x40) != 0 || (empty_state == 0 && touched != 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool g_tx(uint8_t* tx, int which) {
    __try {
        if (which == 0) E.commit(tx);
        else if (which == 1) E.rollback(tx);
        else E.free_arrays(tx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

class GameEngine final : public SetEngine {
public:
    GameEngine(const Access& a, KilnEntity player, uint8_t* tx, const SetLayout& l, PageSet* pages)
        : a_(a), player_(player), tx_(tx), l_(l), pages_(pages), reader_(a, l) {}
    bool read(SetModel* m, SnapStatus* st) override { return g_read(&reader_, &l_, player_, m, st); }
    bool tx_open(bool dry) override {
        alignas(16) uint8_t setup[kSetupBlock];
        equip_setup_block(a_, player_, dry, setup);  // as the game's equipment_actions sets it up (equip.cpp)
        return g_open(tx_, setup);
    }
    bool equip(uint64_t source, uint8_t slot, uint8_t* error) override { return g_equip(tx_, source, slot, false, error); }
    bool unequip(uint64_t equipped, uint8_t slot, uint8_t* error) override { return g_equip(tx_, equipped, slot, true, error); }
    bool locate(uint32_t pide, uint64_t* slot) override {
        *slot = 0;
        if (!g_pages(&a_, player_, pages_)) return false;
        for (uint32_t i = 0; i < pages_->n; ++i)
            for (const Stack& s : pages_->p[i].s)
                if (!is_empty(s) && pide_of(s) == pide) return *slot = s.slot, true;
        return true;
    }
    bool guards(bool* would) override { return g_guards(tx_, would); }
    bool commit() override { return g_tx(tx_, 0); }
    bool rollback() override { return g_tx(tx_, 1); }
    bool free_arrays() override { return g_tx(tx_, 2); }

private:
    const Access& a_;
    KilnEntity player_;
    uint8_t* tx_;
    const SetLayout& l_;
    PageSet* pages_;
    ProgramReader reader_;
};

uint8_t slot_type_of_item(uint32_t item, void* registry) {
    // ItemInfo +0x104 EquipmentSlotType, through the program's item registry (equip.cpp reads it the same way)
    const auto* info = static_cast<const uint8_t*>(E.item_find(item, registry));
    return info ? info[0x104] : 0;
}

bool g_slot_type(uint32_t item, void* registry, uint8_t* type) {
    __try {
        *type = slot_type_of_item(item, registry);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint8_t guarded_slot_type(uint32_t item, void* registry) {
    uint8_t t = 0;
    return g_slot_type(item, registry, &t) ? t : 0;  // unknown: the game's equip decides (and refuses a misfit)
}

}  // namespace

void set_run(const Access& a, KilnEntity player, uint8_t* tx, const SetJob& job, SetOutcome* out) {
    char why[160] = "";
    const SetLayout* l = sets_layout(why, sizeof why);
    if (!l) {
        *out = SetOutcome{};
        out->status = SetStatus::Unavailable;
        std::snprintf(out->reason, sizeof out->reason, "%s", why);
        return;
    }
    std::vector<PageSet> pages(1);
    GameEngine g(a, player, tx, *l, &pages[0]);
    *out = execute(g, job, sets_plan_options(), &guarded_slot_type, a.items);
}

}  // namespace kitems
