// SPDX-License-Identifier: MIT
// The real api.cpp service lookup/registration block (between its "service lookup" markers), compiled against host
// stubs: Kiln-internal tables ("kiln.internal.*") are registered only by Kiln's own service owners and resolved only
// through the services' KilnApi; plugins, managed mods and foreign DLLs get nothing. Built twice by
// test-service-access.sh with the production build identity (KILN_VERSION + KILN_BUILD): once stamped, once without
// build metadata (KILN_BUILD "", KILN_TEST_UNSTAMPED), where no DLL may get internal access.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "kiln_api.h"
#include "services/kiln_caps.h"
#include "services/kiln_hooks.h"
#include "services/kiln_launch.h"
#include "../src/launch_internal.h"
#include "../src/hooks_protect_internal.h"
#include "services/kiln_profile.h"
#include "services/kiln_progress.h"
#include "../../plugins/compat/original_code_service.h"
#include "../src/mod_values_host.h"  // KILN_MOD_VALUES_BACKEND_SERVICE (Kiln-internal)
#include "../src/owners_internal.h"  // KILN_OWNERS_INTERNAL_SERVICE (Kiln-internal)

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}
std::string last_log;
void KILN_CALL dummy_log(KilnOwner, int32_t, KilnStr) {}
KilnResult KILN_CALL dummy_session(KilnSessionInfo*) { return KILN_OK; }
}  // namespace

namespace kiln {
inline std::string_view sv(KilnStr s) { return s.ptr ? std::string_view(s.ptr, s.len) : std::string_view(); }
bool g_hooks = true;
std::set<KilnOwner> alive{1, 2, 3, 4, 5, 6};
namespace reg {
bool owner_alive(KilnOwner o) { return o == 0 || alive.count(o); }
std::string owner_name(KilnOwner o) { return o == 3 ? std::string("mod:Fishing-Plus") : "owner:" + std::to_string(o); }
std::vector<std::string> host_created;  // the names owner_create_managed_host was given
KilnOwner owner_create_managed_host(std::string_view id) {
    host_created.emplace_back(id);
    return 42;
}
}  // namespace reg
namespace svc {
const void* find(std::string_view, uint32_t) { return nullptr; }
}
namespace profile { const void* table() { return nullptr; } }
namespace progress { const void* table() { return nullptr; } }
namespace compat {
const OriginalCodeApi original{{sizeof(OriginalCodeApi), kOriginalCodeVersion}, nullptr, nullptr, nullptr};
const OriginalCodeApi* original_code_table() { return &original; }  // stands for the core's compat.cpp table
}  // namespace compat
namespace tasks_svc {
const KilnServiceHeader host{sizeof(KilnServiceHeader), 1};  // stands for the core's kiln.internal.tasks.host table
bool running = true;
const void* host_table() { return running ? &host : nullptr; }
}  // namespace tasks_svc
namespace launch {
const KilnLaunchApi own{{sizeof(KilnLaunchApi), KILN_LAUNCH_VERSION}, nullptr, nullptr};  // stands for launch.cpp's table
const KilnLaunchApi* table() { return &own; }
const KilnLaunchInternalApi internal_own{{sizeof(KilnLaunchInternalApi), KILN_LAUNCH_INTERNAL_VERSION}, nullptr};
const KilnLaunchInternalApi* internal_table() { return &internal_own; }  // stands for launch.cpp's internal table
}  // namespace launch
namespace hooks {
const KilnHooksInternalApi internal_own{{sizeof(KilnHooksInternalApi), KILN_HOOKS_INTERNAL_VERSION}, nullptr};
const KilnHooksInternalApi* internal_table() { return &internal_own; }  // stands for hooks_protect.cpp's table
}  // namespace hooks
namespace mod_values {
const KilnServiceHeader backend{sizeof(KilnServiceHeader), 1};  // stands for the core's kiln.internal.mod-values.backend
bool running = true;
const void* backend_table() { return running ? &backend : nullptr; }
}  // namespace mod_values
namespace caps {
const void* table() { return nullptr; }
int published = 0;
void service_published(KilnOwner, std::string_view, uint32_t) { ++published; }
}  // namespace caps
}  // namespace kiln
#define KILN_TASKS_HOST_SERVICE "kiln.internal.tasks.host"  // tasks_host.h
#define KILN_TASKS_HOST_VERSION 1u
#define KLOG_I(...) (void)0
#define KLOG_W(...) (last_log = "warn")
#define KLOG_E(...) (last_log = "error")

