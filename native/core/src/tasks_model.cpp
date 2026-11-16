// SPDX-License-Identifier: MIT
// kiln.tasks' model: see tasks_model.h. All state is guarded by one mutex and nothing here calls user
// code; records are shared_ptr so a piece of work keeps its record alive after release or owner cleanup.
#include "tasks_model.h"

#include <algorithm>

namespace kiln::tasks {

namespace {
// A handle: the owner instance in the top 24 bits, the owner's sequence in the low 40 (never 0, never reused).
constexpr int kSequenceBits = 40;
constexpr uint64_t kSequenceMask = (uint64_t{1} << kSequenceBits) - 1;
constexpr uint64_t kMaxInstance = (uint64_t{1} << (64 - kSequenceBits)) - 1;
KilnHandle make_handle(uint64_t instance, uint64_t sequence) { return (instance << kSequenceBits) | sequence; }
uint64_t instance_of(KilnHandle h) { return h >> kSequenceBits; }
uint64_t sequence_of(KilnHandle h) { return h & kSequenceMask; }

// Dequeue order inside an owner: High, then Normal, then Low (priority never changes an owner's share).
constexpr std::size_t kOrders = 3;
std::size_t order_of(Priority p) { return p == Priority::High ? 0 : p == Priority::Normal ? 1 : 2; }
std::size_t slot_index(Slot s) { return s == Slot::Normal ? 0 : 1; }
}  // namespace

struct Record {
    // immutable after publication
    KilnHandle handle = 0;
    KilnOwner owner = 0;
    Owner* home = nullptr;  // owner instances live as long as the model
    std::string name;
    Priority priority = Priority::Normal;
    Slot slot = Slot::Normal;
    uint64_t session = 0;  // AFTER_STEP: the server session captured at acceptance
    uint64_t edge = 0;
    Payload payload;
    // the cancelled() poll, read without the mutex
    std::atomic<bool> cancel{false};
    // guarded by the model mutex
    State state = State::Queued;
    bool waiting = false;          // in the owner's AFTER_STEP list
    bool segment_running = false;  // a body or continuation of this task is running
    bool armed = false;            // the running segment awaits one resumption
    bool resume_pending = false;   // resumed while that segment still ran: queued when it returns
    uint64_t registration = 0;     // the armed registration
    uint64_t registrations = 0;    // issued so far
    bool in_table = true;
    bool done_returned = false;
};

namespace {
struct Item {
    std::shared_ptr<Record> record;
    Work::Kind kind;
};
}  // namespace

struct Owner {
    KilnOwner id = 0;
    uint64_t instance = 0;
    std::string name;
    bool sealed = false, quarantined = false, retired = false;
    uint64_t issued = 0;  // the last sequence handed out
    std::unordered_map<uint64_t, std::shared_ptr<Record>> records;  // by sequence: not released yet
    std::deque<std::shared_ptr<Record>> finals;                     // done deliveries, before any other work
    std::deque<Item> ready[2][kOrders];                             // [slot][order]
    std::vector<std::shared_ptr<Record>> waiting;                   // AFTER_STEP, not yet runnable
    uint32_t running_normal = 0, running_long = 0;
    uint32_t outstanding = 0;  // accepted tasks whose done has not returned
};

class Model::Lock {
public:
    explicit Lock(const Model& m) : m_(m), lock_(m.mutex_) { m_.locked_.store(true); }
    ~Lock() { m_.locked_.store(false); }

private:
    const Model& m_;
    std::lock_guard<std::mutex> lock_;
};

// ---- names ----

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > kNameMax) return false;
    std::size_t i = 0;
    while (i < name.size()) {
        const auto b = static_cast<unsigned char>(name[i]);
        uint32_t cp = 0;
        std::size_t len = 0;
        if (b < 0x80) {
            cp = b;
            len = 1;
        } else if ((b & 0xE0) == 0xC0) {
            cp = b & 0x1F;
            len = 2;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0F;
            len = 3;
        } else if ((b & 0xF8) == 0xF0) {
            cp = b & 0x07;
            len = 4;
        } else {
            return false;
        }
        if (i + len > name.size()) return false;
        for (std::size_t k = 1; k < len; ++k) {
            const auto c = static_cast<unsigned char>(name[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3F);
        }
        static constexpr uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;  // overlong, range
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) return false;              // control characters
        i += len;
    }
    return true;
}

