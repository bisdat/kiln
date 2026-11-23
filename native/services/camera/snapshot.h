// SPDX-License-Identifier: MIT
// kiln.camera's final camera snapshot (chapter 03, 2.10a, 2.10b, 4.5): what a frame was really drawn with.
//
// Two mid hooks in the game's renderer feed it. camera/RenderFovSelected (the selection: r13 = renderer) is where the
// renderer has copied the selected camera pose and is about to store the selected field of view; every call there is a
// new selection id. camera/VisualFrameReady (frame preparation: rsi = renderer) is after the projection descriptor got
// its aspect, near, far and field of view: the pose and the lens there are final (after the game's shake and after
// any kiln.camera override), so that is where a snapshot is taken, once per selection id, and only for the renderer
// whose client world is the live one (other views and previews are skipped).
//
// The renderer is opaque: no size is assumed, nothing is allocated or copied whole. A Renderer borrow reads only the
// regions the chapter allows (2.10b): the client-world field, the selected pose (56 bytes) and lens, the projection
// descriptor's prefix; and it writes only the selected pose's 52 value bytes (S5). Every access goes through a
// RendererMem, which is a structured-exception guarded copy in the game and a byte buffer in the host tests.
//
// Threads. The hooks run on the render path (any thread; not measured, G7): they read atomics and the renderer, write
// the snapshot store and call nothing else (no game function, no author code, no lock that can wait). get_visual is any
// thread and tries once. Lifecycle (world_started/world_stopping) may come on any thread: atomics only.
#ifndef KILN_CAMERA_SNAPSHOT_H
#define KILN_CAMERA_SNAPSHOT_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

#include "services/kiln_camera.h"

namespace kcam {

// The process's monotonic clock in nanoseconds: QueryPerformanceCounter in the game (MSVC's steady_clock), a fake in
// the host tests. Managed Kiln.Camera reads the same counter (Stopwatch) to compute a snapshot's age.
class Clock {
public:
    virtual ~Clock() = default;
    virtual int64_t now_ns() const = 0;
};

constexpr int64_t kSnapshotValidNs = 250'000'000;  // a snapshot older than this is stale
constexpr uint32_t kPoseBytes = 56, kPoseValueBytes = 52;

// Where the renderer keeps what kiln.camera reads, from the sigdb constants camera/render.* (byte offsets from the
// renderer). make() refuses a set that does not hang together as the chapter describes it.
struct RenderLayout {
    uint32_t client_world = 0;  // the client world the renderer draws (a number, never followed)
    uint32_t pose = 0;          // selected WorldTransform: s64 position[3] (32.32), f32 rotation[4], f32 scale[3], pad
    uint32_t sel_near = 0;      // selected near, far, field of view (radians): 12 bytes right after the pose
    uint32_t proj_aspect = 0;   // projection descriptor: aspect, near, far, field of view (radians), after its kind

    uint32_t sel_far() const { return sel_near + 4; }
    uint32_t sel_fov() const { return sel_near + 8; }
    uint32_t proj_near() const { return proj_aspect + 4; }
    uint32_t proj_far() const { return proj_aspect + 8; }
    uint32_t proj_fov() const { return proj_aspect + 12; }

