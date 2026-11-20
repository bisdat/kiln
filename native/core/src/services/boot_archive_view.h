// SPDX-License-Identifier: MIT
// The base view: one reader of the game's data as this start has it, for code that reads that data before the game
// has it in memory or beside it: kiln.boot's OnBoot authors now (kiln_boot.h read_resource), later the checks and data
// stages that need the same base. Its interface:
//   tree(query)  one query of kiln.boot's grammar (boot_service.cpp query_ok) as a JSON tree: the result kiln.boot
//                answers with, {"resourceId","typeHash","part","value"}
//   bytes(key)   one resource's bytes as the game keeps it, by KilnAssetKey (GUID in game byte order, type hash, part)
//   digest()     the generation digest: 64 lower-case hex digits naming exactly the data the view serves; two views with
//                one digest give the same answers
//   release()    the reads of the generation are over
// Backings here:
//   SnapshotView  the boot game-data snapshot (boot_gamedata.h), made before the start. Trees only: the snapshot holds
//                 decoded values, not the game's bytes. Its answers are boot_gd::Reader's, byte for byte.
//   LiveView      kiln.assets (kiln_assets.h): the bytes the game's resource service serves now, a Kiln override or new
//                 resource first, else the game's own. Bytes only, and only once the game's resources are up
//                 (when_resources_ready): they load after the game's entry point, so before it a read is unavailable.
// Another backing derives from BaseView.
//
// Header-only and free of the core's other headers, like boot_gamedata.h: boot_service.cpp, its host tests and a
// service DLL compile it as it is. Threads: every member may be called from any thread. Each view guards its state
// with a lock of its own; the only lock held across a call into a provider is LiveView's bind lock, around
// when_resources_ready, whose callback takes no lock.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "boot_gamedata.h"
#include "kiln_api.h"
#include "services/kiln_assets.h"

namespace kiln::boot_view {

// Every digest starts with this line: a later digest scheme changes it.
constexpr std::string_view kDigestHead = "kiln base view 1\n";

class BaseView {
   public:
    virtual ~BaseView() = default;
    /// The backing's name in logs and rows: "snapshot", "live".
    virtual const char* source() const = 0;
    /// True: the view serves. False: *why says what is missing.
    virtual bool ready(std::string* why) = 0;
    /// The generation digest; empty while the view does not serve (ready() is false).
    virtual std::string digest() = 0;
    /// One query of kiln.boot's grammar, the contract of boot_gd::Reader::read:
    ///   KILN_OK                    *json holds {"resourceId","typeHash","part","value"}; *value_bytes the value's length
    ///   KILN_ERR_BUFFER_TOO_SMALL  capacity (which counts the NUL) is too small: *required is set
    ///   KILN_ERR_NOT_FOUND         no such record
    ///   KILN_ERR_INVALID_ARGUMENT  a query outside the grammar
    ///   KILN_ERR_UNAVAILABLE       the view cannot answer it, or the value is over result_max or budget_left; *why
    virtual KilnResult tree(std::string_view query, size_t capacity, size_t result_max, uint64_t budget_left, std::string* json,
                            size_t* required, size_t* value_bytes, std::string* why) = 0;
    /// One resource's bytes, at most max of them:
    ///   KILN_OK               *out holds them
    ///   KILN_ERR_NOT_FOUND    no such resource
    ///   KILN_ERR_UNAVAILABLE  the view holds no bytes, is not ready, the resource is over max or kept changing; *why
    virtual KilnResult bytes(const KilnAssetKey& key, size_t max, std::string* out, std::string* why) = 0;
    /// The generation's reads are over: what the view keeps for them is let go. A view bound to one generation refuses
    /// reads afterwards; the snapshot view only lets go of its parsed resource list (a later read loads it again).
    virtual void release() = 0;
};

/// Lower-case hex of n bytes.
inline std::string hex_bytes(const void* p, size_t n) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const auto b = static_cast<const uint8_t*>(p)[i];
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

// ---- the boot snapshot ----
class SnapshotView final : public BaseView {
   public:
    explicit SnapshotView(boot_gd::Reader& reader) : reader_(reader) {}

    const char* source() const override { return "snapshot"; }

    bool ready(std::string* why) override {
        if (reader_.is_open()) return true;
        *why = "no game-data snapshot is open";
        return false;
    }

