-- SPDX-License-Identifier: MIT
-- Buffer precedence matrix: every ordinary method driven through its receiver, argument-conversion, state,
-- bounds and extra-argument classes, in the order the bindings check them. Results are compact strings:
-- "ok <arity> <values>" or "err <first line of the cause>" (userdata addresses replaced). Sizes stay small:
-- no value below converts to an allocation larger than 65536 bytes.
local fmt, byte, concat, pack, unpack, spack = string.format, string.byte, table.concat, table.pack, table.unpack, string.pack
local function hex(s) local o = {} for i = 1, #s do o[i] = fmt('%02x', byte(s, i)) end return concat(o) end
local function first_line(e) return (tostring(e):match('^[^\n]*'):gsub('0x%x+', '<address>')) end
local function show(v)
  local t = type(v)
  if t == 'number' then
    if math.type(v) == 'integer' then return fmt('%d', v) end
    if v ~= v then return 'nan' end
    return 'f' .. hex(spack('>d', v))
  elseif t == 'string' then return 's' .. hex(v)
  elseif t == 'nil' or t == 'boolean' then return tostring(v) end
  return t
end
local function run(f, ...)
  local r = pack(pcall(f, ...))
  if not r[1] then return 'err ' .. first_line(r[2]) end
  local out = { 'ok', tostring(r.n - 1) }
  for i = 2, r.n do out[#out + 1] = show(r[i]) end
  return concat(out, ' ')
end
local function emit(label, list) T.value(label, concat(list, '\n')) end
local function state(b)
  local ok, h, t, c = pcall(function() return b:head(), b:tail(), b:capacity() end)
  if not ok then return 'state ' .. first_line(h) end
  return fmt('state %d %d %d', h, t, c)
end
local function storage(b)
  local h, t = b:head(), b:tail()
  b:head(0); b:tail(b:capacity()); local s = b:to_string(); b:head(h); b:tail(t)
  if #s > 32 then return fmt('%d bytes %s..%s', #s, hex(s:sub(1, 8)), hex(s:sub(-8))) end
  return hex(s)
end

-- Argument classes. Every entry: label, n (argument count) and the argument values.
local V = {}
local function add(label, ...) V[#V + 1] = { label = label, n = select('#', ...), ... } end
add('missing'); add('nil', nil); add('false', false); add('true', true); add('table', {})
add('function', function() end); add('image', image.create(1, 1)); add('buffer', buffer.create())
for _, i in ipairs({ 0, 1, -1, 127, 128, -128, -129, 255, 256, 32767, 32768, -32768, -32769, 65535, 65536,
  2147483647, 2147483648, -2147483648, -2147483649, 4294967295, 4294967296, math.maxinteger, math.mininteger,
  math.mininteger + 1 }) do add(fmt('i:%d', i), i) end
for _, f in ipairs({ { '0.0', 0.0 }, { '-0.0', -0.0 }, { '1.0', 1.0 }, { '1.5', 1.5 }, { '-1.0', -1.0 },
  { '-1.5', -1.5 }, { '255.0', 255.0 }, { '256.0', 256.0 }, { '65504.0', 65504.0 }, { '65505.0', 65505.0 },
  { '65519.5', 65519.5 }, { '65520.0', 65520.0 }, { '16777217.0', 16777217.0 },
  { 'f32max', 3.4028234663852886e38 }, { 'f32max+ulp', 3.4028234663852890e38 }, { '1e39', 1e39 },
  { '2^31', 2.0 ^ 31 }, { '2^32', 2.0 ^ 32 }, { '2^53', 2.0 ^ 53 }, { '2^63', 2.0 ^ 63 }, { '-2^63', -2.0 ^ 63 },
  { '2^64', 2.0 ^ 64 }, { '1e300', 1e300 }, { '-1e300', -1e300 }, { '5e-324', 5e-324 }, { 'inf', math.huge },
  { '-inf', -math.huge }, { 'nan', 0 / 0 } }) do add('f:' .. f[1], f[2]) end
for _, s in ipairs({ '0', '1', '-1', '+1', ' 1', '1 ', '01', '1.0', '1.5', '1e2', '0x10', '', 'inf', '-inf', '+inf',
  'infinity', 'nan', 'NaN', '-nan', '255', '256', '-128', '-129', '65535', '65536', '4294967295', '4294967296',
  '9223372036854775807', '9223372036854775808', '-9223372036854775808', '-9223372036854775809',
  '18446744073709551615', '18446744073709551616', '65504', '65505', '65519', '65520', '3.4028235e38', '3.5e38',
  '1e39', '1e309', '\255', '1\0', 'true' }) do add('s:' .. fmt('%q', s), s) end
local function args(v) return unpack(v, 1, v.n) end

-- Values that would convert to a large usize; never used where they would allocate (create/reserve).
local large = {}
for _, l in ipairs({ 'i:-1', 'i:-128', 'i:-129', 'i:-32768', 'i:-32769', 'i:2147483647', 'i:2147483648',
  'i:-2147483648', 'i:-2147483649', 'i:4294967295', 'i:4294967296', 'i:' .. math.maxinteger, 'i:' .. math.mininteger,
  'i:' .. (math.mininteger + 1), 'f:16777217.0', 'f:f32max', 'f:f32max+ulp', 'f:1e39', 'f:2^31', 'f:2^32',
  'f:2^53', 'f:2^63', 'f:2^64', 'f:1e300', 's:"4294967295"', 's:"4294967296"', 's:"9223372036854775807"',
  's:"9223372036854775808"', 's:"18446744073709551615"' }) do large[l] = true end
-- Of those, the ones whose tail+value (tail 1) still lies inside isize::MAX and would really allocate.
local allocating = { ['i:2147483647'] = true, ['i:2147483648'] = true, ['i:4294967295'] = true,
  ['i:4294967296'] = true, ['f:16777217.0'] = true, ['f:2^31'] = true, ['f:2^32'] = true, ['f:2^53'] = true,
  ['s:"4294967295"'] = true, ['s:"4294967296"'] = true }

local proto = buffer.create()
local function method(name) return proto[name] end
local writes = { 'bool', 'u8', 'i8', 'u16', 'i16', 'u32', 'i32', 'u64', 'i64', 'f16', 'f32', 'f64' }
local reads = { 'bool', 'u8', 'i8', 'u16', 'i16', 'u32', 'i32', 'u64', 'i64', 'f16', 'f32', 'f64' }
local valid = { head = { 0 }, tail = { 0 }, reserve = { 0 }, skip = { 0 }, read_string = { 0 }, order = { 'little' },
  write_bool = { true }, write_string = { 'x' } }
for _, w in ipairs(writes) do
  if w ~= 'bool' then valid['write_' .. w] = { w:sub(1, 1) == 'f' and 1.5 or 1 } end
end
local methods = { 'head', 'tail', 'remaining', 'capacity', 'reset', 'reserve', 'order', 'skip', 'copy', 'close',
  'to_string', 'read_string', 'write_string' }
for _, r in ipairs(reads) do methods[#methods + 1] = 'read_' .. r end
for _, w in ipairs(writes) do methods[#methods + 1] = 'write_' .. w end

-- 1. Receivers: missing, every non-Buffer type and a foreign userdata, each with invalid arguments too.
local receivers = {}
for _, v in ipairs(V) do
  if v.label == 'missing' or v.label == 'nil' or v.label == 'false' or v.label == 'i:0' or v.label == 'f:1.5'
    or v.label == 's:"1"' or v.label == 'table' or v.label == 'function' or v.label == 'image' then
    receivers[#receivers + 1] = v
  end
end
for _, name in ipairs(methods) do
  local out = {}
  for _, r in ipairs(receivers) do
    if r.n == 0 then out[#out + 1] = r.label .. ': ' .. run(method(name))
    else out[#out + 1] = r.label .. ' bad args: ' .. run(method(name), r[1], false, false, false) end
  end
  emit('receiver ' .. name, out)
end
T.value('tostring is the to_string binding', { run(tostring, buffer.wrap('a\0\255')), run(tostring, buffer.create()) })

-- 2. Extra arguments are ignored by every method (state shown after the call).
for _, name in ipairs(methods) do
  local b = buffer.wrap('abcdefgh')
  local extra = { b }
  if name == 'copy' then extra[2] = buffer.wrap('xy'); extra[3] = 0; extra[4] = 1
  else for _, a in ipairs(valid[name] or {}) do extra[#extra + 1] = a end end
  extra[#extra + 1] = 'extra'; extra[#extra + 1] = false; extra[#extra + 1] = {}
  T.value('extra ' .. name, { run(method(name), unpack(extra)), state(b) })
end
T.value('head getter ignores later argument', { run(method('head'), buffer.wrap('ab'), nil, 1) })
T.value('order getter ignores later argument', { run(method('order'), buffer.wrap('ab'), nil, 'big') })

-- 3. Closed receiver: conversion first, then closed, before bounds/overflow/capacity.
local closed_args = { 'missing', 'false', 'i:-1', 'i:256', 'f:1.5', 's:' .. fmt('%q', '\255'), 'i:0', 'i:1', 'true',
  's:"1"' }
local closed_label = {}
for _, l in ipairs(closed_args) do closed_label[l] = true end
local by_label = {}
for _, v in ipairs(V) do by_label[v.label] = v end
for _, name in ipairs(methods) do
  local out = {}
  for _, l in ipairs(closed_args) do
    local b = buffer.wrap('abcdef'); b:close()
    out[#out + 1] = l .. ': ' .. run(method(name), b, args(by_label[l]))
  end
  local b = buffer.wrap('abcdef'); b:close()
  out[#out + 1] = 'little: ' .. run(method(name), b, 'little')
  out[#out + 1] = 'big: ' .. run(method(name), b, 'big')
  out[#out + 1] = 'string x: ' .. run(method(name), b, 'x')
  emit('closed ' .. name, out)
end

-- 4. Integer/float/string conversion of every write method, both orders, then the bytes written.
for _, w in ipairs(writes) do
  local out = {}
  for _, v in ipairs(V) do
    local b = buffer.create()
    local r = run(method('write_' .. w), b, args(v))
    if r:sub(1, 2) == 'ok' then
      local be = buffer.create(); be:order('big')
      local r2 = run(method('write_' .. w), be, args(v))
      r = r .. ' le=' .. hex(b:to_string()) .. ' be=' .. hex(be:to_string()) .. ' ' .. r2 .. ' ' .. state(b)
    end
    out[#out + 1] = v.label .. ': ' .. r
  end
  emit('write_' .. w .. ' conversions', out)
end
do
  local out = {}
  for _, v in ipairs(V) do
    local b = buffer.wrap('ab'); b:skip(1)
    local r = run(method('write_string'), b, args(v))
    out[#out + 1] = v.label .. ': ' .. r .. ' ' .. hex(b:to_string()) .. ' ' .. state(b)
  end
  emit('write_string conversions', out)
  out = {}
  for _, v in ipairs(V) do
    local r = pack(pcall(buffer.wrap, args(v)))
    local text = r[1] and ('ok ' .. (r.n - 1) .. ' ' .. hex(r[2]:to_string()) .. ' ' .. state(r[2]) .. ' ' .. r[2]:order())
      or ('err ' .. first_line(r[2]))
    out[#out + 1] = v.label .. ': ' .. text
  end
  emit('wrap conversions', out)
  out = {}
  for _, v in ipairs(V) do
    if not large[v.label] then
      local r = pack(pcall(buffer.create, args(v)))
      local text = r[1] and ('ok ' .. (r.n - 1) .. ' ' .. state(r[2]) .. ' ' .. storage(r[2]) .. ' ' .. r[2]:order())
        or ('err ' .. first_line(r[2]))
      out[#out + 1] = v.label .. ': ' .. text
    end
  end
  emit('create conversions', out)
  T.value('create called as a method', { run(function() return buffer:create() end) })
end

-- 5. usize arguments: the converted value shows in the bounds diagnostic or in the new cursor.
for _, name in ipairs({ 'head', 'tail', 'skip', 'read_string' }) do
  local out = {}
  for _, v in ipairs(V) do
    local b = buffer.wrap('abcdef')
    out[#out + 1] = v.label .. ': ' .. run(method(name), b, args(v)) .. ' ' .. state(b)
  end
  emit(name .. ' conversions', out)
end
do
  local out = {}
  for _, v in ipairs(V) do
    if not allocating[v.label] then
      local b = buffer.wrap('abc'); b:tail(1)
      out[#out + 1] = v.label .. ': ' .. run(method('reserve'), b, args(v)) .. ' ' .. state(b) .. ' ' .. storage(b)
    end
  end
  emit('reserve conversions', out)
  out = {}
  for _, v in ipairs(V) do
    local b = buffer.wrap('abcdef')
    local r = run(method('order'), b, args(v))
    out[#out + 1] = v.label .. ': ' .. r .. ' ' .. run(method('order'), b)
  end
  for _, s in ipairs({ 'big', 'little', 'Big', 'LITTLE', 'big ', 'little\0', 'default', 'native' }) do
    local b = buffer.wrap('abcdef')
    out[#out + 1] = fmt('%q', s) .. ': ' .. run(method('order'), b, s) .. ' ' .. run(method('order'), b)
  end
  emit('order conversions', out)
end

-- 6. copy: source, offset and length conversion (in that order) before any state check.
do
  local out = {}
  local srcs = { 'missing', 'nil', 'false', 'i:1', 's:"1"', 'table', 'image' }
  for _, l in ipairs(srcs) do
    local dst = buffer.wrap('XY'); dst:close()
    out[#out + 1] = 'src ' .. l .. ': ' .. run(method('copy'), dst, args(by_label[l]))
    out[#out + 1] = 'src ' .. l .. ' bad offset/length: ' .. run(method('copy'), dst, by_label[l][1], false, false)
  end
  for _, v in ipairs(V) do
    local dst, src = buffer.wrap('XY'), buffer.wrap('abcdef'); src:skip(1)
    out[#out + 1] = 'offset ' .. v.label .. ': ' .. run(method('copy'), dst, src, v[1], 0) .. ' ' .. hex(dst:to_string())
    if closed_label[v.label] then
      local dst2 = buffer.wrap('XY'); dst2:close()
      out[#out + 1] = 'offset ' .. v.label .. ' bad length closed dst: ' .. run(method('copy'), dst2, src, v[1], false)
    end
  end
  for _, v in ipairs(V) do
    local dst, src = buffer.wrap('XY'), buffer.wrap('abcdef'); src:skip(1)
    out[#out + 1] = 'length ' .. v.label .. ': ' .. run(method('copy'), dst, src, 1, v[1]) .. ' ' .. hex(dst:to_string())
  end
  emit('copy conversions', out)
  -- State scenarios x offset x length (cartesian), distinct and self copies.
  out = {}
  local offsets = { { 'missing', n = 0 }, { '0', 0, n = 1 }, { '1', 1, n = 1 }, { '5', 5, n = 1 }, { '-1', -1, n = 1 } }
  local lengths = { { 'missing', n = 0 }, { '0', 0, n = 1 }, { '2', 2, n = 1 }, { '5', 5, n = 1 }, { '-1', -1, n = 1 } }
  local function make(kind)
    local b = buffer.wrap('abcdef'); b:skip(1)
    if kind == 'closed' then b:close() elseif kind == 'inverted' then b:head(4); b:tail(2) end
    return b
  end
  local scenarios = {}
  for _, d in ipairs({ 'open', 'closed', 'inverted' }) do
    for _, s in ipairs({ 'open', 'closed', 'inverted' }) do scenarios[#scenarios + 1] = { d, s } end
    scenarios[#scenarios + 1] = { d, 'self' }
  end
  for _, sc in ipairs(scenarios) do
    for _, o in ipairs(offsets) do
      for _, l in ipairs(lengths) do
        local dst = make(sc[1])
        local src = sc[2] == 'self' and dst or make(sc[2])
        local count = l.n > 0 and 2 or o.n
        local r = run(method('copy'), dst, src, unpack({ o[2], l[2] }, 1, count))
        local ok, bytes = pcall(storage, dst)
        out[#out + 1] = fmt('%s<-%s offset %s length %s: %s dst %s %s src %s', sc[1], sc[2], o[1], l[1], r,
          state(dst), ok and bytes or first_line(bytes), state(src))
      end
    end
  end
  emit('copy states', out)
end

-- 7. Read decoding of every primitive, both orders, against fixed patterns; failures keep the cursor.
do
  local patterns = { '\0\0\0\0\0\0\0\0', '\255\255\255\255\255\255\255\255', '\128\0\0\0\0\0\0\0',
    '\0\0\0\0\0\0\0\128', '\1\2\3\4\5\6\7\8', '\127\255\255\255\255\255\255\127', '\0\124\0\0\128\127\240\127',
    '\0\252\0\0\128\255\240\255', '\1\0\0\0\0\0\0\0', '\0\126\1\0\192\127\248\127', '\255\123\255\127\255\255\239\127',
    '\0\60\0\0\128\63\240\63' }
  local out = {}
  for _, p in ipairs(patterns) do
    for _, o in ipairs({ 'little', 'big' }) do
      local row = { hex(p), o }
      for _, r in ipairs(reads) do
        local b = buffer.wrap(p); b:order(o)
        row[#row + 1] = r .. '=' .. run(method('read_' .. r), b) .. ' ' .. state(b)
      end
      out[#out + 1] = concat(row, ' ')
    end
  end
  emit('read decoding', out)
  out = {}
  for _, r in ipairs(reads) do
    local width = ({ bool = 1, u8 = 1, i8 = 1, u16 = 2, i16 = 2, f16 = 2, u32 = 4, i32 = 4, f32 = 4 })[r] or 8
    for _, setup in ipairs({ 'exact', 'short', 'inverted', 'empty' }) do
      local b = buffer.wrap(string.rep('\1', 10))
      if setup == 'exact' then b:head(10 - width) elseif setup == 'short' then b:head(11 - width)
      elseif setup == 'inverted' then b:head(6); b:tail(3) else b:reset() end
      out[#out + 1] = r .. ' ' .. setup .. ': ' .. run(method('read_' .. r), b, 'ignored') .. ' ' .. state(b)
    end
  end
  for _, n in ipairs({ 0, 1, 4 }) do
    local b = buffer.wrap('abcdef'); b:head(5); b:tail(3)
    out[#out + 1] = 'read_string inverted ' .. n .. ': ' .. run(method('read_string'), b, n) .. ' ' .. state(b)
    b = buffer.wrap('abcdef'); b:head(2)
    out[#out + 1] = 'read_string ' .. n .. ': ' .. run(method('read_string'), b, n) .. ' ' .. state(b)
    b = buffer.wrap('abcdef'); b:head(2)
    out[#out + 1] = 'skip ' .. n .. ': ' .. run(method('skip'), b, n) .. ' ' .. state(b)
    b = buffer.wrap('abcdef'); b:head(5); b:tail(3)
    out[#out + 1] = 'skip inverted ' .. n .. ': ' .. run(method('skip'), b, n) .. ' ' .. state(b)
  end
  emit('read bounds', out)
end

-- 8. Write state: writes land at tail (overwriting storage, even below head), grow exactly, keep head.
do
  local out = {}
  for _, w in ipairs(writes) do
    local value = (valid['write_' .. w] or { true })[1]
    local b = buffer.wrap('abcdefghijkl'); b:head(3); b:tail(1)
    local r = run(method('write_' .. w), b, value)
    out[#out + 1] = w .. ' overwrite: ' .. r .. ' ' .. state(b) .. ' ' .. storage(b)
    b = buffer.wrap('abc'); b:skip(1)
    r = run(method('write_' .. w), b, value)
    out[#out + 1] = w .. ' grow: ' .. r .. ' ' .. state(b) .. ' ' .. storage(b)
    b = buffer.create(20)
    r = run(method('write_' .. w), b, value)
    out[#out + 1] = w .. ' within capacity: ' .. r .. ' ' .. state(b) .. ' ' .. storage(b)
  end
  for _, s in ipairs({ '', 'Z', 'ZZZZ' }) do
    local b = buffer.wrap('abcdef'); b:head(4); b:tail(2)
    out[#out + 1] = 'string ' .. fmt('%q', s) .. ': ' .. run(method('write_string'), b, s) .. ' ' .. state(b) .. ' ' .. storage(b)
  end
  for _, n in ipairs({ 0, 1, 2, 5 }) do
    local b = buffer.wrap('abc'); b:tail(1)
    out[#out + 1] = 'reserve ' .. n .. ': ' .. run(method('reserve'), b, n) .. ' ' .. state(b) .. ' ' .. storage(b)
    b = buffer.wrap('abcdef'); b:head(5); b:tail(2)
    out[#out + 1] = 'reserve inverted ' .. n .. ': ' .. run(method('reserve'), b, n) .. ' ' .. state(b) .. ' ' .. storage(b)
  end
  local b = buffer.wrap('abcdef'); b:head(5); b:tail(2)
  out[#out + 1] = 'inverted getters: ' .. run(function() return b:remaining(), b:capacity(), b:to_string(), tostring(b) end)
  out[#out + 1] = 'inverted reset: ' .. run(method('reset'), b) .. ' ' .. state(b) .. ' ' .. storage(b)
  b = buffer.wrap('abcdef'); b:order('big')
  out[#out + 1] = 'order setter keeps bytes: ' .. run(method('order'), b, 'little') .. ' ' .. storage(b) .. ' ' .. b:order()
  b = buffer.wrap('abcdef'); b:order('big'); b:head(2)
  out[#out + 1] = 'reset keeps order and bytes: ' .. run(method('reset'), b) .. ' ' .. b:order() .. ' ' .. storage(b)
  b = buffer.wrap('abcdef')
  out[#out + 1] = 'close: ' .. run(method('close'), b) .. ' ' .. state(b) .. ' ' .. run(method('close'), b)
  emit('write and state', out)
end

-- 9. The Buffer userdata itself: no call/compare/arithmetic/concat/len/close/iteration metamethods.
do
  local a, b = buffer.wrap('a'), buffer.wrap('a')
  local out = {
    'type: ' .. type(a) .. ' ' .. typeof(a),
    'call: ' .. run(function() return a() end),
    'eq: ' .. run(function() return a == b, a == a, a ~= b, rawequal(a, a) end),
    'lt: ' .. run(function() return a < b end),
    'le: ' .. run(function() return a <= b end),
    'add: ' .. run(function() return a + 1 end),
    'unm: ' .. run(function() return -a end),
    'concat: ' .. run(function() return a .. 'x' end),
    'len: ' .. run(function() return #a end),
    'pairs: ' .. run(function() for _ in pairs(a) do end end),
    'ipairs: ' .. run(function() for _ in ipairs(a) do end end),
    'index integer: ' .. run(function() return a[1] end),
    'index __name: ' .. run(function() return a.__name end),
    'method identity: ' .. run(function() return a.head == b.head, type(a.head) end),
    'metatable: ' .. run(function() return getmetatable(a) end),
    'global Buffer: ' .. run(function() return type(Buffer) end),
  }
  emit('Buffer userdata', out)
end

-- 10. The buffer factory table: exactly create/wrap, plain mutable table aliased by builtin.buffer; rebinding the
-- global leaves the alias in place. Runs last because it mutates the namespace.
do
  local keys = {}
  for k, v in pairs(buffer) do keys[#keys + 1] = k .. '=' .. type(v) end
  table.sort(keys)
  T.value('buffer namespace', { concat(keys, ','), getmetatable(buffer) == nil, rawequal(buffer, builtin.buffer) })
  local original, create = buffer, buffer.create
  buffer.extra = 1; buffer.create = nil
  T.value('buffer namespace is mutable', { builtin.buffer.extra, builtin.buffer.create == nil, rawequal(builtin.buffer, original) })
  buffer.create = create; buffer.extra = nil
  buffer = nil
  T.value('global rebinding keeps builtin.buffer', { buffer == nil, rawequal(builtin.buffer, original),
    run(function() return builtin.buffer.create(2):capacity() end) })
  buffer = 'shadow'
  T.value('global shadowing keeps builtin.buffer', { buffer, rawequal(builtin.buffer, original) })
end