namespace kiln {
namespace {
#include "service_access.inc"
}  // namespace
}  // namespace kiln

// The managed host's view (api_for_managed_host): exactly the core's own kiln.internal.tasks.host and
// kiln.internal.mod-values.backend on top of the plugins' lookup; no other view resolves them, nobody publishes those
// names, and the owners the host creates through it are marked as the host's.
void managed_host_view() {
    using namespace kiln;
    const KilnStr host{KILN_TASKS_HOST_SERVICE, std::strlen(KILN_TASKS_HOST_SERVICE)};
    const KilnStr other{"kiln.internal.example", 21};
    const KilnServiceHeader spoof{sizeof(KilnServiceHeader), 1};
    check(api_service_managed_host(host, 1) == &tasks_svc::host, "the host view resolves the core's lease table");
    check(api_service_managed_host(host, 2) == nullptr, "...not for a later version");
    check(api_service(host, 1) == nullptr, "the plugins' view never resolves it");
    check(api_service_services(host, 1) == nullptr, "the services' view never resolves it");
    check(api_register_service_services(1, host, &spoof) == KILN_ERR_INVALID_ARGUMENT &&
              api_register_service(2, host, &spoof) == KILN_ERR_INVALID_ARGUMENT,
          "nobody publishes that name, not even a Kiln service owner");
    check(api_service_managed_host(host, 1) == &tasks_svc::host && api_service_services(host, 1) == nullptr,
          "and the views still say the same after the attempts");
    check(api_service_managed_host(other, 1) == nullptr, "the host view resolves no other internal table");
    const KilnStr open{"kiln.example", 12};
    check(api_service_managed_host(open, 1) == api_service(open, 1), "public names resolve as for plugins");
    KilnApi plugins{};
    plugins.size = sizeof plugins;
    plugins.service = &api_service;
    plugins.register_service = &api_register_service;
    plugins.log = &dummy_log;
    const KilnApi view = managed_host_api_from(plugins);
    KilnApi expected = plugins;
    expected.service = &api_service_managed_host;
    expected.owner_create = &api_owner_create_managed_host;
    check(std::memcmp(&view, &expected, sizeof view) == 0,
          "the host's KilnApi is the plugins' one with the host lookup and the host's owner_create only (it publishes like a plugin)");
    check(view.owner_create(KilnStr{"mod:fishing", 11}) == 42 && reg::host_created == std::vector<std::string>{"mod:fishing"},
          "an owner the host creates through its KilnApi is made as the managed host's (kiln.mod-values maps mod:<id> only for those)");
    tasks_svc::running = false;
    check(api_service_managed_host(host, 1) == nullptr, "while kiln.tasks is not running: nothing (the host reports it)");
    tasks_svc::running = true;

    // kiln.internal.mod-values.backend: Kiln.ModValues binds the native facade's backend through it, nothing else
    const KilnStr mv{KILN_MOD_VALUES_BACKEND_SERVICE, std::strlen(KILN_MOD_VALUES_BACKEND_SERVICE)};
    check(std::string_view(KILN_MOD_VALUES_BACKEND_SERVICE) == "kiln.internal.mod-values.backend" && internal_name(sv(mv)),
          "the mod-values backend has a Kiln-internal name");
    check(api_service_managed_host(mv, 1) == &mod_values::backend, "the host view resolves the core's mod-values backend");
    check(api_service_managed_host(mv, 2) == nullptr, "...not for a later version");
    check(api_service(mv, 1) == nullptr && api_service_services(mv, 1) == nullptr, "the plugins' and the services' views never resolve it");
    check(api_register_service_services(1, mv, &spoof) == KILN_ERR_INVALID_ARGUMENT &&
              api_register_service(2, mv, &spoof) == KILN_ERR_INVALID_ARGUMENT,
          "nobody publishes that name, not even a Kiln service owner");
    check(api_service_managed_host(mv, 1) == &mod_values::backend && api_service_services(mv, 1) == nullptr,
          "and the views still say the same after the attempts");
    const KilnStr old{"kiln.mod-values.backend", 23};
    check(api_service_managed_host(old, 1) == nullptr && api_service(old, 1) == nullptr, "the old public name resolves nowhere");
    mod_values::running = false;
    check(api_service_managed_host(mv, 1) == nullptr, "while kiln.mod-values is not running: nothing (Kiln.ModValues reports it)");
    mod_values::running = true;
}

