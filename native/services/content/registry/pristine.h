// SPDX-License-Identifier: MIT
// kiln.content: the live base view with Kiln's own writes taken back out (review4 NS1).
//
// A registry base decoded after resources-ready (the first live begin/read/select of a start whose stage had no registry
// user) reads the game's resident resource bytes through kiln.assets. By then kiln.content has written data packs,
// Content.* code and registry layers into some of those pages. This view copies the bytes as the inner view serves them,
// then puts back the original bytes of every store slot inside the resource's resident range (store_pristine: a leaf lock,
// safe under the stage runner's lock), so the decoded base, KILN_REGISTRY_VIEW_BASE and the cache digest are the game's.
// Writes by other loaders are not Kiln's to undo and stay in the bytes. The digest has LiveView's form over the restored
// bytes, so a cache written by an eager stage decode stays valid.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "../../../core/src/services/boot_archive_view.h"

namespace kc::registry {

class PristineView final : public kiln::boot_view::BaseView {
   public:
    // Where the resource's bytes live in the game (kiln.assets resource_data); false: unknown, the bytes are served as read.
    using Resident = std::function<bool(const KilnAssetKey& key, uintptr_t* at, size_t* len)>;
    // Put back the originals of the slots fully inside [at, at + len) into bytes (store_pristine).
    using Restore = std::function<void(uintptr_t at, size_t len, uint8_t* bytes)>;

    PristineView(kiln::boot_view::BaseView& inner, Resident resident, Restore restore)
        : inner_(inner), resident_(std::move(resident)), restore_(std::move(restore)) {}

    const char* source() const override { return inner_.source(); }
    bool ready(std::string* why) override { return inner_.ready(why); }

    std::string digest() override {
        std::string why;
        if (!inner_.ready(&why)) return {};
        std::lock_guard lk(mutex_);
        kiln::boot_gd::Sha256 h;
        h.update(kiln::boot_view::kDigestHead).update("live\n");
        for (const auto& [key, shas] : served_)
            for (const auto& sha : shas) h.update(key).update(" ").update(sha).update("\n");
        return kiln::boot_gd::hex_of(h.final());
    }

    KilnResult tree(std::string_view query, size_t capacity, size_t result_max, uint64_t budget_left, std::string* json,
                    size_t* required, size_t* value_bytes, std::string* why) override {
        return inner_.tree(query, capacity, result_max, budget_left, json, required, value_bytes, why);
    }

    KilnResult bytes(const KilnAssetKey& key, size_t max, std::string* out, std::string* why) override {
        std::string data;
        const KilnResult rc = inner_.bytes(key, max, &data, why);
        if (rc != KILN_OK) return rc;
        uintptr_t at = 0;
        size_t len = 0;
        if (!data.empty() && resident_ && restore_ && resident_(key, &at, &len) && at && len == data.size())
            restore_(at, len, reinterpret_cast<uint8_t*>(data.data()));
        uint8_t raw[24];
        std::memcpy(raw, key.guid, 16);
        for (int i = 0; i < 4; ++i) raw[16 + i] = static_cast<uint8_t>(key.type >> (8 * i));
        for (int i = 0; i < 4; ++i) raw[20 + i] = static_cast<uint8_t>(key.part >> (8 * i));
        const std::string sha = kiln::boot_gd::hex_of(kiln::boot_gd::sha256(data));
        {
            std::lock_guard lk(mutex_);
            if (released_) return *why = "the live base view was released", KILN_ERR_UNAVAILABLE;
            served_[kiln::boot_view::hex_bytes(raw, sizeof raw)].insert(sha);
        }
        *out = std::move(data);
        return KILN_OK;
    }

    void release() override {
        inner_.release();
        std::lock_guard lk(mutex_);
        released_ = true;
        served_.clear();
    }
    // A new read generation (the caller restarts the inner LiveView too).
    void restart() {
        std::lock_guard lk(mutex_);
        released_ = false;
        served_.clear();
    }

   private:
    kiln::boot_view::BaseView& inner_;
    Resident resident_;
    Restore restore_;
    std::mutex mutex_;
    bool released_ = false;
    std::map<std::string, std::set<std::string>> served_;  // key hex -> SHA-256 of each byte string served for it
};

}  // namespace kc::registry
