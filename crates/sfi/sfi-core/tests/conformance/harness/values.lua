-- SPDX-License-Identifier: MIT
T.value("nil", nil)
T.value("boolean", true)
T.value("function", function() end)
T.value("thread", coroutine.create(function() end))
T.value("bytes", "\0\255\"\\")
T.expect("different numeric types", 1.0, 1)
T.note("number note", 12)
