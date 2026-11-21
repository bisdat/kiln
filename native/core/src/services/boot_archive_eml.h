// SPDX-License-Identifier: MIT
// kiln.boot's base view in a kiln-backend start (kiln.launch KILN): OnBoot reads the archive generation kiln-eml's
// startup pass committed, cached or restored and verified for this launch, through kiln.eml.archive, and never the boot
// snapshot made before the start (a same-version ShroudForge or EML change makes it stale).
//
// Kiln-internal, with kiln.eml.archive (the startup-pass seam of Kiln's interpreter for ShroudForge mods).
// boot_archive_eml.cpp gives kiln.boot its sources in kiln-core.dll.
//
// The rules (no silent fallback):
//  - bind() runs once, at kiln.boot's bound(), when every on_ready ran (kiln-eml's startup barrier among them). The
//    archive table must carry acquire, identity, resource_json and release (KILN_EML_ARCHIVE_HAS). The identity source
//    (production: kiln.eml.startup's session-free published_identity, published_identity_of below) must hand a PUBLISHED
//    identity whose launch pins name this launch (launch_id: 8 zero bytes then kiln.launch's id big-endian; plan_sha256:
//    kiln.launch's plan hash), with a nonzero generation, reload epoch 1 and nonzero archive and registry pins. kiln.boot's owner acquires the view with exactly those pins, and the view's own identity must equal
//    them. Before any of it kiln.boot checks kiln-eml's kiln.caps rows for what it uses (eml.archive.metadata and
//    eml.archive.read, their acquire, identity, release and resource_json rows: boot_service.cpp); a held, off or absent
//    row refuses (refuse()). Anything else leaves the view unbound with the reason: OnBoot's game data is unavailable.
//  - tree() answers item:, template:, skill:, material: and resource: through resource_json; names:... lists are the
//    snapshot's and answer unavailable. Each reply's pin must be the view's. The lossless envelope is translated into
//    kiln.boot's result {"resourceId","typeHash","part","value"}, the value as the snapshot holds values
//    (docs/kiln/modding/boot-gamedata.md "The value") as far as the tree carries it (translate_value below). A
//    refused, faulted or revoked read is unavailable, never another generation's answer.
//  - release() (the commit) waits for the reads in flight and then releases the view: no read after it, and a bind()
//    after it or still under way binds nothing (what it acquired is given back).
//
// Header-only, host-tested (native/core/tests/boot_archive_eml_test.cpp, against a fake kiln.eml.archive table and a fake
// kiln.eml.startup table; native/core/tests/sfi_startup binds it to the real kiln-eml.dll).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include "boot_archive_view.h"
#include "services/kiln_eml_archive.h"
#include "services/kiln_eml_startup.h"

