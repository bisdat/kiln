// SPDX-License-Identifier: MIT
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "../cook/vfxcook.h"
#include "../held.h"
#include <type_traits>
#include "services/kiln_assets.h"
#define KV_I(...) ((void)0)
#define KV_W(...) ((void)0)
#define KV_E(...) ((void)0)
static unsigned refusals = 0;
static const char* fingerprint = "ok";
static void set_status(const char*, const std::string&, const kvb::CookResult*) {}
static void refuse_required(bool, bool, const std::string&) { ++refusals; }
void build_forget();
#include "boot_build_impl.inc"
static unsigned failures = 0, subscriptions = 0, checks = 0;
static int fail_type = 0;
static std::set<KilnOwner> resources, blobs;
static void check(bool b, const char* why) { ++checks; if (!b) { ++failures; std::printf("FAIL %s\n", why); } }
static KilnResult KILN_CALL put(KilnOwner o, const KilnAssetKey* k, const void*, size_t, uint32_t) {
    if (k->type == static_cast<uint32_t>(fail_type)) return KILN_ERR_UNAVAILABLE;
    resources.insert(o); return KILN_OK;
}
static KilnResult KILN_CALL blob(KilnOwner o, const KilnContentHash*, const void*, size_t) { blobs.insert(o); return KILN_OK; }
static kvb::Bytes payload{1, 2, 3};
static KilnResult KILN_CALL read(const KilnContentHash*, void* out, size_t, size_t* n) { std::memcpy(out, payload.data(), payload.size()); *n = payload.size(); return KILN_OK; }
static void KILN_CALL revert(KilnOwner o) { resources.erase(o); blobs.erase(o); }
static KilnOwner KILN_CALL owner(KilnStr) { return 2; }
static KilnResult KILN_CALL post(KilnOwner, KilnTaskFn, void*) { return KILN_OK; }
static KilnResult KILN_CALL remove(KilnOwner, const KilnAssetKey*) { return KILN_OK; }
static KilnResult KILN_CALL subscribe(KilnOwner, int32_t, KilnTickFn, void*, KilnHandle* h) { *h = ++subscriptions; return KILN_OK; }
template<class F> bool required_serve(F fn, const kvb::CookResult& r, const kiln::boot::Input& input) {
    if constexpr (std::is_invocable_v<F, const kvb::CookResult&, const kiln::boot::Input*>) return fn(r, &input);
    else return fn(r);
}
int main() {
    KilnApi api{}; api.owner_create = owner; api.post = post; api.subscribe_tick = subscribe;
    KilnAssetsApi assets{}; assets.resource_put = put; assets.resource_remove = remove;
    assets.blob_override = blob; assets.blob_read = read; assets.revert_owner = revert;
    g_api = &api; g_assets = &assets; g_self = 1;
    kvb::CookResult r;
    kvb::ServedBlob b; b.data = payload; b.hash = kvb::content_hash(payload.data(), payload.size()); r.blobs.push_back(b);
    kvb::ServedResource res; res.type = kvb::kTypeVfx; res.data = payload; r.resources.push_back(res);
    kiln::boot::Input required; required.required.insert("reskin");
    fail_type = kvb::kTypeVfx;
    check(!required_serve(&serve, r, required), "a refused VFX resource must fail the build");
    build_serve(r, &required);
    check(refusals == 1, "failed serving reaches the required-participant refusal check");
    check(resources.empty() && blobs.empty(), "failed serving rolls back before returning");
    build_forget(); check(resources.empty() && blobs.empty(), "rollback removes all served data");
    fail_type = kvb::kTypeVfx;
    res.type = kvb::kTypeMaterial; res.override = true; res.mod = "reskin"; r.resources.push_back(res);
    check(serve(r), "ordinary on_ready retains its per-VFX fallback");
    build_forget();
    fail_type = kvb::kTypeMaterial;
    check(!required_serve(&serve, r, required), "a refused required reskin must fail the build");
    build_forget();
    check(serve(r), "optional reskin retains the original-material fallback");
    build_forget();
    fail_type = 0;
    for (int round = 0; round < 3; ++round) {
        check(serve(r), "retry serves its resources");
        subscribe_reskins(&api, 1);
        g_material_records[{"reskin", "m"}] = {};
        g_reskin_watch.store(true); g_late_done.store(false); g_off.store(true);
        build_forget();
        check(resources.empty() && blobs.empty() && g_reskins.empty() && g_material_records.empty(), "rollback clears reskins and material records");
        check(!g_reskin_watch && g_late_done && !g_off, "rollback clears late and owner-watch flags");
    }
    check(subscriptions == 1, "reprepare registers only one lifetime reskin tick");
    check(g_reskin_owners.size() == 1, "reprepare reuses reskin owner");
    std::printf("boot build: %u checks, %u failures\n", checks, failures); return failures ? 1 : 0;
}
