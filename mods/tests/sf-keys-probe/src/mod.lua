-- Kiln test probe for the keyboard shortcuts of ShroudForge-format mods in kiln mode (MIT). Uses only the mod API:
-- shroudforge.input on its keybind setting probeKey, shroudforge.log and shroudforge.notifications.
-- The client-pass check sfi-keybinds reads its lines in kiln/logs/kiln.log.
local presses, held, last_error = 0, false, nil

local function label()
    local ok, text = pcall(shroudforge.input.keybind_label, "probeKey")
    if not ok then return "error: " .. tostring(text) end
    return text or "none"
end

local function on_load()
    local ok, why = pcall(shroudforge.input.is_keybind_down, "notAKeybind")
    shroudforge.log.info("probe loaded: shortcut " .. label() .. "; undeclared: " .. (ok and "no error" or tostring(why)))
end

local function on_update()
    local ok, pressed = pcall(shroudforge.input.was_keybind_pressed, "probeKey")
    if not ok then
        if tostring(pressed) ~= last_error then
            last_error = tostring(pressed)
            shroudforge.log.warn("probe error: " .. last_error)
        end
        return
    end
    local down = shroudforge.input.is_keybind_down("probeKey") == true
    if pressed then
        presses = presses + 1
        local text = label() .. " pressed (" .. presses .. ")"
        shroudforge.log.info("probe pressed: " .. text)
        pcall(shroudforge.notifications.publish, { id = "press", title = "Keys probe", message = text })
    end
    if down ~= held then
        held = down
        shroudforge.log.info("probe " .. (down and "held: " or "released: ") .. label())
    end
end

return {
    update_interval_ms = 50,
    on_load = on_load,
    on_update = on_update,
}
