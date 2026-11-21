-- SPDX-License-Identifier: MIT
-- Oracle-observed on both pinned SF release builds. Never invokes a known panic.
local widths = {"u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"}
local binary = {"add", "sub", "mul", "div", "rem", "bit_and", "bit_or", "bit_xor"}
local unary = {"count_ones", "count_zeros", "leading_ones", "leading_zeros", "trailing_ones", "trailing_zeros", "swap_bytes", "reverse_bits", "from_be", "from_le", "to_be", "to_le", "bit_not", "checked_neg", "wrapping_neg", "overflowing_neg"}
local shifts = {"shl", "shr", "checked_shl", "checked_shr", "wrapping_shl", "wrapping_shr", "overflowing_shl", "overflowing_shr", "rotate_left", "rotate_right"}
for _, name in ipairs(widths) do
    local a = integer[name]
    T.value(name .. '.constants', a.MIN, a.MAX, a.BITS)
    T.expect(name .. '.alias', rawequal(a, builtin.integer[name]), true)
    local names = {}
    for key, value in pairs(a) do if type(value) == 'function' then names[#names+1] = key end end
    table.sort(names)
    T.value(name .. '.exports', names)
    for _, key in ipairs(names) do
        -- u64 clamp's FLOAT path is valid, its INTEGER path panics for every value.
        if key == 'clamp' and name == 'u64' then T.call(name .. '.' .. key, a[key], 1.0)
        elseif key == 'parse' then T.call(name .. '.' .. key, a[key], '1')
        else T.call(name .. '.' .. key, a[key], 1, 1) end
    end
    if name:sub(1, 1) == 'u' then
        T.expect(name .. '.neg.absent', a.neg, nil)
        T.call(name .. '.neg.nil-call', function() return a.neg(1) end)
    else
        T.call(name .. '.neg.min', a.neg, a.MIN)
        T.expect(name .. '.neg.min.release', a.neg(a.MIN), a.MIN)
    end
    local values = {a.MIN, a.MAX, 0, 1}
    for i, value in ipairs(values) do
        for _, key in ipairs(unary) do T.call(name .. '.' .. key .. '.edge' .. i, a[key], value) end
        for _, key in ipairs(binary) do
            for j, rhs in ipairs(values) do
                if (key ~= 'div' and key ~= 'rem') or rhs ~= 0 then
                    T.call(name .. '.' .. key .. '.edge' .. i .. '.' .. j, a[key], value, rhs)
                end
            end
        end
    end
    for _, prefix in ipairs({'checked_', 'wrapping_', 'overflowing_', 'saturating_'}) do
        for _, op in ipairs({'add', 'sub', 'mul', 'div', 'pow'}) do
            local rhs = (op == 'pow' or op == 'mul') and 2 or 1
            T.call(name .. '.' .. prefix .. op .. '.max', a[prefix .. op], a.MAX, rhs)
            T.call(name .. '.' .. prefix .. op .. '.min', a[prefix .. op], a.MIN, rhs)
        end
        if prefix ~= 'saturating_' then
            T.call(name .. '.' .. prefix .. 'rem.max', a[prefix .. 'rem'], a.MAX, 2)
            T.call(name .. '.' .. prefix .. 'rem.min', a[prefix .. 'rem'], a.MIN, 2)
        end
    end
    for _, key in ipairs({'checked_add', 'checked_sub', 'checked_mul', 'checked_div', 'checked_rem', 'checked_pow'}) do
        T.call(name .. '.' .. key .. '.zero', a[key], 0, 0)
    end
    for _, key in ipairs(shifts) do
        for _, shift in ipairs({0, a.BITS-1, a.BITS, a.BITS+1, 4294967295}) do
            T.call(name .. '.' .. key .. '.shift' .. shift, a[key], a.MAX, shift)
        end
    end
    if name:sub(1,1) == 'i' then
        for _, key in ipairs({'div', 'rem', 'wrapping_div', 'wrapping_rem', 'overflowing_div', 'overflowing_rem', 'checked_div', 'checked_rem', 'saturating_div'}) do
            T.call(name .. '.' .. key .. '.min-neg-one', a[key], a.MIN, -1)
        end
    end
    T.expect(name .. '.checked-div.arity', select('#', a.checked_div(1, 0)), 1)
    T.expect(name .. '.checked-div.nil', a.checked_div(1, 0), nil)
    T.expect(name .. '.overflowing.arity', select('#', a.overflowing_add(1,1)), 2)
    T.expect(name .. '.base.arity', select('#', a.add(1,1)), 1)
    T.expect(name .. '.zero-neg', a.wrapping_neg(0), 0)
end
T.expect('u8.add.wrap', integer.u8.add(255,1), 0)
T.expect('i8.add.wrap', integer.i8.add(127,1), -128)
T.expect('u64.high.bits', math.type(integer.u64.add(-1,0)), 'integer')
T.expect('u64.high.format', integer.u64.to_string(-1), '18446744073709551615')
