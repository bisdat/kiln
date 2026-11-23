// SPDX-License-Identifier: MIT
// Portable Fisher body lowering. Never publishes a resource or changes a vanilla key. The boot adapter must
// validate body_layout() against the running type registry before calling lower_body, then apply TemplateEdit
// to the returned bytes. Cooked rig/slot/closure and material admission belong to that adapter, not this serializer.
#pragma once
#include <span>

#include "questbuild.h"

namespace kiln::qb {
struct BodyLayoutField {
    const char* name;
    const char* type;
    uint32_t offset;
};
struct BodyLayoutValue {
    const char* name;
    int64_t value;
};
struct BodyLayoutType {
    const char* name;
    uint32_t size;
    std::span<const BodyLayoutField> fields;
    const char* inner = nullptr;
    std::span<const BodyLayoutValue> values = {};
};
std::span<const BodyLayoutType> body_layout();
struct BodySpec {
    Guid model, graph;
    double radius = 0, height = 0;
};
// Failure leaves out and the input unchanged. Payloads remain at their original absolute offsets; only component
// variant headers move. Old headers and removed payloads remain unreachable bytes, not live components.
bool lower_body(const uint8_t* res, size_t len, const BodySpec& spec, Bytes* out, std::string* err);
}  // namespace kiln::qb
