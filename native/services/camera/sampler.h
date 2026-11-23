// SPDX-License-Identifier: MIT
// kiln.camera's client sampler (chapter 03, 4.2, 2.13): in the client world's Pre(N), before the step runs, it copies
// the RenderTransform of the local player (and, for a follow profile, of its target) through the game's GetComponent:
// the results of completed step N-1, values only, with the step, the capture time and the world's epoch. The render
// transaction uses those copies, never the game's memory: one completed client step of deliberate follow latency, no
// extrapolation, no lookup inside the simulation or the render path. If the renderer draws again before the next Pre,
// it reuses the newest copies and their age grows.
//
// Threads. sample() runs on the client world's thread in its Pre; the render path reads the board and tries once (a
// copy that is being written is skipped, the renderer keeps its previous one).
#ifndef KILN_CAMERA_SAMPLER_H
#define KILN_CAMERA_SAMPLER_H

#include <atomic>
#include <cstdint>
#include <cstring>

#include "snapshot.h"

namespace kcam {

enum : uint32_t {
    kSampleNone = 0,     // not sampled
    kSamplePresent = 1,  // copied
    kSampleAbsent = 2,   // asked for, and the entity has no RenderTransform in the client world (gone, not replicated)
};

// One entity's current WorldTransform as copied (its scale is not kept: a target's scale never changes distances).
struct Sample {
    uint32_t entity = 0;
    uint32_t state = kSampleNone;
    uint64_t step = 0;    // the client step whose Pre copied it
    int64_t ns = 0;       // when (the monotonic clock)
    uint32_t epoch = 0;   // the client world it is of
    uint32_t pad = 0;
    double position[3] = {0, 0, 0};
    double rotation[4] = {0, 0, 0, 1};  // normalized
};

// Slot 0 is the local player; the others are follow targets (the profile of the live and of the staged request).
constexpr uint32_t kSampleSlots = 4;
struct SampleSet {
    Sample s[kSampleSlots];
    // The sample of `entity` (0: the local player), or null when it was not asked for in this set.
    const Sample* find(uint32_t entity) const {
        if (!entity) return s[0].entity ? &s[0] : nullptr;
        for (const Sample& x : s)
            if (x.entity == entity && x.state != kSampleNone) return &x;
        return nullptr;
    }
};
static_assert(sizeof(SampleSet) % 8 == 0, "copied as whole words");

// The newest set, from the client Pre to the render path: one writer, readers try once.
class SampleBoard {
public:
    void publish(const SampleSet& set);
    bool try_read(SampleSet* out) const;

private:
    static constexpr size_t kWords = sizeof(SampleSet) / 8;
    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> words_[kWords]{};
};

// What reads an entity's RenderTransform (its current WorldTransform, the first 52 bytes) in the client world.
// Production: the game's GetComponent with the client world, under a structured-exception guard (local.cpp).
class SampleSource {
public:
    enum class Read { Ok, Absent, Fault };
    virtual ~SampleSource() = default;
    virtual Read read(uint32_t entity, EnginePose* out) = 0;
};

class Sampler {
public:
    Sampler(SampleSource* source, SampleBoard* board, const Clock* clock) : source_(source), board_(board), clock_(clock) {}

    // The client world's Pre (its thread): copies the local player and up to kSampleSlots - 1 targets (0 or the local
    // player's id are skipped: slot 0 has it) and publishes the set. A fault turns the sampler off until the next world.
    void sample(uint32_t epoch, uint64_t step, uint32_t local_player, const uint32_t* targets, uint32_t n_targets);
    void world_started() { faulted_.store(false, std::memory_order_release); }
    bool faulted() const { return faulted_.load(std::memory_order_acquire); }
    const SampleSet& last() const { return last_; }  // the client thread's own copy

private:
    void take(uint32_t entity, uint32_t epoch, uint64_t step, Sample* out);

    SampleSource* source_;
    SampleBoard* board_;
    const Clock* clock_;
    std::atomic<bool> faulted_{false};
    SampleSet last_{};
};

}  // namespace kcam
#endif
