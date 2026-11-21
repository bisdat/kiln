-- SPDX-License-Identifier: MIT
-- The conformance prelude shared by Kiln's interpreter (crates/sfi) and ShroudForge's own mod runtime (in a local
-- differential test, not published). Both engines load this same file before a fixture, so a difference between two transcripts is a
-- difference in the API under test, never in the harness.
--
-- Host contract (both engines):
--   1. Load this file as a chunk named "=prelude" in an environment with Lua's standard base, string, table and math
--      libraries plus a trusted metatable inspector (__sfi_getmetatable or debug.getmetatable, never exposed to the
--      fixture's sandbox), and give
--      the table it returns to the fixture as the global `T`.
--   2. Load the fixture as a chunk named "=fixture" (so error positions read "fixture:<line>: ..." in both engines) and
--      call it with no arguments, protected. If it raises, call T.fail_fixture(err) with the error value.
--   3. Read T.transcript(): a JSON text (UTF-8, no trailing newline) matching transcript.schema.json.
-- Fixtures use only T.call, T.value, T.expect and T.note. Nothing here touches the API under test.
local T = {}
local records = {}
local fixture_error = nil

local math_type, math_huge = math.type, math.huge
local string_format, string_byte, string_gsub, string_pack = string.format, string.byte, string.gsub, string.pack
local table_sort, table_concat, table_pack, table_unpack = table.sort, table.concat, table.pack, table.unpack
local rawget, pcall, type, tostring, getmetatable, next, ipairs = rawget, pcall, type, tostring, getmetatable, next, ipairs
local inspect_metatable = __sfi_getmetatable or debug.getmetatable

-- v2 message policy: only strings or values with an actual __tostring have text. Public getmetatable can be hidden
-- (notably by mlua error userdata); the trusted host inspector bypasses __metatable. All inspection/conversion is
-- protected. A failed diagnostic, or a non-string result, means message is absent, never a replacement error.
-- SF both pins, modules/mod-runtime/lua/errors.rs:16-38,91-95: only the final "got" value of a top-level type
-- diagnostic is formatter-owned. mlua Debug names userdata (Buffer/Image), not just "userdata". A label anywhere
-- else (including module IDs, paths and traceback/chunk names) is literal data, even if it looks like a pointer.
-- Match the entire first line, never search inside interpolated input. Unknown formats remain byte-exact.
local string_match = string.match
local function normalize_message(text)
  local line, rest = string_match(text, "^([^\n]*)(.*)$")
  for _, class in ipairs({ "table", "userdata", "function", "thread", "Buffer", "Image" }) do
    local prefix, hex, closing = string_match(line, "^(.-" .. class .. ": )0[xX](%x+)(%)?)$")
    if prefix and #hex <= 16 then
      local value_slot = " expected, got " .. class .. ": $"
      local parenthesized = string_match(prefix, "^bad argument #%d+ %([^():\r\n]+" .. value_slot)
        or string_match(prefix, "^attempt to call method on invalid receiver %([^():\r\n]+" .. value_slot)
      local mismatch = string_match(prefix, "^[^():\r\n]+" .. value_slot)
      if (parenthesized and closing == ")") or (mismatch and closing == "") then
        return prefix .. "<address>" .. closing .. rest
      end
    end
  end
  return text
end

local function error_message(err)
  local ok, text, formatted = pcall(function()
    if type(err) == "string" then return err end
    local mt = inspect_metatable(err)
    if type(mt) == "table" and rawget(mt, "__tostring") ~= nil then
      -- Pinned mlua 0.11.6, src/util/error.rs:348-357 marks its internal error formatter with __type="error".
      -- Custom __tostring output (including other userdata) and string errors are always literal caller text.
      local is_error = type(err) == "userdata" and rawget(mt, "__type") == "error"
      return tostring(err), is_error
    end
  end)
  if ok and type(text) == "string" then
    if formatted then return normalize_message(text) end
    return text
  end
end

-- ---- canonical value model -------------------------------------------------------------------------------------
-- Every Lua value becomes a plain table that json() writes in one fixed order:
--   nil      {t="nil"}
--   boolean  {t="boolean", v=true|false}
--   integer  {t="integer", v="<decimal>"}              exact for every 64-bit value
--   float    {t="float", v="<16 hex digits>"}         the IEEE-754 bits, big-endian; "inf", "-inf", "nan" (every NaN)
--   string   {t="string", v="<%q quoted>"}             exact for any bytes, readable for text
--   table    {t="table", mt=bool, v={{k,v},...}}       pairs sorted by the JSON text of their key; cycles {t="cycle"}
--   function {t="function"}
--   userdata {t="userdata", name="<__name or ?>"}      never an address
--   thread   {t="thread"}

-- No printf float format here: "%a" and "%.17g" differ between C libraries (glibc on the Linux host, the Windows CRT
-- the oracle runs on), and the transcript must not.
local function float_text(x)
  if x ~= x then return "nan" end
  if x == math_huge then return "inf" end
  if x == -math_huge then return "-inf" end
  local bytes = string_pack(">d", x)
  local hex = {}
  for i = 1, 8 do hex[i] = string_format("%02x", string_byte(bytes, i)) end
  return table_concat(hex)
end

local function meta_name(v)
  local ok, mt = pcall(getmetatable, v)
  if not ok or type(mt) ~= "table" then return "?" end
  local okn, name = pcall(rawget, mt, "__name")
  if okn and type(name) == "string" then return name end
  return "?"
end

local json -- forward

local function canon(v, seen)
  local t = type(v)
  if t == "nil" then return { t = "nil" } end
  if t == "boolean" then return { t = "boolean", v = v } end
  if t == "number" then
    if math_type(v) == "integer" then return { t = "integer", v = string_format("%d", v) } end
    return { t = "float", v = float_text(v) }
  end
  if t == "string" then return { t = "string", v = string_format("%q", v) } end
  if t == "function" then return { t = "function" } end
  if t == "thread" then return { t = "thread" } end
  if t == "userdata" then return { t = "userdata", name = meta_name(v) } end
  -- table
  if seen[v] then return { t = "cycle" } end
  seen[v] = true
  local pairs_list = {}
  local k, val = next(v)
  while k ~= nil do
    local ck = canon(k, seen)
    pairs_list[#pairs_list + 1] = { key_text = json(ck), k = ck, v = canon(val, seen) }
    k, val = next(v, k)
  end
  seen[v] = nil
  table_sort(pairs_list, function(a, b) return a.key_text < b.key_text end)
  local out = {}
  for i = 1, #pairs_list do out[i] = { pairs_list[i].k, pairs_list[i].v } end
  local has_mt = false
  local okm, mt = pcall(getmetatable, v)
  if okm and mt ~= nil then has_mt = true end
  return { t = "table", mt = has_mt, v = out }
end

-- ---- JSON writer (fixed key order, no dependence on host libraries) -----------------------------------------
local escapes = { ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r',
  ['\t'] = '\\t' }

local function json_string(s)
  -- Strings written here are ASCII by construction (labels are the fixture's; values went through %q, which keeps
  -- bytes >= 0x80 as they are). Bytes >= 0x80 are written as \u00XX so the transcript is valid JSON whatever they are.
  local body = string_gsub(s, '[%c"\\\128-\255]', function(c)
    return escapes[c] or string_format("\\u%04x", string_byte(c))
  end)
  return '"' .. body .. '"'
end

local KEY_ORDER = { "seq", "label", "kind", "ok", "t", "name", "mt", "v", "values", "error", "message", "expected", "pass", "text" }

json = function(x)
  local tx = type(x)
  if tx == "string" then return json_string(x) end
  if tx == "boolean" then return x and "true" or "false" end
  if tx == "number" then return string_format("%d", x) end
  if tx == "nil" then return "null" end
  -- arrays are tables with [1..n] (an empty one carries __array = true); objects use KEY_ORDER
  if x[1] ~= nil or rawget(x, "__array") then
    local parts = {}
    for i = 1, #x do parts[i] = json(x[i]) end
    return "[" .. table_concat(parts, ",") .. "]"
  end
  if next(x) == nil then return "[]" end
  local parts = {}
  for _, key in ipairs(KEY_ORDER) do
    local val = rawget(x, key)
    if val ~= nil then parts[#parts + 1] = json_string(key) .. ":" .. json(val) end
  end
  return "{" .. table_concat(parts, ",") .. "}"
end

local function canon_list(packed, first)
  local out = {}
  for i = first, packed.n do out[#out + 1] = canon(packed[i], {}) end
  if #out == 0 then out.__array = true end
  return out
end

local function push(rec)
  rec.seq = #records + 1
  records[#records + 1] = rec
end

-- ---- fixture API -------------------------------------------------------------------------------------------------

--- Calls fn(...) protected and records every value it returns, or the error it raised. Returns what pcall returns.
function T.call(label, fn, ...)
  local r = table_pack(pcall(fn, ...))
  if r[1] then
    push({ label = label, kind = "call", ok = true, values = canon_list(r, 2) })
  else
    -- Snapshot identity before __tostring can have side effects; never alter r or its arity.
    local original = canon(r[2], {})
    push({ label = label, kind = "call", ok = false, error = original, message = error_message(r[2]) })
  end
  return table_unpack(r, 1, r.n)
end

--- Records one value as it is.
function T.value(label, v)
  push({ label = label, kind = "value", values = { canon(v, {}) } })
  return v
end

--- Records whether `actual` equals `expected` in the canonical model (a fixture-side check, for behaviour the
--- contract states; both engines must pass it).
function T.expect(label, actual, expected)
  local a, e = canon(actual, {}), canon(expected, {})
  local pass = json(a) == json(e)
  push({ label = label, kind = "expect", pass = pass, values = { a }, expected = e })
  return pass
end

--- Records a free-form note (for example which branch a fixture took). Notes are compared like everything else.
function T.note(label, text)
  push({ label = label, kind = "note", text = tostring(text) })
end

--- Host use: the fixture chunk itself raised.
function T.fail_fixture(err)
  fixture_error = canon(err, {})
end

--- Host use: the transcript as JSON text.
function T.transcript()
  local recs = {}
  for i = 1, #records do recs[i] = records[i] end
  if #recs == 0 then recs.__array = true end
  local parts = { '"version":2', '"records":' .. json(recs) }
  if fixture_error ~= nil then parts[#parts + 1] = '"fixture_error":' .. json(fixture_error) end
  return "{" .. table_concat(parts, ",") .. "}"
end

return T
