-- SPDX-License-Identifier: MIT
-- The scalar differential (as typed_values.lua): GUIDs, object references, enums and numbers, including the texts
-- Source prints for a found userdata (its TypeId label, its {value:#?} name). SCALARS is an owned or a mapped
-- m6.Scalars, ROOT the m6.Root of the same storage, FOREIGN a userdata neither side knows.
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
local S = SCALARS

note("reads", S.u8, S.s8, S.u16, S.s16, S.s32, S.u32, S.u64, S.s64, S.f32, S.f64, S.b, S.color, S.guid, S.ref, S.alias)
try("guid-function", function() S.guid = function() end end)
try("guid-thread", function() S.guid = coroutine.create(function() end) end)
try("guid-foreign", function() S.guid = FOREIGN end)
try("guid-typed", function() S.guid = ROOT.child end)
try("guid-variant", function() S.guid = ROOT.choice end)
try("guid-bad", function() S.guid = "zz" end)
try("guid-number", function() S.guid = 5 end)
try("guid-table", function() S.guid = {} end)
try("ref-typed", function() S.ref = ROOT.child end)
try("ref-foreign", function() S.ref = FOREIGN end)
try("ref-bad", function() S.ref = "zz" end)
try("ref-number", function() S.ref = 5 end)
S.guid = "00000000-0000-0000-0000-0000000000ff"
S.ref = "00000000-0000-0000-0000-0000000000ee"
note("guid", S.guid, S.ref)
try("u8-big", function() S.u8 = 256 end)
try("s8-small", function() S.s8 = -129 end)
try("u64-negative", function() S.u64 = -1 end)
try("color-bad", function() S.color = "Purple" end)
try("color-number", function() S.color = 99 end)
S.color = "Green"
note("color", S.color)
S.color = 7
note("color-number", S.color)
try("flags-bad", function() S.flags = {"Z"} end)
try("b-number", function() S.b = 1 end)
S.f32 = 0.1
S.f64 = 0.1 + 0.2
note("floats", S.f32, S.f64, S.f32 == 0.1, S.f64 == 0.1 + 0.2)
S.s64 = math.mininteger
S.u64 = math.maxinteger
note("wide", S.s64, S.u64)
try("alias-negative", function() S.alias = -1 end)
S.alias = 4294967295
note("alias", S.alias)
return table.concat(out, "\n")
