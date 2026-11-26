// SPDX-License-Identifier: MIT
// host tests of kiln.net's pairing of the game's session message calls (session_io.cpp) over a fake session that holds
// messages like the game: a read message stays held until SessionFree, an allocated one until SessionCommit or
// SessionAbort (test-net-host.sh). Numbered: 1 a drain frees every read message, also when the sink faults; 2 the
// per-drain bound and the empty socket; 3 a fault in the game's read or free; 4 send: commit, padding, a refused
// allocation holds nothing; 5 send: too big and a fault while copying abort the allocation; 6 a commit that faults is
// not aborted after it; 7 a drain-and-send soak leaves nothing held.
#include <cstdio>
#include <cstring>
#include <deque>
#include <set>
#include <vector>

#include "../session_io.h"

using namespace knet;

namespace {

int g_checks = 0, g_failures = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL line %d: %s: ", __LINE__, #cond);            \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\n");                                             \
        }                                                                  \
    } while (0)

// ---- the fake session: one socket's received messages, and what is held ----
struct Fake {
    std::deque<std::vector<uint8_t>> inbox;   // waiting on the socket
    std::set<uint64_t> held_reads;            // read and not freed
    std::set<uint64_t> held_allocs;           // allocated and neither committed nor aborted
    std::vector<std::vector<uint8_t>> sent;   // committed, as sized
    std::vector<std::vector<uint8_t>> reading;  // the bytes of each held read (the handle indexes it)
    std::vector<std::vector<uint8_t>> buffers;  // the buffer of each allocation
    uint64_t next = 1;
    uint8_t alloc_status = 0;                 // what SessionAlloc answers
    uint64_t capacity = 8192;
    bool read_faults = false, free_faults = false, alloc_faults = false, copy_faults = false, commit_faults = false;
    int aborts = 0, frees = 0, commits = 0;
};
Fake* g = nullptr;

bool f_read(uintptr_t, uintptr_t, ReadRes* out) {
    if (g->read_faults) return false;
    std::memset(out, 0, sizeof(*out));
    if (g->inbox.empty()) {
        out->status = 1;  // nothing waiting: no handle
        return true;
    }
    const uint64_t h = g->next++;
    g->reading.resize(h + 1);
    g->reading[h] = std::move(g->inbox.front());
    g->inbox.pop_front();
    g->held_reads.insert(h);
    out->handle = h;
    out->data = g->reading[h].data();
    out->size = g->reading[h].size();
    return true;
}
bool f_release(uintptr_t, uint64_t* handle) {
    if (g->free_faults) return false;
    ++g->frees;
    g->held_reads.erase(*handle);
    return true;
}
bool f_alloc(uintptr_t, uintptr_t, const AllocSpec*, AllocRes* out) {
    if (g->alloc_faults) return false;
    std::memset(out, 0, sizeof(*out));
    out->status = g->alloc_status;
    if (g->alloc_status != 0) return true;  // the game zeroes the handle on a failure
    const uint64_t h = g->next++;
    g->buffers.resize(h + 1);
    g->buffers[h].assign(g->capacity, 0xCC);
    g->held_allocs.insert(h);
    out->handle = h;
    out->data = g->buffers[h].data();
    out->capacity = g->capacity;
    return true;
}
bool f_commit(uintptr_t, uint64_t* handle) {
    ++g->commits;
    if (g->commit_faults) return false;  // the game may own it now: never aborted after this
    // SessionCommit(&res.handle) reads the final size at +0x18 from the handle (res+0x20)
    const auto* res = reinterpret_cast<const AllocRes*>(reinterpret_cast<const uint8_t*>(handle) - offsetof(AllocRes, handle));
    g->sent.emplace_back(g->buffers[*handle].begin(), g->buffers[*handle].begin() + res->size);
    g->held_allocs.erase(*handle);
    return true;
}
bool f_abort(uintptr_t, uint64_t* handle) {
    ++g->aborts;
    g->held_allocs.erase(*handle);
    return true;
}
bool f_copy(uint8_t* to, const uint8_t* from, size_t n, size_t pad) {
    if (g->copy_faults) return false;
    std::memcpy(to, from, n);
    std::memset(to + n, 0, pad);
    return true;
}
bool f_deliver(FrameSink sink, const uint8_t* data, uint64_t size, void* user) {
    // the sink signals a fault (a hardware fault in Kiln's own code, caught by game.cpp's guard) with a size of 13
    if (size == 13) return false;
    sink(data, size, user);
    return true;
}
const SessionCalls kCalls{&f_read, &f_release, &f_alloc, &f_commit, &f_abort, &f_copy, &f_deliver};

struct Got {
    std::vector<std::vector<uint8_t>> frames;
};
void sink(const uint8_t* data, uint64_t size, void* user) {
    static_cast<Got*>(user)->frames.emplace_back(data, data + size);
}

std::vector<uint8_t> bytes(size_t n, uint8_t v) { return std::vector<uint8_t>(n, v); }

