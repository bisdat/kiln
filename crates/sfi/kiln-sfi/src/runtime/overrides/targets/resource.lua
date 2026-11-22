-- SPDX-License-Identifier: MIT
-- Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
-- Source (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/overrides/create_override_targets.lua:10-13 (scalar),
-- :199-232 (the resource target).
-- runtime.overrides.resource(resource, path): the override target of one reflected scalar field of a loaded resource, on
-- the same public checked readers and writers every mod has (game.assets.get_loaded_resource, read_loaded_field,
-- write_loaded_field with the snapshot's value as the expected one). normalize is the field codec's canonical value of
-- the snapshot's field type. Shared: a loaded resource outlives its world, so the tracker restores it in a later one.
return function(runtime, assets, normalize_type)
    local api = runtime.overrides
    local function scalar(value)
        return type(value) == "boolean" or type(value) == "string"
            or (type(value) == "number" and value == value and math.abs(value) < math.huge)
    end
    return function(resource, path)
        local claim, reason = api.get_resource_claim(resource)
        if not claim then return nil, reason end
        local guid, part, ty, session = resource.guid, resource.part, resource.type, resource.session_id
        return {
            key="resource/" .. guid .. "/" .. ty.qualified_name .. "/" .. part .. "/" .. path,
            claim=claim, shared=true,
            normalize=function(snapshot, value) return normalize_type(snapshot.type, value) end,
            read=function()
                if runtime.world.session_id() ~= session then return nil, "Resource target belongs to another world session" end
                local fresh, why = assets.get_loaded_resource(guid, ty, part)
                if not fresh then return nil, why end
                if fresh.session_id ~= session then return nil, "Resource snapshot belongs to another world session" end
                local value, failure, field_type = assets.read_loaded_field(fresh, path)
                if value == nil then return nil, failure end
                if not scalar(value) then return nil, "Resource field is not a finite scalar: " .. path end
                return {value=value, resource=fresh, type=field_type}
            end,
            write=function(snapshot, value)
                if runtime.world.session_id() ~= session or snapshot.resource.session_id ~= session then
                    return false, nil, "Resource target belongs to another world session"
                end
                local updated, failure = assets.write_loaded_field(snapshot.resource, path, value, snapshot.value)
                if not updated then return false, nil, failure end
                local actual, why = assets.read_loaded_field(updated, path)
                if actual == nil then return false, nil, "Uncertain resource write: " .. tostring(why) end
                return true, actual
            end,
        }
    end
end
