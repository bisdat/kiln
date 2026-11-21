-- SPDX-License-Identifier: MIT
local keys = {}
for key in pairs(_G) do if key ~= "T" then keys[#keys + 1] = key end end
table.sort(keys)
T.value("sandbox.globals", keys)
for _, name in ipairs({"os", "debug", "package", "load", "dofile", "loadfile"}) do
    T.expect("sandbox.absent." .. name, _G[name], nil)
end
-- SF's bindings/write_mod_log.rs:23-35 installs its own warning function.
T.expect("sandbox.warn", type(warn), "function")
T.call("sandbox.warn.call", warn, "oracle warning")
T.call("sandbox.print.call", print, "oracle print")
T.expect("sandbox.self", _G._G == _G, true)
T.expect("builtin.io", builtin.io == io, true)
T.expect("typeof.table", typeof({}), "table")
T.expect("typeof.number", typeof(3), "number")
T.expect("require.cache_identity", require("cached") == require("cached"), true)
T.expect("require.qualified_identity", require("cached") == require("mods.oracle.cached"), true)
T.expect("require.owner", require("mods.oracle").owner, "oracle")
T.call("require.nil", require, "nil_value")
T.call("require.circular", require, "cycle")
T.call("require.circular.retry", require, "cycle")
T.call("require.missing", require, "missing")
T.call("require.missing.retry", require, "missing")
T.call("require.foreign", require, "mods.missing")
T.call("require.traversal", require, "../../outside")
local array = {1, 2}
T.call("table.insert", table.insert, array, 3)
T.expect("table.insert.result", array, {1, 2, 3})
T.call("table.remove", table.remove, array, 2)
T.expect("table.remove.result", array, {1, 3})
T.call("table.clear", table.clear, array)
T.expect("table.clear.result", array, {})
T.call("prelude.multireturn", function() return nil, 1, false, "\0\255" end)
T.call("prelude.string_error", function() error("sentinel", 0) end)
