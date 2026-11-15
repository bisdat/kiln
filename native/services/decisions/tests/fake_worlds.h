// SPDX-License-Identifier: MIT
// Host tests: the three worlds of a hosting game client in made-up memory, laid out as the game lays them out for
// world_gate.h (world.storage, storage.kind), and the program contexts of each. Entity ids are the tests' own: the
// same id may be held in the server world and visited in the client or prediction world.
#pragma once
#include <cstdint>
#include <cstring>

#include "../world_gate.h"

struct FakeWorlds {
    static constexpr int64_t kWorldStorage = 0x100, kStorageKind = 8;

    // A world object with its storage at +kWorldStorage; the prediction world's storage lives inside another object
    // (the client world's, as in docs/re/client.md), so `prediction - kWorldStorage` is no world anybody publishes.
    alignas(16) uint8_t server[0x200] = {}, client[0x200] = {}, holder[0x400] = {};
    // A job per world: qword 0 is the slice's storage. A ProgramContext: qword 0 is the job.
    alignas(16) uint8_t job_server[0x40] = {}, job_client[0x40] = {}, job_prediction[0x40] = {};
    alignas(16) uint8_t ctx_server[0x10] = {}, ctx_client[0x10] = {}, ctx_prediction[0x10] = {};
    kiln::decisions::WorldGate gate;

    FakeWorlds() {
        gate.set_layout(kWorldStorage, kStorageKind);
        world(server, 0, job_server, ctx_server);
        world(client, 1, job_client, ctx_client);
        // prediction: storage at holder + 0x2c0, kind 2
        uint8_t* storage = holder + 0x2c0;
        storage[kStorageKind] = 2;
        put(job_prediction, reinterpret_cast<uintptr_t>(storage));
        put(ctx_prediction, reinterpret_cast<uintptr_t>(job_prediction));
    }
    uintptr_t server_world() const { return reinterpret_cast<uintptr_t>(server); }

private:
    static void put(uint8_t* at, uintptr_t v) { std::memcpy(at, &v, sizeof v); }
    static void world(uint8_t* w, uint8_t kind, uint8_t* job, uint8_t* ctx) {
        uint8_t* storage = w + kWorldStorage;
        storage[kStorageKind] = kind;
        put(job, reinterpret_cast<uintptr_t>(storage));
        put(ctx, reinterpret_cast<uintptr_t>(job));
    }
};
