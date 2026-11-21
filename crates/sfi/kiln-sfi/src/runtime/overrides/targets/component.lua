-- SPDX-License-Identifier: MIT
-- Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
-- Source (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/overrides/create_override_targets.lua:10-13 (scalar),
-- :234-260 (the component target).
-- runtime.overrides.component(entity, type, path, expected_paths): the override target of one reflected scalar field of
-- a component, on the same public checked readers and writers every mod has. normalize_field is the field codec's
-- canonical value (ShroudForge's runtime.values.normalize_field).
return function(runtime, normalize_field)
    local api = runtime.overrides
    local function scalar(value)
        return type(value) == "boolean" or type(value) == "string"
            or (type(value) == "number" and value == value and math.abs(value) < math.huge)
    end
    return function(entity, ty, path, expected_paths)
        local claim, reason = api.get_component_claim(entity, ty)
        if not claim then return nil, reason end
        local fields = {path}
        for _, extra in ipairs(expected_paths or {}) do
            if extra ~= path then fields[#fields + 1] = extra end
        end
        return {
            key="component/" .. runtime.world.session_id() .. "/" .. entity .. "/" .. tostring(ty) .. "/" .. path,
            claim=claim,
            normalize=function(_, value) return normalize_field(ty, path, value) end,
            read=function()
                local values, why = runtime.ecs.read_fields(entity, ty, fields)
                if not values then return nil, why end
                if not scalar(values[path]) then return nil, "Component field is not a finite scalar: " .. path end
                return {value=values[path], values=values}
            end,
            write=function(snapshot, value)
                local ok, why, confirmed = runtime.ecs.write_fields(entity, ty, {[path]=value}, snapshot.values)
                if not ok then return false, nil, why end
                if not confirmed or confirmed[path] == nil then
                    return false, nil, "Uncertain component write: confirmed scalar snapshot is unavailable"
                end
                return true, confirmed[path], nil, confirmed[path]
            end,
        }
    end
end
