-- SPDX-License-Identifier: MIT
T.value("nested sorted", {z = false, a = {true, "x"}})
local cyclic = {}; cyclic.self = cyclic
T.value("cycle", cyclic)
local shared = {n = 3}
T.value("shared not cyclic", {a = shared, b = shared})
T.value("metatable", setmetatable({}, {__tostring = function() return "ignored" end}))