namespace kiln::boot_view {

/// What kiln.boot knows of this start on its own, from kiln.launch's frozen record.
struct LaunchFacts {
    uint64_t launch_id = 0;
    std::array<uint8_t, 32> plan_sha256{};
};

/// The identity of the generation kiln-eml granted for this launch: KILN_OK with *out filled (out->size is the v1 size
/// on the call), anything else with *why.
using IdentitySource = KilnResult (*)(const LaunchFacts& launch, KilnEmlArchiveIdentityV1* out, std::string* why);

/// What a kiln-backend start hands kiln.boot (boot_archive_eml.cpp; the host tests their own).
struct CommittedSource {
    bool (*kiln_mode)(LaunchFacts* out);    // true: this start runs the kiln backend; *out its launch facts
    const KilnEmlArchiveApiV1* (*table)();  // kiln.eml.archive (kiln-eml's early export), null without it
    IdentitySource identity;                // the committed generation's identity
};

/// The identity source of every kiln-backend reader of the committed generation (kiln.boot's boot_archive_eml.cpp; the
/// same call serves kiln.content's resource-patch check): kiln.eml.startup's session-free published_identity
/// (kiln_eml_startup.h). `getter` is the kiln_eml_startup_service export of the kiln-eml.dll this process already loaded
/// (null without one; the caller finds it, no load). The key is {size 56, flags 0, kiln.launch's launch id and plan hash,
/// session 0}; *out is the caller's record (its size at least the v1 size, kept). KILN_OK: *out holds the PUBLISHED
/// identity kiln-eml granted this launch, once its startup barrier finished with a grant. Anything else: *why, and *out
/// untouched: no table, a table without the member (a kiln-eml before it), no grant (refused, failed, not run), a key
/// kiln-eml does not take. The caller checks every pin itself (CommittedView::bind) and acquires the view with exactly
/// them; a refusal leaves the reader without game data, never with the boot snapshot.
inline KilnResult published_identity_of(KilnEmlStartupServiceFn getter, const LaunchFacts& launch, KilnEmlArchiveIdentityV1* out,
                                        std::string* why) {
    const KilnEmlStartupApiV1* api = getter ? getter(KILN_EML_STARTUP_VERSION) : nullptr;
    if (!api) return *why = "kiln-eml offers no kiln.eml.startup table (kiln-eml.dll is not loaded, or is too old)", KILN_ERR_UNAVAILABLE;
    if (!KILN_EML_STARTUP_HAS(api, published_identity))
        return *why = "kiln-eml is too old to hand this start's committed archive generation (its kiln.eml.startup table has no "
                      "published_identity)",
               KILN_ERR_UNAVAILABLE;
    if (!out || out->size < sizeof *out) return *why = "the committed generation's identity record is short", KILN_ERR_INVALID_ARGUMENT;
    KilnEmlStartupKeyV1 key{};
    key.size = sizeof key;
    key.flags = 0;
    key.launch_id = launch.launch_id;
    std::memcpy(key.plan_sha256, launch.plan_sha256.data(), sizeof key.plan_sha256);
    key.session = 0;
    const KilnResult rc = api->published_identity(&key, out);
    switch (rc) {
        case KILN_OK: return KILN_OK;
        case KILN_ERR_UNAVAILABLE:
            *why = "kiln-eml granted no archive generation for this launch (its startup pass refused, failed or did not run)";
            break;
        case KILN_ERR_INVALID_ARGUMENT:
            *why = "kiln-eml refused this start's launch key (its startup pass ran for another launch or plan)";
            break;
        default: *why = "kiln-eml could not hand this start's committed archive generation (result " + std::to_string(rc) + ")";
    }
    return rc;
}

// ---- the lossless tree to kiln.boot's value ----
// The rules follow the live game-data reader whose JSON the snapshot holds (tools/bootsnap/bootdata/live.py):
//   nil -> null; bool; u64/i64 -> the number; f32/f64 -> the number, NaN and infinities -> null;
//   guid/reference -> canonical text, "none" for the null GUID; string -> the text when it is 1..512 printable ASCII
//   bytes, else "" (BlobString); bytes -> "(N bytes)" above 16, else the numbers;
//   map -> an object in field order (field keys only: a non-string or repeated key refuses the value); a keen::Time
//   (one i64 field) -> seconds; a struct of one scalar field -> that scalar; variant -> its struct with "$type" (the
//   selected type's name after its last "::") first; optional -> null or its value; array -> the items;
//   depth: 12 levels below the root as the live reader, "..." below them (scalars are read at any depth). Nesting
//   past 256 levels (an optional does not count toward the 12) refuses the value: the walk's recursion is bounded.
// Not carried by the tree, so not as the snapshot: enum and flag values stay numbers (no names), impact
// configurations keep their stored structure (no alias vocabulary), and an array of bytes is one number per element.
namespace eml_tree {
constexpr int kRootDepth = 12;
constexpr size_t kNodesMax = 100000;
constexpr int kNestMax = 256;

inline int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
/// "<prefix><2n lower hex>" -> n bytes (want: the exact n, SIZE_MAX for any).
inline bool unhex(std::string_view text, std::string_view prefix, std::string* out, size_t want = SIZE_MAX) {
    if (text.substr(0, prefix.size()) != prefix) return false;
    text.remove_prefix(prefix.size());
    if (text.size() % 2 || (want != SIZE_MAX && text.size() != want * 2)) return false;
    out->clear();
    out->reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = hex_digit(text[i]), lo = hex_digit(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<char>(hi << 4 | lo));
    }
    return true;
}
inline bool u64_of(const nlohmann::json& v, std::string_view prefix, uint64_t* out) {
    std::string b;
    if (!v.is_string() || !unhex(v.get_ref<const std::string&>(), prefix, &b, 8)) return false;
    uint64_t x = 0;
    for (const char c : b) x = x << 8 | static_cast<uint8_t>(c);
    *out = x;
    return true;
}
/// keen::Guid memory order {u32, u16, u16, u8[8]} as canonical lower-case text.
inline std::string guid_text(const uint8_t* b) {
    char s[37];
    std::snprintf(s, sizeof s, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned>(b[0] | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24), static_cast<unsigned>(b[4] | b[5] << 8),
                  static_cast<unsigned>(b[6] | b[7] << 8), b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}
/// "keen::ecs::LifeTime" -> "LifeTime"; a template keeps its arguments (live.py brief_of).
inline std::string brief(std::string_view q) {
    const size_t lt = q.find('<');
    const std::string_view head = lt == std::string_view::npos ? q : q.substr(0, lt);
    const size_t c = head.rfind("::");
    return std::string(c == std::string_view::npos ? q : q.substr(c + 2));
}
inline std::string_view tag_of(const nlohmann::json& v) {
    const auto t = v.is_object() ? v.find("tag") : v.end();
    return t != v.end() && t->is_string() ? std::string_view(t->get_ref<const std::string&>()) : std::string_view();
}
inline std::string_view type_of(const nlohmann::json& v) {
    const auto t = v.is_object() ? v.find("type") : v.end();
    return t != v.end() && t->is_string() ? std::string_view(t->get_ref<const std::string&>()) : std::string_view();
}

struct Walk {
    size_t nodes = 0;
    int level = 0;  // node() calls open now
    std::string why;
    bool fail(std::string text) {
        if (why.empty()) why = std::move(text);
        return false;
    }
};

// A scalar node (bool, u64, i64, f32, f64, guid, reference, nil): true with *out; false when v is no scalar, or is a
// broken one (*broken then, and w says why).
inline bool scalar(const nlohmann::json& v, nlohmann::ordered_json* out, Walk& w, bool* broken) {
    const std::string_view tag = tag_of(v);
    const auto value = v.find("value");
    const bool has = value != v.end();
    *broken = false;
    uint64_t bits = 0;
    if (tag == "nil") return *out = nullptr, true;
    if (tag == "bool") {
        if (!has || !value->is_boolean()) return *broken = true, w.fail("a bool node without a boolean");
        return *out = value->get<bool>(), true;
    }
    if (tag == "u64" || tag == "i64") {
        if (!has || !u64_of(*value, tag == "u64" ? "u64:" : "i64:", &bits)) return *broken = true, w.fail("an integer node without its bits");
        if (tag == "u64")
            *out = bits;
        else
            *out = static_cast<int64_t>(bits);
        return true;
    }
    if (tag == "f32") {
        std::string b;
        if (!has || !value->is_string() || !unhex(value->get_ref<const std::string&>(), "f32:", &b, 4))
            return *broken = true, w.fail("an f32 node without its bits");
        const uint32_t u = static_cast<uint32_t>(static_cast<uint8_t>(b[0])) << 24 | static_cast<uint32_t>(static_cast<uint8_t>(b[1])) << 16 |
                           static_cast<uint32_t>(static_cast<uint8_t>(b[2])) << 8 | static_cast<uint8_t>(b[3]);
        float f;
        std::memcpy(&f, &u, 4);
        if (!std::isfinite(f)) return *out = nullptr, true;
        return *out = static_cast<double>(f), true;
    }
    if (tag == "f64") {
        if (!has || !u64_of(*value, "f64:", &bits)) return *broken = true, w.fail("an f64 node without its bits");
        double d;
        std::memcpy(&d, &bits, 8);
        if (!std::isfinite(d)) return *out = nullptr, true;
        return *out = d, true;
    }
    if (tag == "guid" || tag == "reference") {
        std::string b;
        if (!has || !value->is_string() || !unhex(value->get_ref<const std::string&>(), "guid:", &b, 16))
            return *broken = true, w.fail("a GUID node without its 16 bytes");
        const auto* p = reinterpret_cast<const uint8_t*>(b.data());
        bool zero = true;
        for (int i = 0; i < 16; ++i) zero = zero && p[i] == 0;
        return *out = zero ? std::string("none") : guid_text(p), true;
    }
    return false;
}

inline bool node(const nlohmann::json& v, int depth, Walk& w, nlohmann::ordered_json* out);

inline bool fields(const nlohmann::json& map, int depth, Walk& w, nlohmann::ordered_json* o) {
    const auto entries = map.find("entries");
    if (entries == map.end() || !entries->is_array()) return w.fail("a map node without entries");
    for (const auto& e : *entries) {
        const auto key = e.is_object() ? e.find("key") : e.end();
        const auto value = e.is_object() ? e.find("value") : e.end();
        std::string name;
        if (key == e.end() || value == e.end() || tag_of(*key) != "string" || !key->contains("value") || !(*key)["value"].is_string() ||
            !unhex((*key)["value"].get_ref<const std::string&>(), "string:", &name))
            return w.fail("a map entry whose key is not a field name");
        if (o->contains(name)) return w.fail("the field " + name.substr(0, 64) + " twice in one struct");
        nlohmann::ordered_json fv;
        if (!node(*value, depth - 1, w, &fv)) return false;
        (*o)[name] = std::move(fv);
    }
    return true;
}

inline bool node(const nlohmann::json& v, int depth, Walk& w, nlohmann::ordered_json* out) {
    if (++w.nodes > kNodesMax) return w.fail("the value has more than 100000 nodes");
    if (w.level >= kNestMax) return w.fail("the value nests deeper than 256 levels");
    struct Level {
        int& n;
        ~Level() { --n; }
    } const level{++w.level};
    if (!v.is_object()) return w.fail("a node that is not an object");
    const std::string_view tag = tag_of(v);
    bool broken = false;
    if (scalar(v, out, w, &broken)) return true;
    if (broken) return false;
    if (tag == "map") {
        // leaves read before the depth test, as the live reader's leaf_of: keen::Time, a struct of one scalar field
        const auto entries = v.find("entries");
        if (entries != v.end() && entries->is_array() && entries->size() == 1 && (*entries)[0].is_object() && (*entries)[0].contains("value")) {
            const auto& only = (*entries)[0]["value"];
            if (type_of(v) == "keen::Time" && tag_of(only) == "i64") {
                uint64_t bits = 0;
                if (!only.contains("value") || !u64_of(only["value"], "i64:", &bits)) return w.fail("a keen::Time without its bits");
                return *out = static_cast<double>(static_cast<int64_t>(bits)) / 1e9, true;
            }
            const std::string_view t = tag_of(only);
            if (t == "bool" || t == "u64" || t == "i64" || t == "f32" || t == "f64") return scalar(only, out, w, &broken) && !broken;
        }
    }
    if (depth <= 0) return *out = "...", true;
    if (tag == "string" || tag == "bytes") {
        std::string b;
        const auto value = v.find("value");
        if (value == v.end() || !value->is_string() || !unhex(value->get_ref<const std::string&>(), tag == "string" ? "string:" : "bytes:", &b))
            return w.fail("a string node without its bytes");
        if (tag == "bytes") {
            if (b.size() > 16) return *out = "(" + std::to_string(b.size()) + " bytes)", true;
            *out = nlohmann::ordered_json::array();
            for (const char c : b) out->push_back(static_cast<uint8_t>(c));
            return true;
        }
        bool printable = !b.empty() && b.size() <= 512;
        for (const char c : b) printable = printable && c >= 0x20 && c < 0x7F;
        return *out = printable ? b : std::string(), true;
    }
    if (tag == "optional") {
        const auto present = v.find("present");
        if (present == v.end() || !present->is_boolean()) return w.fail("an optional node without presence");
        if (!present->get<bool>()) return *out = nullptr, true;
        const auto value = v.find("value");
        if (value == v.end()) return w.fail("a present optional without its value");
        return node(*value, depth, w, out);
    }
    if (tag == "variant") {
        const auto value = v.find("value");
        if (value == v.end() || tag_of(*value) != "map") return w.fail("a variant whose value is not a struct");
        *out = nlohmann::ordered_json::object();
        (*out)["$type"] = brief(type_of(*value));
        return fields(*value, depth, w, out);
    }
    if (tag == "map") {
        *out = nlohmann::ordered_json::object();
        return fields(v, depth, w, out);
    }
    if (tag == "array") {
        const auto items = v.find("items");
        if (items == v.end() || !items->is_array()) return w.fail("an array node without items");
        *out = nlohmann::ordered_json::array();
        for (const auto& item : *items) {
            nlohmann::ordered_json iv;
            if (!node(item, depth - 1, w, &iv)) return false;
            out->push_back(std::move(iv));
        }
        return true;
    }
    return w.fail("a node of an unknown tag");
}
}  // namespace eml_tree

/// The tagged value of a resource_json reply as kiln.boot's value; false with *why when it cannot be translated.
inline bool translate_value(const nlohmann::json& tagged, nlohmann::ordered_json* out, std::string* why) {
    eml_tree::Walk w;
    if (eml_tree::node(tagged, eml_tree::kRootDepth, w, out)) return true;
    *why = "its value cannot be translated (" + w.why + ")";
    return false;
}

/// The pin object resource_json replies carry for a view of the identity id (the provider's text form).
inline nlohmann::json pin_of(const KilnEmlArchiveIdentityV1& id) {
    auto uuid = [](const uint8_t* b) {
        return hex_bytes(b, 4) + "-" + hex_bytes(b + 4, 2) + "-" + hex_bytes(b + 6, 2) + "-" + hex_bytes(b + 8, 2) + "-" + hex_bytes(b + 10, 6);
    };
    auto u64s = [](uint64_t v) {
        char s[24];
        std::snprintf(s, sizeof s, "u64:%016llx", static_cast<unsigned long long>(v));
        return std::string(s);
    };
    return {{"launch_id", uuid(id.launch_id)},
            {"plan_id", uuid(id.plan_id)},
            {"generation", u64s(id.generation)},
            {"reload_epoch", u64s(id.reload_epoch)},
            {"plan_sha256", hex_bytes(id.plan_sha256, 32)},
            {"archive_sha256", hex_bytes(id.archive_sha256, 32)},
            {"registry_id", u64s(id.registry_id)},
            {"registry_sha256", hex_bytes(id.registry_sha256, 32)}};
}

// ---- the view ----
class CommittedView final : public BaseView {
   public:
    static constexpr size_t kCauseMax = 1024;  // bytes of a provider's cause kept in a reason

