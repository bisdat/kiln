-- SPDX-License-Identifier: GPL-3.0-or-later
-- Identical old/candidate Brabb3l EML startup fixture, using the shared v2 prelude unchanged.
local function call(label, fn, ...)
    local r = table.pack(T.call(label, fn, ...))
    if not r[1] then io.export('raw/' .. label .. '.txt', tostring(r[2])) end
    return table.unpack(r, 1, r.n)
end
local a, types = game.assets, game.types
local root = types.get('m6.Root')
local r = a.get_resource('01234567-89ab-cdef-0123-456789abcdef', root)
local d = r.data
T.call('nil-false-integer-float-binary', function() return nil, false, 1, 1.0, -0.0, 'a\0b\255', nil end)
for _, field in ipairs({'name','qualified_name','impact_name','namespace','size','alignment',
    'element_alignment','field_count','primitive_type','name_hash','impact_hash','qualified_hash',
    'internal_hash','inner_type','struct_fields','enum_fields','attributes'}) do
    call('type-' .. field, function() return root[field] end)
end
call('type-unknown', function() return root.no_such_field end)
call('type-write-readonly', function() root.name = 'changed' end)
call('resource-unknown', function() return r.no_such_field end)
call('resource-write-unknown', function() r.no_such_field = 4 end)
for _, field in ipairs({'guid','part','type','data'}) do call('resource-' .. field, function() return r[field] end) end
call('types-hash', types.get, root.qualified_hash)
call('types-missing', types.get, 'm6.Missing')
call('types-get-all', function() return #types.get_all(), #types.get_all(false), #types.get_all(true) end)
call('types-of', function() return types.of(d) == root, types.of(false), types.of(d.child) end)
for _, label in ipairs({'missing','nil','boolean','string','float'}) do
    call('types-argument-' .. label, function()
        if label == 'missing' then return types.get() end
        if label == 'nil' then return types.get(nil) end
        if label == 'boolean' then return types.get(false) end
        if label == 'string' then return types.get('1') end
        return types.get(1.5)
    end)
end
call('invalid-guid', a.get_resource, 'not-a-guid', root)
call('resource-missing-part', a.create_resource, d.child, 'm6.Child', r.guid)
call('mapped-fields', function() return d.child.base, d.child.extra, d.fixed[1], d.maybe.base,
    d.choice.type, d.choice.value.extra, d.text, d.unknown end)
call('require-alias', function()
    local alias = require('alias')
    return alias.child == d.child, alias.children == d.children, require('alias') == alias,
        d == a.get_resource(r.guid, root).data
end)
call('mapped-new-field', function() d.child.no_such_field = 1 end)
call('mapped-invalid-field', function() d.child.extra = false end)
call('mapped-valid-field', function() d.child.extra = 101; return d.child.extra end)
local first = d.children[1]
call('mapped-insert', table.insert, d.children, {base=127, extra=131})
call('mapped-pairs-versus-length', function() local n=0; for _ in pairs(d.children) do n=n+1 end; return n,#d.children end)
call('mapped-remove-alias', function() local v=table.remove(d.children,1); return v==first,v.base,#d.children end)
call('mapped-remove-zero', table.remove, d.children, 0)
call('mapped-remove-past-end', table.remove, d.children, #d.children+2)
call('mapped-invalid-insert', table.insert, d.children, false)
call('static-insert', table.insert, d.fixed, 7)
call('static-remove', table.remove, d.fixed)
call('static-write', function() d.fixed[2] = 107; return d.fixed[2] end)
local created = a.create_resource(d.child, 'm6.Child', 'fedcba98-7654-3210-fedc-ba9876543210', 5)
call('owned-clone', function() return created.data~=d.child,created.data.extra end)
call('owned-write', function() created.data.extra=151; return created.data.extra,d.child.extra end)
call('owned-unknown', function() return created.data.unknown end)
call('owned-invalid-field', function() created.data.extra=false end)
call('duplicate-resource', a.create_resource, d.child, 'm6.Child', created.guid, 5)
local c = a.create_content(buffer.wrap('a\0b\255'))
call('content-new-lookup', function() return a.get_content(c.guid):read_data():to_string() end)
for _, field in ipairs({'guid','size','hash0','hash1','hash2','no_such_field'}) do
    call('content-'..field,function() return c[field] end)
end
call('content-read-arity', function() return c:read_data() end)
call('content-readonly', function() c:read_data():write_u8(1) end)
call('typeof-userdata', function() return typeof(root),typeof(r),typeof(d),typeof(d.children),typeof(c),typeof(c:read_data()) end)
call('buffer-numeric-conversion', function()
    local numeric=buffer.create()
    numeric:write_i64(9223372036854775807)
    numeric:write_i64(-9223372036854775807-1)
    numeric:write_f64(-0.0)
    numeric:write_f64(1.5)
    return numeric:read_i64(),numeric:read_i64(),numeric:read_f64(),numeric:read_f64()
end)
local b = buffer.wrap('a\0b\255')
call('buffer-string', function() return b:to_string() end)
call('buffer-unknown', function() return b.no_such_field end)
call('buffer-write-unknown', function() b.no_such_field=1 end)
call('buffer-method-invalid-receiver', b.read_u8, false)
call('buffer-method-missing', b.read_u8)
call('buffer-close', b.close, b)
call('buffer-closed-read', b.read_u8, b)
call('buffer-closed-write', b.write_u8, b, 1)
call('callback-values', function() return (function(f) return f(nil,false,1,1.0) end)(function(...) return ... end) end)
call('callback-error', function() return (function(f) return f() end)(function() error('literal callback failure',0) end) end)
call('metatable-struct-reader', function()
    created.data = setmetatable({}, {__index=function(_, key)
        T.value('struct-read-key',key)
        return key=='base' and 157 or 163
    end})
    return created.data.base,created.data.extra
end)
call('borrow-reentry', function()
    created.data = setmetatable({}, {__index=function(_, key)
        call('borrow-read-'..key,function() return created.data end)
        return key=='base' and 167 or 173
    end})
    return created.data.base,created.data.extra
end)
call('metatable-struct-error', function()
    created.data = setmetatable({}, {__index=function() error('literal lookup failure',0) end})
end)
T.call('completed',function() return true end)
