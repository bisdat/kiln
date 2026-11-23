// SPDX-License-Identifier: MIT
// kiln.camera's client sampler: sampler.h says what it is and which thread may do what.
#include "sampler.h"

#include "cam_math.h"

namespace kcam {

void SampleBoard::publish(const SampleSet& set) {
    uint64_t w[kWords];
    std::memcpy(w, &set, sizeof w);
    seq_.fetch_add(1, std::memory_order_acq_rel);
    for (size_t i = 0; i < kWords; ++i) words_[i].store(w[i], std::memory_order_relaxed);
    seq_.fetch_add(1, std::memory_order_release);
}

bool SampleBoard::try_read(SampleSet* out) const {
    const uint64_t before = seq_.load(std::memory_order_acquire);
    if (before & 1) return false;
    uint64_t w[kWords];
    for (size_t i = 0; i < kWords; ++i) w[i] = words_[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq_.load(std::memory_order_relaxed) != before) return false;
    std::memcpy(out, w, sizeof w);
    return true;
}

void Sampler::take(uint32_t entity, uint32_t epoch, uint64_t step, Sample* out) {
    *out = Sample{};
    out->entity = entity;
    out->step = step;
    out->epoch = epoch;
    out->ns = clock_->now_ns();
    EnginePose p{};
    switch (faulted() ? SampleSource::Read::Fault : source_->read(entity, &p)) {
    case SampleSource::Read::Fault:
        faulted_.store(true, std::memory_order_release);
        out->state = kSampleNone;  // nothing usable: a pose request ends SafetyRejected when its samples go stale
        return;
    case SampleSource::Read::Absent:
        out->state = kSampleAbsent;
        return;
    case SampleSource::Read::Ok:
        break;
    }
    Quat q{p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]};
    Vec3 pos{fixed_to_metres(p.position[0]), fixed_to_metres(p.position[1]), fixed_to_metres(p.position[2])};
    if (!engine_unit(&q) || !finite(pos)) {
        out->state = kSampleNone;  // not a pose this service follows
        return;
    }
    out->state = kSamplePresent;
    out->position[0] = pos.x, out->position[1] = pos.y, out->position[2] = pos.z;
    out->rotation[0] = q.x, out->rotation[1] = q.y, out->rotation[2] = q.z, out->rotation[3] = q.w;
}

void Sampler::sample(uint32_t epoch, uint64_t step, uint32_t local_player, const uint32_t* targets, uint32_t n_targets) {
    SampleSet set{};
    if (epoch && local_player) take(local_player, epoch, step, &set.s[0]);
    uint32_t slot = 1;
    for (uint32_t i = 0; i < n_targets && slot < kSampleSlots; ++i) {
        const uint32_t e = targets[i];
        if (!e || e == local_player || !epoch) continue;
        bool seen = false;
        for (uint32_t j = 1; j < slot; ++j) seen = seen || set.s[j].entity == e;
        if (!seen) take(e, epoch, step, &set.s[slot++]);
    }
    last_ = set;
    board_->publish(set);
}

}  // namespace kcam