    const char* source() const override { return "committed"; }

    /// A new generation (kiln.boot's decide()): a view still held is released, then the view waits for bind().
    void reset() {
        release();
        std::lock_guard lk(mutex_);
        state_ = State::Unbound;
        why_ = kUnbound;
    }

    /// Refuses this generation before the provider is asked anything (a reason kiln.boot found itself, such as a
    /// kiln.caps row that is off): an unbound view stays without game data, with that reason. False, with *why.
    bool refuse(const std::string& reason, std::string* why) {
        std::lock_guard lk(mutex_);
        if (state_ == State::Unbound) state_ = State::Failed, why_ = reason;
        *why = why_;
        return false;
    }

    /// Binds the view to the generation the identity source hands for this launch (the rules above). True: bound.
    /// A release() before it or while it runs wins: nothing acquired is served, the view is given back.
    bool bind(KilnOwner owner, const LaunchFacts& launch, const KilnEmlArchiveApiV1* api, IdentitySource identity, std::string* why) {
        std::lock_guard bl(bind_mutex_);
        {
            std::lock_guard lk(mutex_);
            if (state_ != State::Unbound) {
                if (state_ == State::Bound) return why->clear(), true;
                return *why = why_, false;
            }
        }
        std::string fail;
        KilnEmlArchiveIdentityV1 want{};
        KilnEmlArchiveView view = 0;
        if (!api) {
            fail = "kiln-eml offers no kiln.eml.archive table (kiln-eml.dll is not loaded, or is too old)";
        } else if (!(KILN_EML_ARCHIVE_HAS(api, acquire) && KILN_EML_ARCHIVE_HAS(api, identity) && KILN_EML_ARCHIVE_HAS(api, resource_json) &&
                     KILN_EML_ARCHIVE_HAS(api, release))) {
            fail = "kiln-eml's kiln.eml.archive table is too short or lacks acquire, identity, resource_json or release";
        } else if (!owner) {
            fail = "kiln.boot has no owner to acquire the view with";
        } else if (!identity) {
            fail = "no committed archive generation was handed to kiln.boot";
        } else if (launch.launch_id == 0) {
            fail = "kiln.launch has no launch id for this start";
        } else {
            want.size = sizeof want;
            std::string text;
            const KilnResult rc = identity(launch, &want, &text);
            if (rc != KILN_OK)
                fail = text.empty() ? "kiln-eml handed no committed archive generation for this launch" : text;
            else
                fail = check_identity(want, launch);
            if (fail.empty()) {
                const KilnResult a = api->acquire(owner, &want, &view);
                if (a != KILN_OK || !view) {
                    fail = "kiln-eml refused the committed archive generation's view (result " + std::to_string(a) + ")";
                    view = 0;
                } else {
                    KilnEmlArchiveIdentityV1 got{};
                    got.size = sizeof got;
                    if (api->identity(view, &got) != KILN_OK || !same_pins(got, want)) {
                        fail = "kiln-eml's view names another archive generation than the one it handed";
                        api->release(view);
                        view = 0;
                    }
                }
            }
        }
        std::unique_lock lk(mutex_);
        if (fail.empty() && state_ != State::Unbound) {  // released (the commit) while it bound: nothing of it is served
            fail = why_;
            lk.unlock();
            api->release(view);
            *why = fail;
            return false;
        }
        if (!fail.empty()) {
            if (state_ == State::Unbound) state_ = State::Failed, why_ = fail;
            *why = fail;
            return false;
        }
        state_ = State::Bound;
        api_ = api;
        view_ = view;
        owner_ = owner;
        identity_ = want;
        identity_.size = sizeof identity_;
        pin_ = pin_of(want);
        boot_gd::Sha256 h;
        h.update(kDigestHead).update("committed\n").update(&identity_, sizeof identity_);
        digest_ = boot_gd::hex_of(h.final());
        why->clear();
        return true;
    }