    /// SHA-256 of kDigestHead, "snapshot\n<key>\n", then "<file> <SHA-256 hex>\n" for each file the snapshot serves,
    /// sorted by name: the producer's checksums (checked against the files first), or, for a snapshot written before
    /// them, each file's SHA-256 as it is now (computed once per opening of the snapshot).
    std::string digest() override {
        std::string why;
        if (!reader_.verify(&why)) return {};  // closed, or a file does not match its checksum (the reader is closed then)
        const uint64_t opens = reader_.opens();
        {
            std::lock_guard lk(mutex_);
            if (opens_ == opens && !digest_.empty()) return digest_;
        }
        auto sums = reader_.checksums();
        if (sums.empty()) {
            const auto dir = reader_.dir();
            for (const auto& name : reader_.files()) {
                const std::string sha = boot_gd::file_sha256(dir / name);
                if (sha.empty()) return {};  // a file that cannot be read: the snapshot does not serve
                sums.emplace_back(name, sha);
            }
        }
        std::sort(sums.begin(), sums.end());
        boot_gd::Sha256 h;
        h.update(kDigestHead).update("snapshot\n").update(reader_.key()).update("\n");
        for (const auto& [name, sha] : sums) h.update(name).update(" ").update(sha).update("\n");
        const std::string out = boot_gd::hex_of(h.final());
        std::lock_guard lk(mutex_);
        opens_ = opens;
        digest_ = out;
        return out;
    }

    KilnResult tree(std::string_view query, size_t capacity, size_t result_max, uint64_t budget_left, std::string* json,
                    size_t* required, size_t* value_bytes, std::string* why) override {
        return reader_.read(query, capacity, result_max, budget_left, json, required, value_bytes, why);
    }

    KilnResult bytes(const KilnAssetKey&, size_t, std::string*, std::string* why) override {
        *why = "the boot snapshot holds decoded values, not the game's bytes";
        return KILN_ERR_UNAVAILABLE;
    }

    void release() override { reader_.release_resource_list(); }

   private:
    boot_gd::Reader& reader_;
    std::mutex mutex_;
    uint64_t opens_ = 0;  // the reader's opening digest_ belongs to
    std::string digest_;
};

// ---- kiln.assets' live bytes ----
class LiveView final : public BaseView {
   public:
    using Getter = const KilnAssetsApi* (*)();
    static constexpr int kReadTries = 3;  // a resource that changes size between the size and the copy is read again

    /// get() answers kiln.assets' table (null while it is not running); it is asked until a call finds it.
    explicit LiveView(Getter get) : get_(get) {}

    const char* source() const override { return "live"; }

    /// True once kiln.assets is bound and the game's resources are up.
    bool ready(std::string* why) override {
        if (!bind(why)) return false;
        std::lock_guard lk(mutex_);
        if (released_) return *why = "the live base view was released", false;
        if (!up_.load(std::memory_order_acquire))
            return *why = "the game's resources are not up yet: they load after the game's entry point", false;
        return true;
    }

    /// SHA-256 of kDigestHead, "live\n", then "<key> <SHA-256 hex of the bytes>\n" for every resource the view served,
    /// sorted (the key as 48 hex digits: GUID bytes, then type and part little-endian). The live bytes have no
    /// generation of their own: the digest names what was read, so a value read again with other bytes adds a line.
    std::string digest() override {
        std::string why;
        if (!ready(&why)) return {};
        std::lock_guard lk(mutex_);
        boot_gd::Sha256 h;
        h.update(kDigestHead).update("live\n");
        for (const auto& [key, shas] : served_)
            for (const auto& sha : shas) h.update(key).update(" ").update(sha).update("\n");
        return boot_gd::hex_of(h.final());
    }

    KilnResult tree(std::string_view, size_t, size_t, uint64_t, std::string*, size_t*, size_t*, std::string* why) override {
        *why = "the live game data has no decoded values: kiln.boot reads them from a snapshot or a committed generation";
        return KILN_ERR_UNAVAILABLE;
    }