// kiln.launch (launch.h): the core's own table in every view, hooked or safe idle, and nobody publishes the name.
void launch_views() {
    using namespace kiln;
    const KilnStr name{KILN_LAUNCH_SERVICE, std::strlen(KILN_LAUNCH_SERVICE)};
    for (const bool hooks : {true, false}) {
        g_hooks = hooks;
        const char* mode = hooks ? " (hooks on)" : " (safe idle)";
        check(api_service(name, 1) == &launch::own, std::string("the plugins' view resolves kiln.launch").append(mode).c_str());
        check(api_service_services(name, 1) == &launch::own,
              std::string("the services' view resolves kiln.launch").append(mode).c_str());
        check(api_service_managed_host(name, 1) == &launch::own,
              std::string("the managed host's view resolves kiln.launch").append(mode).c_str());
        check(!api_service(name, 2) && !api_service_services(name, 2) && !api_service_managed_host(name, 2),
              std::string("...not for a later version").append(mode).c_str());
    }
    g_hooks = true;
    const KilnServiceHeader spoof{sizeof(KilnLaunchApi), 1};
    alive.insert(7);
    service_owner_add(7);  // a Kiln service of this build
    const int before = caps::published;
    last_log.clear();
    check(api_register_service(2, name, &spoof) == KILN_ERR_INVALID_ARGUMENT, "a plugin cannot publish kiln.launch");
    check(last_log == "warn", "...and the refusal is logged");
    check(api_register_service(7, name, &spoof) == KILN_ERR_INVALID_ARGUMENT &&
              api_register_service_services(7, name, &spoof) == KILN_ERR_INVALID_ARGUMENT,
          "nor can a Kiln service, through either KilnApi");
    check(caps::published == before, "a refused publication adds no capability row");
    check(api_service(name, 1) == &launch::own && api_service_services(name, 1) == &launch::own,
          "and every view still resolves the core's table");
    alive.erase(7);
}

// kiln.internal.launch (launch_internal.h): the core's own, through the services' view only; nobody publishes it.
void internal_launch_views() {
    using namespace kiln;
    const KilnStr name{KILN_LAUNCH_INTERNAL_SERVICE, std::strlen(KILN_LAUNCH_INTERNAL_SERVICE)};
    for (const bool hooks : {true, false}) {
        g_hooks = hooks;
        const char* mode = hooks ? " (hooks on)" : " (safe idle)";
        check(api_service_services(name, 1) == &launch::internal_own,
              std::string("the services' view resolves kiln.internal.launch").append(mode).c_str());
        check(api_service(name, 1) == nullptr, std::string("the plugins' view never resolves it").append(mode).c_str());
        check(api_service_managed_host(name, 1) == nullptr,
              std::string("nor the managed host's view").append(mode).c_str());
        check(api_service_services(name, 2) == nullptr, std::string("...not for a later version").append(mode).c_str());
    }
    g_hooks = true;
    const KilnServiceHeader spoof{sizeof(KilnLaunchInternalApi), 1};
    alive.insert(7);
    service_owner_add(7);
    const int before = caps::published;
    check(api_register_service_services(7, name, &spoof) == KILN_ERR_INVALID_ARGUMENT,
          "not even a Kiln service publishes kiln.internal.launch");
    check(api_register_service(2, name, &spoof) == KILN_ERR_INVALID_ARGUMENT, "nor a plugin");
    check(caps::published == before && api_service_services(name, 1) == &launch::internal_own,
          "and the services' view still resolves the core's table");
    alive.erase(7);
}