    bool ready(std::string* why) override {
        std::lock_guard lk(mutex_);
        if (state_ == State::Bound) return true;
        *why = why_;
        return false;
    }

    /// SHA-256 of kDigestHead, "committed\n", then the generation's 160-byte identity record (every pin).
    std::string digest() override {
        std::lock_guard lk(mutex_);
        return state_ == State::Bound ? digest_ : std::string();
    }

    KilnResult tree(std::string_view query, size_t capacity, size_t result_max, uint64_t budget_left, std::string* json, size_t* required,
                    size_t* value_bytes, std::string* why) override {
        std::string q;
        const KilnResult form = normalized(query, &q, why);
        if (form != KILN_OK) return form;
        Pinned p;
        if (!enter(&p, why)) return KILN_ERR_UNAVAILABLE;
        std::string reply;
        const KilnResult rc = read_json(p, q, &reply, why);
        leave();
        if (rc != KILN_OK) return rc;
        const auto j = nlohmann::json::parse(reply, nullptr, false);
        if (!j.is_object() || !j.contains("schema_version") || j["schema_version"] != 1 || !j.contains("pin") || !j.contains("value"))
            return *why = "kiln-eml's reply is not a resource_json envelope", KILN_ERR_UNAVAILABLE;
        if (j["pin"] != p.pin) return *why = "kiln-eml's reply belongs to another archive generation", KILN_ERR_UNAVAILABLE;
        std::string guid;
        const auto id = j.find("resourceId"), type = j.find("typeHash"), part = j.find("part");
        if (id == j.end() || !id->is_string() || !eml_tree::unhex(id->get_ref<const std::string&>(), "guid:", &guid, 16) || type == j.end() ||
            !type->is_number_unsigned() || type->get<uint64_t>() > 0xFFFFFFFFu || part == j.end() || !part->is_number_unsigned() ||
            part->get<uint64_t>() > 0xFFFFFFFFu)
            return *why = "kiln-eml's reply names no resource", KILN_ERR_UNAVAILABLE;
        nlohmann::ordered_json value;
        if (!translate_value(j["value"], &value, why)) return KILN_ERR_UNAVAILABLE;
        const std::string text = value.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
        char hash[9];
        std::snprintf(hash, sizeof hash, "%08x", static_cast<unsigned>(type->get<uint64_t>()));
        const std::string head = "{\"resourceId\":\"" + eml_tree::guid_text(reinterpret_cast<const uint8_t*>(guid.data())) + "\",\"typeHash\":\"" +
                                 hash + "\",\"part\":" + std::to_string(part->get<uint64_t>()) + ",\"value\":";
        // the result's limits and the author's budget as the snapshot's (boot_gd::Reader::read)
        if (text.size() > result_max || head.size() + text.size() + 2 > result_max)
            return *why = "the record is larger than one result may be", KILN_ERR_UNAVAILABLE;
        if (text.size() > budget_left) return *why = "the mod's game-data budget is spent", KILN_ERR_UNAVAILABLE;
        const size_t need = head.size() + text.size() + 2;
        if (capacity < need) return *required = need, KILN_ERR_BUFFER_TOO_SMALL;
        *json = head + text + "}";
        *value_bytes = text.size();
        return KILN_OK;
    }

