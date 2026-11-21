-- SPDX-License-Identifier: MIT
-- Argument classes for EVERY function of EVERY integer type, per parameter position.
-- Both pins: modules/mod-runtime/lua/bindings/integer_operations.rs:72-226 (FunctionArgs::get::<T>
-- for CastLua parameters, raw Values for truncate/clamp/is_valid/to_string), arguments.rs:14-24
-- (missing = nil, extra arguments ignored), cast_values.rs:243-315 (integer/number/string casts),
-- cast_values.rs:83-95 (String), errors.rs:16-27,91-95 (diagnostic text).
-- Known panic inputs are NEVER run here (they live in support/linked/integer_safety.lua): a zero
-- divisor of div/rem/wrapping_div/wrapping_rem/overflowing_div/overflowing_rem/saturating_div,
-- detected with checked_div's own cast before the call, and every u64 clamp INTEGER value.
-- Each record is one (type, function, parameter): one line per argument class, "class=outcome".
-- Outcomes: "ok <n> <values>" or "e<k>" indexing the messages record (first line of the error,
-- addresses normalized as the prelude does), or "skip" for a known panic input.
local unary = {'count_ones', 'count_zeros', 'leading_ones', 'leading_zeros', 'trailing_ones',
    'trailing_zeros', 'swap_bytes', 'reverse_bits', 'from_be', 'from_le', 'to_be', 'to_le',
    'checked_neg', 'wrapping_neg', 'overflowing_neg', 'bit_not'}
local amount = {'rotate_left', 'rotate_right', 'checked_shl', 'checked_shr', 'checked_pow',
    'saturating_pow', 'wrapping_shl', 'wrapping_shr', 'wrapping_pow', 'overflowing_shl',
    'overflowing_shr', 'overflowing_pow', 'shl', 'shr', 'pow'}
local binary = {'checked_add', 'checked_sub', 'checked_mul', 'checked_div', 'checked_rem',
    'saturating_add', 'saturating_sub', 'saturating_mul', 'saturating_div', 'wrapping_add',
    'wrapping_sub', 'wrapping_mul', 'wrapping_div', 'wrapping_rem', 'overflowing_add',
    'overflowing_sub', 'overflowing_mul', 'overflowing_div', 'overflowing_rem', 'add', 'sub', 'mul',
    'div', 'rem', 'bit_and', 'bit_or', 'bit_xor'}
local raw = {'truncate', 'clamp', 'is_valid', 'to_string'}
local divides = {div = true, rem = true, wrapping_div = true, wrapping_rem = true,
    overflowing_div = true, overflowing_rem = true, saturating_div = true}

local MISSING = {}
local classes = {
    {'i3', 3}, {'i0', 0}, {'im1', -1}, {'i300', 300}, {'i70000', 70000}, {'i5e9', 5000000000},
    {'imax', math.maxinteger}, {'imin', math.mininteger},
    {'f3', 3.0}, {'fm1', -1.0}, {'fmz', -0.0}, {'f2.5', 2.5}, {'fm2.5', -2.5}, {'f300', 300.0},
    {'f2p63', 2.0^63}, {'f2p64', 2.0^64}, {'f1e100', 1e100}, {'fm1e100', -1e100},
    {'fnan', 0/0}, {'finf', math.huge}, {'fminf', -math.huge},
    {'s3', '3'}, {'sp3', '+3'}, {'sm1', '-1'}, {'s300', '300'}, {'sm0', '-0'}, {'s3.0', '3.0'},
    {'ssp3', ' 3'}, {'s3sp', '3 '}, {'s0x3', '0x3'}, {'s1e1', '1e1'}, {'sabc', 'abc'}, {'sempty', ''},
    {'sbytes', string.char(255)}, {'s2p64m1', '18446744073709551615'}, {'snan', 'NaN'},
    {'nil', nil}, {'missing', MISSING}, {'false', false}, {'true', true}, {'table', {}},
    {'function', function() end}, {'thread', coroutine.create(function() end)},
    {'buffer', buffer.create()},
}

