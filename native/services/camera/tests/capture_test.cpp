// SPDX-License-Identifier: MIT
// kiln.camera's final snapshot (S4) on a fake renderer: the layout and its permitted regions, the store that never
// waits, one snapshot per selection, 250 ms validity, the client world's epoch, other views, a view v1 refuses, a fault.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../snapshot.h"

static int checks;
#define CHECK(x)                                                                    \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            std::fprintf(stderr, "FAIL capture:%d: %s\n", __LINE__, #x);            \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)

namespace {

using namespace kcam;

// client 26430625's numbers (sigdb/symbols/client.d/dev28-03-camera.json)
const std::map<std::string, int64_t> kConstants = {
    {"camera/render.client_world", 464},     {"camera/render.pose_start", 4616872},   {"camera/render.pose_block1", 4616888},
    {"camera/render.pose_block2", 4616904},  {"camera/render.pose_tail", 4616920},    {"camera/render.near", 4616928},
    {"camera/render.far", 4616932},          {"camera/render.selected_fov", 4616936}, {"camera/render.proj_aspect", 4625336},
    {"camera/render.proj_near", 4625340},    {"camera/render.proj_far", 4625344},     {"camera/render.proj_fov", 4625348},
};

struct FakeClock : Clock {
    int64_t now = 1'000'000'000;
    int64_t now_ns() const override { return now; }
};

// A renderer-sized byte block: every access is recorded as an offset from the block, a fault can be switched on.
struct FakeRenderer : RendererMem {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x469400, 0xcd);
    bool fault = false;
    struct Access { uint64_t offset; size_t n; bool write; };
    std::vector<Access> log;
    uintptr_t base() const { return reinterpret_cast<uintptr_t>(bytes.data()); }
    bool copy_in(uintptr_t a, void* dst, size_t n) override {
        log.push_back({a - base(), n, false});
        if (fault || a < base() || a + n > base() + bytes.size()) return false;
        std::memcpy(dst, reinterpret_cast<const void*>(a), n);
        return true;
    }
    bool copy_out(uintptr_t a, const void* src, size_t n) override {
        log.push_back({a - base(), n, true});
        if (fault || a < base() || a + n > base() + bytes.size()) return false;
        std::memcpy(reinterpret_cast<void*>(a), src, n);
        return true;
    }
    template <class T> void put(uint64_t off, T v) { std::memcpy(&bytes[off], &v, sizeof v); }
};

RenderLayout layout() {
    RenderLayout l;
    std::string why;
    CHECK(RenderLayout::make([](const char* n, int64_t* out) { auto it = kConstants.find(n); return it != kConstants.end() && (*out = it->second, true); }, &l, &why));
    return l;
}

constexpr uint64_t kWorld = 0x7ff612340000ull;

// A stock frame: position (10.5, 2, -3), identity, scale 1, lens 1.13446 rad, 16:9, 0.1..1000.
void stock(FakeRenderer& r, const RenderLayout& l, uint64_t world = kWorld) {
    r.put<uint64_t>(l.client_world, world);
    EnginePose p{};
    p.position[0] = static_cast<int64_t>(10.5 * 4294967296.0);
    p.position[1] = static_cast<int64_t>(2 * 4294967296.0);
    p.position[2] = static_cast<int64_t>(-3 * 4294967296.0);
    p.rotation[3] = 1;
    p.scale[0] = p.scale[1] = p.scale[2] = 1;
    std::memcpy(&r.bytes[l.pose], &p, sizeof p);
    r.put<float>(l.sel_near, 0.1f);
    r.put<float>(l.sel_far(), 1000.f);
    r.put<float>(l.sel_fov(), 1.13446f);
    r.put<float>(l.proj_aspect, 16.f / 9.f);
    r.put<float>(l.proj_near(), 0.1f);
    r.put<float>(l.proj_far(), 1000.f);
    r.put<float>(l.proj_fov(), 1.13446f);
}

void frame(Frames& f, FakeRenderer& mem, const RenderLayout& l) {
    Renderer r(mem, l, mem.base());
    f.select(r);
    f.capture(r);
}

uint32_t visual(const Frames& f, KilnCameraVisual* v) {
    std::memset(v, 0x5a, sizeof *v);
    v->size = sizeof *v;
    return f.get_visual(v);
}

