// SPDX-License-Identifier: MIT
// kiln.markers' rows in the game's map list (chapter 04 §2.3, §4.5): appended in a completed client Post to the normal
// world-marker list of the client world's UI play data, never through the game's own append function and never beyond the
// list's inline storage. Pure: it works on the memory it is given (the glue calls it under a fault guard on the game's
// object; the host tests on a fake one with sentinels).
//
// Guards, all of which must hold or nothing is written: the list's data pointer is exactly its header + 0x28 (the inline
// storage that follows the 40-byte header), its capacity is the layout's (1024 on the measured build), its count is not
// above the capacity. Only rows beyond the current count are written; the count is raised once, after the rows. Nothing
// before the count (the game's entries, another loader's) is read for writing or changed.
//
// Entry encoding (MapLabel.None, v1): the whole entry zeroed, the position as signed 64-bit 32.32 metres at +position
// (x, y) and +z, the marker type id at +type, visibility 2 (shown) at +visibility. Entity, marker loca id and both text
// views stay zero: the native compass cannot reach an id-0 entry and no tooltip is promised.
#ifndef KILN_MARKERS_MAP_LIST_H
#define KILN_MARKERS_MAP_LIST_H
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kmk {

struct MapLayout {
    int64_t header = 0;        // the normal list's header in the UI play data (its count offset - 8)
    uint64_t capacity = 0;     // the inline capacity every list is built with
    uint32_t entry_size = 0;   // the stride
    uint32_t position = 0;     // x at +position, y at +position + 8
    uint32_t z = 0;
    uint32_t type = 0;
    uint32_t visibility = 0;
    bool valid() const;        // offsets inside the entry, sizes the measured ones
};

struct MapRow {
    double x, y, z;
    uint32_t type;
};

enum class ListState { Ok, BadHeader };

// The list's count and capacity, after the guards. room = capacity - count.
ListState read_list(const uint8_t* ui, const MapLayout& l, uint64_t* count, uint64_t* room);

// Encodes one entry (entry_size bytes).
void encode_entry(uint8_t* e, const MapLayout& l, const MapRow& row);

// What a previous append left, to recognise a list the game did not clear since (no step ran its clear program).
struct Appended {
    uint64_t base = 0, n = 0;  // rows [base, base + n) were ours
    uint64_t step = 0;         // nonzero once something was appended
};

// Appends up to n rows (the guards first). When the list still ends with exactly our previous rows (the game did not
// clear it in between), they are replaced in place instead of appended again. Returns the rows written; *state says
// whether the guards held.
uint32_t append_rows(uint8_t* ui, const MapLayout& l, const MapRow* rows, uint32_t n, Appended* last, ListState* state);

// ---- every client step (GitHub issue 159) -----------------------------------------------------------------------------
// The game clears this list and fills it again at every client SimStep (clear_map_markers_for_ui, then
// replicate_map_markers_for_ui; StepPrograms waits for their jobs before SimStep returns), the map reads it in the UI frame
// on the same thread, and Kiln's Post runs only at the SimSteps that advance the client clock (60 Hz). Above 60 fps the
// SimSteps in between left the list without Kiln's rows and the icons flickered. So the rows a Post wrote are kept and
// written again at the end of every client SimStep (docs/re/map.md, "The list in a frame").
struct MapKeep {
    uint64_t epoch = 0;        // the client world epoch of the Post that wrote them; 0 = nothing kept
    std::vector<MapRow> rows;  // in the Post's order
    void set(uint64_t e, const MapRow* r, uint32_t n);
    void clear();
};

// The end of a client SimStep: the kept rows again through append_rows when they are this epoch's (the same guards and
// room: the game's rows are never displaced, rows beyond the room are left out). A step whose clear did not run leaves the
// list ending with them, and append_rows writes them over in place. Returns the rows written; nothing kept for this epoch
// writes nothing and leaves *state Ok.
uint32_t rewrite_kept(uint8_t* ui, const MapLayout& l, const MapKeep& keep, uint64_t epoch, Appended* last, ListState* state);

}  // namespace kmk
#endif