void t1_drain_frees_every_read() {
    Fake f;
    g = &f;
    f.inbox = {bytes(8, 1), bytes(16, 2), bytes(4, 3)};
    Got got;
    CHECK(drain_messages(kCalls, 1, 1, &sink, &got) == 3, "three read");
    CHECK(got.frames.size() == 3 && got.frames[1].size() == 16, "delivered in order");
    CHECK(f.held_reads.empty(), "every read freed (%zu held)", f.held_reads.size());
    // the sink faults on the second message: it is freed all the same, the drain stops and says so
    f.inbox = {bytes(8, 1), bytes(13, 2), bytes(4, 3)};
    got = {};
    CHECK(drain_messages(kCalls, 1, 1, &sink, &got) == -1, "a sink fault is reported");
    CHECK(f.held_reads.empty(), "the message the sink faulted on is freed (%zu held)", f.held_reads.size());
    CHECK(got.frames.size() == 1, "nothing delivered after the fault");
    CHECK(f.inbox.size() == 1, "the rest stays on the socket for the next drain (game frees it with the socket)");
    // an empty message is freed without the sink
    f.inbox = {std::vector<uint8_t>{}};
    got = {};
    CHECK(drain_messages(kCalls, 1, 1, &sink, &got) == 1 && got.frames.empty() && f.held_reads.empty(), "empty message freed");
}

void t2_bounds() {
    Fake f;
    g = &f;
    for (int i = 0; i < 600; ++i) f.inbox.push_back(bytes(4, static_cast<uint8_t>(i)));
    Got got;
    CHECK(drain_messages(kCalls, 1, 1, &sink, &got) == 512, "at most 512 per drain, the game's own bound");
    CHECK(f.inbox.size() == 88 && f.held_reads.empty(), "the rest waits; nothing held");
    CHECK(drain_messages(kCalls, 1, 1, &sink, &got) == 88, "the next drain reads the rest");
    CHECK(drain_messages(kCalls, 1, 1, &sink, &got) == 0, "an empty socket reads nothing");
}

void t3_game_faults() {
    Fake f;
    g = &f;
    f.inbox = {bytes(8, 1)};
    f.read_faults = true;
    Got got;
    CHECK(drain_messages(kCalls, 1, 1, &sink, &got) == -1 && f.held_reads.empty(), "a faulting read holds nothing");
    f.read_faults = false;
    f.free_faults = true;
    CHECK(drain_messages(kCalls, 1, 1, &sink, &got) == -1, "a faulting free stops the drain");
    f.free_faults = false;
}

void t4_send() {
    Fake f;
    g = &f;
    const uint8_t m[6] = {1, 2, 3, 4, 5, 6};
    uint64_t cap = 0;
    CHECK(send_message(kCalls, 1, 1, m, 6, &cap) == 0 && cap == 8192, "sent");
    CHECK(f.sent.size() == 1 && f.sent[0].size() == 8 && f.sent[0][5] == 6 && f.sent[0][6] == 0 && f.sent[0][7] == 0,
          "padded with zeros to 8 bytes");
    CHECK(f.held_allocs.empty(), "committed");
    f.alloc_status = 2;  // the socket's bandwidth budget
    CHECK(send_message(kCalls, 1, 1, m, 6, &cap) == 2, "the game's status");
    CHECK(f.held_allocs.empty() && f.aborts == 0, "a refused allocation holds nothing and is not aborted");
    f.alloc_status = 0;
}

void t5_send_aborts() {
    Fake f;
    g = &f;
    std::vector<uint8_t> big(9000, 7);
    uint64_t cap = 0;
    CHECK(send_message(kCalls, 1, 1, big.data(), static_cast<uint32_t>(big.size()), &cap) == 0xFE && cap == 8192, "too big");
    CHECK(f.held_allocs.empty() && f.aborts == 1, "too big: aborted");
    f.copy_faults = true;
    const uint8_t m[4] = {1, 2, 3, 4};
    CHECK(send_message(kCalls, 1, 1, m, 4, &cap) == 0xFF, "a copy fault is reported");
    CHECK(f.held_allocs.empty(), "an allocation whose copy faulted is aborted (%zu held)", f.held_allocs.size());
    CHECK(f.commits == 0, "and never committed");
    f.copy_faults = false;
}

void t6_commit_fault() {
    Fake f;
    g = &f;
    f.commit_faults = true;
    const uint8_t m[4] = {1, 2, 3, 4};
    uint64_t cap = 0;
    CHECK(send_message(kCalls, 1, 1, m, 4, &cap) == 0xFF, "a commit fault is reported");
    CHECK(f.aborts == 0, "a faulted commit is not aborted after it");
}

void t7_soak() {
    Fake f;
    g = &f;
    Got got;
    for (int round = 0; round < 2000; ++round) {
        const int n = round % 7;
        for (int i = 0; i < n; ++i) f.inbox.push_back(bytes(static_cast<size_t>(4 + (round + i) % 23), 1));  // 13 = a sink fault
        drain_messages(kCalls, 1, 1, &sink, &got);
        f.copy_faults = round % 5 == 0;
        f.alloc_status = round % 11 == 0 ? 2 : 0;
        const uint8_t m[5] = {1, 2, 3, 4, 5};
        uint64_t cap = 0;
        send_message(kCalls, 1, 1, m, 5, &cap);
    }
    f.inbox.clear();
    CHECK(f.held_reads.empty(), "soak: no read message held (%zu)", f.held_reads.size());
    CHECK(f.held_allocs.empty(), "soak: no allocation held (%zu)", f.held_allocs.size());
}

}  // namespace

int main() {
    t1_drain_frees_every_read();
    t2_bounds();
    t3_game_faults();
    t4_send();
    t5_send_aborts();
    t6_commit_fault();
    t7_soak();
    std::printf("session io host: %s %d checks\n", g_failures ? "FAIL" : "PASS", g_checks);
    return g_failures ? 1 : 0;
}
