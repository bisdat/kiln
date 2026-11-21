-- Kiln test probe for kiln mode (MIT): a ShroudForge-format mod that calls the entity, cursor, prop, attribute,
-- named service, loaded resource, voxel grid and building members of the mod API once each and logs what they
-- answer. Uses only the mod API (the 1.7.9 format), with its documented names and argument shapes.
-- One line per probe in kiln/logs/kiln.log (README.md lists them):
--   sf-kiln-probe: <probe> run=<n> ok=<true|false|skip> <key>=<value> ... [reason=<text>]
-- ok=false: the member refused (nil or false and a reason) or raised (reason=error: ...); ok=skip: the probe had
-- nothing to call it with this run. Every call is guarded, so one refusal never stops the probes after it.
-- The one write (a field of one component, then its old value back) runs only with the "Allow writes" setting on and
-- only on a run started with the shortcut or the button.

local ID = "sf-kiln-probe"
local VERSION = "0.1.0"
local TRANSFORM = "keen::ecs::CurrentTransform"
local PLAYER = "keen::ecs::PlayerInput"
local ITEM_INFO = "keen::ItemInfo"
local SERVICE = "ecs::GameSettings"
local SERVICE_FIELD = "enableDurability"
local Q = 4294967296 -- positions are Q32.32 fixed point
local F = table.pack

local state = {
    run = 0,
    elapsed = 0,
    cursor_elapsed = 0,
    first_update = true,
    queried_before = {}, -- per world session: the props query ran once before any recipe was registered
    item_bounds = nil, -- per world session: itemId -> {bounds, feedback} read from the loaded ItemInfo resources
    item_session = nil,
}

-- ---- formatting ----

local function word(value)
    local kind = type(value)
    if kind == "number" then
        if math.type(value) == "integer" then return string.format("%d", value) end
        if value ~= value then return "nan" end
        if value == math.huge then return "inf" end
        if value == -math.huge then return "-inf" end
        return string.format("%.4f", value)
    end
    local text = tostring(value):gsub("%s+", "_")
    if #text > 120 then text = text:sub(1, 117) .. "..." end
    return text
end

local function one_line(value)
    local text = tostring(value):gsub("[\r\n]+", " / ")
    if #text > 400 then text = text:sub(1, 397) .. "..." end
    return text
end

-- An error's first line (its message, without the traceback).
local function first_line(value)
    return one_line(tostring(value):match("^[^\r\n]*"))
end