std::string bounded_owner_name(std::string_view name) {
    if (name.size() <= kNameMax) return std::string(name);
    std::size_t n = kNameMax;
    while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) --n;  // never cut a character
    return std::string(name.substr(0, n));
}

bool Work::cancel_requested() const { return record && record->cancel.load(std::memory_order_acquire); }

// ---- model ----

Model::Model(Limits limits) : limits_(limits) {}
Model::~Model() = default;

void Model::set_test_hook(std::function<void(Point)> hook) { hook_ = std::move(hook); }
void Model::hook(Point p) const {
    if (hook_) hook_(p);
}

void Model::note_ready(std::ptrdiff_t delta) {
    ready_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(ready_) + delta);
    ready_max_ = std::max(ready_max_, ready_);
}

std::shared_ptr<Owner> Model::find_open(KilnOwner owner) const {
    const auto it = current_.find(owner);
    return it == current_.end() ? nullptr : it->second;
}

std::shared_ptr<Record> Model::find_record(const Owner& o, KilnHandle task, bool* foreign, bool* released) const {
    *foreign = *released = false;
    const uint64_t seq = sequence_of(task);
    if (!task || instance_of(task) != o.instance || seq == 0 || seq > o.issued) {
        *foreign = true;
        return nullptr;
    }
    const auto it = o.records.find(seq);
    if (it == o.records.end()) {
        *released = true;  // issued to this instance and released since: idempotent, no tombstone kept
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<Record> Model::find_lease(KilnHandle task) const {
    const auto it = instances_.find(instance_of(task));
    if (it == instances_.end()) return nullptr;
    const auto r = it->second->records.find(sequence_of(task));
    return r == it->second->records.end() ? nullptr : r->second;
}

void Model::enqueue(Owner& o, const std::shared_ptr<Record>& r, Work::Kind kind) {
    o.ready[slot_index(r->slot)][order_of(r->priority)].push_back({r, kind});
    note_ready(+1);
}

void Model::drop_from_table(Owner& o, Record& r) {
    if (!r.in_table) return;
    r.in_table = false;  // before the erase: the table may hold the last reference to a quiescent record
    o.records.erase(sequence_of(r.handle));
}

void Model::publish_terminal(Owner& o, const std::shared_ptr<Record>& r, State state) {
    r->state = state;  // exactly once: only Queued/Running records reach here
    o.finals.push_back(r);
    note_ready(+1);
    if (o.sealed || o.retired) drop_from_table(o, *r);  // a sealed owner's terminal records release themselves
}

void Model::cancel_queued(Owner& o, const std::shared_ptr<Record>& r) {
    if (r->waiting) {
        o.waiting.erase(std::remove(o.waiting.begin(), o.waiting.end(), r), o.waiting.end());
        r->waiting = false;
    } else {
        auto& q = o.ready[slot_index(r->slot)][order_of(r->priority)];
        const auto it = std::find_if(q.begin(), q.end(),
                                     [&](const Item& i) { return i.record == r && i.kind == Work::Kind::Body; });
        if (it != q.end()) {
            q.erase(it);
            note_ready(-1);
        }
    }
    publish_terminal(o, r, State::Cancelled);  // its done is still delivered, on a worker, never inline
}

void Model::cancel_all_queued(Owner& o) {
    std::vector<std::shared_ptr<Record>> queued;
    for (const auto& [seq, r] : o.records)
        if (r->state == State::Queued) queued.push_back(r);
    std::sort(queued.begin(), queued.end(), [](const auto& a, const auto& b) { return a->handle < b->handle; });
    for (const auto& r : queued) cancel_queued(o, r);
}

void Model::signal_running(Owner& o) {
    for (const auto& [seq, r] : o.records)
        if (r->state == State::Running) r->cancel.store(true, std::memory_order_release);
}

void Model::drop_terminal(Owner& o) {
    std::vector<std::shared_ptr<Record>> terminals;  // kept alive until every one is out of the table
    for (const auto& [seq, r] : o.records)
        if (terminal(r->state)) terminals.push_back(r);
    for (const auto& r : terminals) drop_from_table(o, *r);
}

void Model::leave_ring_if_done(Owner& o) {
    if (!o.retired || o.outstanding) return;
    const auto it = std::find_if(ring_.begin(), ring_.end(), [&](const auto& p) { return p.get() == &o; });
    if (it == ring_.end()) return;
    const auto index = static_cast<std::size_t>(it - ring_.begin());
    ring_.erase(it);
    for (std::size_t& cursor : cursor_) {
        if (index < cursor) --cursor;
        if (cursor >= ring_.size()) cursor = 0;
    }
}

// ---- owners ----

KilnResult Model::ensure_owner(KilnOwner owner, std::string_view name) {
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    auto o = std::make_shared<Owner>();  // allocated before the lock; dropped when the owner already has one
    o->id = owner;
    o->name = bounded_owner_name(name);
    Lock lock(*this);
    if (closing_) return KILN_ERR_UNAVAILABLE;
    if (const auto existing = find_open(owner)) return existing->retired ? KILN_ERR_REVOKED : KILN_OK;
    if (next_instance_ > kMaxInstance) return KILN_ERR_UNAVAILABLE;
    instances_.reserve(instances_.size() + 1);  // every allocation before the first change
    ring_.reserve(ring_.size() + 1);
    current_.reserve(current_.size() + 1);
    o->instance = next_instance_++;
    current_[owner] = o;
    instances_[o->instance] = o;
    ring_.push_back(o);
    return KILN_OK;
}

KilnResult Model::open_owner(KilnOwner owner, std::string_view name) {
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    auto o = std::make_shared<Owner>();
    o->id = owner;
    o->name = bounded_owner_name(name);
    Lock lock(*this);
    if (const auto existing = find_open(owner); existing && !existing->retired) return KILN_ERR_INVALID_ARGUMENT;
    if (next_instance_ > kMaxInstance) return KILN_ERR_UNAVAILABLE;
    o->instance = next_instance_++;
    current_[owner] = o;
    instances_[o->instance] = o;
    ring_.push_back(o);
    return KILN_OK;
}

KilnResult Model::seal(KilnOwner owner, uint32_t* outstanding) {
    Lock lock(*this);
    const auto o = find_open(owner);
    if (!o) return KILN_ERR_REVOKED;
    o->sealed = true;
    cancel_all_queued(*o);
    signal_running(*o);
    drop_terminal(*o);
    if (outstanding) *outstanding = o->outstanding;
    return o->retired ? KILN_ERR_REVOKED : KILN_OK;
}

void Model::seal_all() {
    Lock lock(*this);
    closing_ = true;  // no new owner instance and no new root from now on (continuations stay admitted)
    for (const auto& [id, o] : current_) {
        if (o->retired) continue;
        o->sealed = true;
        cancel_all_queued(*o);
        signal_running(*o);
        drop_terminal(*o);
    }
}

KilnResult Model::quarantine(KilnOwner owner) {
    Lock lock(*this);
    const auto o = find_open(owner);
    if (!o || o->retired) return KILN_ERR_REVOKED;
    o->quarantined = true;
    cancel_all_queued(*o);
    return KILN_OK;
}

KilnResult Model::retire(KilnOwner owner, uint32_t* outstanding) {
    Lock lock(*this);
    const auto o = find_open(owner);
    if (!o) return KILN_ERR_REVOKED;
    o->sealed = o->retired = true;
    cancel_all_queued(*o);
    signal_running(*o);
    drop_terminal(*o);
    if (outstanding) *outstanding = o->outstanding;
    leave_ring_if_done(*o);
    return KILN_OK;
}

KilnResult Model::outstanding(KilnOwner owner, uint32_t* count) const {
    if (!count) return KILN_ERR_INVALID_ARGUMENT;
    Lock lock(*this);
    const auto o = find_open(owner);
    *count = o ? o->outstanding : 0;
    return o ? KILN_OK : KILN_ERR_REVOKED;
}

std::string Model::owner_name(KilnOwner owner) const {
    Lock lock(*this);
    const auto o = find_open(owner);
    return o ? o->name : std::string();
}

// ---- the public operations ----

KilnResult Model::submit(KilnOwner owner, const SubmitArgs& args, KilnHandle* task, Refusal* why) {
    const auto fail = [&](KilnResult r, Refusal w) {
        if (task) *task = 0;
        if (why) *why = w;
        return r;
    };
    if (!task || !valid_name(args.name) || static_cast<uint32_t>(args.priority) > 2 || (args.after_step && !args.session))
        return fail(KILN_ERR_INVALID_ARGUMENT, Refusal::Argument);
    // copied and allocated before the model mutex; published (or dropped) under it
    auto r = std::make_shared<Record>();
    r->owner = owner;
    r->name = std::string(args.name);
    r->priority = args.priority;
    r->slot = args.long_running ? Slot::LongRunning : Slot::Normal;
    r->session = args.session;
    r->edge = args.edge;
    r->payload = args.payload;
    hook(Point::SubmitBeforePublish);
    Lock lock(*this);
    if (closing_) return fail(KILN_ERR_UNAVAILABLE, Refusal::Shutdown);
    const auto o = find_open(owner);
    if (!o || o->retired) return fail(KILN_ERR_REVOKED, Refusal::OwnerSealed);
    if (o->quarantined) return fail(KILN_ERR_UNAVAILABLE, Refusal::OwnerQuarantined);
    if (o->sealed) return fail(KILN_ERR_UNAVAILABLE, Refusal::OwnerSealed);
    if (o->records.size() >= limits_.owner_capacity || o->issued >= limits_.max_sequence)
        return fail(KILN_ERR_UNAVAILABLE, Refusal::QueueFull);
    // All or nothing: the one allocation that can fail after this point is placing the record (the owner's table,
    // then a queue); on an exception everything is undone before it leaves, under the same lock, so no other thread
    // ever sees a half-published task.
    const uint64_t seq = o->issued + 1;
    r->handle = make_handle(o->instance, seq);
    r->home = o.get();
    o->records.emplace(seq, r);
    try {
        hook(Point::SubmitPublishing);
        // Compared under the same mutex as edge()/end_session(), so an edge or an end between the caller's capture
        // and this point is never lost: the task is queued as if it had been accepted after it.
        if (!args.after_step) {
            enqueue(*o, r, Work::Kind::Body);
        } else if (args.session <= ended_ || args.session < session_) {
            publish_terminal(*o, r, State::Cancelled);  // its session is over: cancelled at once, never carried over
        } else if (args.session == session_ && args.edge < last_edge_) {
            enqueue(*o, r, Work::Kind::Body);  // its session already had a newer edge
        } else {
            o->waiting.push_back(r);
            r->waiting = true;
        }
    } catch (...) {
        o->records.erase(seq);
        throw;
    }
    o->issued = seq;
    ++o->outstanding;
    *task = r->handle;
    return KILN_OK;
}

KilnResult Model::cancel(KilnOwner owner, KilnHandle task, State* state) {
    if (state) *state = State::None;
    Lock lock(*this);
    const auto o = find_open(owner);
    if (!o || o->retired) return KILN_ERR_REVOKED;
    bool foreign = false, released = false;
    const auto r = find_record(*o, task, &foreign, &released);
    if (!r) return KILN_ERR_NOT_FOUND;
    if (r->state == State::Queued) cancel_queued(*o, r);
    else if (r->state == State::Running) r->cancel.store(true, std::memory_order_release);
    if (state) *state = r->state;
    return KILN_OK;
}

KilnResult Model::status(KilnOwner owner, KilnHandle task, State* state) const {
    if (!state) return KILN_ERR_INVALID_ARGUMENT;
    *state = State::None;
    Lock lock(*this);
    const auto o = find_open(owner);
    if (!o || o->retired) return KILN_ERR_REVOKED;
    bool foreign = false, released = false;
    const auto r = find_record(*o, task, &foreign, &released);
    if (!r) return KILN_ERR_NOT_FOUND;
    *state = r->state;
    return KILN_OK;
}

KilnResult Model::release(KilnOwner owner, KilnHandle task) {
    Lock lock(*this);
    const auto o = find_open(owner);
    if (!o || o->retired) return KILN_ERR_REVOKED;
    bool foreign = false, released = false;
    const auto r = find_record(*o, task, &foreign, &released);
    if (released) return KILN_OK;
    if (!r) return KILN_ERR_NOT_FOUND;
    if (!terminal(r->state)) return KILN_ERR_UNAVAILABLE;
    drop_from_table(*o, *r);
    return KILN_OK;
}

// ---- leases ----

KilnResult Model::arm(KilnHandle task, uint64_t* registration) {
    if (!registration) return KILN_ERR_INVALID_ARGUMENT;
    Lock lock(*this);
    const auto r = find_lease(task);
    if (!r || r->state != State::Running || !r->segment_running || r->armed || r->resume_pending)
        return KILN_ERR_INTERNAL;
    r->armed = true;
    r->registration = ++r->registrations;
    *registration = r->registration;
    return KILN_OK;
}

KilnResult Model::resume(KilnHandle task, uint64_t registration) {
    Lock lock(*this);
    const auto r = find_lease(task);
    if (!r || !r->armed || r->registration != registration) return KILN_ERR_INTERNAL;
    r->armed = false;
    r->registration = 0;
    if (r->segment_running) r->resume_pending = true;  // segments of one task never overlap
    else enqueue(*r->home, r, Work::Kind::Continuation);
    return KILN_OK;
}

// ---- Post edge ----

void Model::cancel_waiting_before(uint64_t session) {
    for (const auto& o : ring_) {
        std::vector<std::shared_ptr<Record>> over;
        for (const auto& r : o->waiting)
            if (r->session < session) over.push_back(r);
        for (const auto& r : over) cancel_queued(*o, r);  // Cancelled, its done queued (never inline)
    }
}

void Model::end_session(uint64_t session) {
    Lock lock(*this);
    if (session <= ended_) return;
    ended_ = session;
    if (session > session_) {
        session_ = session;
        last_edge_ = 0;
    }
    cancel_waiting_before(session + 1);
}

void Model::edge(uint64_t session, uint64_t edge) {
    Lock lock(*this);
    if (session <= ended_ || session < session_) return;  // an ended or older session's edge releases nothing
    if (session > session_) {  // a newer session: every older one is over, even without its end
        cancel_waiting_before(session);
        session_ = session;
        last_edge_ = 0;
    }
    if (edge <= last_edge_) return;
    last_edge_ = edge;
    for (const auto& o : ring_) {
        std::vector<std::shared_ptr<Record>> still;
        for (const auto& r : o->waiting) {
            if (r->session == session && r->edge < edge) {
                r->waiting = false;
                enqueue(*o, r, Work::Kind::Body);
            } else {
                still.push_back(r);
            }
        }
        o->waiting.swap(still);
    }
}

// ---- executor ----

bool Model::dequeue(Slot slot, Work* out) {
    if (!out) return false;
    Lock lock(*this);
    const std::size_t n = ring_.size();
    std::size_t& cursor = cursor_[slot_index(slot)];  // each worker kind keeps its own turn (no cross-kind starvation)
    for (std::size_t step = 0; step < n; ++step) {
        const std::size_t index = (cursor + step) % n;
        Owner& o = *ring_[index];
        std::shared_ptr<Record> r;
        Work::Kind kind = Work::Kind::None;
        if (slot == Slot::Normal) {
            if (o.running_normal >= limits_.owner_running) continue;
            if (!o.finals.empty()) {
                r = o.finals.front();
                o.finals.pop_front();
                kind = Work::Kind::Done;
            }
        } else if (o.running_long >= limits_.long_running_per_owner || long_running_ >= limits_.long_running_global) {
            continue;
        }
        if (!r) {
            for (auto& q : o.ready[slot_index(slot)]) {
                if (q.empty()) continue;
                r = q.front().record;
                kind = q.front().kind;
                q.pop_front();
                break;
            }
        }
        if (!r) continue;
        note_ready(-1);
        if (kind == Work::Kind::Body) r->state = State::Running;
        if (kind != Work::Kind::Done) r->segment_running = true;
        if (slot == Slot::Normal) ++o.running_normal;
        else {
            ++o.running_long;
            ++long_running_;
        }
        ++running_;
        Work w;
        w.kind = kind;
        w.slot = slot;
        w.owner = r->owner;
        w.task = r->handle;
        w.final_state = kind == Work::Kind::Done ? r->state : State::None;
        w.payload = r->payload;
        w.record = r;
        *out = std::move(w);
        cursor = (index + 1) % n;
        return true;
    }
    return false;
}

KilnResult Model::segment_ended(const Work& work, bool suspended, Outcome outcome) {
    if (!work.record || (work.kind != Work::Kind::Body && work.kind != Work::Kind::Continuation))
        return KILN_ERR_INTERNAL;
    Lock lock(*this);
    const auto& r = work.record;
    Owner& o = *r->home;
    if (!r->segment_running) return KILN_ERR_INTERNAL;
    r->segment_running = false;
    if (work.slot == Slot::Normal) --o.running_normal;
    else {
        --o.running_long;
        --long_running_;
    }
    --running_;
    if (suspended) {
        if (r->resume_pending) {
            r->resume_pending = false;
            enqueue(o, r, Work::Kind::Continuation);
            return KILN_OK;
        }
        if (r->armed) return KILN_OK;  // waits for its resumption
        publish_terminal(o, r, State::Faulted);  // suspended without a resumption: never lose its done
        return KILN_ERR_INTERNAL;
    }
    const bool consistent = !r->armed && !r->resume_pending;
    r->armed = r->resume_pending = false;
    const State final = outcome == Outcome::Completed ? State::Completed
                        : outcome == Outcome::Faulted ? State::Faulted
                        : r->cancel.load(std::memory_order_acquire) ? State::Cancelled
                                                                    : State::Faulted;
    publish_terminal(o, r, final);
    return consistent ? KILN_OK : KILN_ERR_INTERNAL;
}

KilnResult Model::done_returned(const Work& work) {
    if (!work.record || work.kind != Work::Kind::Done) return KILN_ERR_INTERNAL;
    Lock lock(*this);
    const auto& r = work.record;
    Owner& o = *r->home;
    if (r->done_returned) return KILN_ERR_INTERNAL;
    r->done_returned = true;
    --o.running_normal;
    --running_;
    --o.outstanding;
    leave_ring_if_done(o);
    return KILN_OK;
}

State Model::diag_state(KilnHandle task) const {
    Lock lock(*this);
    const auto r = find_lease(task);
    return r ? r->state : State::None;
}

Counts Model::counts() const {
    Lock lock(*this);
    Counts c;
    c.owners = instances_.size();
    for (const auto& [instance, o] : instances_) {
        c.records += o->records.size();
        c.waiting += o->waiting.size();
        c.outstanding += o->outstanding;
    }
    c.ready = ready_;
    c.ready_max = ready_max_;
    c.running = running_;
    return c;
}

}  // namespace kiln::tasks
