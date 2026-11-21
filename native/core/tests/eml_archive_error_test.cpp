// SPDX-License-Identifier: MIT
// ABI/consumer reference-provider probes ONLY. This is not the GPL provider or
// proof of Source IO/codec parity; kiln-eml's provider tests run the same scenarios on real bodies.
#include "services/kiln_eml_archive.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<int> checks{0}, failures{0};
void check(bool ok, const char* message) { ++checks; if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", message); } }
struct ReferenceProvider {
    struct Error { KilnOwner owner; std::string message; };
    std::mutex mutex;
    std::map<KilnHandle, std::shared_ptr<Error>> errors;
    std::map<KilnHandle, KilnOwner> issued;
    KilnHandle next = 1;
    bool view_live = true;
    std::atomic<int> lookups{0}, reads{0};
    std::atomic<bool> fail_io{true}, fault_io{false};
    std::string message = std::string("path\0type: ", 11) + std::string(70000, 'x');
    std::mutex barrier_mutex; std::condition_variable barrier;
    bool block_copy = false, copying = false, continue_copy = false;

    static bool valid(KilnArchiveErrorV1* e) {
        return !e || (e->size >= sizeof *e && !e->flags && !e->reserved);
    }
    static KilnResult reply(KilnArchiveErrorV1* e, KilnResult rc) {
        if (e) { const auto size = e->size; *e = {}; e->size = size; e->result = rc; }
        return rc;
    }
    KilnResult failure(KilnOwner owner, KilnArchiveErrorV1* e, const std::string& text) {
        reply(e, KILN_ERR_INTERNAL);
        if (e) {
            auto entry = std::make_shared<Error>(Error{owner, text});
            std::lock_guard lock(mutex);
            const auto h = next++;
            issued.emplace(h, owner); errors.emplace(h, std::move(entry));
            e->token = h; e->message_bytes = text.size();
        }
        return KILN_ERR_INTERNAL;
    }
    KilnResult read(KilnOwner owner, KilnEmlArchiveView view, uint64_t count,
                    void* out, size_t cap, size_t* required, KilnArchiveErrorV1* e) {
        if (!valid(e)) return KILN_ERR_INVALID_ARGUMENT; // before even a lookup
        reply(e, KILN_ERR_INVALID_ARGUMENT);
        if (!required || (!out && cap) || count > 4) return KILN_ERR_INVALID_ARGUMENT;
        ++lookups;
        { std::lock_guard lock(mutex); if (view != 42 || !view_live) return reply(e, KILN_ERR_REVOKED); }
        *required = static_cast<size_t>(count); // indexed length, not actual IO
        if (cap < count) return reply(e, KILN_ERR_BUFFER_TOO_SMALL);
        ++reads; // includes count0: the real Source path must open/seek/validate
        try {
            if (fault_io) throw std::runtime_error("contained reference IO fault");
            if (fail_io) return failure(owner, e, message);
            if (count) std::memset(out, 0x3a, static_cast<size_t>(count));
            return reply(e, KILN_OK);
        } catch (const std::exception& ex) { return failure(owner, e, ex.what()); }
    }
    KilnResult copy(KilnOwner owner, KilnHandle token, void* out, size_t cap, size_t* required) {
        if (!required || (!out && cap)) return KILN_ERR_INVALID_ARGUMENT;
        std::shared_ptr<Error> entry;
        {
            std::lock_guard lock(mutex);
            const auto known = issued.find(token);
            if (known == issued.end()) return KILN_ERR_INVALID_ARGUMENT;
            if (known->second != owner || !view_live) return KILN_ERR_REVOKED;
            const auto at = errors.find(token);
            if (at == errors.end()) return KILN_ERR_REVOKED;
            entry = at->second;
            *required = entry->message.size();
            if (cap < *required) return KILN_ERR_BUFFER_TOO_SMALL;
        }
        {
            std::unique_lock lock(barrier_mutex);
            if (block_copy) { copying = true; barrier.notify_all(); barrier.wait(lock, [&] { return continue_copy; }); }
        }
        if (!entry->message.empty()) std::memcpy(out, entry->message.data(), entry->message.size());
        return KILN_OK; // the call pin outlives release/view retirement
    }
    KilnResult close(KilnOwner owner, KilnHandle token) {
        std::lock_guard lock(mutex);
        const auto at = issued.find(token);
        if (at == issued.end()) return KILN_ERR_INVALID_ARGUMENT;
        if (at->second != owner) return KILN_ERR_REVOKED;
        errors.erase(token); return KILN_OK;
    }
    void retire_view() { std::lock_guard lock(mutex); view_live = false; errors.clear(); }
};
ReferenceProvider* provider = nullptr;
KilnResult KILN_CALL read_range(KilnOwner o, KilnEmlArchiveView v, const KilnContentHash*, uint64_t,
                               uint64_t n, void* p, size_t cap, size_t* required, KilnArchiveErrorV1* e) {
    return provider->read(o, v, n, p, cap, required, e);
}
KilnResult KILN_CALL error_copy(KilnOwner o, KilnHandle h, void* p, size_t cap, size_t* required) {
    return provider->copy(o, h, p, cap, required);
}
KilnResult KILN_CALL error_release(KilnOwner o, KilnHandle h) { return provider->close(o, h); }
KilnArchiveErrorV1 error_out() { KilnArchiveErrorV1 e{}; e.size = sizeof e; return e; }
KilnEmlArchiveApiV1 api() {
    KilnEmlArchiveApiV1 a{}; a.header = {sizeof a, KILN_EML_ARCHIVE_VERSION};
    a.content_read_range = &read_range; a.error_copy = &error_copy; a.error_release = &error_release; return a;
}
void failures_and_buffers() {
    ReferenceProvider p; provider = &p; auto a = api(); auto e = error_out(); size_t required = 99;
    auto short_e = e; short_e.size = sizeof e - 1;
    check(a.content_read_range(7, 42, nullptr, 0, 0, nullptr, 0, &required, &short_e) == KILN_ERR_INVALID_ARGUMENT &&
          !p.lookups && !p.reads, "short error record refuses before lookup/IO");
    e.flags = 1;
    check(a.content_read_range(7, 42, nullptr, 0, 0, nullptr, 0, &required, &e) == KILN_ERR_INVALID_ARGUMENT && !p.lookups,
          "unknown flags refuse before work");
    e = error_out(); e.reserved = 1;
    check(a.content_read_range(7, 42, nullptr, 0, 0, nullptr, 0, &required, &e) == KILN_ERR_INVALID_ARGUMENT && !p.lookups,
          "nonzero reserved refuses before work");
    e = error_out();
    check(a.content_read_range(7, 42, nullptr, 0, 4, nullptr, 0, &required, &e) == KILN_ERR_BUFFER_TOO_SMALL &&
          required == 4 && !e.token && !e.message_bytes && !p.reads, "index-only capacity probe performs no IO or invented error");
    check(a.content_read_range(7, 42, nullptr, 0, 0, nullptr, 0, &required, &e) == KILN_ERR_INTERNAL && e.token &&
          e.message_bytes == p.message.size() && p.reads == 1, "count0 real IO failure has retained message with no output capacity");
    const auto first = e.token;
    check(a.error_copy(7, first, nullptr, 0, &required) == KILN_ERR_BUFFER_TOO_SMALL && required == p.message.size(),
          "error message sizing is nonconsuming");
    std::vector<char> bytes(required, 'z');
    check(a.error_copy(7, first, bytes.data(), required - 1, &required) == KILN_ERR_BUFFER_TOO_SMALL && bytes.front() == 'z' && bytes.back() == 'z',
          "short error copy writes nothing");
    check(a.error_copy(7, first, bytes.data(), bytes.size(), &required) == KILN_OK &&
          std::memcmp(bytes.data(), p.message.data(), required) == 0 && p.reads == 1,
          "long counted/NUL message survives without repeat IO");
    check(e.result == KILN_ERR_INTERNAL && e.token == first, "copy does not overwrite original failure result");
    auto second = error_out();
    a.content_read_range(7, 42, nullptr, 0, 0, nullptr, 0, &required, &second);
    check(second.token && second.token != first && p.reads == 2, "each failing call has an independent fresh token");
    check(a.error_copy(8, first, nullptr, 0, &required) == KILN_ERR_REVOKED, "foreign owner cannot copy error");
    check(a.error_copy(7, 9999, nullptr, 0, &required) == KILN_ERR_INVALID_ARGUMENT, "never-issued error cannot be copied");
    check(a.error_release(8, first) == KILN_ERR_REVOKED, "foreign owner cannot close error");
    check(a.error_release(7, first) == KILN_OK && a.error_release(7, first) == KILN_OK, "issued error release is idempotent");
    check(a.error_copy(7, first, nullptr, 0, &required) == KILN_ERR_REVOKED, "closed error is not copied");
    check(a.error_release(7, 9999) == KILN_ERR_INVALID_ARGUMENT, "never-issued error token refused");
    // File conditions can change after the index-only probe; read actual IO once.
    p.fail_io = false; e = error_out();
    check(a.content_read_range(7, 42, nullptr, 0, 0, nullptr, 0, &required, &e) == KILN_OK && !e.token && !e.message_bytes &&
          required == 0 && e.result == KILN_OK && p.reads == 3, "count0 success distinct from failure and still executes IO");
    p.fault_io = true; e = error_out();
    check(a.content_read_range(7, 42, nullptr, 0, 0, nullptr, 0, &required, &e) == KILN_ERR_INTERNAL && e.token,
          "contained IO fault retains diagnostic");
    check(a.content_read_range(7, 42, nullptr, 0, 0, nullptr, 0, &required, nullptr) == KILN_ERR_INTERNAL,
          "NULL error caller explicitly opts out of detail");
}
void copy_close_overlap() {
    ReferenceProvider p; provider = &p; auto a = api(); auto e = error_out(); size_t required = 0;
    a.content_read_range(7, 42, nullptr, 0, 0, nullptr, 0, &required, &e);
    std::vector<char> bytes(e.message_bytes);
    p.block_copy = true;
    std::thread copier([&] { size_t n = 0; check(a.error_copy(7, e.token, bytes.data(), bytes.size(), &n) == KILN_OK,
                                               "in-flight copy pins message across close"); });
    { std::unique_lock lock(p.barrier_mutex); p.barrier.wait(lock, [&] { return p.copying; }); }
    check(a.error_release(7, e.token) == KILN_OK, "close overlaps admitted copy");
    p.retire_view();
    { std::lock_guard lock(p.barrier_mutex); p.continue_copy = true; p.barrier.notify_all(); }
    copier.join();
    check(bytes == std::vector<char>(p.message.begin(), p.message.end()), "copied error survives owner/view retirement");
    check(a.error_copy(7, e.token, nullptr, 0, &required) == KILN_ERR_REVOKED, "retired view refuses new copy");
    check(a.error_release(7, e.token) == KILN_OK, "cleanup stays idempotent after retirement");
}
}
int main() {
    static_assert(sizeof(KilnArchiveErrorV1) == 32);
    static_assert(sizeof(KilnEmlArchiveApiV1) == 152);
    static_assert(offsetof(KilnEmlArchiveApiV1, error_copy) == 136);
    static_assert(offsetof(KilnEmlArchiveApiV1, error_release) == 144);
    failures_and_buffers(); copy_close_overlap();
    std::printf("archive error ABI reference: %d checks, %d failures (NOT GPL provider parity)\n", checks.load(), failures.load());
    return failures ? 1 : 0;
}
