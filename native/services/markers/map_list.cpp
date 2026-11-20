// SPDX-License-Identifier: MIT
#include "map_list.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace kmk {

namespace {
constexpr uint64_t kHeaderBytes = 0x28;  // data pointer, count, capacity, growth callback, allocator word

int64_t fixed(double v) { return static_cast<int64_t>(std::llround(v * 4294967296.0)); }

bool same_rows(const uint8_t* data, const MapLayout& l, uint64_t base, uint64_t n, const std::vector<uint8_t>& written) {
    if (written.size() != n * l.entry_size) return false;
    return std::memcmp(data + base * l.entry_size, written.data(), written.size()) == 0;
}

// The bytes of the rows we wrote last (to recognise them); one copy, replaced by each append.
std::vector<uint8_t> g_written;
}  // namespace

bool MapLayout::valid() const {
    return header > 0 && capacity > 0 && capacity <= 0x4000 && entry_size >= 0x38 && entry_size <= 0x400 && position + 16 <= entry_size &&
           z + 8 <= entry_size && type + 4 <= entry_size && visibility < entry_size;
}

ListState read_list(const uint8_t* ui, const MapLayout& l, uint64_t* count, uint64_t* room) {
    *count = 0;
    *room = 0;
    if (!l.valid()) return ListState::BadHeader;
    const uint8_t* h = ui + l.header;
    uint64_t data = 0, cnt = 0, cap = 0;
    std::memcpy(&data, h, 8);
    std::memcpy(&cnt, h + 8, 8);
    std::memcpy(&cap, h + 16, 8);
    if (data != reinterpret_cast<uintptr_t>(h) + kHeaderBytes || cap != l.capacity || cnt > cap) return ListState::BadHeader;
    *count = cnt;
    *room = cap - cnt;
    return ListState::Ok;
}

void encode_entry(uint8_t* e, const MapLayout& l, const MapRow& row) {
    std::memset(e, 0, l.entry_size);
    const int64_t x = fixed(row.x), y = fixed(row.y), z = fixed(row.z);
    std::memcpy(e + l.position, &x, 8);
    std::memcpy(e + l.position + 8, &y, 8);
    std::memcpy(e + l.z, &z, 8);
    std::memcpy(e + l.type, &row.type, 4);
    e[l.visibility] = 2;
}

uint32_t append_rows(uint8_t* ui, const MapLayout& l, const MapRow* rows, uint32_t n, Appended* last, ListState* state) {
    uint64_t count = 0, room = 0;
    *state = read_list(ui, l, &count, &room);
    if (*state != ListState::Ok) return 0;
    uint8_t* h = ui + l.header;
    uint8_t* data = h + kHeaderBytes;
    // the game did not clear its list since our last append: the tail is still ours, write over it
    if (last->step && last->n && count == last->base + last->n && same_rows(data, l, last->base, last->n, g_written)) {
        count = last->base;
        room = l.capacity - count;
    }
    const uint32_t take = static_cast<uint32_t>(n < room ? n : room);
    g_written.assign(static_cast<size_t>(take) * l.entry_size, 0);
    for (uint32_t i = 0; i < take; ++i) encode_entry(g_written.data() + static_cast<size_t>(i) * l.entry_size, l, rows[i]);
    if (take) std::memcpy(data + count * l.entry_size, g_written.data(), g_written.size());
    const uint64_t next = count + take;
    std::memcpy(h + 8, &next, 8);
    last->base = count;
    last->n = take;
    last->step += 1;
    return take;
}

}  // namespace kmk
