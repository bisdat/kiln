// SPDX-License-Identifier: MIT
#include "bridge.h"
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace kcam {

struct TransportBinding::State {
    mutable std::mutex mutex;
    std::condition_variable drained;
    KilnCameraTransport table{};
    KilnOwner owner = 0;
    uint64_t generation = 0;
    uint32_t calls = 0;
    uint32_t wait_ms = 2000;
    bool ready = false, closing = false;
};

namespace {
// Calls are acquired, used and destroyed on one thread. Counting reservations before the table escapes is what
// makes successful unbind the root-release barrier; no mutex is held while a bridge delegate runs.
thread_local std::vector<const void*> inside;
}

TransportBinding::TransportBinding() : state_(std::make_shared<State>()) {}

TransportBinding::Call::Call(std::shared_ptr<State> state) : state_(std::move(state)) { inside.push_back(state_.get()); }
TransportBinding::Call::Call(Call&& other) noexcept : state_(std::move(other.state_)) {}

TransportBinding::Call::~Call() {
    if (!state_) return;
    for (size_t i = inside.size(); i-- > 0;)
        if (inside[i] == state_.get()) {
            inside.erase(inside.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        --state_->calls;
    }
    state_->drained.notify_all();
}

const KilnCameraTransport* TransportBinding::Call::table() const { return state_ ? &state_->table : nullptr; }
KilnOwner TransportBinding::Call::owner() const { return state_ ? state_->owner : 0; }
uint64_t TransportBinding::Call::generation() const { return state_ ? state_->generation : 0; }

KilnResult TransportBinding::bind(KilnOwner owner, const KilnCameraTransport* table) {
    if (!owner || !table || table->header.size < sizeof(KilnCameraTransport) || table->header.version != 1 ||
        !table->submit || !table->caps || !table->retire) return KILN_ERR_INVALID_ARGUMENT;
    // Copy exactly the known prefix, never an advertised tail. The caller may discard its stack table on return.
    KilnCameraTransport copy = *table;
    copy.header.size = sizeof(KilnCameraTransport);
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->owner) return KILN_ERR_INVALID_ARGUMENT;
    if (state_->generation == std::numeric_limits<uint64_t>::max()) return KILN_ERR_UNAVAILABLE;
    ++state_->generation;
    state_->table = copy;
    state_->owner = owner;
    state_->ready = false;
    state_->closing = false;
    return KILN_OK;
}

KilnResult TransportBinding::unbind(KilnOwner owner) {
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (!state_->owner) return KILN_ERR_NOT_FOUND;
    if (state_->owner != owner) return KILN_ERR_REVOKED;
    const uint64_t generation = state_->generation;
    state_->ready = false;
    state_->closing = true; // atomically closes the table before waiting or returning a failed drain
    for (const void* current : inside)
        if (current == state_.get()) return KILN_ERR_WRONG_THREAD;
    if (!state_->drained.wait_for(lock, std::chrono::milliseconds(state_->wait_ms), [&] {
            return state_->calls == 0 || state_->owner != owner || state_->generation != generation;
        })) return KILN_ERR_INTERNAL; // retain the copied table and owner; the framework must retain its delegate roots
    // Another unbinder may have drained and cleared this generation, then a new bridge bound. Never clear that one.
    if (state_->owner != owner || state_->generation != generation) return KILN_ERR_REVOKED;
    state_->table = {};
    state_->owner = 0;
    lock.unlock();
    state_->drained.notify_all();
    return KILN_OK;
}

KilnResult TransportBinding::set_ready(KilnOwner owner, bool ready) {
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->owner) return KILN_ERR_NOT_FOUND;
    if (state_->owner != owner || state_->closing) return KILN_ERR_REVOKED;
    state_->ready = ready;
    return KILN_OK;
}

bool TransportBinding::ready() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->owner && state_->ready && !state_->closing;
}

TransportBinding::Call TransportBinding::acquire() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->owner || !state_->ready || state_->closing || state_->calls == std::numeric_limits<uint32_t>::max()) return {};
    // Reserve on this thread before returning its table. A failed allocation fails closed without handing out a table.
    Call call(state_);
    ++state_->calls;
    return call;
}

void TransportBinding::set_drain_wait_ms(uint32_t ms) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->wait_ms = ms;
}