    KilnResult bytes(const KilnAssetKey& key, size_t max, std::string* out, std::string* why) override {
        Pinned p;
        if (!enter(&p, why)) return KILN_ERR_UNAVAILABLE;
        const KilnResult rc = read_bytes(p, key, max, out, why);
        leave();
        return rc;
    }

    void release() override {
        KilnEmlArchiveView view = 0;
        const KilnEmlArchiveApiV1* api = nullptr;
        {
            std::unique_lock lk(mutex_);
            if (state_ == State::Bound || state_ == State::Stale || state_ == State::Unbound) {  // unbound: bind() refuses now
                why_ = "the committed archive generation's reads are over (kiln.boot released its view)";
                state_ = State::Released;
            }
            idle_.wait(lk, [&] { return inflight_ == 0; });  // the reads in flight finish first: none after the release
            view = view_, api = api_;
            view_ = 0;
        }
        if (view) api->release(view);
    }

   private:
    enum class State { Unbound, Bound, Failed, Stale, Released };
    static constexpr const char* kUnbound =
        "the committed archive generation is bound once kiln-eml's startup pass has run (kiln.boot's bound)";
    struct Pinned {
        const KilnEmlArchiveApiV1* api = nullptr;
        KilnEmlArchiveView view = 0;
        KilnOwner owner = 0;
        nlohmann::json pin;
    };

