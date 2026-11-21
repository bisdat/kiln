-- SPDX-License-Identifier: MIT
-- Source-derived no-panic deviations, deliberately NOT run in the ordinary SF oracle.
for _, name in ipairs({'u8','u16','u32','u64','i8','i16','i32','i64'}) do
    local a = integer[name]
    for _, key in ipairs({'div','rem','wrapping_div','wrapping_rem','overflowing_div','overflowing_rem','saturating_div'}) do
        local ok, err = pcall(a[key], 1, 0)
        assert(not ok, name .. '.' .. key .. ' must reject zero')
        local cause = key:find('rem') and 'attempt to calculate the remainder with a divisor of zero' or 'attempt to divide by zero'
        assert(tostring(err):find(cause, 1, true), tostring(err))
        assert(a.add(1,2) == 3, 'worker remains usable after the error')
    end
    assert(a.checked_div(1,0) == nil)
    assert(a.checked_rem(1,0) == nil)
    if name:sub(1,1) == 'i' then
        -- Release SF wraps -MIN; Kiln must do so even in a debug host.
        assert(a.neg(a.MIN) == a.MIN)
        assert(a.div(a.MIN,-1) == a.MIN)
        assert(a.rem(a.MIN,-1) == 0)
        assert(a.saturating_div(a.MIN,-1) == a.MAX)
        -- Source-derived signed-shift cases supplement the release oracle matrix.
        for _, key in ipairs({'shr','wrapping_shr','checked_shr'}) do
            assert(a[key](a.MIN,1) == a.MIN // 2, name .. '.' .. key)
        end
        assert(a.shl(a.MIN,1) == 0)
        assert(a.rotate_left(a.MIN,1) == 1)
        assert(a.rotate_right(a.MIN,1) == 1 << (a.BITS-2))
    end
end
for _, value in ipairs({0,1,-1,math.maxinteger,math.mininteger}) do
    local ok, err = pcall(integer.u64.clamp, value)
    assert(not ok, 'every u64 integer clamp has reversed SF bounds')
    assert(tostring(err):find('min > max. min = 0, max = -1', 1, true), tostring(err))
end
assert(integer.u64.clamp(1.0) == 1)
assert(integer.i8.add(1,2) == 3)
