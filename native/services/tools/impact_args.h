// SPDX-License-Identifier: MIT
#pragma once
// Terraforming native schema: tools/impact/data/natives.json, types.json. NativeCtx: abilities/engine.h.
// Pure owned argument preparation; the engine adapter guards the source reads. No game memory is written.
#include "../abilities/engine.h"
#include "include/kiln_tools_policy.h"
#include <array>
#include <cmath>
#include <climits>
#include <vector>

namespace kt {
struct ImpactCopy {
    kab::NativeCtx context{};
    kab::ArgSlice arguments{};
    std::array<uint32_t, 9> handles{};
    std::vector<uint8_t> data;
    uint32_t actor{}, used_item{};
    double hit[3]{}, radius{};
};
inline bool copy_tool_impact(const kab::NativeCtx& source, const kab::ArgSlice& arguments,
                             uint32_t mode, ImpactCopy& out) {
    if ((mode != KILN_TOOL_POLICY_REMOVE && mode != KILN_TOOL_POLICY_ADD) || !source.layout ||
        !source.data || !source.layout_count || source.layout_count > 4096 || !source.data_size ||
        source.data_size > 65536 || !arguments.data || arguments.count != 9) return false;
    const auto* layout = reinterpret_cast<const kab::ImpactVariable*>(source.layout);
    auto variable = [&](uint32_t handle, uint32_t type, uint16_t size) -> const kab::ImpactVariable* {
        const uint32_t index = handle & 0xffff;
        if (!handle || index >= source.layout_count) return nullptr;
        const auto& v = layout[index];
        if (v.type != type || v.size != size || v.offset > source.data_size || size > source.data_size - v.offset)
            return nullptr;
        return &v;
    };
    const auto* root = variable(arguments.data[0], 0x737c8f86, 4);
    const auto* transform = variable(arguments.data[1], 0x9493a8c5, 56);
    const auto* type = variable(arguments.data[2], 0x4835c9e9, 1);
    const auto* damage = variable(arguments.data[3], 0xc4b5300b, 20);
    const auto* range = variable(arguments.data[5], 0xe89f7410, 4);
    if (mode == KILN_TOOL_POLICY_ADD && arguments.data[6])
        range = variable(arguments.data[6], 0xe89f7410, 4);
    if (!root || !transform || !type || !damage || !range || source.data[type->offset] != 0xff) return false;
    uint32_t actor=0, used=0;
    bool found_used=false;
    std::memcpy(&actor, source.data + root->offset, 4);
    if (!actor) return false;  // unowned impacts bypass vanilla player permissions; never permit them
    for (uint64_t i=0; i<source.layout_count; ++i) {
        const auto& v=layout[i];
        if (v.config_id != 0x220e6b10) continue; // UsedItem (ItemId), bound by the native sequence event
        if (found_used || v.type != 0x5bfc0dbb || v.size != 4 || v.offset > source.data_size || 4 > source.data_size-v.offset)
            return false;
        found_used=true;
        std::memcpy(&used, source.data+v.offset, 4);
    }
    if (!used) return false;
    float radius=0;
    std::memcpy(&radius, source.data+range->offset, 4);
    if (!std::isfinite(radius) || radius < 0.25f || radius > 8.0f) return false;
    int64_t fixed[3]{};
    std::memcpy(fixed, source.data+transform->offset, sizeof(fixed));
    for (int i=0;i<3;++i) {
        // TerraformImpact first narrows the fixed integer to float, then multiplies by 2^-32.
        out.hit[i] = static_cast<double>(static_cast<float>(fixed[i]) * 0x1p-32f);
        if (std::abs(out.hit[i]) > 1000000.0) return false;
    }
    // Reject aliasing of rewritten values with other inputs (even if a malformed program used overlapping variables).
    for (const auto* change : {type, damage}) {
        for (uint64_t i=0;i<source.layout_count;++i) {
            const auto* other=&layout[i];
            if (other==change) continue;
            if (change->offset < uint32_t(other->offset)+other->size && other->offset < uint32_t(change->offset)+change->size)
                return false;
        }
    }
    out.data.assign(source.data, source.data+source.data_size);
    out.context=source;
    out.context.data=out.data.data();
    std::copy(arguments.data,arguments.data+9,out.handles.begin());
    out.arguments={out.handles.data(),9};
    out.context.data[type->offset] = mode==KILN_TOOL_POLICY_REMOVE ? 0 : 1;
    // These are core-owned invocation values, not shared program constants or a mod's raw damage workaround.
    // Unbreakable materials still follow the engine's hardness refusal; filtering remains mandatory.
    const int32_t complete_damage[5]={INT32_MAX,INT32_MAX,INT32_MAX,INT32_MAX,INT32_MAX};
    std::memcpy(out.context.data+damage->offset,complete_damage,sizeof(complete_damage));
    out.actor=actor; out.used_item=used; out.radius=radius;
    return true;
}
} // namespace kt
