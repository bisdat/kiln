// SPDX-License-Identifier: MIT
// kiln.boot's kiln-backend sources in kiln-core.dll (boot_archive_eml.h): whether this start runs the kiln backend
// (kiln.launch), kiln.eml.archive from kiln-eml's early export, and the identity of the generation kiln-eml committed
// for this launch. Handed to kiln.boot when the DLL loads.
//
// Kiln-internal, with kiln.eml.archive and kiln.launch.
//
// The identity: kiln.eml.startup's session-free published_identity, from kiln-eml's early export
// (boot_archive_eml.h published_identity_of): the identity of the generation kiln-eml granted this launch, once its
// startup barrier finished with a grant. Without it (no kiln-eml, an older one, no grant) a kiln-backend start's OnBoot
// has no game data, with the reason; never the snapshot. kiln.boot checks every pin itself (bind()). The lookups run at
// kiln.boot's bound(), after the core loaded its service DLLs and plugins (boot.cpp load_plugins, whose loads already
// waited for any package DLL initializer that held the loader lock), and ask only for a module already loaded.
#include <cstring>
#include <string>

#include <windows.h>

#include "../launch.h"
#include "boot_archive_eml.h"

namespace kiln::boot_svc {
namespace {

// An export of the kiln-eml.dll this process already loaded: no load and no reference taken. Null without either.
void* eml_export(const char* name) {
    const HMODULE module = GetModuleHandleW(L"kiln-eml.dll");
    return module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
}

bool kiln_mode(boot_view::LaunchFacts* out) {
    if (launch::backend() != KILN_LAUNCH_BACKEND_KILN) return false;
    // the kiln backend, whatever else: no snapshot is read. Facts that cannot be read leave launch_id 0, and the view
    // refuses to bind.
    KilnLaunchStatus st{};
    st.size = sizeof st;
    const KilnLaunchApi* t = launch::table();
    if (t && t->status && t->status(&st) == KILN_OK && st.backend == KILN_LAUNCH_BACKEND_KILN) {
        out->launch_id = st.launch_id;
        std::memcpy(out->plan_sha256.data(), st.plan_hash, sizeof st.plan_hash);
    }
    return true;
}

const KilnEmlArchiveApiV1* archive_table() {
    const auto get = reinterpret_cast<KilnEmlArchiveServiceFn>(eml_export(KILN_EML_ARCHIVE_SERVICE_EXPORT));
    return get ? get(KILN_EML_ARCHIVE_VERSION) : nullptr;
}

KilnResult published_identity(const boot_view::LaunchFacts& launch, KilnEmlArchiveIdentityV1* out, std::string* why) {
    const auto get = reinterpret_cast<KilnEmlStartupServiceFn>(eml_export(KILN_EML_STARTUP_SERVICE_EXPORT));
    return boot_view::published_identity_of(get, launch, out, why);
}

const boot_view::CommittedSource kSource{&kiln_mode, &archive_table, &published_identity};
struct Handed {
    Handed() { use_committed_generation(&kSource); }
} const kHanded;  // when kiln-core.dll loads, before boot() runs

}  // namespace
}  // namespace kiln::boot_svc
