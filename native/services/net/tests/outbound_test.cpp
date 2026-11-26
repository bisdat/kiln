// SPDX-License-Identifier: MIT
// host tests of kiln.net's outbound side (outbound.cpp): the gate that closes at the game's shutdown line and from the
// server world's stop to the next start, and the retry queue's order and bounds (test-net-host.sh). Numbered: 1 the
// queue keeps order and stops at the first refusal; 2 age and gone peers drop; 3 the frame bound holds also behind a
// peer's earlier frames; 4 the shutdown line drops what waits and closes both roles for good; 5 the shutdown begins
// while a flush is sending: nothing more goes out; 6 the server world's stop closes the server role until the next
// start; 7 threads: queue, flush and the shutdown line at once.
#include <atomic>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

#include "../outbound.h"

using knet::Outbound;

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

struct Wire {
    std::vector<std::pair<uint32_t, uint8_t>> sent;  // peer, first byte
    std::map<uint32_t, int> dropped;
    int refuse_after = 1 << 30;                       // the game refuses from this send on
    std::map<uint32_t, bool> gone;
    int calls = 0;
    Outbound::SendFn send() {
        return [this](uint32_t peer, const uint8_t* d, size_t) {
            ++calls;
            if (gone[peer]) return Outbound::Sent::Never;
            if (static_cast<int>(sent.size()) >= refuse_after) return Outbound::Sent::Later;
            sent.push_back({peer, d[0]});
            return Outbound::Sent::Ok;
        };
    }
    Outbound::DropFn drop() {
        return [this](uint32_t peer) { ++dropped[peer]; };
    }
};

bool q(Outbound& o, uint32_t peer, uint8_t v, uint64_t now) {
    const uint8_t b[4] = {v, 0, 0, 0};
    return o.queue(peer, b, 4, now) == Outbound::Queued::Yes;
}

void t1_order() {
    Outbound o(100, 2000);
    for (uint8_t i = 0; i < 6; ++i) q(o, 0x81 + (i % 2), i, 0);
    Wire w;
    w.refuse_after = 3;
    auto f = o.flush(10, w.send(), w.drop());
    CHECK(f.sent == 3 && f.kept == 3 && o.pending() == 3, "three sent, the first refusal stops the rest (sent %zu kept %zu)", f.sent, f.kept);
    CHECK(w.calls == 4, "no send tried after the refusal (%d calls)", w.calls);
    w.refuse_after = 1 << 30;
    f = o.flush(20, w.send(), w.drop());
    CHECK(f.sent == 3 && o.pending() == 0, "the rest goes next time");
    bool ordered = w.sent.size() == 6;
    for (size_t i = 0; ordered && i < 6; ++i) ordered = w.sent[i].second == i;
    CHECK(ordered, "in the order queued");
}

void t2_age_and_gone() {
    Outbound o(100, 2000);
    q(o, 0x81, 1, 0);
    q(o, 0x82, 2, 1500);
    Wire w;
    w.refuse_after = 0;
    auto f = o.flush(2001, w.send(), w.drop());
    CHECK(f.dropped == 1 && w.dropped[0x81] == 1 && o.pending() == 1, "older than 2 s: dropped and counted for its peer");
    w.refuse_after = 1 << 30;
    w.gone[0x82] = true;
    f = o.flush(2100, w.send(), w.drop());
    CHECK(f.dropped == 1 && w.dropped[0x82] == 1 && o.pending() == 0, "a peer with no socket: dropped");
}

void t3_bound() {
    Outbound o(8, 2000);
    int yes = 0;
    for (int i = 0; i < 20; ++i) yes += q(o, 0x81, static_cast<uint8_t>(i), 0) ? 1 : 0;
    CHECK(yes == 8 && o.pending() == 8, "at most 8 frames wait, also behind the same peer's earlier ones (%d queued, %zu pending)", yes, o.pending());
}

