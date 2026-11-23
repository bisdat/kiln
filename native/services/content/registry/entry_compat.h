// SPDX-License-Identifier: MIT
#pragma once
#include "ir.h"
#include <functional>
#include <optional>
namespace kc::registry {
// One install-wide record, outside the per-build caches and saves. Caller serializes read/atomic replacement across processes.
bool pin_entry(const std::optional<std::string>& stored, const std::string& mod, const std::string& id,
               const std::string& registry, const std::string& guid, const std::string& base_guid,
               const std::function<bool(const std::string&)>& write, std::string* why);
}
