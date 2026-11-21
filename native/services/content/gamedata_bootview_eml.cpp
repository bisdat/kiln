// SPDX-License-Identifier: MIT
// kiln.content's kiln-backend inputs in kiln-content.dll (gamedata_bootview_eml.h): whether this start runs
// the kiln backend (kiln.launch), kiln.eml.archive from kiln-eml's early export, the identity of the generation kiln-eml
// committed for this launch, kiln.caps' rows and kiln.content's owner. Registered as the resource patches' base view
// source (gamedata_bootview.h) when the DLL loads; nothing is read before the first prepare asks.
//
// Kiln-internal, with kiln.eml.archive and kiln.launch.
//
// The identity: kiln.eml.startup's session-free published_identity, from kiln-eml's early export, through the
// call kiln.boot's boot_archive_eml.cpp makes (boot_archive_eml.h published_identity_of): the identity of the generation
// kiln-eml granted this launch, once its startup barrier finished with a grant. Without it (no kiln-eml, an older one,
// no grant) there is no view, with the reason, and the resource patches are checked when they are written, as in any
// other start. CommittedView::bind() checks every pin itself. The lookups run at the first prepare, after kiln.boot's
// bound(), and ask only for a module already loaded.
#include <windows.h>

#include <cstring>
#include <string>

#include "content.h"
#include "gamedata_bootview_eml.h"
#include "services/kiln_caps.h"
#include "services/kiln_launch.h"

namespace kc {
namespace {

// An export of the kiln-eml.dll this process already loaded: no load and no reference taken. Null without either.
void* eml_export(const char* name) {
    const HMODULE module = GetModuleHandleW(L"kiln-eml.dll");
    return module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
}

bool kiln_mode(kiln::boot_view::LaunchFacts* out) {
    const auto* t = g_api ? static_cast<const KilnLaunchApi*>(g_api->service(KilnStr{KILN_LAUNCH_SERVICE, sizeof(KILN_LAUNCH_SERVICE) - 1}, KILN_LAUNCH_VERSION))
                          : nullptr;
    if (!t || t->header.size < offsetof(KilnLaunchApi, status) + sizeof(void*) || !t->status) return false;
    KilnLaunchStatus st{};
    st.size = sizeof st;
    if (t->status(&st) != KILN_OK || st.backend != KILN_LAUNCH_BACKEND_KILN) return false;
    out->launch_id = st.launch_id;
    std::memcpy(out->plan_sha256.data(), st.plan_hash, sizeof st.plan_hash);
    return true;
}

const KilnEmlArchiveApiV1* archive_table() {
    const auto get = reinterpret_cast<KilnEmlArchiveServiceFn>(eml_export(KILN_EML_ARCHIVE_SERVICE_EXPORT));
    return get ? get(KILN_EML_ARCHIVE_VERSION) : nullptr;
}

KilnResult published_identity(const kiln::boot_view::LaunchFacts& launch, KilnEmlArchiveIdentityV1* out, std::string* why) {
    return kiln::boot_view::published_identity_of(reinterpret_cast<KilnEmlStartupServiceFn>(eml_export(KILN_EML_STARTUP_SERVICE_EXPORT)),
                                                  launch, out, why);
}

bool row(const char* name, std::string* text) {
    const auto* caps = g_api ? static_cast<const KilnCapsApi*>(g_api->service(KilnStr{KILN_CAPS_SERVICE, sizeof(KILN_CAPS_SERVICE) - 1}, 1)) : nullptr;
    KilnCapRow r{};
    r.size = sizeof r;
    if (!caps || !caps->find || caps->find(KilnStr{name, std::strlen(name)}, &r) != KILN_OK) return *text = "no such row", false;
    if (r.state == KILN_CAP_AVAILABLE) return true;
    *text = r.text.ptr ? std::string(r.text.ptr, r.text.len) : std::string(r.state == KILN_CAP_PENDING ? "pending" : "unavailable");
    return false;
}

KilnOwner owner() { return g_owner; }

const bootview_eml::Inputs kInputs{&kiln_mode, &archive_table, &published_identity, &row, &owner};
bootview_eml::Backing& g_backing = *new bootview_eml::Backing(&kInputs);  // never destroyed: apply may run at any time
// no final_for answer: nothing here can say which resources nothing else in the start creates or overrides
const bootview::Source kSource{[](std::string* why) { return g_backing.view(why); }, [] { g_backing.finished(); }};
struct Registered {
    Registered() { bootview::set_source(&kSource); }
} const kRegistered;  // when kiln-content.dll loads

}  // namespace
}  // namespace kc
