-- SPDX-License-Identifier: GPL-3.0-or-later
-- game.assets on an EMPTY registry and archive without PATCH, the only state ShroudForge's own mod runtime can be
-- given in a local differential test (not published). Argument casts, raised texts, single-nil results, the
-- PATCH refusal and its order after the argument and Buffer checks are archive-independent; the archive-backed
-- behaviour is covered by source-derived host tests (m6_assets.rs).
local a = game.assets
local G = "01234567-89ab-cdef-0123-456789abcdef"
for _, name in ipairs({"get_resource", "get_resource_parts", "get_resources_by_type", "get_all_resources",
    "get_resource_types", "get_content", "get_all_contents", "create_content"}) do
  T.value("type." .. name, type(a[name]))
end
local function count(label, f, ...)
  local ok, r = pcall(f, ...)
  T.value(label, ok and (type(r) .. "#" .. #r .. (getmetatable(r) == nil and ":plain" or ":meta")) or "error")
end
count("get_all_resources", a.get_all_resources)
count("get_all_resources.args", a.get_all_resources, 1, {}, "x")
count("get_resource_types", a.get_resource_types)
count("get_all_contents", a.get_all_contents, true)
T.call("get_resource.nil", a.get_resource)
T.call("get_resource.integer", a.get_resource, 12, "m6.Root")
T.call("get_resource.float", a.get_resource, 1.5, "m6.Root")
T.call("get_resource.bad", a.get_resource, "not-a-guid", "m6.Root")
T.call("get_resource.short", a.get_resource, "01234567-89ab", "m6.Root")
T.call("get_resource.table", a.get_resource, {}, "m6.Root")
T.call("get_resource.notype", a.get_resource, G)
T.call("get_resource.missing", a.get_resource, G, "m6.Root")
T.call("get_resource.number_type", a.get_resource, G, 42)
T.call("get_resource.bool_type", a.get_resource, G, true)
T.call("get_resource.table_type", a.get_resource, G, {})
T.call("get_resource.bad_part", a.get_resource, G, "m6.Root", -1)
T.call("get_resource_parts.nil", a.get_resource_parts)
T.call("get_resource_parts.notype", a.get_resource_parts, G)
T.call("get_resource_parts.missing", a.get_resource_parts, G, "m6.Root")
T.call("get_resources_by_type.nil", a.get_resources_by_type)
T.call("get_resources_by_type.missing", a.get_resources_by_type, "m6.Root")
T.call("get_resources_by_type.float", a.get_resources_by_type, 1.5)
T.call("get_content.nil", a.get_content)
T.call("get_content.bool", a.get_content, true)
T.call("get_content.bad", a.get_content, "zz")
T.call("get_content.guid", a.get_content, G)
T.call("get_content.partial", a.get_content, {size = 1})
T.call("get_content.words", a.get_content, {size = 1, hash0 = 2, hash1 = 3, hash2 = 4})
T.call("get_content.string_words", a.get_content, {size = "1", hash0 = "2", hash1 = 3, hash2 = 4})
T.call("get_content.negative", a.get_content, {size = -1, hash0 = 2, hash1 = 3, hash2 = 4})
T.call("get_content.throws", a.get_content, setmetatable({}, {__index = function(_, k) error("boom " .. k, 0) end}))
T.call("create_content.nil", a.create_content)
T.call("create_content.string", a.create_content, "x")
T.call("create_content.unpatched", a.create_content, buffer.wrap("x"))
local closed = buffer.wrap("x")
closed:close()
T.call("create_content.closed", a.create_content, closed)
