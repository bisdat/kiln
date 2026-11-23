// SPDX-License-Identifier: MIT
#pragma once
#include "kiln_api.h"
#include <span>

namespace kt {
struct ToolPackage {
    bool (*install)(const KilnApi*, KilnOwner);
    void (*uninstall)();
};
// Called only by the all-services-ready callback, not DllMain. Failed packages retain their refusal bits; continue
// discovering the others so every capability has an accurate cause. No family becomes active until its entire set is on.
inline bool install_packages(const KilnApi* api, KilnOwner owner, std::span<const ToolPackage> packages) {
    bool complete=true;
    for(const auto& package:packages) {
        try {complete=package.install && package.install(api,owner) && complete;}
        catch(...) {complete=false;}
    }
    return complete;
}
// Global process shutdown only. An admission drain timeout leaves every module and pointer resident, fail-closed.
// This is not called when a tool mod unregisters: existing native undo receipts still need the history guard.
inline bool uninstall_packages(std::span<const ToolPackage> packages, bool (*close_and_drain)()) {
    if(!close_and_drain || !close_and_drain())return false;
    for(auto i=packages.rbegin();i!=packages.rend();++i)if(i->uninstall)i->uninstall();
    return true;
}
} // namespace kt
