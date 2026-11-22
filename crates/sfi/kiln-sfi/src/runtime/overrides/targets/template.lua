-- SPDX-License-Identifier: MIT
-- Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer)
-- Source (1.7.9 d8a394bc): modules/mod-runtime/lua/ingame/overrides/create_override_targets.lua:10-13 (scalar),
-- :171-205 (view_target), :267-269 (the template target).
-- runtime.overrides.template(view, path, expected_paths): the override target of one reflected scalar field of a
-- template component view (runtime.ecs.get_template_component), on the view's own checked reader and writer. normalize_field is the field codec's canonical value
-- (ShroudForge's runtime.values.normalize_field).
return function(runtime, normalize_field)
    local api = runtime.overrides
    local function scalar(value)
        return type(value) == "boolean" or type(value) == "string"
            or (type(value) == "number" and value == value and math.abs(value) < math.huge)
    end
    return function(view, path, expected_paths)
        local claim, reason = api.get_template_claim(view)
        if not claim then return nil, reason end
        local paths = {path}
        for _, extra in ipairs(expected_paths or {}) do
            if extra ~= path then paths[#paths + 1] = extra end
        end
        return {
            key="view/" .. view.session_id .. "/" .. view.key .. "/" .. path,
            claim=claim, shared=true,
            normalize=function(_, value) return normalize_field(view.type, path, value) end,
            read=function()
                local fields, failure = view:read_fields(paths)
                if not fields then return nil, failure end
                if not scalar(fields[path]) then return nil, "Field is not a finite scalar: " .. path end
                return {value=fields[path], values=fields}
            end,
            write=function(snapshot, value)
                local expected
                if expected_paths and #expected_paths > 0 then
                    expected = {}
                    for _, extra in ipairs(expected_paths) do expected[extra] = snapshot.values[extra] end
                end
                local ok, failure
                if expected then
                    ok, failure = view:write_field(path, value, snapshot.value, expected)
                else
                    ok, failure = view:write_field(path, value, snapshot.value)
                end
                if not ok then return false, nil, failure end
                -- write_field confirms the canonical value under the engine gate.
                return true, value
            end,
        }
    end
end
