// SPDX-License-Identifier: MIT
// Exercise the real cache with only engine resolution replaced.
#include <cstdint>
#include <cstdio>
#include <cstring>
namespace kb {
struct VoxelWorld {
    bool ok = false;
    uintptr_t world = 0, voxels = 0;
    double origin[3]{}, cell = .5;
    unsigned tile[2]{256,256}, tiles[2]{80,80};
};
struct { bool read = true; } F;
uintptr_t active = 42, wrapper = 100;
int resolutions = 0;
uintptr_t server_world() { return active; }
bool resolve(uintptr_t w, VoxelWorld* v) {
    ++resolutions;
    *v = VoxelWorld{}; v->ok = true; v->world = w; v->voxels = wrapper;
    return true;
}
void detail_copy(char* p, size_t n) { if(n) *p = 0; }
void set_detail(const char*) {}
void vox_world_closed();
}
#define KILN_VOXEL_HOST_TEST
#define KB_D(...) ((void)0)
#define KB_I(...) ((void)0)
#include "../voxel.cpp"
int main() {
    if (kb::vox()->voxels != 100 || kb::resolutions != 1) return 1;
    if (kb::vox()->voxels != 100 || kb::resolutions != 1) return 2;
    kb::vox_world_closed();
    // The allocator reused the ECS world address, but the new voxel world is elsewhere.
    kb::wrapper = 200;
    if (kb::vox()->voxels != 200 || kb::resolutions != 2) return 3;
    kb::active = 0;
    if (kb::vox()) return 4;
    std::puts("voxel lifecycle: 4 passed, 0 failed");
}