    static std::string check_identity(const KilnEmlArchiveIdentityV1& id, const LaunchFacts& launch) {
        auto zero = [](const uint8_t* p, size_t n) {
            for (size_t i = 0; i < n; ++i)
                if (p[i]) return false;
            return true;
        };
        if (id.size < sizeof id) return "the committed generation's identity record is short";
        if (id.phase != KILN_EML_ARCHIVE_PUBLISHED) return "the archive generation handed to kiln.boot is not the published one";
        uint8_t launch_pin[16] = {};
        for (int i = 0; i < 8; ++i) launch_pin[8 + i] = static_cast<uint8_t>(launch.launch_id >> (56 - 8 * i));
        if (std::memcmp(id.launch_id, launch_pin, 16) != 0) return "the committed archive generation belongs to another launch";
        if (std::memcmp(id.plan_sha256, launch.plan_sha256.data(), 32) != 0) return "the committed archive generation was made for another launch plan";
        if (zero(id.plan_id, 16)) return "the committed archive generation names no launch plan";
        if (id.generation == 0 || id.reload_epoch != 1) return "the committed archive generation's number or epoch is not this start's";
        if (zero(id.archive_sha256, 32) || id.registry_id == 0 || zero(id.registry_sha256, 32))
            return "the committed archive generation's archive or registry digest is missing";
        return {};
    }
    static bool same_pins(const KilnEmlArchiveIdentityV1& a, const KilnEmlArchiveIdentityV1& b) {
        return a.phase == b.phase && !std::memcmp(a.launch_id, b.launch_id, 16) && !std::memcmp(a.plan_id, b.plan_id, 16) &&
               a.generation == b.generation && a.reload_epoch == b.reload_epoch && !std::memcmp(a.plan_sha256, b.plan_sha256, 32) &&
               !std::memcmp(a.archive_sha256, b.archive_sha256, 32) && a.registry_id == b.registry_id &&
               !std::memcmp(a.registry_sha256, b.registry_sha256, 32);
    }

