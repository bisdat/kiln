// SPDX-License-Identifier: MIT
#include <atomic>
#include <cstdio>
#include <cstring>
#include "natives_core.h"
#include "services/kiln_world.h"
using namespace kab;
struct EcsView { KilnForeignLayout l{}; int32_t system_slot = -1; };
std::atomic<const EcsView*> g_ecs{nullptr}, g_ecs_checked{nullptr};
std::atomic<uint32_t> g_ecs_state{KILN_NATIVE_ECS_ON}, g_ecs_reason{0};
int t_slot = -1;
NativeCtx* t_ctx = nullptr;
const KilnWorldApi* g_world_api = nullptr;
void logw(const char*, ...) {}
void ecs_off(uint32_t reason) { g_ecs_reason = reason; g_ecs_state = KILN_NATIVE_ECS_OFF; }
bool kab::read_bytes(const void* p, void* o, size_t n) { std::memcpy(o, p, n); return true; }
bool kab::write_bytes(void* p, const void* v, size_t n) { std::memcpy(p, v, n); return true; }
uint8_t data[16]{};
KilnResult read_rc = KILN_OK;
const void* KILN_CALL rd(const KilnForeignLayout*, const uint64_t*, KilnStr, uint32_t, uint32_t* size, KilnResult* rc) { *size = 16; *rc = read_rc; return data; }
void* KILN_CALL wr(const KilnForeignLayout*, const uint64_t*, KilnStr, uint32_t, uint32_t* size, KilnResult* rc) { *size = 16; *rc = KILN_OK; return data; }
KilnResult KILN_CALL attr(const KilnForeignLayout*, const uint64_t*, void*) { return KILN_OK; }
uint32_t serial_answer = KILN_NATIVE_ECS_REASON_NONE;  // the pin and the calling thread (ecs_serial_now)
uint32_t ecs_serial_now() { return serial_answer; }
const char* ecs_reason_text(uint32_t) { return ""; }
#include "ecs_under_test.h"
int main() {
    int failed = 0;
    EcsView view; g_ecs = &view; g_ecs_checked = &view;
    KilnWorldApi api{}; api.foreign_read = &rd; api.foreign_write = &wr; api.foreign_attr_changed = &attr; g_world_api = &api;
    api.header.size = sizeof api;
    if (world_foreign()) { ++failed; puts("FAIL a full-sized foreign table with null functions must be unavailable"); }
    NativeCtx ctx{}; uint64_t iterator[1]{}; ctx.iter_ctx = reinterpret_cast<uintptr_t>(iterator);
    KilnNativeCall call{}; call.size = sizeof call; call.engine[2] = &ctx;
    const uint64_t* iter = nullptr;
    if (ecs_for(&call, &iter) != nullptr) { ++failed; puts("FAIL ECS access outside the active thunk"); }
    t_slot = 0; t_ctx = &ctx;
    data[6] = 1; data[4] = 8; data[7] = 2;
    uint8_t value[16]{};
    if (t_native_ecs_write(&call, {}, 7, value, 16, KILN_NATIVE_ECS_ATTRIBUTE, nullptr, nullptr) != KILN_ERR_INVALID_ARGUMENT || data[6] != 1) {
        ++failed; puts("FAIL attribute header must not be overwritten");
    }
    read_rc = KILN_ERR_INTERNAL;
    uint8_t out[16];
    t_native_ecs_read(&call, {}, 7, out, 16);
    if (g_ecs_state != KILN_NATIVE_ECS_OFF) { ++failed; puts("FAIL a foreign access fault must close this session"); }
    // the serial condition at an access: off for the world with its reason, and refused
    g_ecs_state = KILN_NATIVE_ECS_ON;
    g_ecs_reason = 0;
    read_rc = KILN_OK;
    serial_answer = KILN_NATIVE_ECS_REASON_NOT_SERIAL;
    if (ecs_for(&call, &iter) != nullptr || g_ecs_state != KILN_NATIVE_ECS_OFF || g_ecs_reason != KILN_NATIVE_ECS_REASON_NOT_SERIAL ||
        t_native_ecs_read(&call, {}, 7, out, 16) != KILN_ERR_UNAVAILABLE) {
        ++failed; puts("FAIL a call that is not serial on the world's thread must switch the bridge off and be refused");
    }
    serial_answer = KILN_NATIVE_ECS_REASON_NONE;
    if (ecs_for(&call, &iter) != nullptr) { ++failed; puts("FAIL once off for the world, it stays off"); }
    printf("ecs_test: %d failures\n", failed);
    return failed ? 1 : 0;
}