    // Looks up camera/render.<name> through `constant`. False with the first problem in *why.
    static bool make(const std::function<bool(const char* name, int64_t* out)>& constant, RenderLayout* out, std::string* why);
    // The chapter's permitted regions: [client_world, +8), [pose, +56), [sel_near, +12), [proj_aspect - 4, +20).
    bool readable(uint64_t offset, size_t n) const;
    // Only the selected pose's first 52 bytes (position and rotation; the scale and the padding stay).
    bool writable(uint64_t offset, size_t n) const;
};

// The raw, guarded memory access (production: SEH memcpy at an absolute address). False: it faulted.
class RendererMem {
public:
    virtual ~RendererMem() = default;
    virtual bool copy_in(uintptr_t address, void* dst, size_t n) = 0;
    virtual bool copy_out(uintptr_t address, const void* src, size_t n) = 0;
};

// One hook invocation's borrow of the renderer. Discarded before the hook returns.
class Renderer {
public:
    Renderer(RendererMem& mem, const RenderLayout& layout, uintptr_t base) : mem_(mem), layout_(layout), base_(base) {}
    uintptr_t base() const { return base_; }
    const RenderLayout& layout() const { return layout_; }
    // False: outside the permitted regions, or the copy faulted (*fault true then).
    bool read(uint32_t offset, void* dst, size_t n, bool* fault) const;
    bool write(uint32_t offset, const void* src, size_t n, bool* fault) const;

private:
    RendererMem& mem_;
    const RenderLayout& layout_;
    uintptr_t base_;
};

// The selected pose as the engine keeps it.
struct EnginePose {
    int64_t position[3];
    float rotation[4];
    float scale[3];
};
static_assert(offsetof(EnginePose, rotation) == 24 && offsetof(EnginePose, scale) + 12 == kPoseValueBytes && sizeof(EnginePose) == kPoseBytes,
              "a WorldTransform: 52 value bytes in 56");

// One published snapshot. reason 0: valid; else why there is none (NO_CAMERA_FRAME for a view the service refuses to
// describe: a lens or a scale v1 does not accept, a rotation that is not a unit quaternion).
struct Snapshot {
    uint64_t frame = 0;        // 0: nothing was published yet
    uint64_t sample_step = 0;  // the client step whose samples the frame's camera was built from
    int64_t captured_ns = 0;
    uint32_t epoch = 0;        // the client world's epoch
    uint32_t reason = 0;
    double position[3] = {0, 0, 0};
    float rotation[4] = {0, 0, 0, 1};
    float fov = 0, aspect = 0, near_plane = 0, far_plane = 0;  // radians for fov
};
static_assert(sizeof(Snapshot) % 8 == 0, "copied as whole words");

// The snapshot store: one writer (the capture hook), any reader, nobody waits. A reader that meets a write in progress
// is answered "busy" (SNAPSHOT_BUSY), never a torn value. Every byte goes through an atomic word.
class SnapshotStore {
public:
    void publish(const Snapshot& s);
    // False: a write was in progress (or ran over the read).
    bool try_read(Snapshot* out) const;

    // Tests: hold a write open, as the capture thread would be in the middle of one.
    void hold_for_test() { seq_.fetch_add(1, std::memory_order_acq_rel); }
    void release_for_test() { seq_.fetch_add(1, std::memory_order_acq_rel); }

private:
    static constexpr size_t kWords = sizeof(Snapshot) / 8;
    std::atomic<uint64_t> seq_{0};  // odd while a write is in progress
    std::atomic<uint64_t> words_[kWords]{};
};

// The two hooks' shared state and get_visual. S5's render transaction sits inside select() (frame()).
class Frames {
public:
    explicit Frames(const Clock* clock) : clock_(clock) {}

    void set_layout(const RenderLayout& layout) { layout_ = layout; }
    const RenderLayout& layout() const { return layout_; }
    // Whether the selection hook is installed: the capture then takes one snapshot per selection id. Without it every
    // capture is a frame of its own. Setup only.
    void set_selection_hooked(bool on) { selection_hooked_ = on; }

    // ---- lifecycle (any thread) ----
    void world_started(uint32_t epoch, uint64_t world_id);
    void world_stopping();
    uint32_t epoch() const { return epoch_.load(std::memory_order_acquire); }
    // The client step of the newest local samples (client Pre). Recorded with each selection.
    void set_sample_step(uint64_t step) { sample_step_.store(step, std::memory_order_release); }