local messages, message_index = {}, {}
local function first_line(err)
    local text = tostring(err)
    text = string.match(text, '^([^\n]*)')
    text = string.gsub(text, '0[xX]%x+', '<address>')
    local key = type(err) .. ' ' .. text
    local k = message_index[key]
    if not k then
        messages[#messages + 1] = key
        k = #messages
        message_index[key] = k
    end
    return 'e' .. k
end
local function render(v)
    local t = type(v)
    if t == 'number' then
        if math.type(v) == 'integer' then return string.format('%d', v) end
        return 'float'
    elseif t == 'string' then return string.format('%q', v)
    elseif t == 'boolean' or t == 'nil' then return tostring(v) end
    return t
end
local function outcome(f, args, n)
    local result = table.pack(pcall(f, table.unpack(args, 1, n)))
    if not result[1] then return first_line(result[2]) end
    local parts = {'ok', tostring(result.n - 1)}
    for i = 2, result.n do parts[#parts + 1] = render(result[i]) end
    return table.concat(parts, ' ')
end

for _, name in ipairs({'u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64'}) do
    local a = integer[name]
    local kinds = {}
    for _, key in ipairs(unary) do kinds[key] = {'T'} end
    if name:sub(1, 1) == 'i' then kinds.neg = {'T'} end
    for _, key in ipairs(amount) do kinds[key] = {'T', 'u32'} end
    for _, key in ipairs(binary) do kinds[key] = {'T', 'T'} end
    for _, key in ipairs(raw) do kinds[key] = {'raw'} end
    kinds.parse = {'string', 'radix'}
    -- The swept list is exactly the table's function set: nothing exported goes unswept.
    local names, swept = {}, {}
    for key, value in pairs(a) do
        if type(value) == 'function' then names[#names + 1] = key end
    end
    for key in pairs(kinds) do swept[#swept + 1] = key end
    table.sort(names)
    table.sort(swept)
    T.expect(name .. '.swept-all', table.concat(names, ','), table.concat(swept, ','))
    local default = {T = 7, u32 = 3, raw = 7, string = '7', radix = 10}
    for _, key in ipairs(names) do
        local f, params = a[key], kinds[key]
        local n = #params
        local defaults = {}
        for p = 1, n do defaults[p] = default[params[p]] end
        if n == 2 and params[2] == 'T' then defaults[2] = 3 end
        -- u64 clamp panics for every Lua integer, so its valid default is a float.
        if key == 'clamp' and name == 'u64' then defaults[1] = 7.0 end
        for p = 1, n do
            local lines = {}
            for _, class in ipairs(classes) do
                local label, value = class[1], class[2]
                local args, count = {}, n
                for q = 1, n do args[q] = defaults[q] end
                if value == MISSING then
                    count = p - 1
                    for q = p, n do args[q] = nil end
                else
                    args[p] = value
                end
                local result
                if value == MISSING and p < n then
                    result = nil -- a later argument cannot be present when this one is missing
                elseif divides[key] and p == 2 and value ~= MISSING then
                    local ok, quotient = pcall(a.checked_div, 7, value)
                    if ok and quotient == nil then result = 'skip' end
                elseif key == 'clamp' and name == 'u64' and math.type(value) == 'integer' then
                    result = 'skip'
                end
                if result ~= nil or not (value == MISSING and p < n) then
                    lines[#lines + 1] = label .. '=' .. (result or outcome(f, args, count))
                end
            end
            T.value(name .. '.' .. key .. '.' .. p, table.concat(lines, '\n'))
        end
        -- Extra trailing arguments are ignored; all-invalid arguments report the first one.
        local extra = {}
        for p = 1, n do extra[p] = defaults[p] end
        extra[n + 1], extra[n + 2], extra[n + 3] = 'extra', {}, false
        local bad = {}
        for p = 1, n do bad[p] = {} end
        T.value(name .. '.' .. key .. '.extra',
            outcome(f, extra, n + 3) .. '\n' .. outcome(f, {nil, nil, 1}, 3) .. '\n'
            .. outcome(f, bad, n))
    end
end
T.value('messages', table.concat(messages, '\n'))
