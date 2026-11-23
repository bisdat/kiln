// SPDX-License-Identifier: MIT
// kiln.camera's final camera snapshot: snapshot.h says what it is and which thread may do what.
#include "snapshot.h"

#include <cmath>

#include "cam_math.h"

namespace kcam {

namespace {

constexpr int64_t kMaxOffset = int64_t(1) << 26;  // the renderer's fields are a few MiB in; anything past 64 MiB is wrong

bool inside(uint64_t offset, size_t n, uint64_t start, uint64_t len) {
    return offset >= start && n <= len && offset - start <= len - n;
}

// The lens and the planes v1 accepts of a stock frame (chapter 03, 4.4).
bool lens_ok(float fov, float aspect, float near_plane, float far_plane) {
    return std::isfinite(fov) && std::isfinite(aspect) && std::isfinite(near_plane) && std::isfinite(far_plane) && fov >= 0.01f &&
           fov <= static_cast<float>(3.14159265358979323846 - 0.01) && aspect >= 0.05f && aspect <= 20.0f && near_plane > 0.0f && far_plane > near_plane;
}

bool scale_ok(const float s[3]) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(s[i]) || std::fabs(s[i] - 1.0f) > 0.0001f) return false;
    return true;
}

}  // namespace

// ---- the layout ---------------------------------------------------------------------------------------------------------

bool RenderLayout::make(const std::function<bool(const char*, int64_t*)>& constant, RenderLayout* out, std::string* why) {
    struct Want {
        const char* name;
        int64_t value;
    } w[] = {{"camera/render.client_world", 0}, {"camera/render.pose_start", 0}, {"camera/render.pose_block1", 0}, {"camera/render.pose_block2", 0},
             {"camera/render.pose_tail", 0},    {"camera/render.near", 0},       {"camera/render.far", 0},         {"camera/render.selected_fov", 0},
             {"camera/render.proj_aspect", 0},  {"camera/render.proj_near", 0},  {"camera/render.proj_far", 0},    {"camera/render.proj_fov", 0}};
    for (Want& x : w) {
        if (!constant(x.name, &x.value)) return *why = std::string(x.name) + " is not resolved", false;
        if (x.value <= 0 || x.value >= kMaxOffset) return *why = std::string(x.name) + " is out of range", false;
    }
    const int64_t pose = w[1].value, near_plane = w[5].value, aspect = w[8].value;
    // the selected pose's three 16-byte stores and its tail, then near, far and field of view; the projection
    // descriptor's aspect, near, far and field of view in a row (2.10a)
    const bool pose_ok = w[2].value == pose + 16 && w[3].value == pose + 32 && w[4].value == pose + 48;
    const bool lens = near_plane == pose + kPoseBytes && w[6].value == near_plane + 4 && w[7].value == near_plane + 8;
    const bool proj = w[9].value == aspect + 4 && w[10].value == aspect + 8 && w[11].value == aspect + 12;
    if (!pose_ok) return *why = "the selected pose's stores are not 16 bytes apart", false;
    if (!lens) return *why = "the selected near, far and field of view do not follow the pose", false;
    if (!proj) return *why = "the projection descriptor's fields are not in a row", false;
    if (w[0].value + 8 > pose || pose + kPoseBytes + 12 > aspect - 4)
        return *why = "the client-world field, the selected pose and the projection descriptor overlap", false;
    out->client_world = static_cast<uint32_t>(w[0].value);
    out->pose = static_cast<uint32_t>(pose);
    out->sel_near = static_cast<uint32_t>(near_plane);
    out->proj_aspect = static_cast<uint32_t>(aspect);
    return true;
}

bool RenderLayout::readable(uint64_t offset, size_t n) const {
    if (!pose || !n) return false;
    return inside(offset, n, client_world, 8) || inside(offset, n, pose, kPoseBytes) || inside(offset, n, sel_near, 12) ||
           inside(offset, n, proj_aspect - 4u, 20);
}

bool RenderLayout::writable(uint64_t offset, size_t n) const { return pose && n && inside(offset, n, pose, kPoseValueBytes); }