    KilnResult bytes(const KilnAssetKey& key, size_t max, std::string* out, std::string* why) override {
        if (!ready(why)) return KILN_ERR_UNAVAILABLE;
        const KilnAssetsApi* a = api_.load(std::memory_order_acquire);
        for (int attempt = 0; attempt < kReadTries; ++attempt) {
            char probe = 0;
            size_t len = 0;
            KilnResult rc = a->resource_read(&key, &probe, 0, &len);
            if (rc == KILN_ERR_NOT_FOUND) return *why = "not in the game's resources", KILN_ERR_NOT_FOUND;
            if (rc == KILN_OK) {  // an empty resource
                out->clear();
                return keep(key, *out, why);
            }
            if (rc != KILN_ERR_BUFFER_TOO_SMALL) return *why = "kiln.assets could not read it", KILN_ERR_UNAVAILABLE;
            if (len > max) return *why = "the resource is larger than the read may be", KILN_ERR_UNAVAILABLE;
            std::string data(len, '\0');
            size_t got = 0;
            rc = a->resource_read(&key, data.data(), data.size(), &got);
            if (rc == KILN_OK && got <= len) {  // one copy: it may have shrunk between the two calls, never torn
                data.resize(got);
                *out = std::move(data);
                return keep(key, *out, why);
            }
            if (rc == KILN_ERR_NOT_FOUND) return *why = "not in the game's resources", KILN_ERR_NOT_FOUND;
            if (rc != KILN_ERR_BUFFER_TOO_SMALL) return *why = "kiln.assets could not read it", KILN_ERR_UNAVAILABLE;
        }
        return *why = "the resource kept changing size while it was read", KILN_ERR_UNAVAILABLE;
    }

    void release() override {
        std::lock_guard lk(mutex_);
        released_ = true;
        served_.clear();
    }

    /// A new generation reads through the view again (kiln.boot's decide()). The binding and the readiness stay:
    /// kiln.assets keeps the callback it was given.
    void restart() {
        std::lock_guard lk(mutex_);
        released_ = false;
        served_.clear();
    }

   private:
    static void KILN_CALL on_up(void* user) { static_cast<LiveView*>(user)->up_.store(true, std::memory_order_release); }

    // kiln.assets' table, found once, and its when_resources_ready asked once (it calls on_up at once when the resources
    // are up already). A table without when_resources_ready, or one that refuses it, never serves.
    bool bind(std::string* why) {
        if (bound_.load(std::memory_order_acquire)) return bound_result(why);
        std::lock_guard lk(bind_mutex_);
        if (bound_.load(std::memory_order_acquire)) return bound_result(why);
        const KilnAssetsApi* a = get_ ? get_() : nullptr;
        if (!a) return *why = "kiln.assets is not running", false;  // asked again next time: it may load later
        constexpr size_t need = offsetof(KilnAssetsApi, when_resources_ready) + sizeof(a->when_resources_ready);
        std::string fail;
        if (a->header.size < need || !a->resource_read || !a->when_resources_ready)
            fail = "kiln.assets is too old for the live base view (it has no when_resources_ready)";
        else if (a->when_resources_ready(&on_up, this) != KILN_OK)
            fail = "kiln.assets serves no resources on this build";
        else
            api_.store(a, std::memory_order_release);
        {
            std::lock_guard l2(mutex_);
            why_ = fail;
        }
        bound_.store(true, std::memory_order_release);
        return bound_result(why);
    }
    bool bound_result(std::string* why) {
        std::lock_guard lk(mutex_);
        if (why_.empty()) return true;
        *why = why_;
        return false;
    }

    KilnResult keep(const KilnAssetKey& key, const std::string& data, std::string* why) {
        uint8_t raw[24];
        std::memcpy(raw, key.guid, 16);
        for (int i = 0; i < 4; ++i) raw[16 + i] = static_cast<uint8_t>(key.type >> (8 * i));
        for (int i = 0; i < 4; ++i) raw[20 + i] = static_cast<uint8_t>(key.part >> (8 * i));
        const std::string sha = boot_gd::hex_of(boot_gd::sha256(data));
        std::lock_guard lk(mutex_);
        if (released_) return *why = "the live base view was released", KILN_ERR_UNAVAILABLE;  // released while it read
        served_[hex_bytes(raw, sizeof raw)].insert(sha);
        return KILN_OK;
    }

    Getter get_;
    std::mutex bind_mutex_;
    std::atomic<bool> bound_{false}, up_{false};
    std::atomic<const KilnAssetsApi*> api_{nullptr};
    std::mutex mutex_;
    std::string why_;  // why the binding failed (empty: bound)
    bool released_ = false;
    std::map<std::string, std::set<std::string>> served_;  // key hex -> SHA-256 of each byte string served for it
};

}  // namespace kiln::boot_view