    // kiln.boot's grammar (query_ok) to resource_json's: a resource's type name stands for its hash, hex in lower case.
    static KilnResult normalized(std::string_view query, std::string* q, std::string* why) {
        const size_t colon = query.find(':');
        if (colon == std::string_view::npos) return KILN_ERR_INVALID_ARGUMENT;
        const std::string_view form = query.substr(0, colon), arg = query.substr(colon + 1);
        if (form == "names")
            return *why = "name lists (names:...) come from the boot snapshot, which a kiln-backend start does not read", KILN_ERR_UNAVAILABLE;
        if (form == "skill") return *q = "skill:" + boot_gd::lowered(arg), KILN_OK;
        if (form == "resource") {
            const size_t a = arg.find('/'), b = arg.rfind('/');
            if (a == std::string_view::npos || b <= a) return KILN_ERR_INVALID_ARGUMENT;
            const std::string_view type = arg.substr(a + 1, b - a - 1);
            std::string hash = boot_gd::lowered(type);
            if (!boot_gd::lower_hex(hash, 8)) {
                char h[9];
                std::snprintf(h, sizeof h, "%08x", boot_gd::fnv1a32(type));
                hash = h;
            }
            return *q = "resource:" + boot_gd::lowered(arg.substr(0, a)) + "/" + hash + std::string(arg.substr(b)), KILN_OK;
        }
        if (form == "item" || form == "template" || form == "material") return *q = std::string(query), KILN_OK;
        return KILN_ERR_INVALID_ARGUMENT;
    }

    bool enter(Pinned* p, std::string* why) {
        std::lock_guard lk(mutex_);
        if (state_ != State::Bound) return *why = why_, false;
        ++inflight_;
        p->api = api_, p->view = view_, p->owner = owner_, p->pin = pin_;
        return true;
    }
    void leave() {
        std::lock_guard lk(mutex_);
        if (--inflight_ == 0) idle_.notify_all();
    }
    // A REVOKED answer: the generation was retired under the view. Its token stays held until release() gives it back.
    void stale() {
        std::lock_guard lk(mutex_);
        if (state_ == State::Bound) {
            state_ = State::Stale;
            why_ = "kiln-eml closed the committed archive generation's view (a stale handle)";
        }
    }

    // The provider's cause behind a failure's error token (copied, then the token released), at most kCauseMax bytes.
    static std::string cause(const Pinned& p, KilnArchiveErrorV1& e) {
        if (!e.token) return {};
        std::string text;
        if (e.message_bytes && e.message_bytes <= (64u << 10)) {
            std::string buf(static_cast<size_t>(e.message_bytes), '\0');
            size_t req = 0;
            if (p.api->error_copy(p.owner, e.token, buf.data(), buf.size(), &req) == KILN_OK) text = buf.substr(0, (std::min)(req, kCauseMax));
        }
        p.api->error_release(p.owner, e.token);
        e.token = 0;
        return text;
    }
    static KilnArchiveErrorV1* record(const Pinned& p, KilnArchiveErrorV1& e) {
        std::memset(&e, 0, sizeof e);
        e.size = sizeof e;
        return KILN_EML_ARCHIVE_HAS(p.api, error_copy) && KILN_EML_ARCHIVE_HAS(p.api, error_release) ? &e : nullptr;
    }
    KilnResult refused(const Pinned& p, KilnResult rc, KilnArchiveErrorV1& e, std::string* why) {
        const std::string c = cause(p, e);
        const std::string tail = c.empty() ? std::string() : " (" + c + ")";
        switch (rc) {
            case KILN_ERR_NOT_FOUND: return *why = "not in the committed archive generation", KILN_ERR_NOT_FOUND;
            case KILN_ERR_REVOKED:
                stale();
                return *why = "kiln-eml closed the committed archive generation's view (a stale handle)", KILN_ERR_UNAVAILABLE;
            case KILN_ERR_UNAVAILABLE:
                return *why = "kiln-eml cannot answer it from the committed archive generation" + tail, KILN_ERR_UNAVAILABLE;
            case KILN_ERR_INVALID_ARGUMENT: return *why = "kiln-eml refused the query" + tail, KILN_ERR_UNAVAILABLE;
            default: return *why = "kiln-eml failed to read the committed archive generation" + tail, KILN_ERR_UNAVAILABLE;
        }
    }

