// SPDX-License-Identifier: MIT
// kiln.net: pairing the game's session message calls (session_io.h).
#include "session_io.h"

namespace knet {

int drain_messages(const SessionCalls& c, uintptr_t session, uintptr_t socket, FrameSink sink, void* user, uint32_t max) {
    int n = 0;
    for (uint32_t i = 0; i < max; ++i) {
        ReadRes r{};
        if (!c.read(session, socket, &r)) return -1;  // the game's read faulted: nothing is known to be held
        if (r.status != 0) break;                      // no message (or no socket): the game holds nothing for us
        const bool delivered = !(r.data && r.size > 0) || c.deliver(sink, r.data, r.size, user);
        if (!c.release(session, &r.handle)) return -1;  // freed whenever the game lets it be freed, delivered or not
        ++n;
        if (!delivered) return -1;  // the sink faulted: the rest waits on the socket for the next drain
    }
    return n;
}

uint32_t send_message(const SessionCalls& c, uintptr_t session, uintptr_t socket, const uint8_t* data, uint32_t len,
                      uint64_t* capacity) {
    AllocRes r{};
    AllocSpec spec{};
    if (!c.alloc(session, socket, &spec, &r)) {
        if (r.status == 0 && r.handle) c.abort(session, &r.handle);  // the fault came after the game handed one out
        return 0xFF;
    }
    if (capacity) *capacity = r.capacity;
    if (r.status != 0) return r.status;  // nothing allocated (0xa033d0: the game zeroes the handle)
    // The game's bit writers flush whole 32-bit words: keep the size a multiple of 4 (zero padding).
    const uint32_t padded = (len + 3u) & ~3u;
    if (!r.data || r.capacity < padded) {
        c.abort(session, &r.handle);
        return 0xFE;
    }
    if (!c.copy(r.data, data, len, padded - len)) {
        c.abort(session, &r.handle);  // never left allocated: the socket would hold it until the session ends
        return 0xFF;
    }
    r.size = padded;
    if (!c.commit(session, &r.handle)) return 0xFF;  // not aborted after a commit: the game may own it already
    return 0;
}

}  // namespace knet
