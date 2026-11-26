// SPDX-License-Identifier: MIT
// kiln.net's use of the game's session messages (docs/re/network.md "Message API"), with no game code in it: how a read
// is paired with its free and an allocation with its commit or abort, over a table of calls. game.cpp gives it the
// game's functions, each behind a structured-exception guard; host tests give it a fake session
// (tests/session_io_test.cpp).
//
// The rules (server f0d824be: SessionRead 0xa03220, SessionAlloc 0xa033b0):
//  - a read with status 0 holds one received message of the socket (the game took it off the socket's list) until
//    SessionFree; every such read is freed, also when the sink that gets its bytes faults;
//  - a failed read or allocation (status not 0) holds nothing: the game zeroes the handle;
//  - an allocation with status 0 holds one message until SessionCommit sends it or SessionAbort drops it; every such
//    allocation ends in one of the two, also when copying into it faults. A commit that faults is not aborted after it
//    (the game may already own the message).
#pragma once
#include <cstddef>
#include <cstdint>

namespace knet {

// Result of SessionRead (0x28 bytes): +0 status (0 = a message), +8 message handle (SessionFree takes &res.handle),
// +0x18 data, +0x20 size in bytes.
struct ReadRes {
    uint8_t status;
    uint8_t pad[7];
    uint64_t handle;
    uint64_t handle2;
    const uint8_t* data;
    uint64_t size;
};
static_assert(sizeof(ReadRes) == 0x28);

// Result of SessionAlloc (0x38 bytes): +0 status, +8 handle (Commit/Abort take &res.handle), +0x20 final size (u32,
// set before Commit), +0x28 buffer, +0x30 capacity in bytes.
struct AllocRes {
    uint8_t status;
    uint8_t pad[7];
    uint64_t handle;
    uint64_t h1;
    uint64_t h2;
    uint32_t size;
    uint32_t pad2;
    uint8_t* data;
    uint64_t capacity;
};
static_assert(sizeof(AllocRes) == 0x38);
static_assert(offsetof(AllocRes, size) == 0x20 && offsetof(AllocRes, data) == 0x28);

// SessionAlloc's request: explicit = 0 asks for the socket's maximum (8 KiB on channel 12).
struct AllocSpec {
    uint64_t min_size;
    uint64_t max_size;
    uint8_t explicit_size;
    uint8_t pad[7];
};
static_assert(sizeof(AllocSpec) == 0x18);

// Receives one message read from a game socket. Must not throw.
using FrameSink = void (*)(const uint8_t* data, uint64_t size, void* user);

// The game's message calls. Each returns false when the call faulted (game.cpp: a structured-exception guard).
struct SessionCalls {
    bool (*read)(uintptr_t session, uintptr_t socket, ReadRes* out);
    bool (*release)(uintptr_t session, uint64_t* handle);  // SessionFree(&res.handle)
    bool (*alloc)(uintptr_t session, uintptr_t socket, const AllocSpec* spec, AllocRes* out);
    bool (*commit)(uintptr_t session, uint64_t* handle);  // SessionCommit(&res.handle); reads res.size
    bool (*abort)(uintptr_t session, uint64_t* handle);   // SessionAbort(&res.handle)
    // Copies n bytes, then pad zero bytes, into the game's buffer; false on a fault.
    bool (*copy)(uint8_t* to, const uint8_t* from, size_t n, size_t pad);
    // Calls sink(data, size, user); false when it faulted.
    bool (*deliver)(FrameSink sink, const uint8_t* data, uint64_t size, void* user);
};

constexpr uint32_t kMaxMessagesPerDrain = 512;  // the game's own per-socket per-frame bound

// Reads every waiting message of the socket (at most max) into the sink and frees each one. Returns the messages read,
// -1 when a game call or the sink faulted (the message in hand is freed first, unless the free itself faults).
int drain_messages(const SessionCalls& c, uintptr_t session, uintptr_t socket, FrameSink sink, void* user,
                   uint32_t max = kMaxMessagesPerDrain);

// Sends one message. Returns the game's allocation status (0 = sent), 0xFE = too big for the socket (capacity in
// *capacity), 0xFF = a fault.
uint32_t send_message(const SessionCalls& c, uintptr_t session, uintptr_t socket, const uint8_t* data, uint32_t len,
                      uint64_t* capacity);

}  // namespace knet
