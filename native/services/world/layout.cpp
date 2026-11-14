// SPDX-License-Identifier: MIT
// The iterator layout of one of the game's own programs, from its descriptor (layout.h has the algorithm, shared with
// Kiln's programs). The descriptor lives in the game's image; its names are read through SEH-guarded copies.
#include "layout.h"

namespace kw {
namespace {

bool read_names(const NameListE& src, std::vector<std::string>& out, std::string& why, const char* list) {
    out.clear();
    if (!src.n) return true;
    if (!src.p || src.n > 160) {
        why = std::string(list) + ": a list out of bounds";
        return false;
    }
    std::vector<NameRef> refs(src.n);
    if (!seh_copy(refs.data(), src.p, src.n * sizeof(NameRef))) {
        why = std::string(list) + ": unreadable";
        return false;
    }
    for (const NameRef& r : refs) {
        char buf[121];
        if (!r.s || !r.n || r.n > 120 || !seh_copy(buf, r.s, r.n)) {
            why = std::string(list) + ": an unreadable name";
            return false;
        }
        out.emplace_back(buf, r.n);
    }
    return true;
}

}  // namespace

bool read_desc(const EngineDesc* desc, ProgramLayout& out, bool* no_view, std::string& why) {
    EngineDesc d{};
    if (!desc || !seh_copy(&d, desc, sizeof d)) {
        why = "the descriptor is unreadable";
        return false;
    }
    if (no_view) *no_view = d.other_write_unser.n || d.all_read.n || d.all_write.n;
    char name[121] = {};
    if (d.name.s && d.name.n && d.name.n <= 120) seh_copy(name, d.name.s, d.name.n);
    out = ProgramLayout{};
    out.name = name;
    const NameListE* src[L_COUNT] = {&d.tpl, &d.tpl_opt, &d.read, &d.read_opt, &d.write, &d.write_opt, &d.sys_read,
                                     &d.sys_write, &d.other_tpl, &d.other_read, &d.other_write, &d.with, &d.without,
                                     &d.any_of, &d.events_in, &d.events_out};
    for (int id = 0; id < L_COUNT; ++id)
        if (!read_names(*src[id], out.lists[id].names, why, kListName[id])) return false;
    out.sread_opt = d.sys_read_opt;
    out.swrite_opt = d.sys_write_opt;
    return true;
}

bool game_layout(const EngineDesc* desc, uintptr_t world, ProgramLayout& out, std::string& why) {
    bool no_view = false;
    if (!read_desc(desc, out, &no_view, why)) return false;
    if (no_view) {
        why = "the program uses other_write_unser, all_read or all_write, which kiln.world's layout does not model";
        return false;
    }
    if (!resolve_indices(out, world, why)) return false;
    layout(out, [&](int list, size_t i) { return static_cast<uint32_t>(out.lists[list].index[i]); });
    return true;
}

}  // namespace kw