    KilnResult read_json(const Pinned& p, const std::string& q, std::string* reply, std::string* why) {
        KilnArchiveErrorV1 e{};
        const KilnStr qs{q.data(), q.size()};
        size_t req = 0;
        KilnResult rc = p.api->resource_json(p.view, qs, nullptr, 0, &req, record(p, e));
        if (rc != KILN_ERR_BUFFER_TOO_SMALL) {
            if (rc == KILN_OK) return *why = "kiln-eml answered an empty reply", KILN_ERR_UNAVAILABLE;
            return refused(p, rc, e, why);
        }
        if (req == 0 || req > (64u << 20)) return *why = "kiln-eml's reply is empty or larger than 64 MiB", KILN_ERR_UNAVAILABLE;
        std::string buf(req, '\0');
        size_t got = 0;
        rc = p.api->resource_json(p.view, qs, buf.data(), buf.size(), &got, record(p, e));
        if (rc != KILN_OK) {
            if (rc == KILN_ERR_BUFFER_TOO_SMALL) return *why = "kiln-eml's reply changed size between two reads", KILN_ERR_UNAVAILABLE;
            return refused(p, rc, e, why);
        }
        if (got != req) return *why = "kiln-eml's reply changed size between two reads", KILN_ERR_UNAVAILABLE;
        *reply = std::move(buf);
        return KILN_OK;
    }

    KilnResult read_bytes(const Pinned& p, const KilnAssetKey& key, size_t max, std::string* out, std::string* why) {
        if (!KILN_EML_ARCHIVE_HAS(p.api, read_resource)) return *why = "kiln-eml's kiln.eml.archive table has no read_resource", KILN_ERR_UNAVAILABLE;
        KilnArchiveErrorV1 e{};
        char probe = 0;
        size_t req = 0;
        KilnResult rc = p.api->read_resource(p.owner, p.view, &key, &probe, 0, &req, record(p, e));
        if (rc == KILN_OK && req == 0) return out->clear(), KILN_OK;
        if (rc != KILN_ERR_BUFFER_TOO_SMALL) return refused(p, rc == KILN_OK ? KILN_ERR_INTERNAL : rc, e, why);
        if (req > max) return *why = "the resource is larger than the read may be", KILN_ERR_UNAVAILABLE;
        std::string buf(req, '\0');
        size_t got = 0;
        rc = p.api->read_resource(p.owner, p.view, &key, buf.data(), buf.size(), &got, record(p, e));
        if (rc != KILN_OK) return refused(p, rc == KILN_ERR_BUFFER_TOO_SMALL ? KILN_ERR_INTERNAL : rc, e, why);
        if (got != req) return *why = "kiln-eml's resource changed size between two reads", KILN_ERR_UNAVAILABLE;
        *out = std::move(buf);
        return KILN_OK;
    }

    std::mutex bind_mutex_;
    std::mutex mutex_;
    std::condition_variable idle_;
    State state_ = State::Unbound;
    std::string why_ = kUnbound;
    const KilnEmlArchiveApiV1* api_ = nullptr;
    KilnEmlArchiveView view_ = 0;
    KilnOwner owner_ = 0;
    KilnEmlArchiveIdentityV1 identity_{};
    nlohmann::json pin_;
    std::string digest_;
    size_t inflight_ = 0;
};

}  // namespace kiln::boot_view

namespace kiln::boot_svc {
/// kiln.boot takes its kiln-backend sources (defined in boot_service.cpp; boot_archive_eml.cpp calls it at load, the
/// host tests with their fakes). Null: none, and every start reads its policy's base as before.
void use_committed_generation(const boot_view::CommittedSource* source);
}  // namespace kiln::boot_svc
