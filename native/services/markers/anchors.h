// SPDX-License-Identifier: MIT
// kiln.markers S3, following an entity (chapter 04 §4.2, §4.5): the watch table. Pure: no game call, no callback. The
// glue (anchors.cpp) feeds it the world lifecycle and the create/destroy/network-create observers, does the guarded reads
// (world_read.h) on the world's thread and asks it for leases, stored references and follow states.
//
// An incarnation is (world kind, the service's world epoch, entity) as first watched: it gets a serial of its own,
// monotonic and never reused, that does not depend on SpawnTime or on when the entity was created. A structural attempt
// on a watched id (a destroy, a create with that explicit id or returning it, a network create of it on the client) ends
// the incarnation for good: a later entity with the same id is a new incarnation and needs a new capture. A missing
// transform without a structural attempt is not an end: the same serial follows again once the transform is back.
//
// Leases are the callers' (owner-bound handles, nonzero, never reused); stored references are the records' (the local
// store, the link's sent records). A slot lives while it has either. Every lookup the observers make is under one short
// leaf lock and allocates nothing; nothing else is called under it.
#ifndef KILN_MARKERS_ANCHORS_H
#define KILN_MARKERS_ANCHORS_H
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "services/kiln_markers.h"
#include "world_read.h"

namespace kmk {

// Policy limits (chapter 04 §4.5): capture leases per publisher (owner), per world kind and in all; incarnations watched
// at once (leases plus the records' references, bounded by those limits and the follow limits of the stores).
constexpr uint32_t kLeasesPerOwner = 128;
constexpr uint32_t kLeasesPerKind = 256;
constexpr uint32_t kLeasesTotal = 512;
constexpr uint32_t kWatchSlots = 1024;

class Watches {
public:
    Watches();
    Watches(const Watches&) = delete;
    Watches& operator=(const Watches&) = delete;

    // ---- worlds (the lifecycle thread) ----
    // STARTED: storage (world + world.storage) and the service's epoch of that world. A start without a stop ends the
    // old world's watches first. STOPPING ends every watch of that kind and drops its leases before the storage can be
    // reused; the records' references keep their (ended) slots until the stores drop them.
    void world_started(uint32_t kind, uintptr_t storage, uint64_t epoch);
    void world_stopping(uint32_t kind);

    // ---- observers: any thread, the game's workers too ----
    // A structural attempt on id in the world whose storage this is: a destroy (before it runs), a create's explicit id
    // (before) and the id it returned (after). Ends every live watch of (that world, id). Lock-free when id cannot be
    // watched; never allocates.
    void structural(uintptr_t storage, uint32_t id);
    // The client's network create path, before the game creates or skips id: ends every client-world watch of id.
    void client_create(uint32_t id);
    // A guard failed: every watch of that world ends (the glue also turns that side's captures off).
    void end_all(uint32_t kind);

    // ---- capture (the world's thread, between its steps) ----
    struct Pending {
        uint32_t kind = 0, entity = 0, slot = 0;
        uint64_t epoch = 0, serial = 0;
        bool fresh = false;  // begin made the slot (no watch of that incarnation before)
    };
    // Before the read: the epoch is the world's now (else NOT_READY), the owner, the kind and all leases have room (else
    // FULL), and (epoch, entity) is watched from now on: the live slot of that incarnation, or a fresh one with a new
    // serial (FULL when the table is full). KILN_MARKER_ACCEPTED, or the refusal with its words in why.
    uint32_t begin(KilnOwner owner, uint32_t kind, uint64_t epoch, uint32_t entity, Pending* p, char* why, size_t cap);
    // After the read: a lease on that incarnation (KILN_MARKER_ACCEPTED, *handle), or KILN_MARKER_NOT_FOUND (no transform,
    // a bad position or a faulted read, or the entity was destroyed or recreated while it was read) or FULL, with no lease;
    // a fresh slot nobody uses goes again.
    uint32_t finish(KilnOwner owner, const Pending& p, const TransformRead& read, KilnMarkerAnchor* handle, char* why, size_t cap);
    // The caller's lease: KILN_OK, or KILN_ERR_NOT_FOUND for an unknown handle or another owner's (nothing changes).
    KilnResult release(KilnOwner owner, KilnMarkerAnchor handle);
    void release_owner(KilnOwner owner);  // every lease of the owner

