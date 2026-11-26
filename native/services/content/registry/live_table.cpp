// SPDX-License-Identifier: MIT
// kiln.registry.live v1's C table (live_table.h).
#include "live_table.h"

#include <algorithm>

#include "../registry_live.h"
#include "services/kiln_registry.h"

namespace kc::registry::stage {

namespace {

LiveTableEnv g_env;

uint64_t generation() { return g_env.generation ? g_env.generation() : 0; }

KilnResult live_owner(KilnOwner owner, std::string* mod = nullptr) { return bound_owner(owner, mod); }
bool live_out_ok(KilnRegistryLiveResult* out) { return !out || out->size >= sizeof(*out); }
void live_out(const RegistryLiveReply& reply, KilnRegistryLiveResult* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out)); out->size = sizeof(*out);
    out->outcome = static_cast<uint32_t>(reply.outcome); out->accepted = reply.accepted;
    out->refused = reply.refused; out->skipped = reply.skipped; out->world = reply.world;
    out->generation = generation();
    // cut on a UTF-8 boundary, always 0-terminated
    size_t n = std::min(reply.reason.size(), sizeof(out->reason) - 1);
    while (n && n < reply.reason.size() && (static_cast<unsigned char>(reply.reason[n]) & 0xc0) == 0x80) --n;
    std::memcpy(out->reason, reply.reason.data(), n);
}
KilnResult KILN_CALL l_begin(KilnOwner owner, uint32_t point, KilnHandle* out) {
    if (!out) return KILN_ERR_INVALID_ARGUMENT;
    std::string mod; const KilnResult rc = live_owner(owner, &mod);
    return rc == KILN_OK ? registry_live_begin(owner, mod, point, out) : rc;
}
KilnResult KILN_CALL l_submit(KilnOwner owner, KilnHandle handle, KilnStr ir, KilnRegistryLiveResult* out, char* buf, size_t cap, size_t* len) {
    if (!live_out_ok(out) || !str_ok(ir, KILN_REGISTRY_MAX_IR_BYTES) || (!buf && cap)) return KILN_ERR_INVALID_ARGUMENT;
    const KilnResult own = live_owner(owner); if (own != KILN_OK) return own;
    RegistryLiveReply reply; std::string text;
    const KilnResult rc = registry_live_submit(owner, handle, str(ir), &reply, &text);
    live_out(reply, out); report_out(text, buf, cap, len); return rc;
}
KilnResult KILN_CALL l_commit(KilnOwner owner, KilnHandle handle, KilnRegistryLiveResult* out) {
    if (!live_out_ok(out)) return KILN_ERR_INVALID_ARGUMENT;
    const KilnResult own = live_owner(owner); if (own != KILN_OK) return own;
    RegistryLiveReply reply; const KilnResult rc = registry_live_commit(owner, handle, &reply);
    live_out(reply, out); return rc;
}
KilnResult KILN_CALL l_revert(KilnOwner owner, KilnHandle handle, KilnRegistryLiveResult* out) {
    if (!live_out_ok(out)) return KILN_ERR_INVALID_ARGUMENT;
    const KilnResult own = live_owner(owner); if (own != KILN_OK) return own;
    RegistryLiveReply reply; const KilnResult rc = registry_live_revert(owner, handle, &reply);
    live_out(reply, out); return rc;
}
KilnResult KILN_CALL l_read(KilnOwner owner, KilnStr reg, KilnStr id, KilnStr field, char* buf, size_t cap, size_t* len) {
    if (!str_ok(reg, 1024) || !str_ok(id, 1024) || !str_ok(field, 1024) || (!buf && cap)) return KILN_ERR_INVALID_ARGUMENT;
    const KilnResult own = live_owner(owner); if (own != KILN_OK) return own;
    std::string text; const KilnResult rc = registry_live_read(str(reg), str(id), str(field), &text);
    return rc == KILN_OK ? copy_out(text, buf, cap, len) : rc;
}
KilnResult KILN_CALL l_status(KilnRegistryLiveStatus* out) {
    if (!out || out->size < sizeof(*out)) return KILN_ERR_INVALID_ARGUMENT;
    const auto status = registry_live_status();
    std::memset(out, 0, sizeof(*out)); out->size = sizeof(*out);
    out->world = status.world; out->world_kind = status.world_kind;
    out->generation = generation(); out->drafts = status.drafts; out->active = status.active;
    return KILN_OK;
}
const KilnRegistryLiveApi kLiveTable = {{sizeof(KilnRegistryLiveApi), KILN_REGISTRY_LIVE_VERSION},
    &l_begin, &l_submit, &l_commit, &l_revert, &l_read, &l_status};

}  // namespace

KilnResult bound_owner(KilnOwner owner, std::string* mod) {
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    bool revoked = false;
    const std::string id = g_env.bound_mod ? g_env.bound_mod(owner, &revoked) : std::string();
    if (revoked) return KILN_ERR_REVOKED;
    if (id.empty()) return KILN_ERR_INVALID_ARGUMENT;
    if (mod) *mod = id;
    return KILN_OK;
}

const KilnRegistryLiveApi* live_table(const LiveTableEnv& env) {
    g_env = env;
    return &kLiveTable;
}

}  // namespace kc::registry::stage