bool Renderer::read(uint32_t offset, void* dst, size_t n, bool* fault) const {
    if (!layout_.readable(offset, n)) return false;
    if (mem_.copy_in(base_ + offset, dst, n)) return true;
    *fault = true;
    return false;
}

bool Renderer::write(uint32_t offset, const void* src, size_t n, bool* fault) const {
    if (!layout_.writable(offset, n)) return false;
    if (mem_.copy_out(base_ + offset, src, n)) return true;
    *fault = true;
    return false;
}

// ---- the store --------------------------------------------------------------------------------------------------------

void SnapshotStore::publish(const Snapshot& s) {
    uint64_t w[kWords];
    std::memcpy(w, &s, sizeof w);
    seq_.fetch_add(1, std::memory_order_acq_rel);  // odd: a reader from here on is told "busy"
    for (size_t i = 0; i < kWords; ++i) words_[i].store(w[i], std::memory_order_relaxed);
    seq_.fetch_add(1, std::memory_order_release);
}

bool SnapshotStore::try_read(Snapshot* out) const {
    const uint64_t before = seq_.load(std::memory_order_acquire);
    if (before & 1) return false;
    uint64_t w[kWords];
    for (size_t i = 0; i < kWords; ++i) w[i] = words_[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq_.load(std::memory_order_relaxed) != before) return false;
    std::memcpy(out, w, sizeof w);
    return true;
}

// ---- lifecycle ----------------------------------------------------------------------------------------------------------

void Frames::world_started(uint32_t epoch, uint64_t world_id) {
    world_id_.store(world_id, std::memory_order_release);
    faulted_.store(false, std::memory_order_release);
    epoch_.store(epoch, std::memory_order_release);
}

void Frames::world_stopping() {
    epoch_.store(0, std::memory_order_release);  // first: no hook takes the world for live after this
    world_id_.store(0, std::memory_order_release);
}

// ---- the hooks ----------------------------------------------------------------------------------------------------------

void Frames::put_pending(const Pending& p) {
    uint64_t w[sizeof(Pending) / 8];
    std::memcpy(w, &p, sizeof w);
    pend_seq_.fetch_add(1, std::memory_order_acq_rel);
    for (size_t i = 0; i < sizeof w / 8; ++i) pend_words_[i].store(w[i], std::memory_order_relaxed);
    pend_seq_.fetch_add(1, std::memory_order_release);
}

