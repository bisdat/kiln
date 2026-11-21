-- SPDX-License-Identifier: GPL-3.0-or-later
-- The pregame (startup) VM's runtime surface as ShroudForge's own mod runtime builds it in a local differential test (
-- pregame, server side, global EXPORT on and PATCH off, an EMPTY registry and archive, no game process, so no native
-- provider). Every runtime.* table and function of the pregame VM: the tables' keys, then each function's arguments,
-- refusal texts and result shapes. Left out: runtime.ecs.get_attributes/get_attribute/evaluate_attributes and the
-- attribute override target, which read the archive's attribute catalog (the oracle has no reader and panics there).
local function keys(t)
  if type(t) ~= "table" then return type(t) end
  local out = {}
  for k, v in pairs(t) do out[#out + 1] = tostring(k) .. ":" .. type(v) end
  table.sort(out)
  return table.concat(out, ",")
end
local function dump(v, depth)
  depth = depth or 0
  local kind = type(v)
  if kind == "string" then return string.format("%q", v) end
  if kind == "number" or kind == "boolean" or kind == "nil" then return tostring(v) end
  if kind ~= "table" then return kind end
  if depth > 3 then return "{...}" end
  local ks = {}
  for k in pairs(v) do ks[#ks + 1] = k end
  table.sort(ks, function(a, b) return tostring(a) < tostring(b) end)
  local parts = {}
  for _, k in ipairs(ks) do parts[#parts + 1] = tostring(k) .. "=" .. dump(v[k], depth + 1) end
  return "{" .. table.concat(parts, ",") .. "}"
end
-- A call whose results are recorded as one string (tables dumped, userdata and functions by type).
local function D(label, f, ...)
  T.call(label, function(...)
    local r = table.pack(f(...))
    local out = {}
    for i = 1, r.n do out[i] = dump(r[i]) end
    return table.concat(out, " | ")
  end, ...)
end

local rt = runtime
T.value("runtime", keys(rt))
for _, name in ipairs({"ecs", "impact", "services", "overrides", "world", "network", "patch", "functions", "values",
    "player"}) do
  T.value("keys." .. name, keys(rt[name]))
end
T.value("keys.world.voxel", keys(rt.world.voxel))
T.value("keys.world.cursor", keys(rt.world.cursor))
T.value("keys.world.entity", keys(rt.world.entity))
T.value("keys.world.building", keys(rt.world.building))
T.value("phase", rt.phase)

-- runtime.ecs
local ecs = rt.ecs
D("ecs.get_components", ecs.get_components)
D("ecs.get_catalog", ecs.get_catalog)
D("ecs.get_registry", ecs.get_registry)
D("ecs.get_component.name", ecs.get_component, "keen::ecs::Nope")
D("ecs.get_component.nil", ecs.get_component)
D("ecs.get_component.hash", ecs.get_component, 7)
D("ecs.query", ecs.query, "keen::ecs::Nope")
D("ecs.query.none", ecs.query)
D("ecs.query_bounds", ecs.query_bounds, {}, 1, "x")
D("ecs.resolve", ecs.resolve, 1)
D("ecs.resolve.none", ecs.resolve)
D("ecs.read", ecs.read, 1, "x")
D("ecs.read_bytes", ecs.read_bytes, 1, "x")
D("ecs.read_fields", ecs.read_fields, 1, "x", {})
D("ecs.write_fields", ecs.write_fields, 1, "x", {})
D("ecs.write", ecs.write, 1, "x", {})
D("ecs.get_template", ecs.get_template, 1)
D("ecs.get_template_component", ecs.get_template_component, 1, "x")
D("ecs.read_attributes", ecs.read_attributes, 1, "x")
D("ecs.update_attribute", ecs.update_attribute, 1, "x", 2)
D("ecs.read_attribute", ecs.read_attribute, 1, "x")
D("ecs.write_attribute_storage", ecs.write_attribute_storage, 1, "x", 2)

-- runtime.impact and runtime.services
D("impact.call", rt.impact.call, "keen::impact_nodes::IsSameEntity", {})
D("impact.call.none", rt.impact.call)
D("services.get", rt.services.get, "ecs::GameSettings")
D("services.get.none", rt.services.get)

-- runtime.world
local world = rt.world
D("world.operation_available", world.operation_available, "runtime.world.voxel.read")
D("world.operation_available.unknown", world.operation_available, "made.up")
D("world.operation_available.none", world.operation_available)
D("world.operation_available.number", world.operation_available, 5)
D("world.context_active", world.context_active)
D("world.context_kind", world.context_kind)
D("world.session_id", world.session_id)
D("world.voxel.read", world.voxel.read, 0, 0, 0, 1, 1, 1)
D("world.voxel.write", world.voxel.write, 0, 0, 0, 1, 1, 1, {})
D("world.voxel.get_grid_spec", world.voxel.get_grid_spec, "voxel")
D("world.cursor.get", world.cursor.get)
D("world.cursor.get.args", world.cursor.get, 1)
for _, name in ipairs({"query_props", "query_props_in_bounds", "register_prop_recipes", "get_transform", "set_scale",
    "spawn", "place", "destroy", "finish_building"}) do
  D("world.entity." .. name, world.entity[name])
end
D("world.entity.destroy.one", world.entity.destroy, 7)
D("world.building.submit", world.building.submit, {action = "place"})
D("world.building.submit.none", world.building.submit)
D("world.building.status", world.building.status, 1)
D("world.building.status.zero", world.building.status, 0)
D("world.building.status.text", world.building.status, "x")
D("world.building.cancel", world.building.cancel, 1)
D("world.building.cancel.none", world.building.cancel)

-- runtime.network
local net = rt.network
D("network.status", net.status)
D("network.send", net.send, "76561198000000000", "payload")
D("network.send.none", net.send)
D("network.send.options", net.send, "1", "p", 5)
D("network.send_mod", net.send_mod, "1", "other", "payload")
D("network.send_mod.none", net.send_mod, "1")
D("network.accept", net.accept, "1")
D("network.accept.none", net.accept)
D("network.connected_peers", net.connected_peers)
D("network.receive", net.receive)
D("network.receive.options", net.receive, {channel = 3})
D("network.receive.text", net.receive, "x")
D("network.receive_mod", net.receive_mod)
D("network.receive_mod.text", net.receive_mod, "x")

-- runtime.patch
D("patch.available", rt.patch.available, "speed")
D("patch.available.none", rt.patch.available)
D("patch.set_enabled", rt.patch.set_enabled, "speed", true)
D("patch.set_enabled.none", rt.patch.set_enabled)

-- runtime.functions
local fns = rt.functions
D("functions.get_operations", fns.get_operations)
D("functions.list", fns.list)
D("functions.list.page", fns.list, 0, 10)
D("functions.list.text", fns.list, "x")
D("functions.list_native", fns.list_native)
D("functions.find", fns.find)
D("functions.find.name", fns.find, {name = "x"})
D("functions.find.component", fns.find, {component = "keen::ecs::Nope"})
D("functions.find.text", fns.find, "x")
D("functions.get", fns.get, "runtime.ecs.query")
D("functions.get.none", fns.get)
D("functions.get_native", fns.get_native, 1)
D("functions.bind.operation", fns.bind, "runtime.ecs.query")
D("functions.bind.renamed", fns.bind, "runtime.world.context.active")
D("functions.bind.lifecycle", fns.bind, "runtime.lifecycle")
D("functions.bind.unknown", fns.bind, "made.up")
D("functions.bind.none", fns.bind)
D("functions.bind_modifier", fns.bind_modifier, "speed")
D("functions.bind_modifier.none", fns.bind_modifier)

-- runtime.values: no phase check; on the oracle's empty registry every selector is unknown
local values = rt.values
D("values.new", values.new, "keen::Nope")
D("values.new.hash", values.new, 5)
D("values.new.none", values.new)
D("values.decode", values.decode, "keen::Nope", "ab")
D("values.decode.number", values.decode, "keen::Nope", 5)
D("values.decode.none", values.decode, "keen::Nope")
D("values.encode", values.encode, "keen::Nope", 1)
D("values.encode.none", values.encode)
D("values.normalize_field", values.normalize_field, "keen::Nope", "a.b", 1)
D("values.normalize_field.none", values.normalize_field, "keen::Nope")

-- runtime.overrides: the tracker factory, the four claims and the five targets
local ov = rt.overrides
local tracker = ov.new()
T.value("tracker", keys(tracker))
tracker:begin_update()
D("tracker.apply", tracker.apply, tracker, "field", "setting", {
  claim = {acquire = function() return true end, release = function() end},
  read = function() return {value = 1} end,
  write = function(_, target) return true, target end,
}, function() return 2 end)
D("tracker.has_ownership", tracker.has_ownership, tracker)
D("tracker.summary", tracker.summary, tracker, "m")
D("tracker.restore_all", tracker.restore_all, tracker)
D("tracker.has_ownership.after", tracker.has_ownership, tracker)
D("tracker.reset", tracker.reset, tracker)
D("claim.component", ov.get_component_claim, 1, "keen::ecs::Nope")
D("claim.component.none", ov.get_component_claim)
D("claim.component.entity", ov.get_component_claim, "x", "keen::ecs::Nope")
D("claim.component.type", ov.get_component_claim, 1, {})
D("claim.resource", ov.get_resource_claim, {})
D("claim.resource.none", ov.get_resource_claim)
D("claim.service", ov.get_service_claim, {})
D("claim.service.two", ov.get_service_claim, {}, {})
D("claim.template", ov.get_template_claim, 1)
D("target.component", ov.component, 1, "keen::ecs::Nope", "a")
D("target.resource", ov.resource, {}, "a")
D("target.service", ov.service, {}, "a")
D("target.template", ov.template, {}, "a")

-- The the runtime registrar members a package reaches the same way
D("is_main_menu", rt.is_main_menu)
D("player.name", rt.player.name)
