// SPDX-License-Identifier: MIT
// kiln.camera's render path and client Pre on their own threads, built with -fsanitize=thread (run.sh): a render
// thread draws frames (select, the transaction, capture, get_visual) and another thread reads get_visual, while the
// main thread runs the client Pre (sampler, step, the core's deliveries) through applies of every kind, supersessions,
// ends, owner retirements and world changes. ThreadSanitizer fails the run on a data race; the checks below that the
// work happened and that nothing is left held. (After the reviewer's probe, dev.31 03-camera-client review.)
#define main render_tests_main
#include "render_test.cpp"
#undef main

#include <atomic>
#include <chrono>
#include <thread>

namespace {

struct RealClock : Clock {
    int64_t now_ns() const override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};

int concurrency_main() {
    RealClock clock;
    Env env;
    g_env = &env;
    Core core{&env};
    Frames frames{&clock};
    SampleBoard board;
    FakeSource source;
    Sampler sampler{&source, &board, &clock};
    LocalPlayer world;
    LocalBackend backend{&core, &frames, &board, &clock, &world};
    FakeRenderer mem;
    RenderLayout layout;
    std::string why;
    CHECK(RenderLayout::make([](const char* n, int64_t* out) { auto it = kConstants.find(n); return it != kConstants.end() && (*out = it->second, true); }, &layout, &why));
    frames.set_layout(layout);
    frames.set_selection_hooked(true);
    backend.set_serving(kAllLocal);
    core.set_backend(KILN_CAMERA_LOCAL_VISUAL, &backend);
    core.set_writes(true);
    KilnHandle h = 0;
    CHECK(core.subscribe(7, &on_changed, nullptr, &h) == KILN_OK);
    std::memcpy(&mem.bytes[layout.client_world], &kWorld, 8);
    EnginePose stock{};
    metres_to_fixed(1.7, &stock.position[1]);
    stock.rotation[3] = 1;
    stock.scale[0] = stock.scale[1] = stock.scale[2] = 1;
    const float lens[4] = {16.f / 9.f, 0.1f, 1000.f, kNaturalFov};
    std::memcpy(&mem.bytes[layout.proj_aspect], lens, sizeof lens);
    source.put(1, 0, 0, 0);
    source.put(5, 3, 0, 4, yaw_q(0.5));
    core.world_started(kClient, 1);
    frames.world_started(1, kWorld);

    std::atomic<bool> run{true};
    std::atomic<uint64_t> drawn{0}, reads{0};
    std::thread render([&] {
        while (run.load()) {
            std::memcpy(&mem.bytes[layout.pose], &stock, kPoseValueBytes);
            float fov = kNaturalFov;
            Renderer r(mem, layout, mem.base());
            if (const uint64_t sel = frames.select(r)) backend.on_select(r, sel, &fov);
            std::memcpy(&mem.bytes[layout.sel_fov()], &fov, 4);
            std::memcpy(&mem.bytes[layout.proj_fov()], &fov, 4);
            frames.capture(r);
            KilnCameraVisual v{};
            v.size = sizeof v;
            frames.get_visual(&v);
            drawn.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    std::thread reader([&] {
        while (run.load()) {
            KilnCameraVisual v{};
            v.size = sizeof v;
            frames.get_visual(&v);
            reads.fetch_add(1);
            std::this_thread::yield();
        }
    });

    std::vector<KilnCameraKey> keys = {key(0, 0, 10, 0), key(500, 3, 10, 0)};
    constexpr int kRounds = 600;
    std::vector<Result> results(2 * kRounds);
    uint64_t step = 1;
    uint32_t epoch = 1;
    for (int i = 0; i < kRounds; ++i) {
        KilnCameraRequest q{};
        q.size = sizeof q;
        q.endpoint = KILN_CAMERA_LOCAL_VISUAL;
        q.duration_ms = 40 + (i % 7) * 10;
        q.profile.rotation[3] = 1;
        switch (i % 4) {
        case 0: q.parts = KILN_CAMERA_LENS, q.lens_source = KILN_CAMERA_LENS_CONSTANT, q.fov_y_degrees = 40.0f + static_cast<float>(i % 50); break;
        case 1: q.parts = KILN_CAMERA_PATH, q.keys = keys.data(), q.key_count = 2, q.duration_ms = 600, q.blend_out_ms = 30; break;
        case 2:
            q.parts = KILN_CAMERA_SHAKE | KILN_CAMERA_LENS, q.lens_source = KILN_CAMERA_LENS_CONSTANT, q.fov_y_degrees = 70;
            q.shake = KilnCameraShake{0.3f, 5.0f, 0, static_cast<uint32_t>(i)};
            break;
        case 3: q.parts = KILN_CAMERA_PROFILE, q.profile = profile_of(5, 3, KILN_CAMERA_FOLLOW_HEADING); break;
        }
        q.priority = -1000 + (i % 2000);
        KilnCameraLease lease = 0;
        core.apply(100 + i, &q, &lease, &on_done, &results[i], kOnClient);
        for (int k = 0; k < 6; ++k) {
            uint32_t t[kSampleSlots];
            const uint32_t n = backend.targets(t, kSampleSlots - 1);
            sampler.sample(frames.epoch(), ++step, 1, t, n);
            frames.set_sample_step(step);
            backend.step();
            core.pre(kClient);
            std::this_thread::sleep_for(std::chrono::microseconds(700));
        }
        if (lease && i % 5 == 0) core.end(100 + i, lease, &on_done, &results[kRounds + i], kOnClient);
        if (i % 97 == 0) core.owner_retiring(100 + i);
        if (i % 151 == 0) {  // a world change while the render thread draws
            core.world_stopping(kClient, epoch);
            ++epoch;
            core.world_started(kClient, epoch);
            frames.world_started(epoch, kWorld);
        }
    }
    // let the last leases end, then stop the threads
    for (int k = 0; k < 400 && core.live_leases(); ++k) {
        sampler.sample(frames.epoch(), ++step, 1, nullptr, 0);
        backend.step();
        core.pre(kClient);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    run.store(false);
    render.join();
    reader.join();
    int ok = 0;
    for (int i = 0; i < kRounds; ++i) ok += results[i].seen && results[i].r.ok;
    CHECK(drawn.load() > 1000 && reads.load() > 0);
    CHECK(ok > kRounds / 10);         // requests were drawn and confirmed while everything else went on
    CHECK(core.live_leases() == 0);   // nothing is left holding the camera
    std::printf("camera concurrency: PASS (%llu frames, %d applied of %d, %zu events, %u plans held)\n", static_cast<unsigned long long>(drawn.load()), ok, kRounds,
                env.lines.size(), backend.plans_held());
    return 0;
}

}  // namespace

int main() { return concurrency_main(); }