uint32_t TransportBinding::in_flight() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->calls;
}

KilnOwner TransportBinding::owner() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->owner;
}

uint64_t TransportBinding::generation() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->owner ? state_->generation : 0;
}

// ---- the REMOTE_VISUAL backend --------------------------------------------------------------------------------------

namespace {
// The bridge's `done` is a plain C function: it finds the one backend through this (set while it lives).
std::mutex g_self_m;
RemoteBackend* g_self = nullptr;

constexpr uint64_t kLocalParts = KILN_CAMERA_CAP_LOCAL_POSE | KILN_CAMERA_CAP_LOCAL_FOV | KILN_CAMERA_CAP_LOCAL_PROFILE | KILN_CAMERA_CAP_LOCAL_SHAKE;
}  // namespace

RemoteBackend::RemoteBackend(Core* core) : core_(core) {
    std::lock_guard<std::mutex> lock(g_self_m);
    g_self = this;
}

RemoteBackend::~RemoteBackend() {
    std::lock_guard<std::mutex> lock(g_self_m);
    if (g_self == this) g_self = nullptr;
}

KilnResult RemoteBackend::bind(KilnOwner owner, const KilnCameraTransport* table) {
    const KilnResult rc = binding_.bind(owner, table);
    if (rc != KILN_OK) return rc;
    // Kiln.Camera binds once its channel and routes exist: a bound bridge is a ready one
    return binding_.set_ready(owner, true);
}

KilnResult RemoteBackend::unbind(KilnOwner owner) { return binding_.unbind(owner); }

KilnResult RemoteBackend::transport_ended(KilnOwner owner, KilnCameraLease lease, uint64_t revision, uint32_t end_reason, uint32_t reason_code,
                                          KilnStr reason) {
    if (!owner || !lease || !revision || end_reason == KILN_CAMERA_END_NONE || end_reason > KILN_CAMERA_END_CANCELLED_BEFORE_APPLY ||
        reason_code > KILN_CAMERA_REASON_BODY_TOO_LARGE || (reason.len && !reason.ptr))
        return KILN_ERR_INVALID_ARGUMENT;
    const KilnOwner bound = binding_.owner();
    if (!bound) return KILN_ERR_NOT_FOUND;
    if (bound != owner) return KILN_ERR_REVOKED;
    EndedNote note;
    note.lease = lease;
    note.revision = revision;
    note.end_reason = end_reason;
    note.reason_code = reason_code;
    note.text.assign(reason.ptr ? reason.ptr : "", reason.len < 512 ? reason.len : 512);
    std::lock_guard<std::mutex> lock(m_);
    KilnOwner author = 0;
    uint32_t epoch = 0;
    uint64_t seen_revision = 0;
    // A late note for a forgotten lease is harmless, but must never reach another endpoint's lease in the core.
    if (!known(lease, &author, &epoch, &seen_revision)) return KILN_OK;
    if (ended_.size() >= 4 * KILN_CAMERA_MAX_PENDING) return KILN_ERR_BUFFER_TOO_SMALL;
    ended_.push_back(std::move(note));
    return KILN_OK;
}

uint64_t RemoteBackend::caps() const { return binding_.ready() ? KILN_CAMERA_CAP_REMOTE : 0; }

uint64_t RemoteBackend::player_caps(KilnEntity player) const {
    const auto call = binding_.acquire();
    if (!call) return 0;
    uint64_t mask = 0;
    // what that player's client offered (the host's own player: the host's local rows); only local part bits
    if (call.table()->caps(player, &mask) != KILN_OK) mask = 0;
    return KILN_CAMERA_CAP_REMOTE | (mask & kLocalParts);
}

KilnResult RemoteBackend::admit(Stored&, uint32_t* reason, const char**) {
    // the recipient (connected, Kiln, its parts) is the bridge's to judge: it answers through `done`
    *reason = KILN_CAMERA_REASON_NONE;
    return KILN_OK;
}

