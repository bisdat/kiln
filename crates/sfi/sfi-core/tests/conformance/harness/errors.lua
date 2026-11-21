-- SPDX-License-Identifier: MIT
T.call("position", function() error("boom") end)
T.call("table error", function() error({code = 7}) end)
T.call("nil error", function() error(nil) end)
