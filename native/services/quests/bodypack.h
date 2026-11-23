// SPDX-License-Identifier: MIT
// Portable Fisher body lowering. Never publishes a resource or changes a vanilla key. The boot adapter must
// validate body_layout() against the running type registry before calling lower_body, then apply TemplateEdit
// to the returned bytes. Cooked rig/slot/closure and material admission belong to that adapter, not this serializer.
#pragma once
#include <functional>
#include <span>
#include "nlohmann/json.hpp"

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
// What the running game's type registry says about one type (kiln.types on the game side, a fake on the host):
// size, the qualified name of its inner/base record ("" when none), own fields and enum/bitmask values.
struct BodyTypeFacts {
    struct Field { std::string name, type; uint64_t offset = 0; };
    uint32_t size = 0;
    std::string inner;
    std::vector<Field> fields;
    std::vector<std::pair<std::string, int64_t>> values;
};
using BodyTypeLookup = std::function<bool(const std::string& qualified_name, BodyTypeFacts* out)>;
// Every interpreted type, field (name, type, offset) and value of body_layout() must exist exactly in the running
// registry; the first difference refuses with the type's name. Uninterpreted fields are not compared.
bool check_body_layout(const BodyTypeLookup& lookup, std::string* why);
struct BodySpec {
    Guid model, graph;
    double radius = 0, height = 0;
};
// Failure leaves out and the input unchanged. Payloads remain at their original absolute offsets; only component
// variant headers move. Old headers and removed payloads remain unreachable bytes, not live components.
bool lower_body(const uint8_t* res, size_t len, const BodySpec& spec, Bytes* out, std::string* err);
// G2: renderer admission, separate from source-derived identity. Servers do not ask for a renderer or borrow
// author-side output hashes. read_source is bound to a contained, bounded mod-file reader by the boot adapter.
struct BodyMaterialContext {
    bool server = false;
    std::function<bool(const std::string& file, Bytes* bytes, std::string* why)> read_source;
    std::function<bool(nlohmann::ordered_json* report, std::string* why)> build_status;
    std::function<bool(const Guid& guid, uint32_t type)> served;
};
bool check_body_materials(const std::string& mod, const nlohmann::ordered_json& materials,
    const BodyMaterialContext& context, std::string* why);
}  // namespace kiln::qb