void test_layout() {
    const RenderLayout l = layout();
    CHECK(l.client_world == 0x1d0 && l.pose == 0x4672a8 && l.sel_near == 0x4672e0 && l.proj_aspect == 0x4693b8);
    CHECK(l.sel_fov() == 0x4672e8 && l.proj_fov() == 0x4693c4);
    // 2.10b: exactly the permitted regions
    CHECK(l.readable(0x1d0, 8) && !l.readable(0x1d0, 9) && !l.readable(0x1cf, 1));
    CHECK(l.readable(0x4672a8, 56) && l.readable(0x4672e0, 12) && !l.readable(0x4672e0, 13) && !l.readable(0x4672a0, 8));
    CHECK(l.readable(0x4693b4, 20) && !l.readable(0x4693b4, 21) && !l.readable(0x4693b0, 4));
    CHECK(l.writable(0x4672a8, 52) && !l.writable(0x4672a8, 53) && !l.writable(0x4672dc, 4) && !l.writable(0x4672e8, 4) && !l.writable(0x1d0, 8));
    CHECK(!l.readable(UINT64_MAX - 2, 8) && !l.writable(UINT64_MAX - 2, 8));
    // a set that does not hang together is refused, whatever the reason
    for (const char* moved : {"camera/render.pose_tail", "camera/render.far", "camera/render.proj_fov", "camera/render.pose_block1"}) {
        auto c = kConstants;
        c[moved] += 4;
        RenderLayout bad;
        std::string why;
        CHECK(!RenderLayout::make([&](const char* n, int64_t* out) { auto it = c.find(n); return it != c.end() && (*out = it->second, true); }, &bad, &why));
        CHECK(!why.empty());
    }
    RenderLayout none;
    std::string why;
    CHECK(!RenderLayout::make([](const char*, int64_t*) { return false; }, &none, &why));
    auto huge = kConstants;
    huge["camera/render.client_world"] = -8;
    CHECK(!RenderLayout::make([&](const char* n, int64_t* out) { auto it = huge.find(n); return it != huge.end() && (*out = it->second, true); }, &none, &why));
}

void test_store() {
    SnapshotStore s;
    Snapshot a;
    CHECK(s.try_read(&a) && a.frame == 0);
    Snapshot b;
    b.frame = 9;
    b.position[1] = 2.5;
    b.fov = 1.25f;
    s.publish(b);
    CHECK(s.try_read(&a) && a.frame == 9 && a.position[1] == 2.5 && a.fov == 1.25f);
    s.hold_for_test();
    CHECK(!s.try_read(&a));  // a write in progress: busy, not a torn value
    s.release_for_test();
    CHECK(s.try_read(&a) && a.frame == 9);
}

