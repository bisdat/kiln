// SPDX-License-Identifier: MIT
// What the link's two roles (server.h, receiver.h) take from the process: a clock, randomness, the thread check, the
// anchors' follow reads, logging and this build's identity. link.cpp fills it from the core; host tests fill it with fakes.
#ifndef KILN_MARKERS_LINKENV_H
#define KILN_MARKERS_LINKENV_H
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "kiln_api.h"
#include "catalog.h"
#include "services/kiln_markers.h"

namespace kmk {

struct LinkEnv {
    std::function<uint64_t()> now_ms;                              // steady milliseconds
    std::function<void(uint8_t* out, size_t n)> random;            // session tokens (epochs, nonces)
    std::function<bool()> on_server_thread;                        // the server world's (tick) thread
    std::function<bool(uint64_t serial, KilnVec3* position, uint32_t* reason)> follow;  // glue.h anchors_follow
    // A follow lease of a server-side publisher to its incarnation serial (S3, the anchors). Empty: markers.anchors.server
    // is off, a followed send is KILN_ERR_UNAVAILABLE. Returns KILN_OK and the serial, or KILN_ERR_NOT_FOUND for a lease
    // that is not a live server-world lease of that owner.
    std::function<KilnResult(KilnOwner owner, KilnMarkerAnchor lease, uint64_t* serial)> resolve_follow;
    // Gives back one reference resolve_follow took (the server holds one per followed incarnation while a stored
    // definition follows it). Empty in host tests that do not count references.
    std::function<void(uint64_t serial)> release_follow;
    std::function<void(int level, const char* text)> log;
    std::string exe;                    // this process's exe sha256, lower-case hex
    uint32_t side = KILN_SIDE_SERVER;   // KILN_SIDE_CLIENT on a game client (a hosting one runs both roles)
    // The verified build pair to hold a client's HELLO and CATALOG against; null: catalog.h's table for exe and side
    // (host tests use a synthetic pair, no game data).
    const BuildPair* pair = nullptr;
};

}  // namespace kmk
#endif
