// SPDX-License-Identifier: MIT
// kiln.stationmenus (native/core/abi/services/kiln_station_menus.h, since 0.2.0-dev.31): the crafting-menu layouts of mods'
// own workshops ("station menu tabs", chapter 16a; dev31 out/16a-station-tabs/DESIGN.md).
//  - The store: one parser and one serializer for the normalized record (format 1), layers per (owner, definition id),
//    the remote collection (a joined client: the server's snapshot), one local generation, the applied queue. Any thread,
//    one mutex. The C table (table()) is registered by content.cpp next to kiln.content.
//  - The materializer: the crafting menu's own-workshop trees (newcontent.cpp's own_build calls own_publish on the UI
//    thread, inside the menu hook). Every array is built in near memory first; both collection headers switch last, and
//    go back to the game's together when that fails. Game memory, near memory, header writes and loca texts go through
//    MenuIo, so that menulayout_test.cpp checks the bytes on the host and fails every allocation and every header write.
// Nothing here reads game memory or calls Windows itself.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "content.h"  // kc::Write
#include "kiln_api.h"
#include "services/kiln_station_menus.h"

namespace kc {
// newcontent.cpp: whether this process draws layouts (a game client with the crafting-menu hook and the menu's layout
// from the game's reflection); else *reason (KILN_CAP_REASON_*) and *detail say why. Any thread.
bool nc_menu_render(uint32_t* reason, std::string* detail);
}  // namespace kc