void test_capture() {
    FakeClock clock;
    const RenderLayout l = layout();
    FakeRenderer mem;
    stock(mem, l);
    Frames f(&clock);
    f.set_layout(l);
    f.set_selection_hooked(true);
    KilnCameraVisual v;

    // no client world: nothing
    {
        Renderer r(mem, l, mem.base());
        CHECK(f.select(r) == 0);
        f.capture(r);
    }
    CHECK(visual(f, &v) == KILN_CAMERA_REASON_NO_CAMERA_FRAME && v.valid == 0 && v.size == sizeof v && v.frame == 0 && v.pose.rotation[3] == 0);

    f.world_started(1, kWorld);
    f.set_sample_step(41);
    Renderer r(mem, l, mem.base());
    const uint64_t first = f.select(r);
    CHECK(first == 1);
    f.capture(r);
    CHECK(f.published() == 1);
    CHECK(visual(f, &v) == 0);
    CHECK(v.valid == 1 && v.local_generation == 1 && v.frame == 1 && v.target_sample_step == 41 && v.captured_monotonic_ns == static_cast<uint64_t>(clock.now));
    CHECK(v.pose.position[0] == 10.5 && v.pose.position[1] == 2 && v.pose.position[2] == -3 && v.pose.rotation[3] == 1);
    CHECK(v.fov_y_radians == 1.13446f && std::fabs(v.aspect - 16.f / 9.f) < 1e-6f && v.near_plane == 0.1f && v.far_plane == 1000.f);

    // once per selection id: a paused image (no new selection) does not refresh the age
    clock.now += 100'000'000;
    f.capture(r);
    f.capture(r);
    CHECK(f.published() == 1);
    CHECK(visual(f, &v) == 0 && v.captured_monotonic_ns == static_cast<uint64_t>(clock.now - 100'000'000));
    clock.now += 150'000'000;  // 250 ms exactly: still valid
    CHECK(visual(f, &v) == 0);
    clock.now += 1;
    CHECK(visual(f, &v) == KILN_CAMERA_REASON_SNAPSHOT_STALE && v.valid == 0 && v.frame == 0);
    // the next selection publishes again
    f.set_sample_step(42);
    CHECK(f.select(r) == 2);
    f.capture(r);
    CHECK(f.published() == 2 && visual(f, &v) == 0 && v.frame == 2 && v.target_sample_step == 42);

    // a held write: busy, never garbage
    f.store_for_test().hold_for_test();
    CHECK(visual(f, &v) == KILN_CAMERA_REASON_SNAPSHOT_BUSY && v.valid == 0);
    f.store_for_test().release_for_test();

    // another view (its world field is not the live client world): skipped by both hooks
    FakeRenderer preview;
    stock(preview, l, kWorld + 0x1000);
    {
        Renderer p(preview, l, preview.base());
        CHECK(f.select(p) == 0);
        f.capture(p);
    }
    CHECK(f.published() == 2);
    // the capture of a renderer that is not the one of the newest selection: skipped
    FakeRenderer twin;
    stock(twin, l);
    f.select(r);
    {
        Renderer t(twin, l, twin.base());
        f.capture(t);
    }
    CHECK(f.published() == 2);
    f.capture(r);
    CHECK(f.published() == 3);

    // the world stops: the snapshot is invalid at once, also after a new world started, until that world's own frame
    f.world_stopping();
    CHECK(visual(f, &v) == KILN_CAMERA_REASON_NO_CAMERA_FRAME);
    {
        Renderer again(mem, l, mem.base());
        CHECK(f.select(again) == 0);
    }
    f.world_started(2, kWorld);
    CHECK(visual(f, &v) == KILN_CAMERA_REASON_NO_CAMERA_FRAME);
    frame(f, mem, l);
    CHECK(visual(f, &v) == 0 && v.local_generation == 2);

    // a view v1 does not describe: published as "no frame", the older snapshot does not stay valid
    for (int bad = 0; bad < 5; ++bad) {
        stock(mem, l);
        if (bad == 0) mem.put<float>(l.pose + 40, 1.5f);                // scale x
        if (bad == 1) mem.put<float>(l.proj_fov(), 0.0f);               // lens under 0.01 rad
        if (bad == 2) mem.put<float>(l.pose + 36, 2.0f);                // rotation w: not a unit quaternion
        if (bad == 3) mem.put<float>(l.proj_far(), 0.05f);              // far before near
        if (bad == 4) mem.put<float>(l.proj_aspect, std::nanf(""));
        frame(f, mem, l);
        CHECK(visual(f, &v) == KILN_CAMERA_REASON_NO_CAMERA_FRAME);
    }
    stock(mem, l);
    mem.put<float>(l.pose + 36, 1.004f);  // within the engine tolerance: normalized
    frame(f, mem, l);
    CHECK(visual(f, &v) == 0 && v.pose.rotation[3] == 1.0f);

    // every access stayed inside the permitted regions, and the snapshot path wrote nothing
    for (const auto& a : mem.log) CHECK(!a.write && l.readable(a.offset, a.n));

    // a fault: no snapshot for the rest of this world; the next world starts clean
    mem.fault = true;
    frame(f, mem, l);
    CHECK(f.faulted() && visual(f, &v) == KILN_CAMERA_REASON_NO_CAMERA_FRAME);
    mem.fault = false;
    frame(f, mem, l);
    CHECK(visual(f, &v) == KILN_CAMERA_REASON_NO_CAMERA_FRAME);
    f.world_stopping();
    f.world_started(3, kWorld);
    CHECK(!f.faulted());
    frame(f, mem, l);
    CHECK(visual(f, &v) == 0 && v.local_generation == 3);
}

void test_without_selection_hook() {
    FakeClock clock;
    const RenderLayout l = layout();
    FakeRenderer mem;
    stock(mem, l);
    Frames f(&clock);
    f.set_layout(l);
    f.world_started(7, kWorld);
    Renderer r(mem, l, mem.base());
    f.capture(r);
    f.capture(r);
    KilnCameraVisual v;
    CHECK(f.published() == 2 && visual(f, &v) == 0 && v.frame == 2 && v.local_generation == 7);
}

}  // namespace

int main() {
    test_layout();
    test_store();
    test_capture();
    test_without_selection_hook();
    std::printf("camera capture: PASS (%d checks)\n", checks);
}
