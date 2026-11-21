// SPDX-License-Identifier: MIT
// kiln.content: the base view a boot generation's resource patches are checked against before the game starts
// (gamedata_bootpatch.cpp, prepare_boot). The view is kiln.boot's base view (native/core/src/services/boot_archive_view.h),
// asked for one resource's decoded value by its key, as kiln.boot's OnBoot readers ask it.
//
// Which view. A source, registered once before the first prepare, answers the view of the game data this start reads,
// when it has one that is newer than the boot snapshot (for instance the archive generation a writer committed for this
// start). With no source, or when it answers none, nothing changes: a patch is checked against the game's types before
// the start and against the bytes when it is written, as before. The boot snapshot never serves here: it is made before
// the start, so what an archive writer changed in this start (a list made longer, a value changed) is not in it, and a
// check against it would refuse patches the game's data allows. A view that is not ready (the live game data before the
// entry point) answers nothing either.
//
// What a read says about one resource: its decoded value (Found::Yes, the value in the form kiln.boot's result
// {"resourceId","typeHash","part","value"} carries it), that the data has no such resource (Found::Missing), or nothing
// (Found::Unknown, with why: a value larger than a check reads, a type the view cannot decode, the view gone).
//
// What the view says about a resource is not always what the patch meets when it is written: Kiln, a boot mod, a data
// pack or another mod's assets.json can create a resource or override one in this start (kiln.assets serves an
// assets.json "new" or "override" resource before the patches are written). So what the view says about a resource (it is
// not there, an index past its list, a member, a component, a scalar) refuses a patch before the start only when the
// source says its view of that resource is final for this start (final_for); otherwise the bytes say, as before.
//
// Threads: set_source() before the first prepare (a service's bind, or when its DLL loads). current() is called by
// prepare (kiln.boot's boot thread), finished() once by apply (the resource service's readiness, any thread). The slot
// is atomic; a source keeps its own state behind its own lock.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "../../core/src/services/boot_archive_view.h"
#include "nlohmann/json.hpp"

namespace kc::bootview {

using kiln::boot_view::BaseView;

struct Source {
    /// The view of the game data this start reads, newer than the boot snapshot; null with *why when there is none.
    BaseView* (*view)(std::string* why) = nullptr;
    /// The checks are over (the generation is published and the game's resources are up): what the source holds for
    /// them may be let go. May be null.
    void (*finished)() = nullptr;
    /// True: the view's answer for the resource `key` (its value, or that it has none) is final for this start: nothing
    /// in the start creates or overrides it before the patch is written (no Kiln service, boot mod, data pack or other
    /// mod's assets.json). Only then does what the view says about that resource refuse a patch before the start. Null,
    /// or false: the bytes say when the patch is written.
    bool (*final_for)(const KilnAssetKey& key) = nullptr;
};

inline std::atomic<const Source*>& source_slot() {
    static std::atomic<const Source*> slot{nullptr};
    return slot;
}
/// Registers the source (null: none). It must live as long as the process.
inline void set_source(const Source* s) { source_slot().store(s, std::memory_order_release); }
inline bool has_source() {
    const Source* s = source_slot().load(std::memory_order_acquire);
    return s && s->view;
}

/// The view a prepare checks against, or null with *why.
inline BaseView* current(std::string* why) {
    const Source* s = source_slot().load(std::memory_order_acquire);
    if (!s || !s->view) return *why = "this start has no view of its game data newer than the boot snapshot", nullptr;
    BaseView* v = s->view(why);
    if (!v) return nullptr;
    if (std::string_view(v->source()) == "snapshot")
        return *why = "the boot snapshot is made before the start, so it may not hold what this start's game data holds", nullptr;
    if (!v->ready(why)) return nullptr;
    return v;
}

/// Whether the view of the resource is final for this start (Source::final_for; no answer: it is not).
inline bool final_for(const KilnAssetKey& key) {
    const Source* s = source_slot().load(std::memory_order_acquire);
    return s && s->final_for && s->final_for(key);
}

/// The checks of this start are over (apply, once).
inline void finished() {
    const Source* s = source_slot().load(std::memory_order_acquire);
    if (s && s->finished) s->finished();
}

/// kiln.boot's query for one resource: "resource:<GUID>/<type hash, 8 hex digits>/<part>".
inline std::string query_of(const std::string& guid, uint32_t type_hash, uint32_t part) {
    char hash[9];
    std::snprintf(hash, sizeof hash, "%08x", static_cast<unsigned>(type_hash));
    return "resource:" + guid + "/" + hash + "/" + std::to_string(part);
}

enum class Found { Yes, Missing, Unknown };
struct Read {
    Found found = Found::Unknown;
    nlohmann::json value;  // Found::Yes: the resource's value
    std::string why;       // Missing / Unknown: why
    size_t bytes = 0;      // Found::Yes: the answer's length
};

/// One resource's decoded value through the view, at most `max` bytes of it.
inline Read read(BaseView& view, const std::string& query, size_t max) {
    Read r;
    std::string json, why;
    size_t required = 0, value_bytes = 0;
    const KilnResult rc = view.tree(query, max + 1, max, UINT64_MAX, &json, &required, &value_bytes, &why);
    if (rc == KILN_ERR_NOT_FOUND) {
        r.found = Found::Missing;
        r.why = why.empty() ? "no such resource" : why;
        return r;
    }
    if (rc == KILN_ERR_BUFFER_TOO_SMALL) return r.why = "its value is larger than a check reads", r;
    if (rc != KILN_OK) return r.why = why.empty() ? "the view cannot read it" : why, r;
    nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object() || !j.contains("value")) return r.why = "the view's answer holds no value", r;
    r.found = Found::Yes;
    r.value = std::move(j["value"]);
    r.bytes = json.size();
    return r;
}

}  // namespace kc::bootview