    // ---- stored references (the local store, the link) ----
    // The owner's live lease of that world kind and its current world, whose incarnation still lives: one more reference
    // to it and its serial (KILN_MARKER_ACCEPTED), else KILN_MARKER_INVALID_ANCHOR with the words in why.
    uint32_t retain(KilnOwner owner, uint32_t kind, KilnMarkerAnchor handle, uint64_t* serial, char* why, size_t cap);
    void drop(uint64_t serial);  // one reference less (an unknown serial: nothing)

    // ---- follow reads (that world's thread, at its completed Post) ----
    struct ToRead {
        uint32_t slot = 0, entity = 0;
        uint64_t serial = 0;
    };
    // The live incarnations of that kind a record references, at most cap: one read each.
    uint32_t to_read(uint32_t kind, ToRead* out, uint32_t cap) const;
    // What the read gave: READY with the position, FOLLOW_MISSING (no transform: not an end), BAD_POSITION. An
    // incarnation that ended meanwhile stays ended.
    void read_done(const ToRead& t, const TransformRead& r);
    // The follow state as of the last read (or the capture): KILN_MARKER_READY with *position, FOLLOW_MISSING,
    // BAD_POSITION, or ANCHOR_ENDED (terminal). False for serial 0 or an unknown serial (gone: treat as ended).
    bool follow(uint64_t serial, KilnVec3* position, uint32_t* reason) const;
    // A lease of the owner: its incarnation (slot, entity, serial), kind and epoch. False for an unknown or foreign handle.
    bool lease(KilnOwner owner, KilnMarkerAnchor handle, ToRead* t, uint32_t* kind, uint64_t* epoch) const;

    struct Counts {
        uint32_t slots = 0, live = 0, leases = 0, references = 0;
        uint64_t serials = 0, handles = 0, ended = 0;
    };
    Counts counts() const;

private:
    struct Slot {
        uint64_t serial = 0;  // 0: free
        uint64_t epoch = 0;
        uint32_t entity = 0;
        uint32_t kind = 0;
        uint32_t leases = 0, refs = 0;
        bool live = false;    // false once ended: terminal
        uint32_t reason = KILN_MARKER_READY;
        KilnVec3 position{};
    };
    struct Lease {
        KilnMarkerAnchor handle = 0;  // 0: free
        KilnOwner owner = 0;
        uint32_t slot = 0, kind = 0;
    };

    static uint64_t key(uint64_t epoch, uint32_t entity) { return (epoch << 32) | entity; }
    uint32_t room(KilnOwner owner, uint32_t kind, char* why, size_t cap) const;  // FULL with words, or ACCEPTED
    void end_slot(Slot& s);                          // no allocation
    void end_matching(uint32_t kind, uint32_t id);   // no allocation
    void stop_locked(uint32_t kind);
    void maybe_free(uint32_t index);
    void drop_lease(uint32_t index);

    mutable std::mutex mu_;  // the leaf lock: nothing is called under it
    Slot slots_[kWatchSlots];
    uint32_t free_slots_[kWatchSlots];
    uint32_t free_slot_count_ = 0;
    Lease leases_[kLeasesTotal];
    uint32_t free_leases_[kLeasesTotal];
    uint32_t free_lease_count_ = 0;
    std::unordered_map<uint64_t, uint32_t> by_serial_, by_entity_, by_handle_;  // never touched by the observers
    std::unordered_map<KilnOwner, uint32_t> per_owner_;
    uint32_t per_kind_[KILN_WORLD_KINDS] = {};
    uintptr_t storage_[KILN_WORLD_KINDS] = {};
    uint64_t epoch_[KILN_WORLD_KINDS] = {};
    uint64_t next_serial_ = 0, next_handle_ = 0;
    uint32_t refs_ = 0;
    // The observers' lock-free test: live slots, and per low 16 bits of an entity id the live slots with such an id.
    std::atomic<uint32_t> live_{0};
    std::atomic<uint64_t> ended_{0};
    std::atomic<uint16_t> filter_[65536];
};

// The capture behind capture_anchor, after its argument, capability and thread checks: the epoch first (NOT_READY
// before any read when expected is not the world's current epoch), then the reservation, then one read through the
// read function with no lock held, then the lease. Fills out (its size kept) and returns its status.
using CaptureReadFn = TransformRead (*)(uint32_t kind, uint32_t entity, void* ctx);
uint32_t capture_into(Watches& w, KilnOwner owner, uint32_t kind, uint64_t expected, uint64_t current, uint32_t entity, CaptureReadFn read,
                      void* ctx, KilnMarkerAnchorResult* out);

}  // namespace kmk
#endif
