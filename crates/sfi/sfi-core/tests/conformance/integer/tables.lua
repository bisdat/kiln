-- SPDX-License-Identifier: MIT
-- The `integer` table and its eight type tables as one package sees them.
-- Both pins: modules/mod-runtime/lua/bindings/mod.rs:66-103 (one integer::create per register call,
-- the same table as env.integer and builtin.integer), integer_operations.rs:3-17,36-69 (plain tables,
-- raw_set entries, no metatable). Mutation and rebinding follow ordinary Lua table/global rules.
local widths = {'u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64'}
local original = integer
local keys = {}
for key, value in pairs(integer) do keys[#keys + 1] = key .. ':' .. type(value) end
table.sort(keys)
T.value('integer.keys', table.concat(keys, ','))
T.expect('integer.alias', rawequal(integer, builtin.integer), true)
T.expect('integer.metatable', getmetatable(integer), nil)
T.expect('integer.length', #integer, 0)
local functions = {}
for _, name in ipairs(widths) do
    local a = integer[name]
    T.expect(name .. '.alias', rawequal(a, builtin.integer[name]), true)
    T.expect(name .. '.metatable', getmetatable(a), nil)
    T.expect(name .. '.length', #a, 0)
    local names = {}
    for key, value in pairs(a) do
        if type(value) == 'function' then names[#names + 1] = key end
    end
    table.sort(names) -- pairs order is not part of the contract
    for _, key in ipairs(names) do
        T.expect(name .. '.' .. key .. '.distinct', functions[a[key]], nil)
        functions[a[key]] = name .. '.' .. key
    end
end

-- Mutating a type table through one name is visible through the other; captured functions do not
-- depend on the table they were read from.
local add = integer.u8.add
integer.u8.add = nil
T.expect('u8.add.removed', builtin.integer.u8.add, nil)
T.expect('u8.add.captured', add(255, 1), 0)
integer.u8.add = function() return 'replaced' end
T.expect('u8.add.replaced', builtin.integer.u8.add(), 'replaced')
builtin.integer.u8.add = add
T.expect('u8.add.restored', integer.u8.add(255, 2), 1)
integer.u8.MAX = 1
T.expect('u8.MAX.writable', builtin.integer.u8.MAX, 1)
T.expect('u8.MAX.not-a-bound', integer.u8.add(255, 0), 255)
integer.u8.extra = 'x'
T.expect('u8.extra.visible', builtin.integer.u8.extra, 'x')
rawset(integer.u16, 'raw', 2)
T.expect('u16.rawset', builtin.integer.u16.raw, 2)

-- Mutating the root table: removing or replacing a type table, adding keys.
local u32 = integer.u32
integer.u32 = nil
T.expect('u32.removed', builtin.integer.u32, nil)
T.expect('u32.detached-works', u32.add(4294967295, 1), 0)
integer.u32 = {stand_in = true}
T.expect('u32.replaced', builtin.integer.u32.stand_in, true)
integer.u32 = u32
T.expect('u32.restored', rawequal(builtin.integer.u32, u32), true)
integer.fresh = 5
T.expect('root.add-key', builtin.integer.fresh, 5)

-- Rebinding a name is not an alias: the other name keeps the original table.
integer = {}
T.expect('global.rebound', rawequal(builtin.integer, original), true)
T.expect('global.rebound.works', builtin.integer.i8.add(127, 1), -128)
integer = original
builtin.integer = nil
T.expect('builtin.rebound', rawequal(integer, original), true)
T.expect('builtin.rebound.works', integer.i8.add(-128, -1), 127)
builtin.integer = 'other'
T.expect('builtin.rebound.value', integer == original and builtin.integer, 'other')
builtin.integer = original
T.expect('both.restored', rawequal(builtin.integer, integer), true)
