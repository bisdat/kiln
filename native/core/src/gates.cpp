// SPDX-License-Identifier: MIT
// The client gate's list and what kiln.json's "preview" lifts of it (gates.h).
#include "gates.h"

#include <algorithm>
#include <mutex>

#include "kiln_api.h"
#include "log.h"
#include "services/kiln_caps.h"

#include "kiln_gates.g.h"  // kBuiltIn, from native/core/gates/*.json
// the release hold of Kiln's interpreter for ShroudForge mods (sfi_release_policy.h)
#include "sfi_release_policy.h"

namespace kiln::gates {
namespace {
const Gate* g_list = kBuiltIn;
std::mutex g_mutex;
std::vector<Held> g_held;
bool g_configured = false;
// the release policy's rows (sfi_release_policy.h) join what the list holds, each replacing the
// list's row of that name. Always with the compiled list; with a test list only through use_policy_list_for_tests.
bool g_policy = true;

// g_mutex held. The names it added, for configure()'s log line.
std::vector<std::string> add_policy(std::vector<Held>& held) {
    std::vector<std::string> names;
    if (!g_policy) return names;
    for (Held& p : sfi_release_policy::forced(g_list)) {
        std::erase_if(held, [&](const Held& h) { return h.name == p.name; });
        names.push_back(p.name);
        held.push_back(std::move(p));
    }
    std::sort(held.begin(), held.end(), [](const Held& a, const Held& b) { return a.name < b.name; });
    return names;
}

std::vector<Held> all_of(const Gate* list, int side_code) {
    std::vector<Held> out;
    for (const Gate* g = list; g && g->name; ++g) {
        if (side_code >= 0 && g->side != kBoth && g->side != static_cast<uint32_t>(side_code)) continue;
        out.push_back({g->name, std::string(KILN_CAP_GATE_TEXT) + (g->checks ? g->checks : "")});
    }
    std::sort(out.begin(), out.end(), [](const Held& a, const Held& b) { return a.name < b.name; });
    return out;
}

// g_mutex held. The early safe-idle exit never calls configure().
const std::vector<Held>& held_locked() {
    if (!g_configured) {
        g_held = all_of(g_list, -1);
        add_policy(g_held);
        g_configured = true;
    }
    return g_held;
}
}  // namespace

void use_list_for_tests(const Gate* list) {
    std::lock_guard lock(g_mutex);
    g_list = list ? list : kBuiltIn;
    g_policy = !list;  // the release policy follows the compiled list (sfi_release_policy.h)
    g_held.clear();
    g_configured = false;
}

void use_policy_list_for_tests(const Gate* list) {
    std::lock_guard lock(g_mutex);
    g_list = list ? list : kBuiltIn;
    g_policy = true;
    g_held.clear();
    g_configured = false;
}

void configure(uint32_t side, bool preview_all, const std::vector<std::string>& preview) {
    std::lock_guard lock(g_mutex);
    const std::vector<Held> every = all_of(g_list, -1);
    std::vector<Held> mine = all_of(g_list, side == KILN_SIDE_CLIENT ? kClient : kServer);
    for (const std::string& p : preview)
        if (std::none_of(every.begin(), every.end(), [&](const Held& h) { return h.name == p; }))
            KLOG_W("config: \"preview\" names '%s', which is not a capability held for a game test; ignored", p.c_str());
    const size_t total = mine.size();
    if (preview_all)
        mine.clear();
    else
        std::erase_if(mine, [&](const Held& h) { return std::find(preview.begin(), preview.end(), h.name) != preview.end(); });
    const size_t listed = mine.size();
    const std::vector<std::string> policy = add_policy(mine);  // after "preview": it never lifts these
    g_held = std::move(mine);
    g_configured = true;
    if (total)
        KLOG_I("gate: %zu of %zu capabilities held until their game test%s", listed, total,
               preview_all ? " (kiln.json \"preview\": all lifted)" : total != listed ? " (the rest lifted by kiln.json \"preview\")" : "");
    for (const std::string& name : policy)
        KLOG_I("gate: %s stays held by the release policy (kiln.json \"preview\" does not lift it)", name.c_str());
}

std::vector<Held> held() {
    std::lock_guard lock(g_mutex);
    return held_locked();
}

bool is_held(std::string_view name) {
    std::lock_guard lock(g_mutex);
    const std::vector<Held>& h = held_locked();
    const auto it = std::lower_bound(h.begin(), h.end(), name, [](const Held& a, std::string_view n) { return a.name < n; });
    return it != h.end() && it->name == name;
}

}  // namespace kiln::gates