namespace kc::ml {

// ---- limits (chapter 16a section 5; DESIGN.md section 2) ----
constexpr size_t kMaxTabs = 15, kMaxSetsPerTab = 28, kMaxRecipesPerTab = 512, kMaxLanguages = 35, kMaxTextBytes = 4096;
constexpr size_t kMaxRecordBytes = size_t(1) << 20;  // one normalized record
constexpr size_t kMaxAllBytes = size_t(64) << 20;    // the local layers' records in all; a remote snapshot
constexpr size_t kMaxLayers = 256;                   // local layers, inactive ones too
constexpr size_t kMaxRemoteMenus = 256;              // records in one remote snapshot
constexpr uint32_t kMaxBuiltPerWorkshop = 16;        // distinct menus built per workshop (per menu configuration)
constexpr size_t kMaxBacking = size_t(64) << 20;     // near memory of the own-workshop menus in all (never freed)
constexpr size_t kMaxApplied = 256;                  // drain_applied's queue; the oldest goes first

// ---- the normalized record, format 1 ----
using TextMap = std::map<std::string, std::string>;  // language key ("*", "en", "pt-br") -> text (UTF-8)
struct MenuSet {
    std::string key, id_text;
    uint8_t id[16] = {};       // the game's GUID byte order (data1..3 little-endian), as guid_parse
    bool has_heading = false;  // false: "heading": null (always so for set 0)
    TextMap heading;
    std::string heading_json;  // canonical, part of the heading label's identity
    std::vector<uint32_t> recipes;
};
struct MenuTab {
    std::string key, id_text;
    uint8_t id[16] = {};
    uint32_t icon = 0;  // iconFromTab: the tab of the base workshop's original tree whose icon it shows
    TextMap name;
    std::string name_json;
    std::vector<MenuSet> sets;
};
struct Record {
    std::string def_mod, def_key, def_id_text, origin_mod, station_key, workshop_guid_text;
    uint8_t def_id[16] = {}, workshop_guid[16] = {};
    uint32_t workshop = 0;  // FNV-1a-32 of workshop_guid
    uint32_t base = 0;      // baseWorkshopId: the game workshop whose menu it is like (icons)
    std::vector<MenuTab> tabs;
    std::string canonical;  // the one serialization (sorted keys, compact, UTF-8 as is)
    uint64_t hash = 0;      // FNV-1a-64 of canonical
};
// Reads and checks one record (define's bytes); false + *error (a sentence) when it is refused.
bool parse_record(std::string_view text, Record* out, std::string* error);

// ---- the store (any thread) ----
KilnResult define(KilnOwner owner, std::string_view record, KilnHandle* out, uint64_t* out_generation, std::string* state,
                  std::string* error);
KilnResult remove(KilnOwner caller, KilnHandle handle, uint64_t* out_generation);
std::string export_layouts();  // {"format":1,"generation":N,"menus":[effective local records by workshopId]}
KilnResult remote_replace(KilnOwner owner, std::string_view snapshot, std::string* state, std::string* error);
KilnResult remote_clear(KilnOwner owner);
void set_pending(bool on);
bool pending();
KilnResult info(KilnStationMenusInfo* out);
// The core's answer whether an owner was revoked (content.cpp: post() of a no-op answers KILN_ERR_REVOKED). Unset: never.
void set_owner_probe(std::function<bool(KilnOwner)> revoked);
// newcontent.cpp's nc_menu_render (content.cpp sets it at boot); unset: render 0.
void set_render_probe(bool (*probe)(uint32_t* reason, std::string* detail));
void probe_owners();  // drops the layers (and the remote collection) of revoked owners; content.cpp calls it every ~1 s
const KilnStationMenusApi* table();

// What the menu hook draws: per workshop the remote record, else the newest local layer.
struct Effective {
    uint32_t workshop = 0;
    std::shared_ptr<const Record> rec;
    bool from_server = false;
    uint64_t generation = 0;  // the remote snapshot's, or the local generation the layer was accepted at
};
std::vector<Effective> effective();  // sorted by workshop
bool has_layout(uint32_t workshop);
uint64_t version();                  // changes whenever effective() may answer otherwise

// One materialization (or refusal) for drain_applied.
struct Applied {
    std::string definition;  // GUID text
    uint32_t workshop = 0;
    uint64_t generation = 0;
    bool from_server = false, ok = false;
    std::string reason;  // "" when ok; else a stable word first: "IconSourceMissing: ...", "RestartRequired: ...", ...
};
void applied_push(Applied a);
std::string drain_applied_text(bool clear);

// ---- the materializer (UI thread) ----
struct MenuIo {
    virtual ~MenuIo() = default;
    virtual bool read(uintptr_t address, void* out, size_t n) = 0;               // game memory, guarded
    virtual void* alloc(uintptr_t anchor, size_t n) = 0;                            // within rel32 reach of anchor; never freed
    virtual size_t write(std::vector<Write>& writes, uintptr_t* failed) = 0;     // write_batch: how many landed
    virtual void guid(const std::string& key, uint8_t out[16]) = 0;              // newcontent's cm_guid
    virtual void texts(uint32_t tag, const TextMap& texts) = 0;                  // newcontent's set_texts: the loca hooks answer tag
};
// The menu's layout constants (sigdb, reflection). An offset of -1: the labels/ids are not known, layouts cannot be drawn.
struct Geometry {
    uint32_t gs = 0x38, ss = 0x28;            // tab (FbUiCraftingRecipeGroup) and set (FbUiCraftingRecipeSet) strides
    uint32_t cat_subs = 0x30, sub_recipes = 0x20;  // the sets / entries BlobArray headers in them
    int64_t g_id = -1, g_label = -1, s_id = -1, s_label = -1;
    bool labels() const { return g_id >= 0 && g_label >= 0 && s_id >= 0 && s_label >= 0; }
};
struct OwnPlace {
    uint32_t tab = 0, sub = 0, id = 0;  // a recipe at the place the automatic placement found in the menu it takes tabs from
};
struct OwnWorkshop {
    uint32_t workshop = 0;
    uint32_t menu = 0;  // placements: the workshop whose tabs it takes (0: none)
    std::vector<OwnPlace> places;
    Effective layout;   // layout.rec null: none (the tree comes from the placements, as before dev.31)
};
struct TreeRef {
    uintptr_t groups = 0;
    uint32_t count = 0;
};
// own_publish's state for one menu configuration (the UI bundle's collection), kept by newcontent.cpp.
struct OwnState {
    uint64_t orig[2] = {};        // the collection's workshops and trees headers, as the game's data has them
    std::vector<uint64_t> trees;  // the trees' headers, as the game's data has them (Kiln's own tabs undone)
    bool on = false;              // the headers point at Kiln's copies (the pair shown[])
    uint64_t shown[2] = {};       // the headers Kiln wrote last (valid while on)
    std::map<std::pair<uint32_t, uint64_t>, TreeRef> cache;  // (workshop, layout hash) -> its tree
    std::map<uint32_t, TreeRef> good;                        // workshop -> the layout tree published last
    std::map<uint32_t, uint32_t> built;                      // workshop -> layout trees built
    std::map<uint32_t, std::string> said;                    // workshop -> the last applied entry (no repeats)
    size_t backing = 0;           // near memory taken in all (never reset: never freed)
    void new_config() {
        orig[0] = orig[1] = 0;
        shown[0] = shown[1] = 0;
        trees.clear();
        on = false;
        cache.clear();
        good.clear();
        built.clear();
    }
};
struct OwnResult {
    bool ok = false;         // the own workshops show their own menus (else their recipes go to the menus they open)
    bool published = false;  // both headers now point at new copies
    bool restored = false;   // both headers went back to the game's (no own workshop left)
    uint32_t own = 0;        // own workshops in the published lists
    uintptr_t failed = 0;    // a header write that failed
    std::string said;        // for the log line
    std::vector<std::pair<bool, std::string>> notes;  // (warning, line) for kiln.log: layouts shown or refused, once each
};
// Builds the own workshops' trees (a layout's from the layout, else from placements), then the collection's workshops and
// trees arrays (the game's entries first, rebased), then writes both headers; on a failed write both go back to the
// game's (a header that cannot be written at all: both stay the pair shown before). Nothing is written when an
// allocation fails.
OwnResult own_publish(uintptr_t coll, const std::vector<OwnWorkshop>& menus, const Geometry& g, MenuIo& io, OwnState& st);
// The tree of one layout (tabs = groups copied from the base tree's tab iconFromTab, with the layout's ids and labels).
enum class TreeOutcome { Ok, IconSourceMissing, LayoutUnknown, NoMemory, OutOfReach };
TreeOutcome layout_tree(const Record& r, uintptr_t base_groups, uint32_t base_count, bool base_found, const Geometry& g,
                        MenuIo& io, uintptr_t anchor, TreeRef* out, size_t* bytes, std::string* why);
// The pending hold: the workshop id to ask the game's lookup for instead of `workshop` (one the live list proves absent),
// or 0 (no hold). game: the game's own workshop list (before Kiln's); def_own: a definition names it its own workshop.
uint32_t hold_workshop(uint32_t workshop, const std::vector<uint32_t>& game, const std::vector<uint32_t>& live, bool def_own);
uint32_t absent_id(const std::vector<uint32_t>& live);  // the smallest id above 0 that live lacks (0: none)

// Tests only.
void reset_for_tests();
void set_generation_for_tests(uint64_t generation);

}  // namespace kc::ml