bool Frames::get_pending(Pending* out) const {
    const uint64_t before = pend_seq_.load(std::memory_order_acquire);
    if (before & 1) return false;
    uint64_t w[sizeof(Pending) / 8];
    for (size_t i = 0; i < sizeof w / 8; ++i) w[i] = pend_words_[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (pend_seq_.load(std::memory_order_relaxed) != before) return false;
    std::memcpy(out, w, sizeof w);
    return true;
}

bool Frames::ours(const Renderer& r, uint32_t* epoch) {
    *epoch = epoch_.load(std::memory_order_acquire);
    const uint64_t live = world_id_.load(std::memory_order_acquire);
    if (!*epoch || !live || !layout_.pose) return false;
    uint64_t world = 0;
    bool fault = false;
    if (!r.read(layout_.client_world, &world, sizeof world, &fault)) {
        if (fault) faulted_.store(true, std::memory_order_release);
        return false;
    }
    return world == live;
}

uint64_t Frames::select(const Renderer& r) {
    uint32_t epoch = 0;
    if (!ours(r, &epoch)) return 0;
    Pending p;
    p.selection = selection_.fetch_add(1, std::memory_order_acq_rel) + 1;
    p.renderer = r.base();
    p.sample_step = sample_step_.load(std::memory_order_acquire);
    p.epoch = epoch;
    put_pending(p);
    return p.selection;
}

void Frames::publish_invalid(uint32_t epoch, uint64_t step, uint32_t reason) {
    Snapshot s;
    s.frame = frames_.fetch_add(1, std::memory_order_acq_rel) + 1;
    s.sample_step = step;
    s.captured_ns = clock_->now_ns();
    s.epoch = epoch;
    s.reason = reason;
    store_.publish(s);
}

void Frames::capture(const Renderer& r) {
    uint32_t epoch = 0;
    if (faulted_.load(std::memory_order_acquire) || !ours(r, &epoch)) return;
    uint64_t step = sample_step_.load(std::memory_order_acquire);
    if (selection_hooked_) {
        Pending p;
        // the selection of this frame, of this renderer, of this world, not taken yet; otherwise this is not a new frame
        if (!get_pending(&p) || !p.selection || p.selection == captured_selection_ || p.renderer != r.base() || p.epoch != epoch) return;
        captured_selection_ = p.selection;
        step = p.sample_step;
    }
    const RenderLayout& l = layout_;
    uint8_t raw[kPoseBytes] = {};
    float lens[4] = {};  // aspect, near, far, field of view
    bool fault = false;
    if (!r.read(l.pose, raw, kPoseValueBytes, &fault) || !r.read(l.proj_aspect, lens, sizeof lens, &fault)) {
        if (fault) faulted_.store(true, std::memory_order_release);  // and no snapshot of this world any more
        publish_invalid(epoch, step, KILN_CAMERA_REASON_NO_CAMERA_FRAME);
        return;
    }
    EnginePose pose;
    std::memcpy(&pose, raw, sizeof pose);
    Quat q{pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]};
    if (!lens_ok(lens[3], lens[0], lens[1], lens[2]) || !scale_ok(pose.scale) || !engine_unit(&q)) {
        // the game's own view is one v1 does not describe: say "no frame" rather than keep an older one valid
        publish_invalid(epoch, step, KILN_CAMERA_REASON_NO_CAMERA_FRAME);
        return;
    }
    Snapshot s;
    s.frame = frames_.fetch_add(1, std::memory_order_acq_rel) + 1;
    s.sample_step = step;
    s.captured_ns = clock_->now_ns();
    s.epoch = epoch;
    for (int i = 0; i < 3; ++i) s.position[i] = fixed_to_metres(pose.position[i]);
    s.rotation[0] = static_cast<float>(q.x);
    s.rotation[1] = static_cast<float>(q.y);
    s.rotation[2] = static_cast<float>(q.z);
    s.rotation[3] = static_cast<float>(q.w);
    s.aspect = lens[0];
    s.near_plane = lens[1];
    s.far_plane = lens[2];
    s.fov = lens[3];
    // an old frame that was in flight when the world stopped cannot publish as live
    if (epoch_.load(std::memory_order_acquire) != epoch) return;
    store_.publish(s);
}

// ---- the read -----------------------------------------------------------------------------------------------------------

uint32_t Frames::get_visual(KilnCameraVisual* out) const {
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    Snapshot s;
    if (!store_.try_read(&s)) return KILN_CAMERA_REASON_SNAPSHOT_BUSY;
    const uint32_t live = epoch_.load(std::memory_order_acquire);
    // a fault ends this world's snapshots (the hooks stop reading): the last one is not handed out either
    if (!s.frame || !live || s.epoch != live || faulted_.load(std::memory_order_acquire)) return KILN_CAMERA_REASON_NO_CAMERA_FRAME;
    if (s.reason) return s.reason;
    const int64_t age = clock_->now_ns() - s.captured_ns;
    if (age > kSnapshotValidNs) return KILN_CAMERA_REASON_SNAPSHOT_STALE;
    // the world may have stopped while we copied: then nothing is given
    if (epoch_.load(std::memory_order_acquire) != s.epoch) return KILN_CAMERA_REASON_NO_CAMERA_FRAME;
    out->valid = 1;
    out->local_generation = s.epoch;
    out->frame = s.frame;
    out->target_sample_step = s.sample_step;
    out->captured_monotonic_ns = static_cast<uint64_t>(s.captured_ns);
    for (int i = 0; i < 3; ++i) out->pose.position[i] = s.position[i];
    for (int i = 0; i < 4; ++i) out->pose.rotation[i] = s.rotation[i];
    out->fov_y_radians = s.fov;
    out->aspect = s.aspect;
    out->near_plane = s.near_plane;
    out->far_plane = s.far_plane;
    return 0;
}

}  // namespace kcam