bool RemoteBackend::stage(const Staged& st, uint32_t* reason, const char** text) {
    Out o;
    o.op = st.update ? (st.timing == KILN_CAMERA_RESTART_TIMELINE ? kUpdateRestart : kUpdatePreserve) : kApply;
    o.lease = st.lease;
    o.replaces = st.replaces;
    o.revision = st.revision;
    o.author = st.owner;
    o.epoch = st.epoch;
    o.alive = st.alive;
    o.request = std::make_unique<Stored>();
    o.request->req = st.request->req;
    o.request->keys = st.request->keys;
    o.request->req.keys = o.request->keys.empty() ? nullptr : o.request->keys.data();
    std::lock_guard<std::mutex> lock(m_);
    if (out_.size() + jobs_.size() >= 2 * KILN_CAMERA_MAX_PENDING) {
        *reason = KILN_CAMERA_REASON_RATE_LIMIT;
        *text = "too many camera requests wait for the transport";
        return false;
    }
    out_.push_back(std::move(o));
    return true;
}

void RemoteBackend::cancel_update(KilnCameraLease lease, uint64_t revision) {
    std::lock_guard<std::mutex> lock(m_);
    for (size_t i = out_.size(); i-- > 0;)
        if (out_[i].lease == lease && out_[i].revision == revision && out_[i].op != kEnd) out_.erase(out_.begin() + static_cast<std::ptrdiff_t>(i));
    for (auto& [id, job] : jobs_)
        if (job.lease == lease && job.revision == revision && job.op != kEnd) job.report = false;  // its answer is history
}

bool RemoteBackend::known(KilnCameraLease lease, KilnOwner* author, uint32_t* epoch, uint64_t* revision) const {
    bool seen = false;
    if (const auto a = active_.find(lease); a != active_.end()) {
        *author = a->second.author;
        *epoch = a->second.epoch;
        *revision = a->second.revision;
        seen = true;
    }
    for (const auto& [id, job] : jobs_)
        if (job.lease == lease && job.op != kEnd) {
            if (!seen || job.revision > *revision) *revision = job.revision;
            *author = job.author;
            *epoch = job.epoch;
            seen = true;
        }
    return seen;
}

bool RemoteBackend::release(KilnCameraLease lease, uint64_t revision) {
    Out o;
    o.op = kEnd;
    o.lease = lease;
    o.revision = revision;
    o.report = true;  // Core::released when the bridge answered (whatever it says: the client lets go by itself)
    std::lock_guard<std::mutex> lock(m_);
    uint64_t seen_revision = 0;
    known(lease, &o.author, &o.epoch, &seen_revision);
    out_.push_back(std::move(o));
    return false;
}

void RemoteBackend::drop(KilnCameraLease lease) {
    std::lock_guard<std::mutex> lock(m_);
    // what never left is forgotten; what did gets an End, answered to nobody
    for (size_t i = out_.size(); i-- > 0;)
        if (out_[i].lease == lease) out_.erase(out_.begin() + static_cast<std::ptrdiff_t>(i));
    Out o;
    o.op = kEnd;
    o.lease = lease;
    o.report = false;
    const bool sent = known(lease, &o.author, &o.epoch, &o.revision);
    active_.erase(lease);
    for (auto& [id, job] : jobs_)
        if (job.lease == lease) job.report = false;
    if (sent) out_.push_back(std::move(o));
}

void RemoteBackend::world_stopped(uint32_t epoch) {
    // atomics only (any thread): the next step forgets that world's work
    stopped_.store(epoch, std::memory_order_release);
}

void RemoteBackend::owner_gone(KilnOwner owner) {
    std::lock_guard<std::mutex> lock(m_);
    for (const KilnOwner o : gone_)
        if (o == owner) return;
    gone_.push_back(owner);
}

void KILN_CALL RemoteBackend::on_done(const KilnCameraResult* result, void* user) {
    if (!result || result->size < sizeof(KilnCameraResult)) return;  // a malformed answer: ignored
    std::lock_guard<std::mutex> lock(g_self_m);
    if (g_self) g_self->record(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(user)), *result);
}