    // ---- the hooks (render path) ----
    // The selection: a new selection id for the live client world's renderer (0: not that renderer, or no world).
    uint64_t select(const Renderer& r);
    // Frame preparation: publishes the snapshot of the renderer's newest selection, once per selection id.
    void capture(const Renderer& r);

    // ---- what the render transaction wrote (render_tx.h) ----
    enum : uint32_t { kWrotePose = 1, kWroteFov = 2 };
    struct Expected {
        uint64_t gen = 0;    // the plan whose values were written (0: nothing was written this frame)
        uint32_t wrote = 0;  // kWrotePose | kWroteFov
        float fov = 0;       // radians, as handed to the game's store
        int64_t position[3] = {0, 0, 0};
        float rotation[4] = {0, 0, 0, 0};
    };
    // The selection hook, after select() gave `selection`: what it wrote, for this frame's capture to check.
    void expect(uint64_t selection, const Expected& e);
    // A frame's verdict on a plan: the final camera is what was written (position exact in 32.32, rotation and field
    // of view within 1e-5), or not (something after kiln.camera changed it).
    struct Outcome {
        uint64_t gen = 0, frame = 0;
        bool matched = false;
    };
    static constexpr uint32_t kOutcomes = 32;
    // The client Pre: the next verdict after *cursor, oldest first (false: none). A reader more than kOutcomes behind
    // loses the oldest.
    bool next_outcome(uint64_t* cursor, Outcome* out) const;

    // ---- the read (any thread) ----
    // 0 with *out filled (valid 1), or the reason there is none (*out zeroed, valid 0): NO_CAMERA_FRAME, SNAPSHOT_BUSY,
    // SNAPSHOT_STALE.
    uint32_t get_visual(KilnCameraVisual* out) const;

    // ---- the log and tests ----
    bool faulted() const { return faulted_.load(std::memory_order_acquire); }
    uint64_t published() const { return frames_.load(std::memory_order_acquire); }
    SnapshotStore& store_for_test() { return store_; }

private:
    // What the selection told the capture (a seqlock: the capture reads it in one go or skips the frame).
    struct Pending {
        uint64_t selection = 0;
        uint64_t renderer = 0;
        uint64_t sample_step = 0;
        uint32_t epoch = 0, pad = 0;
        Expected expected;
    };
    static_assert(sizeof(Expected) == 56 && sizeof(Pending) % 8 == 0, "copied as whole words");
    struct OutcomeSlot {
        std::atomic<uint64_t> seq{0}, gen{0}, frame{0}, matched{0};
    };
    void push_outcome(uint64_t gen, uint64_t frame, bool matched);  // the capture hook only
    void put_pending(const Pending& p);
    bool get_pending(Pending* out) const;
    // The live client world's renderer: its world field equals the live world's number.
    bool ours(const Renderer& r, uint32_t* epoch);
    void publish_invalid(uint32_t epoch, uint64_t step, uint32_t reason, uint64_t frame);

    const Clock* clock_;
    RenderLayout layout_{};
    bool selection_hooked_ = false;
    std::atomic<uint32_t> epoch_{0};
    std::atomic<uint64_t> world_id_{0};
    std::atomic<uint64_t> sample_step_{0};
    std::atomic<uint64_t> selection_{0};   // selection ids handed out (never reused in the process)
    std::atomic<uint64_t> frames_{0};      // snapshots published (the snapshot's frame number)
    std::atomic<bool> faulted_{false};     // a guarded copy faulted: no snapshot for the rest of this world
    std::atomic<uint64_t> pend_seq_{0};
    std::atomic<uint64_t> pend_words_[sizeof(Pending) / 8]{};
    uint64_t captured_selection_ = 0;      // the capture hook's own: the last selection it published
    Pending sel_pending_{};                // the selection hook's own: its newest record
    OutcomeSlot outcomes_[kOutcomes];
    std::atomic<uint64_t> outcome_count_{0};
    SnapshotStore store_;
};

}  // namespace kcam
#endif
