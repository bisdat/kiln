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
#include "services/kiln_profile.h"
#include "services/kiln_progress.h"
#include "../../plugins/compat/original_code_service.h"

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
std::string owner_name(KilnOwner o) { return "owner:" + std::to_string(o); }
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

// The managed host's view (api_for_managed_host): exactly the core's own kiln.internal.tasks.host on top of the
// plugins' lookup; no other view resolves it and nobody publishes that name.
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
    const KilnApi plugins{};
    const KilnApi view = managed_host_api_from(plugins);
    check(view.service == &api_service_managed_host && view.register_service == plugins.register_service,
          "the host's KilnApi is the plugins' one with the host lookup only (it publishes like a plugin)");
    tasks_svc::running = false;
    check(api_service_managed_host(host, 1) == nullptr, "while kiln.tasks is not running: nothing (the host reports it)");
    tasks_svc::running = true;
}

int main() {
    using namespace kiln;
    managed_host_view();
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