// kiln.internal.hooks (hooks_protect_internal.h): the core's own, through the services' view only; nobody publishes it.
void internal_hooks_views() {
    using namespace kiln;
    const KilnStr name{KILN_HOOKS_INTERNAL_SERVICE, std::strlen(KILN_HOOKS_INTERNAL_SERVICE)};
    for (const bool hooks : {true, false}) {
        g_hooks = hooks;
        const char* mode = hooks ? " (hooks on)" : " (safe idle)";
        check(api_service_services(name, 1) == &hooks::internal_own,
              std::string("the services' view resolves kiln.internal.hooks").append(mode).c_str());
        check(api_service(name, 1) == nullptr, std::string("the plugins' view never resolves it").append(mode).c_str());
        check(api_service_managed_host(name, 1) == nullptr,
              std::string("nor the managed host's view").append(mode).c_str());
        check(api_service_services(name, 2) == nullptr, std::string("...not for a later version").append(mode).c_str());
    }
    g_hooks = true;
    const KilnHooksInternalApi spoof{{sizeof(KilnHooksInternalApi), 1}, nullptr};
    alive.insert(7);
    service_owner_add(7);
    const int before = caps::published;
    last_log.clear();
    check(api_register_service_services(7, name, &spoof) == KILN_ERR_INVALID_ARGUMENT && last_log == "warn",
          "not even a Kiln service publishes kiln.internal.hooks (refused with a warning)");
    check(api_register_service(2, name, &spoof) == KILN_ERR_INVALID_ARGUMENT, "nor a plugin");
    check(caps::published == before && api_service_services(name, 1) == &hooks::internal_own && api_service(name, 1) == nullptr,
          "and the services' view still resolves the core's table, the plugins' view still nothing");
    alive.erase(7);
}

// kiln.internal.owners (owners_internal.h): an owner's id for Kiln's services (kiln.registry binds its callers with it);
// the core's own, through the services' view only; nobody publishes it.
void internal_owners_views() {
    using namespace kiln;
    const KilnStr name{KILN_OWNERS_INTERNAL_SERVICE, std::strlen(KILN_OWNERS_INTERNAL_SERVICE)};
    const auto* t = static_cast<const KilnOwnersInternalApi*>(api_service_services(name, 1));
    check(t && t->header.version == 1 && t->header.size == sizeof(KilnOwnersInternalApi) && t->owner_id, "the services' view resolves kiln.internal.owners");
    check(api_service(name, 1) == nullptr, "the plugins' view never resolves it (an unstamped or foreign DLL gets that view)");
    check(api_service_managed_host(name, 1) == nullptr, "nor the managed host's view");
    check(api_service_services(name, 2) == nullptr, "...not for a later version");
    if (!t) return;
    char buf[64];
    size_t len = 0;
    check(t->owner_id(3, buf, sizeof buf, &len) == KILN_OK && std::string(buf) == "mod:Fishing-Plus" && len == 16, "owner_id: the core's id of a live owner");
    check(t->owner_id(3, buf, 4, &len) == KILN_ERR_BUFFER_TOO_SMALL && len == 16 && std::string(buf) == "mod", "a buffer too small: cut, NUL-terminated, the needed length");
    check(t->owner_id(3, nullptr, 0, &len) == KILN_ERR_BUFFER_TOO_SMALL && len == 16, "a size query");
    check(t->owner_id(99, buf, sizeof buf, &len) == KILN_ERR_NOT_FOUND, "an owner never created: not found");
    alive.insert(8);
    alive.erase(8);
    check(t->owner_id(8, buf, sizeof buf, &len) == KILN_ERR_NOT_FOUND, "a revoked owner: not found");
    check(t->owner_id(0, buf, sizeof buf, &len) == KILN_ERR_INVALID_ARGUMENT && t->owner_id(3, nullptr, 8, &len) == KILN_ERR_INVALID_ARGUMENT,
          "owner 0 and a NULL buffer with a capacity: invalid");
    const KilnOwnersInternalApi spoof{{sizeof(KilnOwnersInternalApi), 1}, nullptr};
    alive.insert(7);
    service_owner_add(7);
    last_log.clear();
    check(api_register_service_services(7, name, &spoof) == KILN_ERR_INVALID_ARGUMENT && last_log == "warn" && api_register_service(2, name, &spoof) == KILN_ERR_INVALID_ARGUMENT,
          "nobody publishes kiln.internal.owners, not even a Kiln service");
    check(api_service_services(name, 1) == t, "the services' view still resolves the core's table");
    alive.erase(7);
}

