-- SPDX-License-Identifier: MIT
local open = buffer.create()
local closed = buffer.create(); closed:close()
T.call('unknown method', function() return open:unknown_method() end)
T.call('no len', function() return #open end)
T.call('no close metamethod', function() local x <close> = open end)
for _,spec in ipairs({
  {'bool', true, 0}, {'u8', 255, false}, {'i8', -128, false},
  {'u16', 65535, false}, {'i16', -32768, false}, {'u32', 4294967295, false},
  {'i32', -2147483648, false}, {'u64', -1, false}, {'i64', -1, false},
  {'f16', 1.25, false}, {'f32', 1.25, false}, {'f64', 1.25, false},
}) do
  local name, valid, invalid = spec[1], spec[2], spec[3]
  local write, read = 'write_'..name, 'read_'..name
  T.call(name..' empty read', function() return open[read](open, 'ignored') end)
  T.call(name..' closed read', function() return closed[read](closed) end)
  T.call(name..' bad write conversion', function() return closed[write](closed, invalid) end)
  T.call(name..' closed write', function() return closed[write](closed, valid) end)
  T.call(name..' bad receiver', function() return open[write](false, invalid) end)
  T.call(name..' write arity', function() return open[write](open, valid) end)
  T.call(name..' read arity', function() return open[read](open) end)
  T.expect(name..' head unchanged on failed reads', open:head(), open:tail())
end
T.call('bad head conversion', function() closed:head(false) end)
T.call('bad tail conversion', function() closed:tail(false) end)
T.call('bad reserve conversion', function() closed:reserve(false) end)
T.call('bad skip conversion', function() closed:skip(false) end)
T.call('bad read string conversion', function() closed:read_string(false) end)
T.call('bad write string conversion', function() closed:write_string(0) end)
T.call('source bounds precede readonly or closed destination', function() closed:copy(open, -1, 0) end)
T.call('closed destination valid source', function() closed:copy(open, 0, 0) end)
T.call('closed order valid setter', function() closed:order('little') end)
T.call('closed order getter', function() closed:order() end)
local bools = buffer.wrap('\0\1\128\255')
T.value('bool means nonzero', {bools:read_bool(), bools:read_bool(), bools:read_bool(), bools:read_bool()})
