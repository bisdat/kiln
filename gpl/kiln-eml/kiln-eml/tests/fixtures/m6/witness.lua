-- SPDX-License-Identifier: GPL-3.0-or-later
-- Current Brabb3l EML observations, NOT ShroudForge expectations or a trusted conformance VM.
-- FINAL eml-assets-types: game.types.get/get_all and assets resource/content visibility;
-- FINAL eml-pure: delegated typed table.insert/remove. Startup phase only.
local a, types = game.assets, game.types
local root = types.get("m6.Root")
assert(root.qualified_name == "m6.Root")
assert(types.get(root.qualified_hash) == root)
assert(types.get_by_qualified_name("m6.Root") == root)
assert(types.get(root.internal_hash) == nil)
assert(select("#", types.get(root.qualified_hash)) == 1)
assert(select("#", types.get("m6.Missing")) == 1)
assert(types.get("m6.Missing") == nil)
assert(types.get("m6.DsChildren") ~= nil)
for _, include in ipairs({false, true}) do
    local all = types.get_all(include) -- old EML ignores the argument and always excludes HAS_DS
    assert(#all == 9)
    for _, t in ipairs(all) do assert(t.qualified_name:sub(1, 5) ~= "m6.Ds") end
end
assert(#types.get_all() == 9)
assert(game.version == "m6-synthetic-v1")

local guid = "01234567-89ab-cdef-0123-456789abcdef"
local r = a.get_resource(guid, root)
assert(r.part == 0 and r.type == root)
assert(select("#", a.get_resource(guid, root)) == 1)
local parts = a.get_resource_parts(guid, root)
local part_set = {}
for _, p in ipairs(parts) do part_set[p.part] = true end
assert(#parts == 3 and part_set[0] and part_set[3] and part_set[9])
assert(#a.get_all_resources() == 3 and #a.get_resources_by_type(root) == 3)
local d = r.data
assert(d == a.get_resource(guid, root).data)
local alias = require("alias")
assert(alias.child == d.child and alias.children == d.children)
assert(require("alias") == alias)
assert(d.child.base == 7 and d.child.extra == 11)
assert(d.fixed[1] == 2 and d.fixed[2] == 3)
assert(d.maybe.base == 29 and d.choice.type == "m6.Child" and d.choice.value.extra == 41)
assert(d.text == "synthetic" and d.unknown == nil)

d.child.extra = 101
d.children[1].base = 103
d.fixed[2] = 107
d.maybe.extra = 109
d.choice.value.extra = 113
assert(alias.child.extra == 101 and alias.children[1].base == 103)
local first = d.children[1]
assert(select("#", table.insert(d.children, {base=127, extra=131})) == 1)
assert(#d.children == 3)
local pairs_count = 0
for _ in pairs(d.children) do pairs_count = pairs_count + 1 end
assert(pairs_count == 2) -- mapped pairs sees original length, # sees edited length
local detached = table.remove(d.children, 1)
assert(detached == first and detached.base == 103)
detached.extra = 139
assert(#d.children == 2 and d.children[1].base == 19)
assert(select("#", table.remove(d.children, 0)) == 1)
assert(table.remove(d.children, 0) == nil)

local created = a.create_resource(d.child, "m6.Child", "fedcba98-7654-3210-fedc-ba9876543210", 5)
assert(created.part == 5 and created.type == types.get("m6.Child"))
assert(created.data ~= d.child and created.data.extra == 101)
d.child.extra = 137
assert(created.data.extra == 101) -- clone on insertion, alias on repeated access
created.data.extra = 151 -- the old EML field/element validation fix must remain intact
assert(created.data == a.get_resource("fedcba98-7654-3210-fedc-ba9876543210", "m6.Child", 5).data)
created.data = d.child -- explicit Resource.data assignment also clones the typed child
assert(created.data ~= d.child and created.data.extra == 137)
created.data.extra = 151
assert(#a.get_all_resources() == 3 and #a.get_resources_by_type("m6.Child") == 0)

local c = a.create_content(buffer.wrap("m6 created content"))
assert(select("#", a.get_content(c.guid)) == 1)
assert(a.get_content(c.guid):read_data():to_string() == "m6 created content") -- unlike SF: new cache is visible
assert(c:read_data():to_string() == "m6 created content")
assert(a.create_content(buffer.wrap("m6 created content")).guid == c.guid)
assert(#a.get_all_contents() == 3) -- lists remain baseline-only
assert(select("#", c:read_data()) == 1)

-- Source-derived refusal/arity assertions, not new message goldens. Keep complete raw errors for the binding differential,
-- without a private normalizer/prelude or claiming shared-v2 conformance-message verification.
local function refuses(label, fragment, fn)
    local result = table.pack(pcall(fn))
    assert(result.n == 2 and result[1] == false)
    assert(type(result[2]) == "userdata")
    local text = tostring(result[2])
    assert(text:find(fragment, 1, true), text)
    io.export("errors/" .. label .. ".txt", text)
end
refuses("missing-type", "bad argument #1", function() types.get() end)
refuses("missing-part", "bad argument #4", function() a.create_resource(d.child, "m6.Child", guid) end)
-- Old EML's Guid cast returns no match, so FunctionArgs supplies a bad-argument diagnostic.
refuses("invalid-guid", "bad argument #1 (guid expected, got string)", function() a.get_resource("not-a-guid", root) end)
refuses("static-array", "attempt to modify static array", function() table.insert(d.fixed, 7) end)
refuses("invalid-field", "expected", function() d.child.extra = false end)
assert(d.child.extra == 137)
refuses("duplicate-resource", "already exists", function()
    a.create_resource(d.child, "m6.Child", "fedcba98-7654-3210-fedc-ba9876543210", 5)
end)
refuses("readonly-content", "not writable", function() c:read_data():write_u8(1) end)
assert(select("#", io.export("completed.txt", "m6 old-eml witness complete")) == 0)