void RemoteBackend::record(uint64_t job, const KilnCameraResult& r) {
    Answer a;
    a.job = job;
    a.ok = r.ok ? 1 : 0;
    a.reason = r.reason_code <= KILN_CAMERA_REASON_BODY_TOO_LARGE ? r.reason_code : static_cast<uint32_t>(KILN_CAMERA_REASON_MALFORMED_REMOTE);
    a.flags = r.flags & (KILN_CAMERA_MAY_HAVE_APPLIED | KILN_CAMERA_ALREADY_ENDED);
    a.frame = r.frame;
    if (r.reason.ptr && r.reason.len) a.text.assign(r.reason.ptr, r.reason.len < 512 ? r.reason.len : 512);
    std::lock_guard<std::mutex> lock(m_);
    if (!jobs_.count(job)) return;  // unknown, answered already, or of a world that stopped
    for (const Answer& seen : answers_)
        if (seen.job == job) return;  // exactly one answer counts
    answers_.push_back(std::move(a));
}

void RemoteBackend::forget_epoch(uint32_t epoch) {
    // lock held: everything of a stopped server world (its leases ended with it in the core)
    for (auto it = jobs_.begin(); it != jobs_.end();) it = it->second.epoch == epoch ? jobs_.erase(it) : std::next(it);
    for (size_t i = out_.size(); i-- > 0;)
        if (out_[i].epoch == epoch) out_.erase(out_.begin() + static_cast<std::ptrdiff_t>(i));
    for (auto it = active_.begin(); it != active_.end();) it = it->second.epoch == epoch ? active_.erase(it) : std::next(it);
    answers_.clear();
    ended_.clear();
}