-- One line: the probe id, the run, ok, the fields (pairs; nil values left out), then the reason as the line's end.
local function emit(probe, ok, fields, reason)
    local parts = { ID .. ":", probe, "run=" .. state.run, "ok=" .. tostring(ok) }
    if fields then
        for i = 1, (fields.n or #fields), 2 do
            if fields[i + 1] ~= nil then parts[#parts + 1] = fields[i] .. "=" .. word(fields[i + 1]) end
        end
    end
    if reason ~= nil then parts[#parts + 1] = "reason=" .. one_line(reason) end
    pcall(shroudforge.log.info, table.concat(parts, " "))
end

-- ---- guarded calls ----

local roots = { runtime = function() return runtime end, game = function() return game end,
    shroudforge = function() return shroudforge end }

-- The value at a dotted path ("runtime.ecs.query"), or nil when any part is missing.
local function member(path)
    local node
    for name in path:gmatch("[^.]+") do
        if node == nil then
            local root = roots[name]
            node = root and root()
        else
            local parent = node
            local ok, value = pcall(function() return parent[name] end)
            node = ok and value or nil
        end
        if node == nil then return nil end
    end
    return node
end

-- true and the member's results, or false and why it could not answer ("<path> is missing", "error: ...").
local function call(path, ...)
    local fn = member(path)
    if fn == nil then return false, path .. " is missing" end
    local results = table.pack(pcall(fn, ...))
    if not results[1] then return false, "error: " .. first_line(results[2]) end
    return true, table.unpack(results, 2, results.n)
end

-- A method call on a value the API gave (service:read_fields(...)).
local function method(object, name, ...)
    local ok, fn = pcall(function() return object[name] end)
    if not ok or fn == nil then return false, name .. " is missing" end
    local results = table.pack(pcall(fn, object, ...))
    if not results[1] then return false, "error: " .. first_line(results[2]) end
    return true, table.unpack(results, 2, results.n)
end

local function has(feature)
    local ok, answer = call("runtime.has", feature)
    if not ok then return "error" end
    return answer == true
end

local function get(object, key)
    if object == nil then return nil end
    local ok, value = pcall(function() return object[key] end)
    if ok then return value end
    return nil
end

local function setting(key, fallback)
    local ok, value = call("shroudforge.settings.get", key, fallback)
    if ok and value ~= nil then return value end
    return fallback
end

local function finite(value)
    return type(value) == "number" and value == value and value ~= math.huge and value ~= -math.huge
end

-- ---- reflected field paths ----

local SCALARS = { Bool = true, UInt8 = true, SInt8 = true, UInt16 = true, SInt16 = true, UInt32 = true,
    SInt32 = true, UInt64 = true, SInt64 = true, Float32 = true, Float64 = true }

-- Inline scalar leaves of a reflected type, in field order: { {path=, kind=} ... }, at most 32.
local function leaves(ty, prefix, depth, out)
    out = out or {}
    if ty == nil or depth > 4 or #out >= 32 then return out end
    local kind = get(ty, "primitive_type")
    local hops = 0
    while kind == "Typedef" and hops < 8 do
        ty = get(ty, "inner_type")
        kind = get(ty, "primitive_type")
        hops = hops + 1
    end
    if SCALARS[kind] then
        if prefix ~= "" then out[#out + 1] = { path = prefix, kind = kind } end
        return out
    end
    if kind ~= "Struct" then return out end
    local fields = get(ty, "struct_fields")
    if fields == nil then return out end
    local list = {}
    pcall(function()
        for name, field in pairs(fields) do
            list[#list + 1] = { name = get(field, "name") or name, type = get(field, "type"),
                offset = get(field, "data_offset") or 0 }
        end
    end)
    table.sort(list, function(a, b) return a.offset < b.offset end)
    for _, field in ipairs(list) do
        leaves(field.type, prefix == "" and field.name or (prefix .. "." .. field.name), depth + 1, out)
    end
    return out
end

-- The catalog row of a component (from this run's catalog), if any.
local function catalog_entry(ctx, name)
    for _, entry in ipairs(ctx.catalog_entries or {}) do
        if get(entry, "qualified_name") == name then return entry end
    end
    return nil
end

-- ---- the probes ----

local function probe_run(ctx)
    local ok_session, session = call("runtime.world.session_id")
    local ok_kind, kind = call("runtime.world.context_kind")
    ctx.session = ok_session and session or nil
    local key_ok, key = call("shroudforge.input.keybind_label", "probeKey")
    emit("run", true, F("via", ctx.via, "version", VERSION, "session", ctx.session, "context", ok_kind and kind or nil,
        "writes", setting("allowWrites", false) == true and "on" or "off", "key", key_ok and key or nil))
end

local function probe_catalog(ctx)
    local ok, catalog, why = call("runtime.ecs.get_catalog")
    if not ok then return emit("ecs.catalog", false, nil, catalog) end
    if type(catalog) ~= "table" then return emit("ecs.catalog", false, nil, why or "no catalog") end
    local entries = get(catalog, "entries") or {}
    ctx.catalog_entries = entries
    local writable = 0
    for _, entry in ipairs(entries) do
        if get(entry, "write_fields_available") == true then writable = writable + 1 end
    end
    local transform = catalog_entry(ctx, TRANSFORM)
    emit("ecs.catalog", true, F("entries", #entries, "registry", get(catalog, "registry_available"),
        "reads_ready", get(catalog, "entity_access_ready"), "writes_ready", get(catalog, "entity_writes_ready"),
        "writable", writable, "transform_read", get(transform, "read_fields_available"),
        "transform_write", get(transform, "write_fields_available")))
end

local function probe_query(ctx)
    local h = has("runtime.ecs.query")
    local ok, list, why = call("runtime.ecs.query", TRANSFORM)
    if not ok then return emit("ecs.query", false, F("has", h, "component", TRANSFORM), list) end
    if type(list) ~= "table" then return emit("ecs.query", false, F("has", h, "component", TRANSFORM), why) end
    ctx.entity = list[1]
    emit("ecs.query", true, F("has", h, "component", TRANSFORM, "count", #list, "first", list[1]))
end

local function probe_players(ctx)
    local ok, list, why = call("runtime.ecs.query", PLAYER)
    if not ok then return emit("ecs.players", false, F("component", PLAYER), list) end
    if type(list) ~= "table" then return emit("ecs.players", false, F("component", PLAYER), why) end
    ctx.player = list[1]
    emit("ecs.players", true, F("component", PLAYER, "count", #list, "first", list[1]))
end

-- x, y, z in metres from a CurrentTransform's bytes (three Q32.32 integers first).
local function decode_position(bytes)
    if type(bytes) ~= "string" or #bytes < 24 then return nil end
    local ok, x, y, z = pcall(string.unpack, "<i8i8i8", bytes)
    if not ok then return nil end
    return { x = x / Q, y = y / Q, z = z / Q }
end

local function probe_read_bytes(ctx)
    local h = has("runtime.ecs.read")
    if ctx.entity == nil then return emit("ecs.read_bytes", "skip", F("has", h), "no entity from runtime.ecs.query") end
    local ok, bytes, why = call("runtime.ecs.read_bytes", ctx.entity, TRANSFORM)
    if not ok then return emit("ecs.read_bytes", false, F("has", h, "entity", ctx.entity), bytes) end
    if type(bytes) ~= "string" then return emit("ecs.read_bytes", false, F("has", h, "entity", ctx.entity), why) end
    local at = decode_position(bytes) or {}
    emit("ecs.read_bytes", true, F("has", h, "entity", ctx.entity, "component", TRANSFORM, "size", #bytes,
        "expected", 56, "x", at.x, "y", at.y, "z", at.z))
end

local function probe_read_fields(ctx)
    if ctx.entity == nil then return emit("ecs.read_fields", "skip", nil, "no entity from runtime.ecs.query") end
    local paths = {}
    for _, leaf in ipairs(leaves(get(catalog_entry(ctx, TRANSFORM), "type"), "", 0)) do
        if #paths < 16 then paths[#paths + 1] = leaf.path end
    end
    if #paths == 0 then
        return emit("ecs.read_fields", "skip", F("entity", ctx.entity), "the catalog gave no field list for " .. TRANSFORM)
    end
    local ok, values, why = call("runtime.ecs.read_fields", ctx.entity, TRANSFORM, paths)
    if not ok then return emit("ecs.read_fields", false, F("entity", ctx.entity, "paths", #paths), values) end
    if type(values) ~= "table" then
        return emit("ecs.read_fields", false, F("entity", ctx.entity, "paths", #paths), why)
    end
    local shown, read = {}, 0
    for _, path in ipairs(paths) do
        local value = get(values, path)
        if value ~= nil then
            read = read + 1
            if #shown < 4 then shown[#shown + 1] = path .. ":" .. word(value) end
        end
    end
    emit("ecs.read_fields", true, F("entity", ctx.entity, "paths", #paths, "values", read,
        "first", table.concat(shown, ",")))
end

local function probe_template(ctx)
    if ctx.entity == nil then return emit("ecs.get_template", "skip", nil, "no entity from runtime.ecs.query") end
    local ok, template, why = call("runtime.ecs.get_template", ctx.entity)
    if not ok then return emit("ecs.get_template", false, F("entity", ctx.entity), template) end
    if template == nil then return emit("ecs.get_template", false, F("entity", ctx.entity), why) end
    emit("ecs.get_template", true, F("entity", ctx.entity, "guid", get(template, "guid"),
        "type", get(get(template, "type"), "qualified_name"), "source", get(template, "source")))
end

local function probe_cursor(ctx)
    local h = has("runtime.world.cursor.get")
    local ok, snapshot, why = call("runtime.world.cursor.get")
    if not ok then return emit("world.cursor", false, F("has", h), snapshot) end
    if type(snapshot) ~= "table" then return emit("world.cursor", false, F("has", h), why) end
    local value = get(snapshot, "value")
    local position = get(get(value, "primaryTransform"), "position")
    local x, y, z = tonumber(get(position, "x")), tonumber(get(position, "y")), tonumber(get(position, "z"))
    if x and y and z then
        ctx.cursor = { x = x / Q, y = y / Q, z = z / Q }
    end
    emit("world.cursor", true, F("has", h, "sequence", get(snapshot, "sequence"),
        "x", ctx.cursor and ctx.cursor.x, "y", ctx.cursor and ctx.cursor.y, "z", ctx.cursor and ctx.cursor.z,
        "attachedToProp", get(value, "attachedToProp"), "material", get(value, "material")))
end

-- The box around you: your player's position (its CurrentTransform), else the building cursor.
local function find_box(ctx)
    local half = tonumber(setting("boxHalfSize", 12)) or 12
    local center, from
    if ctx.player ~= nil then
        local ok, bytes = call("runtime.ecs.read_bytes", ctx.player, TRANSFORM)
        if ok then center = decode_position(bytes) end
        if center then from = "player" end
    end
    if center == nil and ctx.cursor then center, from = ctx.cursor, "cursor" end
    -- no position: a box at the origin, so the members still answer
    if center == nil then center, from = { x = 0, y = 0, z = 0 }, "origin" end
    ctx.center_from = from
    ctx.box = { center.x - half, center.y - half, center.z - half, center.x + half, center.y + half, center.z + half }
    return ctx.box
end

local function box_text(box)
    local parts = {}
    for i = 1, 6 do parts[i] = word(box[i]) end
    return table.concat(parts, ",")
end

local function inside(box, x, y, z)
    return finite(x) and finite(y) and finite(z) and x >= box[1] and x < box[4] and y >= box[2] and y < box[5]
        and z >= box[3] and z < box[6]
end

local function item_id(value)
    local kind = type(value)
    if kind == "table" or kind == "userdata" then value = get(value, "value") or get(value, "id") end
    return tonumber(value)
end

-- itemId -> {bounds, feedback} from the loaded ItemInfo resources (placement boxes), read once per world session
-- within a time budget; nil and why when they cannot be read.
local function item_bounds(ctx)
    if state.item_session == ctx.session and state.item_bounds then return state.item_bounds end
    local resources = ctx.item_infos
    if type(resources) ~= "table" then return nil, ctx.item_infos_reason or "no loaded ItemInfo resources" end
    local clock_ok, start = call("shroudforge.clock")
    local map, read = {}, 0
    for index, resource in ipairs(resources) do
        if clock_ok and index % 64 == 0 then
            local _, now = call("shroudforge.clock")
            if type(now) == "number" and now - start > 3 then break end
        end
        pcall(function()
            local data = resource.data
            local id = item_id(data.itemId)
            local equipment = data.equipment
            local minimum, maximum = equipment.placementAABBmin, equipment.placementAABBmax
            local bounds = { tonumber(minimum.x), tonumber(minimum.y), tonumber(minimum.z),
                tonumber(maximum.x), tonumber(maximum.y), tonumber(maximum.z) }
            for i = 1, 6 do if not finite(bounds[i]) then return end end
            for axis = 1, 3 do if bounds[axis] > bounds[axis + 3] then return end end
            if not id or id <= 0 or id % 1 ~= 0 then return end
            local feedback = 0
            for _, collider in ipairs(equipment.placementColliders or {}) do
                for _, entry in ipairs(collider.dataArray or {}) do
                    feedback = item_id(entry.value.materialFeedbackId) or 0
                    if feedback ~= 0 then break end
                end
                if feedback ~= 0 then break end
            end
            map[math.tointeger(id)] = { bounds = bounds, feedback = math.tointeger(feedback) or 0 }
        end)
        read = index
    end
    state.item_bounds, state.item_session, state.item_read = map, ctx.session, read
    return map
end

local function query_props_in_box(ctx, probe)
    local ok, props, why = call("runtime.world.entity.query_props_in_bounds", ctx.box)
    if not ok then emit(probe, false, F("box", box_text(ctx.box)), props) return nil end
    if type(props) ~= "table" then emit(probe, false, F("box", box_text(ctx.box)), why) return nil end
    return props
end

local function probe_props(ctx)
    local h = has("runtime.world.entity.query_props_in_bounds")
    find_box(ctx)
    -- once per world session, before any recipe: what the query answers without recipes
    local session_key = tostring(ctx.session)
    if not state.queried_before[session_key] then
        state.queried_before[session_key] = true
        local props = query_props_in_box(ctx, "world.props.before")
        if props then emit("world.props.before", true, F("box", box_text(ctx.box), "count", #props)) end
    end
    -- the props near you without recipes (their item ids)
    local nearby = {}
    local ok, near, why = call("runtime.world.entity.query_props", ctx.box, 1)
    if ok and type(near) == "table" then
        for _, prop in ipairs(near) do
            local id = math.tointeger(item_id(get(prop, "itemId")))
            if id and id > 0 then nearby[id] = true end
        end
        emit("world.props.nearby", true, F("box", box_text(ctx.box), "count", #near))
    else
        emit("world.props.nearby", false, F("box", box_text(ctx.box)), ok and why or near)
    end
    -- recipes: every ItemInfo placement box, and a 1 m box for a nearby item without one
    local map, map_why = item_bounds(ctx)
    local recipes, from_items, generic = {}, 0, 0
    for id, recipe in pairs(map or {}) do
        recipes[#recipes + 1] = { itemId = id, bounds = recipe.bounds, feedback = recipe.feedback }
        from_items = from_items + 1
    end
    for id in pairs(nearby) do
        if not (map and map[id]) then
            recipes[#recipes + 1] = { itemId = id, bounds = { -0.5, -0.5, -0.5, 0.5, 0.5, 0.5 }, feedback = 0 }
            generic = generic + 1
        end
    end
    local rok, registered, rwhy = call("runtime.world.entity.register_prop_recipes", recipes)
    local fields = F("has", has("runtime.world.entity.register_prop_recipes"), "recipes", #recipes,
        "from_iteminfo", from_items, "generic", generic)
    if not rok then emit("world.props.recipes", false, fields, registered)
    elseif registered ~= true then emit("world.props.recipes", false, fields, rwhy)
    else emit("world.props.recipes", true, fields, map == nil and ("ItemInfo boxes: " .. tostring(map_why)) or nil) end
    -- the query with the recipes
    local props = query_props_in_box(ctx, "world.props.query")
    if not props then return emit("world.props.transform", "skip", nil, "the props query gave no list") end
    emit("world.props.query", true, F("has", h, "center", ctx.center_from, "box", box_text(ctx.box), "count", #props))
    local first = props[1]
    if first == nil then
        return emit("world.props.transform", "skip", nil, "no prop in the box")
    end
    ctx.prop = first
    local th = has("runtime.world.entity.get_transform")
    local tok, prop, twhy = call("runtime.world.entity.get_transform", get(first, "handle"))
    if not tok then return emit("world.props.transform", false, F("has", th, "handle", get(first, "handle")), prop) end
    if type(prop) ~= "table" then
        return emit("world.props.transform", false, F("has", th, "handle", get(first, "handle")), twhy)
    end
    local position = get(get(prop, "transform"), "position")
    local x, y, z = tonumber(get(position, "x")), tonumber(get(position, "y")), tonumber(get(position, "z"))
    emit("world.props.transform", true, F("has", th, "handle", get(first, "handle"), "entityId", get(prop, "entityId"),
        "itemId", get(prop, "itemId"), "x", x, "y", y, "z", z, "inside", inside(ctx.box, x, y, z)))
end

local function probe_resolve(ctx)
    local h = has("runtime.ecs.resolve")
    local id = ctx.prop and tonumber(get(ctx.prop, "entityId"))
    if id == nil then return emit("ecs.resolve", "skip", F("has", h), "no prop entity id this run") end
    local ok, handle, why = call("runtime.ecs.resolve", math.tointeger(id) or id)
    if not ok then return emit("ecs.resolve", false, F("has", h, "entityId", id), handle) end
    if handle == nil then return emit("ecs.resolve", false, F("has", h, "entityId", id), why) end
    emit("ecs.resolve", true, F("has", h, "entityId", id, "handle", handle,
        "same", handle == get(ctx.prop, "handle")))
end

local function probe_attributes(ctx)
    local ok, list, why = call("runtime.ecs.get_attributes")
    if not ok then emit("attributes.list", false, nil, list) list = nil
    elseif type(list) ~= "table" then emit("attributes.list", false, nil, why) list = nil
    else
        emit("attributes.list", true, F("count", get(list, "count"), "entries", #(get(list, "entries") or {}),
            "version", get(list, "version")))
    end
    local h = has("runtime.ecs.read")
    if ctx.player == nil then return emit("attributes.read", "skip", F("has", h), "no player from runtime.ecs.query") end
    local entries = list and get(list, "entries") or {}
    if #entries == 0 then return emit("attributes.read", "skip", F("has", h), "no attribute listed") end
    local last
    for index = 1, math.min(#entries, 16) do
        local name = get(entries[index], "name")
        local rok, value, rwhy = call("runtime.ecs.read_attribute", ctx.player, name)
        if rok and value ~= nil then
            return emit("attributes.read", true, F("has", h, "entity", ctx.player, "name", name, "value", value,
                "tried", index))
        end
        last = rok and rwhy or value
    end
    emit("attributes.read", false, F("has", h, "entity", ctx.player, "tried", math.min(#entries, 16)), last)
end

local function probe_services(ctx)
    local h = has("runtime.ecs.read")
    local ok, service, why = call("runtime.services.get", SERVICE)
    if not ok or service == nil then
        emit("services.get", false, F("has", h, "key", SERVICE), ok and why or service)
        return emit("services.read_fields", "skip", F("key", SERVICE), "no service")
    end
    emit("services.get", true, F("has", h, "key", get(service, "key") or SERVICE,
        "type", get(get(service, "type"), "qualified_name"), "session", get(service, "session_id"),
        "write_supported", get(service, "write_supported")))
    local rok, values, rwhy = method(service, "read_fields", { SERVICE_FIELD })
    if not rok then return emit("services.read_fields", false, F("key", SERVICE), values) end
    if type(values) ~= "table" then return emit("services.read_fields", false, F("key", SERVICE), rwhy) end
    emit("services.read_fields", true, F("key", SERVICE, SERVICE_FIELD, get(values, SERVICE_FIELD)))
end

local function probe_resources(ctx)
    local ok, list, why = call("game.assets.get_loaded_resources_by_type", ITEM_INFO)
    if not ok then
        ctx.item_infos_reason = list
        emit("resources.by_type", false, F("type", ITEM_INFO), list)
    elseif type(list) ~= "table" then
        ctx.item_infos_reason = why or "no list"
        emit("resources.by_type", false, F("type", ITEM_INFO), why)
    else
        ctx.item_infos = list
        emit("resources.by_type", true, F("type", ITEM_INFO, "count", #list))
    end
    local first = type(list) == "table" and list[1] or nil
    if first == nil then return emit("resources.get", "skip", F("type", ITEM_INFO), "no loaded resource listed") end
    local guid, part = get(first, "guid"), get(first, "part")
    local gok, resource, gwhy = call("game.assets.get_loaded_resource", guid, ITEM_INFO, part)
    if not gok then return emit("resources.get", false, F("type", ITEM_INFO, "guid", guid), resource) end
    if resource == nil then
        return emit("resources.get", false, F("type", ITEM_INFO, "guid", guid), gwhy or "not loaded")
    end
    emit("resources.get", true, F("type", ITEM_INFO, "guid", get(resource, "guid"), "part", get(resource, "part"),
        "source", get(resource, "source"), "loaded", get(resource, "loaded"), "session", get(resource, "session_id")))
end

local function triple(value)
    if value == nil then return nil end
    local parts = {}
    for i, axis in ipairs({ "x", "y", "z" }) do
        parts[i] = word(get(value, axis) or get(value, i))
    end
    return table.concat(parts, ",")
end

local function probe_voxel(ctx)
    local h = has("runtime.world.voxel.grid_spec")
    local ok, spec, why = call("runtime.world.voxel.get_grid_spec", "voxel")
    if not ok then return emit("voxel.grid_spec", false, F("has", h), spec) end
    if type(spec) ~= "table" then return emit("voxel.grid_spec", false, F("has", h), why) end
    emit("voxel.grid_spec", true, F("has", h, "id", get(spec, "id"), "origin", triple(get(spec, "origin")),
        "cellSize", triple(get(spec, "cellSize")), "maximum", triple(get(spec, "maximum"))))
end

local function probe_building(ctx)
    -- never submits: the status of a request this mod never made
    local h = has("runtime.world.building.input")
    local ok, status = call("runtime.world.building.status", 1)
    if not ok then return emit("building.status", false, F("has", h), status) end
    emit("building.status", true, F("has", h, "request", 1, "status", status))
end

-- A changed value for one scalar field: a small step for numbers, the other value for a flag.
local function changed(kind, value)
    if kind == "Bool" then
        if type(value) ~= "boolean" then return nil end
        return not value
    end
    if type(value) ~= "number" then return nil end
    if kind == "Float32" or kind == "Float64" then
        if not finite(value) then return nil end
        if math.abs(value) >= 1 then return value * 1.001 end
        return value + 0.001
    end
    local integer = math.tointeger(value)
    if integer == nil then return nil end
    if integer > 0 then return integer - 1 end
    return integer + 1
end

local PREFERRED = { Float32 = 1, Float64 = 1, SInt32 = 2, UInt32 = 2, SInt16 = 2, UInt16 = 2, Bool = 3 }

-- One field of one entity's component: {component, entity, path, kind, before, after} or nil and why.
local function write_target(ctx)
    local wanted = setting("writeComponent", "")
    local names = {}
    if type(wanted) == "string" and wanted ~= "" then
        names[1] = wanted
    else
        for _, entry in ipairs(ctx.catalog_entries or {}) do
            if get(entry, "write_fields_available") == true and get(entry, "read_fields_available") == true then
                names[#names + 1] = get(entry, "qualified_name")
            end
        end
        table.sort(names)
    end
    if #names == 0 then return nil, "the catalog marks no component write_fields_available" end
    local last = "no candidate component has an entity and a plain field"
    for index = 1, math.min(#names, 32) do
        local name = names[index]
        local fields = leaves(get(catalog_entry(ctx, name), "type"), "", 0)
        table.sort(fields, function(a, b) return (PREFERRED[a.kind] or 9) < (PREFERRED[b.kind] or 9) end)
        local field = fields[1]
        if field and PREFERRED[field.kind] then
            local ok, list = call("runtime.ecs.query", name)
            local entity = ok and type(list) == "table" and list[1] or nil
            if entity ~= nil then
                local rok, values, why = call("runtime.ecs.read_fields", entity, name, { field.path })
                local before = rok and type(values) == "table" and get(values, field.path) or nil
                local after = changed(field.kind, before)
                if after ~= nil then
                    return { component = name, entity = entity, path = field.path, kind = field.kind,
                        before = before, after = after, tried = index }
                end
                last = name .. ": " .. tostring(rok and (why or "no readable value") or values)
            end
        end
    end
    return nil, last
end

local function probe_write(ctx)
    local h = has("runtime.ecs.write")
    if setting("allowWrites", false) ~= true then
        return emit("ecs.write_fields", "skip", F("has", h), "Allow writes is off")
    end
    if ctx.via == "timer" then
        return emit("ecs.write_fields", "skip", F("has", h), "writes run only on the shortcut or the button")
    end
    local target, why = write_target(ctx)
    if target == nil then return emit("ecs.write_fields", "skip", F("has", h), why) end
    local base = F("has", h, "component", target.component, "entity", target.entity, "field", target.path,
        "kind", target.kind)
    local function fields(extra)
        local out = F(table.unpack(base, 1, base.n))
        for i = 1, extra.n do out[out.n + i] = extra[i] end
        out.n = out.n + extra.n
        return out
    end
    local ok, written, reason, confirmed = call("runtime.ecs.write_fields", target.entity, target.component,
        { [target.path] = target.after }, { [target.path] = target.before })
    if not ok then return emit("ecs.write_fields", false, fields(F("before", target.before)), written) end
    local seen = get(confirmed, target.path)
    emit("ecs.write_fields", written == true, fields(F("before", target.before, "wrote", target.after,
        "confirmed", seen)), written ~= true and (reason or "refused") or nil)
    if written ~= true then return end
    -- the old value back, expecting the one just written
    local rok, restored, rreason, rconfirmed = call("runtime.ecs.write_fields", target.entity, target.component,
        { [target.path] = target.before }, { [target.path] = seen ~= nil and seen or target.after })
    if not rok then return emit("ecs.write_fields.restore", false, fields(F("value", target.before)), restored) end
    local back = get(rconfirmed, target.path)
    local same = back == target.before
    if not same and type(back) == "number" and type(target.before) == "number" then
        same = math.abs(back - target.before) <= 1e-6 * math.max(1, math.abs(target.before))
    end
    emit("ecs.write_fields.restore", restored == true, fields(F("value", target.before, "confirmed", back,
        "restored", same)), restored ~= true and (rreason or "refused") or nil)
end

local PROBES = {
    { "run", probe_run },
    { "ecs.catalog", probe_catalog },
    { "ecs.query", probe_query },
    { "ecs.players", probe_players },
    { "ecs.read_bytes", probe_read_bytes },
    { "ecs.read_fields", probe_read_fields },
    { "ecs.get_template", probe_template },
    { "world.cursor", probe_cursor },
    { "resources.by_type", probe_resources }, -- before the props: their recipes read the same list
    { "world.props.query", probe_props },
    { "ecs.resolve", probe_resolve },
    { "attributes.list", probe_attributes },
    { "services.get", probe_services },
    { "voxel.grid_spec", probe_voxel },
    { "building.status", probe_building },
    { "ecs.write_fields", probe_write },
}

local function run_all(via)
    state.run = state.run + 1
    local ctx = { via = via }
    for _, entry in ipairs(PROBES) do
        local ok, err = pcall(entry[2], ctx)
        if not ok then emit(entry[1], false, nil, "probe error: " .. first_line(err)) end
    end
    emit("done", true, F("via", via, "probes", #PROBES))
end

-- ---- lifecycle ----

local function on_load()
    local ok = call("shroudforge.ui.on_action", "runProbes", function() run_all("button") end)
    local key_ok, key = call("shroudforge.input.keybind_label", "probeKey")
    emit("loaded", true, F("version", VERSION, "button", ok, "key", key_ok and key or nil,
        "interval", setting("intervalSeconds", 30), "writes", setting("allowWrites", false) == true and "on" or "off"))
end

local function on_update(delta)
    delta = tonumber(delta) or 0
    local ok, pressed = call("shroudforge.input.was_keybind_pressed", "probeKey")
    if ok and pressed == true then
        state.elapsed = 0
        state.cursor_elapsed = 0
        return run_all("key")
    end
    state.elapsed = state.elapsed + delta
    state.cursor_elapsed = state.cursor_elapsed + delta
    local interval = math.max(5, tonumber(setting("intervalSeconds", 30)) or 30)
    if state.first_update or state.elapsed >= interval then
        state.first_update = false
        state.elapsed = 0
        state.cursor_elapsed = 0
        return run_all("timer")
    end
    local every = tonumber(setting("cursorSeconds", 1)) or 1
    if every > 0 and state.cursor_elapsed >= every then
        state.cursor_elapsed = 0
        local pok, err = pcall(probe_cursor, {})
        if not pok then emit("world.cursor", false, nil, "probe error: " .. first_line(err)) end
    end
end

return {
    update_interval_ms = 100,
    on_load = on_load,
    on_update = on_update,
}
