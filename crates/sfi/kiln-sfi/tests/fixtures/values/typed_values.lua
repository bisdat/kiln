-- SPDX-License-Identifier: MIT
-- The typed-value differential: the same script over ShroudForge's port of its wrappers (in kiln-eml) and over
-- kiln-sfi's frontend on the kiln.eml.values provider must print the same lines (addresses and Rust TypeIds
-- normalized). ROOT is an owned or a mapped m6.Root; FOREIGN is a userdata neither side knows.
local out = {}
local function note(...)
  local t = {}
  for i = 1, select('#', ...) do t[#t + 1] = tostring((select(i, ...))) end
  out[#out + 1] = table.concat(t, " ")
end
local function try(label, f, ...)
  local ok, e = pcall(f, ...)
  if ok then note(label, "ok") else note(label, "error", (tostring(e):match("^[^\n]*"))) end
end
local function name(v) return type(v) end
local R = ROOT

note("types", name(R), name(R.child), name(R.fixed), name(R.children), name(R.maybe), name(R.choice), name(R.text))
note("metatable", getmetatable(R), getmetatable(R.child), getmetatable(R.choice))
note("reads", R.child.base, R.child.extra, R.fixed[1], R.fixed[2], #R.fixed, #R.children, #R.child, R.text, R.maybe.base)
note("missing", R.nope, R.child.nope, R.children[0], R.children[4], R.fixed[3], R.children[2.0].base)
note("arity", select('#', R.nope), select('#', R.children[9]), select('#', pairs(R)), select('#', R.child.base))
note("variant", R.choice.type, R.choice.value.base, R.choice.value.extra, name(R.choice.value))
try("variant-unknown-field", function() return R.choice.other end)

-- aliases and clones
local c1, c2 = R.child, R.child
note("alias", rawequal(c1, c2), rawequal(R.children[1], R.children[1]))
c1.base = 30
note("alias-edit", R.child.base, c2.base)
R.children[1] = R.child
R.child.base = 31
note("clone", R.children[1].base, R.child.base, rawequal(R.children[1], R.child))
R.choice.value = R.child
R.child.extra = 32
note("variant-clone", R.choice.value.base, R.choice.value.extra, R.choice.type)

-- assignments read through user metatables, each field once, in the type's order
local reads = {}
R.child = setmetatable({}, {__index = function(_, k) reads[#reads + 1] = k return ({base = 40, extra = 41})[k] end})
note("metatable-reads", table.concat(reads, ","), R.child.base, R.child.extra)
reads = {}
try("metatable-error", function()
  R.child = setmetatable({}, {__index = function(_, k) reads[#reads + 1] = k if k == "extra" then error("no extra") end return 1 end})
end)
note("metatable-error-reads", table.concat(reads, ","), R.child.base)

-- a static array's length is asked as often as Source asks it
local lens = 0
R.fixed = setmetatable({50, 51}, {__len = function() lens = lens + 1 return 2 end})
note("static-len", lens, R.fixed[1], R.fixed[2])
lens = 0
try("static-len-bad", function() R.fixed = setmetatable({52, 53, 54}, {__len = function() lens = lens + 1 return 3 end}) end)
note("static-len-bad-count", lens, R.fixed[1])

-- Source's errors (first lines)
try("invalid-property", function() R.nope = 1 end)
try("struct-int-key", function() R.child[1] = 1 end)
try("wrong-scalar", function() R.child.base = "x" end)
try("negative", function() R.child.base = -1 end)
try("float", function() R.child.base = 1.5 end)
try("table-for-scalar", function() R.child.base = {} end)
try("out-of-bounds", function() R.children[9] = {base = 1, extra = 2} end)
try("index-zero", function() R.children[0] = {base = 1, extra = 2} end)
try("missing-field", function() R.children[1] = {base = 1} end)
try("unknown-variant", function() R.choice = {type = "m6.Nope", value = {}} end)
try("variant-not-subtype", function() R.choice = {type = "m6.Root", value = {}} end)
try("variant-type-number", function() R.choice = {type = 5, value = {base = 1, extra = 2}} end)
try("wrong-class", function() R.children = R.child end)
try("static-array-length", function() R.fixed = {1} end)
try("array-key", function() return R.children.x end)
try("variant-len", function() return #R.choice end)
try("variant-type-set", function() R.choice.type = "m6.Child" end)
try("text-number", function() R.text = 12 end)
note("text", R.text)

-- iteration
local keys = {}
for k, v in pairs(R) do keys[#keys + 1] = k .. ":" .. name(v) end
note("pairs-root", table.concat(keys, ","))
local idx = {}
for i, v in pairs(R.children) do idx[#idx + 1] = i .. "=" .. v.base end
note("pairs-children", table.concat(idx, ","))
local fk = {}
for k, v in pairs(R.fixed) do fk[#fk + 1] = k .. "=" .. v end
note("pairs-fixed", table.concat(fk, ","))

-- typed table operations
local kids = R.children
note("insert", select('#', table.insert(kids, {base = 60, extra = 61})), #kids, kids[#kids].base)
table.insert(kids, 1, {base = 62, extra = 63})
note("insert-at", #kids, kids[1].base, kids[2].base)
try("insert-out-of-bounds", function() table.insert(kids, 99, {base = 1, extra = 1}) end)
try("insert-zero", function() table.insert(kids, 0, {base = 1, extra = 1}) end)
try("insert-invalid", function() table.insert(kids, {base = "bad", extra = 1}) end)
try("insert-arity", function() table.insert(kids) end)
local removed = table.remove(kids, 1)
note("remove", removed.base, #kids, select('#', table.remove(kids, 0)), table.remove(kids, 0), select('#', table.remove(kids, #kids + 1)))
try("remove-out-of-bounds", function() return table.remove(kids, #kids + 2) end)
removed.base = 64
note("removed-detached", removed.base, kids[1].base, name(removed))
removed = nil
collectgarbage("collect")
collectgarbage("collect")
local popped = table.remove(kids)
note("pop", popped.base, #kids)
try("static-insert", function() table.insert(R.fixed, 1) end)
try("static-pop", function() return table.remove(R.fixed) end)
try("static-clear", function() table.clear(R.fixed) end)
note("clear", select('#', table.clear(kids)), #kids, select('#', table.remove(kids)), table.remove(kids))
table.clear(kids)
try("not-an-array-insert", function() table.insert(R.child, 1) end)
try("not-an-array-remove", function() return table.remove(R.child) end)
try("not-an-array-clear", function() table.clear(R.child) end)
try("foreign-insert", function() table.insert(FOREIGN, 1) end)
try("foreign-clear", function() table.clear(FOREIGN) end)
try("remove-arity", function() return table.remove(kids, 1, 2) end)
-- a validation getter that reads the same array while table.insert holds it
try("reentrant-insert", function()
  table.insert(kids, setmetatable({}, {__index = function(_, k) return #kids end}))
end)
note("after-reentrant", #kids)

-- optionals
R.maybe = nil
note("maybe-nil", R.maybe, select('#', R.maybe))
R.maybe = {base = 70, extra = 71}
note("maybe", R.maybe.base, name(R.maybe))

-- texts of arrays (elements in order)
note("tostring", tostring(R.fixed))
return table.concat(out, "\n")
