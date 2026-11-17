// SPDX-License-Identifier: MIT
// The overlay's image budget (image_budget.h): the descriptor pool vk.cpp creates holds every image Kiln lets exist plus
// ImGui's own, a frame creates at most kUploadsPerFrame textures, a texture without a descriptor set is never drawn, and
// loading 700 distinct images (Kiln.Ui's item icons past their cache, mods' PNGs) cannot take more sets than the pool has.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../image_budget.h"

namespace {
int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

using namespace kui::img;

struct Fake {
    bool created = false, uploaded = false, has_set = false, failed = false, freed = false;
};
}  // namespace

int main(int argc, char** argv) {
    // the pool vk.cpp creates is this file's number (read from its source: one place for the number)
    if (argc > 1) {
        std::ifstream f(argv[1]);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string src = ss.str();
        const size_t at = src.find("DescriptorPoolSize =");
        CHECK(at != std::string::npos);
        const std::string value = at == std::string::npos ? "" : src.substr(at + 20, src.find(';', at) - at - 20);
        CHECK(value == " img::kDescriptorPoolSize" || value == " kui::img::kDescriptorPoolSize");
        CHECK(src.find("CheckVkResultFn") != std::string::npos);  // a failed allocation is logged, not ignored
    }
    CHECK(kDescriptorPoolSize >= 1024);
    CHECK(kMaxImages + kReserved <= kDescriptorPoolSize);
    CHECK(kUploadsPerFrame >= 1 && kUploadsPerFrame <= 16);

    // admission: live and dying images together stay under kMaxImages
    CHECK(can_add(0, 0));
    CHECK(can_add(kMaxImages - 2, 1));
    CHECK(!can_add(kMaxImages - 1, 1));
    CHECK(!can_add(kMaxImages, 0));

    // states: drawn only with a descriptor set
    CHECK(state_of(false, false, false, false) == kWaiting);
    CHECK(state_of(false, true, false, false) == kWaiting);   // created, the backend has not uploaded it yet
    CHECK(state_of(false, true, true, true) == kReady);
    CHECK(state_of(false, true, true, false) == kFailed);     // uploaded, no set: the pool was full
    CHECK(state_of(true, true, true, true) == kFailed);

    // 700 distinct images asked for in one frame: at most kMaxImages exist, at most kUploadsPerFrame are created a frame,
    // and the sets in use never exceed the pool (ImGui's own reserve included)
    std::vector<Fake> images;
    size_t refused = 0;
    for (int i = 0; i < 700; ++i) {
        if (!can_add(images.size(), 0)) {
            ++refused;
            continue;
        }
        images.push_back({});
    }
    CHECK(images.size() <= kMaxImages);
    size_t frames = 0, sets = kReserved, most = 0;
    for (; frames < 1000; ++frames) {
        const size_t n = upload_some(images.begin(), images.end(), [](const Fake& f) { return !f.created; }, [&](Fake& f) {
            f.created = f.uploaded = true;
            f.has_set = sets < kDescriptorPoolSize;
            if (f.has_set) ++sets;
        });
        most = n > most ? n : most;
        if (n == 0) break;
    }
    CHECK(most <= kUploadsPerFrame);
    CHECK(sets <= kDescriptorPoolSize);
    size_t ready = 0, failed = 0;
    for (const Fake& f : images) (state_of(f.failed, f.created, f.uploaded, f.has_set) == kReady ? ready : failed)++;
    CHECK(failed == 0);  // the pool is larger than every image Kiln lets exist plus the reserve
    CHECK(ready == images.size());

    if (g_fail) {
        std::fprintf(stderr, "image_budget_test: %d failed\n", g_fail);
        return 1;
    }
    std::printf("image_budget: pool %u, images %u, %zu of 700 refused, %zu frames of at most %u uploads: ok\n", kDescriptorPoolSize,
                kMaxImages, refused, frames, kUploadsPerFrame);
    return 0;
}
