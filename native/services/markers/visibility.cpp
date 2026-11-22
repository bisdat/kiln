// SPDX-License-Identifier: MIT
#include "visibility.h"

#include <cstring>

namespace kmk {

namespace {
void copy_field(char* dst, size_t cap, const std::string& s) {
    const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    if (n) std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}
}  // namespace

void Visibility::fill(Note* n, uint64_t epoch, const Id& id, const State& s, uint32_t before, bool initial) {
    n->kind = Note::Visibility;
    n->role = kRoleClient;
    n->counted = false;
    n->publisher_generation = std::get<1>(id);
    KilnMarkerVisibility& v = n->visibility;
    v = KilnMarkerVisibility{};
    v.size = sizeof(KilnMarkerVisibility);
    v.audience = std::get<0>(id);
    v.recipient = 0;
    v.initial = initial ? 1u : 0u;
    v.world_epoch = epoch;
    v.peer_generation = s.peer_generation;
    v.publisher_generation = std::get<1>(id);
    v.sequence = s.sequence;
    v.requested = s.requested;
    v.before = before;
    v.after = s.after;
    for (int i = 0; i < 3; ++i) v.reason[i] = s.reason[i];
    copy_field(v.owner, sizeof v.owner, std::get<2>(id));
    copy_field(v.key, sizeof v.key, std::get<3>(id));
}

void Visibility::step(uint64_t epoch, const std::vector<ClientRow>& rows, const std::vector<Admitted>& adm, std::vector<Note>* out) {
    for (auto& [id, s] : state_) s.seen = false;
    for (size_t i = 0; i < rows.size(); ++i) {
        const ClientRow& r = rows[i];
        const Admitted& a = adm[i];
        Id id{r.source, r.publisher_generation, r.ns, r.key};
        State next;
        next.requested = r.rec.surfaces;
        next.after = a.after;
        for (int k = 0; k < 3; ++k) next.reason[k] = a.reason[k];
        next.peer_generation = r.peer_generation;
        next.sequence = r.rec.sequence;
        next.seen = true;
        auto it = state_.find(id);
        if (it == state_.end()) {
            Note n;
            fill(&n, epoch, id, next, 0, true);
            out->push_back(n);
            state_.emplace(std::move(id), next);
            continue;
        }
        State& prev = it->second;
        const bool changed = prev.after != next.after || prev.requested != next.requested || prev.reason[0] != next.reason[0] ||
                             prev.reason[1] != next.reason[1] || prev.reason[2] != next.reason[2];
        if (changed) {
            Note n;
            fill(&n, epoch, it->first, next, prev.after, false);
            out->push_back(n);
        }
        prev = next;
    }
    for (auto it = state_.begin(); it != state_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        State end = it->second;
        const uint32_t before = end.after;
        end.after = 0;
        for (int k = 0; k < 3; ++k) end.reason[k] = (end.requested & kSurfaceBit[k]) ? KILN_MARKER_VIS_REMOVED : KILN_MARKER_NOT_REQUESTED;
        Note n;
        fill(&n, epoch, it->first, end, before, false);
        out->push_back(n);
        it = state_.erase(it);
    }
}

void Visibility::clear(uint64_t epoch, std::vector<Note>* out) {
    for (auto& [id, s] : state_) {
        State end = s;
        const uint32_t before = end.after;
        end.after = 0;
        for (int k = 0; k < 3; ++k) end.reason[k] = (end.requested & kSurfaceBit[k]) ? KILN_MARKER_VIS_REMOVED : KILN_MARKER_NOT_REQUESTED;
        Note n;
        fill(&n, epoch, id, end, before, false);
        out->push_back(n);
    }
    state_.clear();
}

}  // namespace kmk
