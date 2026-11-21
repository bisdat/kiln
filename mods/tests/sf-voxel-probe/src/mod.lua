-- Kiln test probe for runtime.world.voxel of ShroudForge-format mods in kiln mode (MIT). Uses only the mod API:
-- runtime.world.voxel and shroudforge.log. It writes nothing to the world. The client-pass check sfi.voxel-grid reads
-- its lines in kiln/logs/kiln.log.
local last_grid, last_read, waited = nil, nil, 0

local function grid()
    local ok, spec, why = pcall(runtime.world.voxel.get_grid_spec, "voxel")
    if not ok then return "error: " .. tostring(spec) end
    if not spec then return "refused: " .. tostring(why) end
    return string.format("%s origin %g,%g,%g cell %g,%g,%g maximum %d,%d,%d", tostring(spec.id), spec.origin.x,
        spec.origin.y, spec.origin.z, spec.cellSize[1], spec.cellSize[2], spec.cellSize[3], spec.maximum.x,
        spec.maximum.y, spec.maximum.z)
end

local function read()
    local ok, cells, why = pcall(runtime.world.voxel.read, 0, 0, 0, 1, 1, 1)
    if not ok then return "error: " .. tostring(cells) end
    if cells then return "cells " .. #cells end
    return "refused: " .. tostring(why)
end

local function on_update(delta_ms)
    waited = waited + (tonumber(delta_ms) or 2000)
    if waited < 2000 then return end
    waited = 0
    local g, r = grid(), read()
    if g ~= last_grid then
        last_grid = g
        shroudforge.log.info("probe grid: " .. g)
    end
    if r ~= last_read then
        last_read = r
        shroudforge.log.info("probe read: " .. r)
    end
end

return {
    update_interval_ms = 500,
    on_load = function() shroudforge.log.info("probe loaded") end,
    on_update = on_update,
}
