-- SPDX-License-Identifier: MIT
-- SF CastLua first casts to i64/u64, then checks bounds (both pins cast_values.rs:243-315).
local bounds = {
    {'u8','0','255','-1','256'}, {'u16','0','65535','-1','65536'},
    {'u32','0','4294967295','-1','4294967296'},
    {'u64','0','18446744073709551615','-1','18446744073709551616'},
    {'i8','-128','127','-129','128'}, {'i16','-32768','32767','-32769','32768'},
    {'i32','-2147483648','2147483647','-2147483649','2147483648'},
    {'i64','-9223372036854775808','9223372036854775807','-9223372036854775809','9223372036854775808'},
}
for _, row in ipairs(bounds) do
    local name, a = row[1], integer[row[1]]
    for i = 2, 5 do
        local value = row[i]
        T.call(name .. '.add.string-bound' .. i, a.add, value, 0)
        T.call(name .. '.parse.bound' .. i, a.parse, value)
        T.call(name .. '.add.float-bound' .. i, a.add, tonumber(value) + 0.0, 0)
    end
    for i, value in ipairs({'1', '1.0', '+1', '-0', ' 1', '1 ', '0xff', '1_0', '', string.char(255)}) do
        T.call(name .. '.numeric-string' .. i, a.add, value, '1')
        T.call(name .. '.parse.string' .. i, a.parse, value, 16)
    end
    for i, value in ipairs({1, 1.0, 1.5, -1, -1.0, -1.5, 0/0, math.huge, -math.huge, 1e100, -1e100}) do
        T.call(name .. '.numeric' .. i, a.add, value, 0)
        T.call(name .. '.parse.numeric' .. i, a.parse, value)
    end
    for _, key in ipairs({'add', 'count_ones', 'parse', 'checked_shl'}) do
        T.call(name .. '.' .. key .. '.missing', a[key])
        T.call(name .. '.' .. key .. '.nil', a[key], nil)
        T.call(name .. '.' .. key .. '.boolean', a[key], false)
        T.call(name .. '.' .. key .. '.table', a[key], {})
        T.call(name .. '.' .. key .. '.function', a[key], function() end)
    end
    for _, key in ipairs({'add', 'checked_add', 'overflowing_add', 'wrapping_add', 'saturating_add', 'rotate_left', 'checked_shl'}) do
        T.call(name .. '.' .. key .. '.rhs-missing', a[key], 1)
        T.call(name .. '.' .. key .. '.rhs-nil', a[key], 1, nil)
        T.call(name .. '.' .. key .. '.rhs-type', a[key], 1, false)
        T.call(name .. '.' .. key .. '.precedence', a[key], false, false)
    end
    for _, value in ipairs({-1, -1.0, 4294967296, 4294967296.0, '4294967296', '-1', '1.0'}) do
        T.call(name .. '.shift-conversion.' .. tostring(value) .. '.' .. type(value), a.shl, 1, value)
    end
    for i, radix in ipairs({0, 1, 2, 16, 37, false, {}, 'bad', 0/0}) do
        T.call(name .. '.parse.ignored-radix' .. i, a.parse, '12', radix)
        T.expect(name .. '.parse.decimal' .. i, a.parse('12', radix), 12)
    end
    T.expect(name .. '.parse.nil-arity', select('#', a.parse('bad')), 1)
    T.expect(name .. '.parse.nil', a.parse('bad'), nil)
    T.expect(name .. '.ignores-trailing', a.add(1,2,nil,false), 3)
end
T.expect('unsigned-negative-float-saturates', integer.u8.add(-1.0, 0), 0)
T.expect('u64-negative-integer-reinterprets', integer.u64.add(-1, 0), -1)
T.expect('u64-negative-float-saturates', integer.u64.add(-1.0, 0), 0)
T.expect('i64-huge-integral-float-saturates', integer.i64.add(1e100,0), 9223372036854775807)
