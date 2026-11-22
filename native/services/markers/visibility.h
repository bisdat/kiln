// SPDX-License-Identifier: MIT
// kiln.markers' visibility events (chapter 04 §4.6): admission, not pixels. One aggregated event per row per client Post
// when its admitted mask or any reason changed; Initial once at the first Post that evaluates a row; a terminal event
// (after 0, the requested surfaces' reasons VIS_REMOVED) when an evaluated row is gone (removed, cleared, its owner or
// world or link ended), then the row is forgotten. A row added and removed between two Posts was never evaluated and gets
// no visibility event. Pure: the surfaces' glue calls step() at each client Post and clear() when the client world stops.
#ifndef KILN_MARKERS_VISIBILITY_H
#define KILN_MARKERS_VISIBILITY_H
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "events.h"
#include "select.h"
#include "store.h"

namespace kmk {

class Visibility {
public:
    // rows and adm are parallel (one Admitted per row). Appends the notes of this Post to out.
    void step(uint64_t world_epoch, const std::vector<ClientRow>& rows, const std::vector<Admitted>& adm, std::vector<Note>* out);
    // Terminal events for every evaluated row (the client world stops), then nothing is remembered.
    void clear(uint64_t world_epoch, std::vector<Note>* out);
    size_t size() const { return state_.size(); }

private:
    using Id = std::tuple<uint32_t, uint64_t, std::string, std::string>;  // source, publisher generation, namespace, key
    struct State {
        uint32_t requested = 0, after = 0;
        uint32_t reason[3] = {};
        uint64_t peer_generation = 0, sequence = 0;
        bool seen = false;  // still present at this Post (a mark of step())
    };
    static void fill(Note* n, uint64_t epoch, const Id& id, const State& s, uint32_t before, bool initial);
    std::map<Id, State> state_;
};

}  // namespace kmk
#endif