void RemoteBackend::step() {
    // 1. a stopped world: its leases ended in the core; forget what the bridge still had of them
    if (const uint32_t stopped = stopped_.exchange(0, std::memory_order_acq_rel)) {
        std::lock_guard<std::mutex> lock(m_);
        forget_epoch(stopped);
    }
    struct Report {
        enum Kind { Committed, Rejected, Released, Ended } kind;
        KilnCameraLease lease;
        uint64_t revision, frame;
        uint32_t reason, flags, end_reason;
        std::string text;
        KilnCameraLease replaces = 0;
        KilnOwner author = 0;
        uint32_t epoch = 0;
    };
    std::vector<Report> reports;
    std::vector<Out> sends;
    std::vector<KilnOwner> gone;
    const uint64_t generation = binding_.ready() ? binding_.generation() : 0;
    {
        std::lock_guard<std::mutex> lock(m_);
        // 2. the bridge went (unbound or replaced): what it carried cannot be answered any more
        if (seen_generation_ && seen_generation_ != generation) {
            for (const auto& [id, job] : jobs_) {
                if (job.generation != seen_generation_ || !job.report) continue;
                if (job.op == kEnd) reports.push_back({Report::Released, job.lease, job.revision, 0, 0, 0, 0, {}});
                else reports.push_back({Report::Rejected, job.lease, job.revision, 0, KILN_CAMERA_REASON_PEER_UNAVAILABLE, KILN_CAMERA_MAY_HAVE_APPLIED, 0,
                                        "the camera transport was unbound before the client answered"});
            }
            for (auto it = jobs_.begin(); it != jobs_.end();) it = it->second.generation == seen_generation_ ? jobs_.erase(it) : std::next(it);
            for (const auto& [lease, a] : active_)
                reports.push_back({Report::Ended, lease, a.revision, 0, KILN_CAMERA_REASON_NONE, 0, KILN_CAMERA_END_CAPABILITY_LOST,
                                   "the camera transport was unbound"});
            active_.clear();
            answers_.clear();
            ended_.clear();
        }
        seen_generation_ = generation;
        // 3. the bridge's answers, in the order they came
        for (Answer& a : answers_) {
            const auto it = jobs_.find(a.job);
            if (it == jobs_.end()) continue;
            const Job job = it->second;
            jobs_.erase(it);
            if (!job.report) continue;
            if (job.op == kEnd) {
                reports.push_back({Report::Released, job.lease, job.revision, a.frame, 0, 0, 0, {}});
            } else if (a.ok) {
                reports.push_back({Report::Committed, job.lease, job.revision, a.frame, 0, 0, 0, {}, job.replaces, job.author, job.epoch});
            } else {
                reports.push_back({Report::Rejected, job.lease, job.revision, 0, a.reason ? a.reason : static_cast<uint32_t>(KILN_CAMERA_REASON_BUSY),
                                   a.flags & KILN_CAMERA_MAY_HAVE_APPLIED, 0, std::move(a.text)});
            }
        }
        answers_.clear();
        // 4. leases the client (or the bridge's bookkeeping) ended by itself
        for (EndedNote& n : ended_)
            reports.push_back({Report::Ended, n.lease, n.revision, 0, n.reason_code, 0, n.end_reason, std::move(n.text)});
        ended_.clear();
        sends.swap(out_);
        gone.swap(gone_);
    }

    // 5. report to the core (outside every lock of this backend; the core takes its own)
    for (Report& r : reports) {
        const char* text = r.text.empty() ? nullptr : r.text.c_str();
        switch (r.kind) {
        case Report::Committed:
            if (core_->committed(r.lease, r.revision, r.frame)) {
                std::lock_guard<std::mutex> lock(m_);
                // the new lease holds the player's slot now; one it superseded is no longer the bridge's to carry (the
                // client replaced it with this one)
                if (r.replaces) active_.erase(r.replaces);
                active_[r.lease] = Active{r.revision, r.author, r.epoch};
            }
            break;
        case Report::Rejected:
            core_->rejected(r.lease, r.revision, r.reason, r.flags, text);
            break;
        case Report::Released: {
            core_->released(r.lease, r.frame);
            std::lock_guard<std::mutex> lock(m_);
            active_.erase(r.lease);
            break;
        }
        case Report::Ended: {
            core_->ended(r.lease, r.revision, r.end_reason, r.reason, 0, text);
            std::lock_guard<std::mutex> lock(m_);
            const auto a = active_.find(r.lease);
            if (a != active_.end() && a->second.revision == r.revision) active_.erase(a);
            break;
        }
        }
    }

    // 6. what waits goes to the bridge, in order (outside every lock: the bridge's code runs)
    if (sends.empty() && gone.empty()) return;
    const auto call = binding_.acquire();
    std::vector<Report> refused;
    for (Out& o : sends) {
        if (o.op != kEnd && (!o.alive || !o.alive->load(std::memory_order_acquire))) continue;  // ended or retired meanwhile
        uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lock(m_);
            id = next_job_++;
            Job job;
            job.op = o.op;
            job.lease = o.lease;
            job.replaces = o.replaces;
            job.revision = o.revision;
            job.author = o.author;
            job.epoch = o.epoch;
            job.generation = call ? call.generation() : 0;
            job.report = o.report;
            if (call) jobs_[id] = job;
        }
        KilnResult rc = KILN_ERR_UNAVAILABLE;
        if (call) {
            const KilnCameraRequest* q = o.request ? &o.request->req : nullptr;
            rc = call.table()->submit(o.author, o.op, o.lease, o.revision, q, &RemoteBackend::on_done, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
        }
        if (rc == KILN_OK) continue;
        {
            std::lock_guard<std::mutex> lock(m_);
            jobs_.erase(id);
            for (size_t i = answers_.size(); i-- > 0;)
                if (answers_[i].job == id) answers_.erase(answers_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (!o.report) continue;
        if (o.op == kEnd) refused.push_back({Report::Released, o.lease, o.revision, 0, 0, 0, 0, {}});
        else refused.push_back({Report::Rejected, o.lease, o.revision, 0, KILN_CAMERA_REASON_PEER_UNAVAILABLE, 0, 0,
                                call ? "the camera transport refused the request" : "no camera transport is bound"});
    }
    if (call)
        for (const KilnOwner owner : gone) call.table()->retire(owner);
    for (Report& r : refused) {
        if (r.kind == Report::Released) {
            core_->released(r.lease, 0);
            std::lock_guard<std::mutex> lock(m_);
            active_.erase(r.lease);
        } else {
            core_->rejected(r.lease, r.revision, r.reason, r.flags, r.text.c_str());
        }
    }
}

uint32_t RemoteBackend::outstanding() const {
    std::lock_guard<std::mutex> lock(m_);
    return static_cast<uint32_t>(jobs_.size());
}

uint32_t RemoteBackend::queued() const {
    std::lock_guard<std::mutex> lock(m_);
    return static_cast<uint32_t>(out_.size());
}

uint32_t RemoteBackend::active() const {
    std::lock_guard<std::mutex> lock(m_);
    return static_cast<uint32_t>(active_.size());
}

}  // namespace kcam