// kiln.internal.registry: kiln-content.dll (a Kiln service owner) publishes it; the managed host's view resolves exactly that
// published table, the plugins' never; a plugin cannot publish the name.
void registry_host_view() {
    using namespace kiln;
    const KilnStr name{"kiln.internal.registry", 22};
    const KilnServiceHeader table{sizeof(KilnServiceHeader), 1};
    check(api_service_managed_host(name, 1) == nullptr, "nothing before kiln-content.dll publishes it");
    check(api_register_service(2, name, &table) == KILN_ERR_INVALID_ARGUMENT, "a plugin cannot publish kiln.internal.registry");
    check(api_service_managed_host(name, 1) == nullptr, "...and the host view still resolves nothing");
    alive.insert(9);
    service_owner_add(9);
    check(api_register_service_services(9, name, &table) == KILN_OK, "a Kiln service publishes it");
    check(api_service_managed_host(name, 1) == &table, "the managed host's view resolves the published table");
    check(api_service_managed_host(name, 2) == nullptr, "...not for a later version");
    check(api_service(name, 1) == nullptr, "the plugins' view never resolves it");
    const KilnStr other{"kiln.internal.example2", 22};
    check(api_register_service_services(9, other, &table) == KILN_OK && api_service_managed_host(other, 1) == nullptr,
          "another published internal table stays out of the host view");
    alive.erase(9);
    check(api_service_managed_host(name, 1) == nullptr, "its owner revoked: gone");
}