void t4_shutdown() {
    Outbound o(100, 2000);
    for (uint8_t i = 0; i < 5; ++i) q(o, 0x81, i, 0);
    CHECK(o.server_open() && o.client_open(), "open before");
    size_t dropped = 0;
    CHECK(o.begin_shutdown(&dropped), "the first shutdown line");
    CHECK(dropped == 5 && o.pending() == 0, "what waited is dropped (%zu, %zu left)", dropped, o.pending());
    CHECK(!o.begin_shutdown(&dropped) && dropped == 0, "a second line changes nothing");
    CHECK(!o.server_open() && !o.client_open(), "both roles closed");
    CHECK(o.queue(0x81, reinterpret_cast<const uint8_t*>("abcd"), 4, 0) == Outbound::Queued::Closed && o.pending() == 0, "nothing queued after it");
    Wire w;
    o.flush(10, w.send(), w.drop());
    CHECK(w.calls == 0, "nothing sent after it");
    o.server_world_started();
    CHECK(!o.server_open(), "a world start does not reopen after the shutdown");
}

void t5_shutdown_during_flush() {
    Outbound o(100, 2000);
    for (uint8_t i = 0; i < 6; ++i) q(o, 0x81, i, 0);
    Wire w;
    int n = 0;
    auto send = [&](uint32_t peer, const uint8_t* d, size_t len) {
        if (++n == 2) o.begin_shutdown();  // the log thread sees the game's line while this flush sends
        return w.send()(peer, d, len);
    };
    auto f = o.flush(10, send, w.drop());
    CHECK(w.sent.size() == 2, "nothing goes out after the shutdown began (%zu sent)", w.sent.size());
    CHECK(o.pending() == 0 && f.dropped == 4 && w.dropped[0x81] == 4, "the rest is dropped, not kept (%zu pending, %zu dropped)", o.pending(), f.dropped);
}

void t6_world_stop() {
    Outbound o(100, 2000);
    q(o, 0x81, 1, 0);
    CHECK(o.server_world_stopped() == 1 && o.pending() == 0, "the world's stop drops what waits");
    CHECK(!o.server_open() && o.client_open(), "the server role is closed, the client role is not");
    CHECK(o.queue(0x81, reinterpret_cast<const uint8_t*>("abcd"), 4, 0) == Outbound::Queued::Closed, "no server frame queued");
    o.server_world_started();
    CHECK(o.server_open(), "the next world reopens it");
    CHECK(q(o, 0x81, 2, 0), "and frames queue again");
}

void t7_threads() {
    for (int round = 0; round < 200; ++round) {
        Outbound o(64, 2000);
        std::atomic<bool> go{false};
        std::atomic<int> after{0};
        std::atomic<bool> closed{false};
        std::thread producer([&] {
            while (!go.load()) {}
            for (int i = 0; i < 300; ++i) q(o, 0x81 + (i % 3), static_cast<uint8_t>(i), 0);
        });
        std::thread flusher([&] {
            while (!go.load()) {}
            for (int i = 0; i < 300; ++i)
                o.flush(10, [&](uint32_t, const uint8_t*, size_t) {
                    if (closed.load()) after.fetch_add(1);
                    return (i % 4) ? Outbound::Sent::Ok : Outbound::Sent::Later;
                }, [](uint32_t) {});
        });
        std::thread logger([&] {
            while (!go.load()) {}
            for (volatile int s = 0; s < round * 50; s = s + 1) {}
            o.begin_shutdown();
            closed.store(true);
        });
        go.store(true);
        producer.join();
        flusher.join();
        logger.join();
        CHECK(o.pending() == 0, "round %d: nothing waits after the shutdown (%zu)", round, o.pending());
        // a send already under way when the line came may finish; none may start once closed is published and the
        // flush has seen it: allow the one in flight
        CHECK(after.load() <= 1, "round %d: %d send(s) after the shutdown", round, after.load());
    }
}

}  // namespace

int main() {
    t1_order();
    t2_age_and_gone();
    t3_bound();
    t4_shutdown();
    t5_shutdown_during_flush();
    t6_world_stop();
    t7_threads();
    std::printf("outbound host: %s %d checks\n", g_failures ? "FAIL" : "PASS", g_checks);
    return g_failures ? 1 : 0;
}
