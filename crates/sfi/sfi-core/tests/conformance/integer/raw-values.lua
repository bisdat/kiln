-- SPDX-License-Identifier: MIT
-- integer_operations.rs:81-126 accepts raw Values, not CastLua coercions.
for _, name in ipairs({'u8','u16','u32','u64','i8','i16','i32','i64'}) do
    local a = integer[name]
    local integers = {a.MIN, a.MAX, 0, 1, -1, 256, -129, 65536, -32769, 4294967296, -2147483649}
    local floats = {0.0, 1.0, 1.5, -1.0, -1.5, 0/0, math.huge, -math.huge, 2.0^63, 2.0^64, 1e100, -1e100}
    for _, key in ipairs({'truncate','clamp','is_valid','to_string'}) do
        for i, value in ipairs(integers) do
            if not (name == 'u64' and key == 'clamp') then
                T.call(name .. '.' .. key .. '.integer' .. i, a[key], value)
            end
        end
        for i, value in ipairs(floats) do T.call(name .. '.' .. key .. '.float' .. i, a[key], value) end
        T.call(name .. '.' .. key .. '.missing', a[key])
        T.call(name .. '.' .. key .. '.nil', a[key], nil)
        T.call(name .. '.' .. key .. '.string', a[key], '1')
        T.call(name .. '.' .. key .. '.bytes', a[key], string.char(255))
        T.call(name .. '.' .. key .. '.boolean', a[key], false)
        T.call(name .. '.' .. key .. '.table', a[key], {})
    end
    T.expect(name .. '.truncate.arity', select('#', a.truncate(1)), 1)
    T.expect(name .. '.valid.arity', select('#', a.is_valid(1)), 1)
    T.expect(name .. '.string.arity', select('#', a.to_string(1)), 1)
end
T.expect('u64.integer-validity.always-false.zero', integer.u64.is_valid(0), false)
T.expect('u64.integer-validity.always-false.high', integer.u64.is_valid(-1), false)
T.expect('u64.float-validity.rounded-bound', integer.u64.is_valid(2.0^64), true)
T.expect('i64.float-validity.rounded-bound', integer.i64.is_valid(2.0^63), true)
T.expect('u64.float-clamp.rounded-bound', integer.u64.clamp(2.0^64), -1)
T.expect('u8.truncate.integer-wrap', integer.u8.truncate(256), 0)
T.expect('u8.truncate.float-saturate', integer.u8.truncate(256.0), 255)
T.expect('u8.truncate.nan', integer.u8.truncate(0/0), 0)
T.expect('u8.truncate.negative', integer.u8.truncate(-1), 255)
T.expect('u8.truncate.negative-float', integer.u8.truncate(-1.0), 0)
