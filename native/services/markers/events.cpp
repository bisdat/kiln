// SPDX-License-Identifier: MIT
#include "events.h"

#include <algorithm>
#include <cstdio>
#include <thread>

namespace kmk {

namespace {
thread_local const void* t_current = nullptr;  // the subscription whose callback runs on this thread
thread_local bool t_delivering = false;

struct Thunk {
    const void* sub;
    KilnMarkerChangedFn changed;
    KilnMarkerVisibilityFn visibility;
    KilnMarkerCatalogFn catalog;
    const Note* note;
    void* user;
};

void run_thunk(void* p) {
    const Thunk* t = static_cast<const Thunk*>(p);
    switch (t->note->kind) {
        case Note::Change: t->changed(&t->note->change, t->user); break;
        case Note::Visibility: t->visibility(&t->note->visibility, t->user); break;
        case Note::Catalog: t->catalog(&t->note->catalog, t->user); break;
    }
}
}  // namespace

KilnResult Subscribers::subscribe(KilnOwner owner, KilnMarkerChangedFn changed, KilnMarkerVisibilityFn visibility,
                                  KilnMarkerCatalogFn catalog, void* user, KilnHandle* out) {
    if (!out) return KILN_ERR_INVALID_ARGUMENT;
    *out = 0;
    if (!owner || (!changed && !visibility && !catalog)) return KILN_ERR_INVALID_ARGUMENT;
    auto s = std::make_shared<Sub>();
    s->owner = owner;
    s->changed = changed;
    s->visibility = visibility;
    s->catalog = catalog;
    s->user = user;
    std::lock_guard g(mu_);
    s->id = ++next_;
    subs_.push_back(s);
    *out = s->id;
    return KILN_OK;
}

void Subscribers::retire(const std::shared_ptr<Sub>& s) {
    s->retired.store(true);  // seq_cst with call()'s admission: one of the two sees the other
    if (t_current == s.get()) return;  // from its own callback: it returns by itself
    while (s->active.load() != 0) std::this_thread::yield();
}

KilnResult Subscribers::unsubscribe(KilnHandle handle) {
    std::shared_ptr<Sub> found;
    {
        std::lock_guard g(mu_);
        auto it = std::find_if(subs_.begin(), subs_.end(), [&](const auto& s) { return s->id == handle; });
        if (it == subs_.end()) return KILN_ERR_NOT_FOUND;
        found = *it;
        subs_.erase(it);
    }
    retire(found);
    return KILN_OK;
}

void Subscribers::remove_owner(KilnOwner owner) {
    std::vector<std::shared_ptr<Sub>> gone;
    {
        std::lock_guard g(mu_);
        for (auto it = subs_.begin(); it != subs_.end();) {
            if ((*it)->owner == owner) {
                gone.push_back(*it);
                it = subs_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& s : gone) retire(s);
}

size_t Subscribers::count() const {
    std::lock_guard g(mu_);
    return subs_.size();
}

bool Subscribers::call(const std::shared_ptr<Sub>& s, const Note& n) {
    Thunk t{s.get(), s->changed, s->visibility, s->catalog, &n, s->user};
    if ((n.kind == Note::Change && !t.changed) || (n.kind == Note::Visibility && !t.visibility) || (n.kind == Note::Catalog && !t.catalog))
        return true;
    // admitted: from here unsubscribe on another thread waits for the call to return
    s->active.fetch_add(1);
    if (s->retired.load()) {
        s->active.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }
    const void* was = t_current;
    t_current = s.get();
    const bool ok = guarded_call(&run_thunk, &t);
    t_current = was;
    s->active.fetch_sub(1, std::memory_order_acq_rel);
    return ok;
}

void Subscribers::dispatch(const std::function<std::vector<Note>()>& take) {
    if (t_delivering) return;
    std::lock_guard serial(deliver_mu_);
    const std::vector<Note> batch = take();
    if (batch.empty()) return;
    t_delivering = true;
    deliver(batch);
    t_delivering = false;
}

void Subscribers::deliver(const std::vector<Note>& batch) {
    std::vector<std::shared_ptr<Sub>> snapshot;
    {
        std::lock_guard g(mu_);
        snapshot = subs_;
    }
    for (const Note& n : batch) {
        for (auto& s : snapshot) {
            if (s->retired.load(std::memory_order_acquire)) continue;
            if (call(s, n)) continue;
            // it faulted: this subscription goes, nobody else's state is touched
            {
                std::lock_guard g(mu_);
                subs_.erase(std::remove(subs_.begin(), subs_.end(), s), subs_.end());
            }
            s->retired.store(true, std::memory_order_release);
            if (log_) {
                char text[160];
                std::snprintf(text, sizeof text, "markers: a subscriber of owner %llu faulted in its callback and was unsubscribed",
                              static_cast<unsigned long long>(s->owner));
                log_(2, text);
            }
        }
    }
}

}  // namespace kmk
