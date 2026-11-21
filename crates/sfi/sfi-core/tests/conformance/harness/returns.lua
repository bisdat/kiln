-- SPDX-License-Identifier: MIT
T.call("multiple with trailing nil", function() return 1, nil, false, nil end)
T.call("none", function() end)
T.call("one nil", function() return nil end)
local ok, a, b = T.call("forwarded", function(a, b) return a, b end, "x", 4)
T.expect("pcall result", ok, true)
T.expect("first return", a, "x")
T.expect("second return", b, 4)
