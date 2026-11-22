// SPDX-License-Identifier: MIT
// kiln.markers' subscriptions and the delivery of queued notifications (chapter 04 §4.6). Pure: the glue decides when a
// batch is delivered (a world's Post, a world's stop) and the store decides what is in it.
//
// Rules: a callback never runs under a lock that an API call takes (the store's lock, the subscription list's lock); only
// one batch is delivered at a time (deliveries are serialized among themselves, never against API calls); a callback that
// faults is unsubscribed; unsubscribe from inside a callback returns at once, from another thread it waits for that
// subscription's running callback.
#ifndef KILN_MARKERS_EVENTS_H
#define KILN_MARKERS_EVENTS_H
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "services/kiln_markers.h"

namespace kmk {

// One queued notification: an immutable copy, delivered to every subscription that has a callback for its kind.
struct Note {
    enum Kind : uint8_t { Change, Visibility, Catalog } kind = Change;
    uint32_t role = 1;                 // 0 server, 1 client: whose dispatch delivers it
    bool counted = false;              // took a pending slot (released when the batch is taken)
    uint64_t publisher_generation = 0; // whose pending slot
    KilnMarkerChange change{};
    KilnMarkerVisibility visibility{};
    KilnMarkerCatalogChange catalog{};
};

// Runs fn(ctx); false when it faulted (a structured exception on Windows; the host build only calls it).
bool guarded_call(void (*fn)(void*), void* ctx);

class Subscribers {
public:
    using LogFn = void (*)(int level, const char* text);
    explicit Subscribers(LogFn log = nullptr) : log_(log) {}

    // KILN_ERR_INVALID_ARGUMENT: owner 0, no callback at all, or out NULL.
    KilnResult subscribe(KilnOwner owner, KilnMarkerChangedFn changed, KilnMarkerVisibilityFn visibility, KilnMarkerCatalogFn catalog,
                         void* user, KilnHandle* out);
    // KILN_ERR_NOT_FOUND: no such subscription (or it was removed already).
    KilnResult unsubscribe(KilnHandle handle);
    // Every subscription of `owner` (release_owner).
    void remove_owner(KilnOwner owner);

    // Takes a batch with `take` and delivers it in order. Deliveries are serialized among themselves (a batch taken
    // first is delivered first); on a thread that is delivering already it takes nothing and returns, so the notes stay
    // queued for the next dispatch (a change a callback makes arrives in the next batch).
    void dispatch(const std::function<std::vector<Note>()>& take);

    size_t count() const;

private:
    struct Sub {
        KilnHandle id = 0;
        KilnOwner owner = 0;
        KilnMarkerChangedFn changed = nullptr;
        KilnMarkerVisibilityFn visibility = nullptr;
        KilnMarkerCatalogFn catalog = nullptr;
        void* user = nullptr;
        std::atomic<bool> retired{false};
        std::atomic<int> active{0};
    };
    void deliver(const std::vector<Note>& batch);  // under deliver_mu_
    void retire(const std::shared_ptr<Sub>& s);  // waits unless this thread is inside s's callback
    bool call(const std::shared_ptr<Sub>& s, const Note& n);

    LogFn log_;
    mutable std::mutex mu_;  // the list
    std::vector<std::shared_ptr<Sub>> subs_;
    KilnHandle next_ = 0;
    std::mutex deliver_mu_;  // serializes deliveries only; never taken by an API call
};

}  // namespace kmk
#endif