int main() {
    using namespace kiln;
    internal_owners_views();
    registry_host_view();
    managed_host_view();
    launch_views();
    internal_launch_views();
    internal_hooks_views();
    const KilnServiceHeader table{sizeof(KilnServiceHeader), 1};
    const KilnStr internal{"kiln.internal.example", 21}, open{"kiln.example", 12};
    // owner 1 = a service loaded from kiln/services, owner 2 = a plugin, owner 3 = a plugin-made owner
    service_owner_add(1);
    // Registration is split like lookup: the plugins' register_service refuses every internal name, whatever owner it
    // is handed (owner IDs are small sequential integers a plugin can guess); only the services' one may publish.
    check(api_register_service(2, internal, &table) == KILN_ERR_INVALID_ARGUMENT, "a plugin cannot publish an internal table");
    check(api_register_service(1, internal, &table) == KILN_ERR_INVALID_ARGUMENT,
          "the plugins' KilnApi refuses an internal table even with a service owner's ID");
    check(api_service_services(internal, 1) == nullptr, "a spoofed publication never reaches the services' lookup");
    check(api_register_service_services(2, internal, &table) == KILN_ERR_INVALID_ARGUMENT,
          "the services' KilnApi still checks the owner");
    check(api_register_service_services(3, internal, &table) == KILN_ERR_INVALID_ARGUMENT, "an owner_create owner cannot publish one");
    check(api_register_service_services(1, internal, &table) == KILN_OK, "a Kiln service publishes an internal table");
    check(api_register_service(1, internal, &table) == KILN_ERR_INVALID_ARGUMENT, "and nobody publishes it twice");
    check(api_register_service(2, open, &table) == KILN_OK, "public tables are unchanged for plugins");
    check(api_service(internal, 1) == nullptr, "the plugins' KilnApi never resolves an internal table");
    check(api_register_service(2, KilnStr{"kiln.internalized", 17}, &table) == KILN_OK, "only the dotted prefix is reserved");
    check(api_service_services(internal, 1) == &table, "the services' KilnApi resolves it");
    check(api_service_services(internal, 2) == nullptr, "min_version still applies");
    check(api_service(open, 1) == &table && api_service_services(open, 1) == &table, "public lookups agree");
    // The core's original-code table (kiln.internal.original-code) is resolved by the shroudtopia plugin through the
    // plugins' KilnApi: it is the one internal name every view resolves, ahead of anything published.
    const KilnStr original{compat::kOriginalCodeService, std::strlen(compat::kOriginalCodeService)};
    check(api_service(original, compat::kOriginalCodeVersion) == &compat::original &&
              api_service_services(original, 1) == &compat::original,
          "the core's original-code table resolves for plugins and services");
    check(api_service(original, compat::kOriginalCodeVersion + 1) == nullptr, "...not for a later version");
    check(api_register_service(2, original, &table) == KILN_ERR_INVALID_ARGUMENT, "a plugin cannot publish that name");
    check(api_service(original, 1) == &compat::original, "and the lookup still gives the core's table");
    check(caps::published == 2, "internal tables get no public capability row; public ones do");
    check(api_register_service_services(1, KilnStr{"kiln.svc-public", 15}, &table) == KILN_OK && caps::published == 3,
          "a service publishes public tables through its own KilnApi as before");
    // the build identity, constructed as in production (api.cpp build_stamp(): KILN_VERSION + KILN_BUILD)
    const std::string version = KILN_VERSION, stamped = std::string(KILN_VERSION) + "+20261115.ddb9bd4";
    std::string why;
    const KilnStr later{"kiln.internal.later", 19};
#ifdef KILN_TEST_UNSTAMPED
    check(build_stamp().empty(), "no build metadata: the core has no build identity");
    check(!service_stamp_trusted(version) && !service_stamp_trusted("") && !service_stamp_trusted(stamped),
          "without build metadata no DLL is trusted, not even one exporting the same version-only identity");
    check(!grant_internal(5, version, why) && why.find("without build metadata") != std::string::npos,
          "load_one's API selection refuses and says the core has no build metadata");
    check(api_register_service_services(5, later, &table) == KILN_ERR_INVALID_ARGUMENT && !api_service_services(later, 1),
          "the refused DLL's owner never became a service owner: no internal table through any KilnApi");
#else
    check(build_stamp() == stamped, "the core's identity is its version plus its build metadata");
    check(service_stamp_trusted(stamped), "a service of the core's build is trusted");
    check(!service_stamp_trusted(version), "a stale DLL from an unstamped build (version only) is not");
    check(!service_stamp_trusted(version + "+20261114.8e0303a"), "a stale service DLL from another build is not");
    check(!service_stamp_trusted(""), "a DLL without a stamp is not");
    check(!service_stamp_trusted(stamped + " "), "the stamp compares exactly");
    check(!grant_internal(6, version, why) && why.find("is not this core's") != std::string::npos &&
              api_register_service_services(6, later, &table) == KILN_ERR_INVALID_ARGUMENT,
          "a core-only rebuild over stale service DLLs: the stale DLL's owner gets no internal access");
    check(grant_internal(5, stamped, why) && api_register_service_services(5, later, &table) == KILN_OK &&
              api_service_services(later, 1) == &table,
          "load_one's API selection grants this build's DLL a service owner that publishes internal tables");
#endif
    alive.erase(1);
    check(api_service_services(internal, 1) == nullptr, "a revoked service owner's table is gone");
    KilnApi plugins{}; plugins.size = sizeof plugins; plugins.abi_version = KILN_ABI_VERSION; plugins.service = &api_service;
    plugins.register_service = &api_register_service;
    plugins.log = &dummy_log; plugins.session = &dummy_session;
    const KilnApi services = services_api_from(plugins);
    KilnApi expected = plugins;
    expected.service = &api_service_services;
    expected.register_service = &api_register_service_services;
    check(std::memcmp(&services, &expected, sizeof services) == 0,
          "the services' KilnApi is the plugins' one with its own lookup and registration");
    // A plugin hands its KilnApi a live service owner's ID for an unused internal name.
    alive.insert(4);
    service_owner_add(4);
    constexpr char name[] = "kiln.internal.from-plugin";
    const KilnStr forged{name, sizeof name - 1};
    check(plugins.register_service(4, forged, &table) == KILN_ERR_INVALID_ARGUMENT,
          "the plugins' slot refuses internal publication with a service owner's ID");
    check(services.service(forged, 1) == nullptr, "and the forged table never reaches service consumers");
    std::printf("service access: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
