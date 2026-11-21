-- SPDX-License-Identifier: MIT
-- The environment and lifecycle cases. Fixed seed: the differential test's seed package ('oracle',
-- startup phase, server). Pure Lua-observable environment facts; no native, input or UI claims.
local function keys(t)
  local out = {}
  for k in pairs(t) do out[#out + 1] = tostring(k) end
  table.sort(out)
  return table.concat(out, ',')
end

-- typeof: create_sandbox.rs:82-105 (both pins).
T.call('typeof values', function()
  local named = setmetatable({}, {__name = 'named', __type = 'typed', __tostring = function() return 'x' end})
  return typeof(), typeof(nil), typeof(false), typeof(0), typeof(1.5), typeof('x'),
    typeof(string.char(255, 0, 254)), typeof({}), typeof(named), typeof(print),
    typeof(coroutine.create(print)), typeof(buffer.create(1)), select('#', typeof())
end)
T.call('typeof extra arguments', function() return typeof(1, 'ignored', {}) end)
T.call('typeof captures the original type', function()
  local original = type
  type = function() return 'changed' end
  local a, b, c = typeof(1), typeof({}), typeof(buffer.create(1))
  type = original
  return a, b, c
end)

-- _ENV: require_mod.rs:119,166 and create_sandbox.rs:80 (both pins).
T.call('environment identity', function()
  return _ENV == _G, rawget(_ENV, '_G') == _ENV, getmetatable(_ENV) == nil,
    rawget(_ENV, 'shroudforge') == shroudforge, rawget(_ENV, 'T') == T
end)
T.call('no fallback to VM globals', function()
  return os, package, debug, load, loadfile, dofile, rawget(_ENV, 'os'), rawget(_ENV, 'require') == require
end)
T.call('global assignment lands in the package environment', function()
  sfi_env_marker = 41
  local function reader() return sfi_env_marker end
  local raw, nested = rawget(_ENV, 'sfi_env_marker'), reader()
  sfi_env_marker = nil
  return raw, nested, rawget(_ENV, 'sfi_env_marker')
end)
T.call('assigning _G does not rebind the chunk environment', function()
  local saved = _G
  _G = {}
  sfi_after_rebind = 1
  local in_env, in_new = rawget(saved, 'sfi_after_rebind'), rawget(_G, 'sfi_after_rebind')
  _G = saved
  sfi_after_rebind = nil
  return in_env, in_new, _G == _ENV
end)
T.call('required chunks run in the owner environment', function()
  local m = require('mod')
  local cached = require('cached')
  return m.owner, m == require('mod'), m == require('mods.oracle'), cached == require('mods.oracle.cached'),
    cached.answer, select('#', require('cached')), require('nil_value'), select('#', require('nil_value'))
end)

-- loader.has_mod: create_loader_api.rs:12-17, arguments via mlua String (both pins).
T.call('loader.has_mod values', function()
  return loader.has_mod('oracle'), loader.has_mod('oracle', 'ignored'), loader.has_mod('Oracle'),
    loader.has_mod(''), loader.has_mod(7), loader.has_mod(7.5), select('#', loader.has_mod('oracle'))
end)
T.call('loader.has_mod binary id', loader.has_mod, string.char(255))
T.call('loader.has_mod boolean id', loader.has_mod, true)
T.call('loader shape', function()
  return type(rawget(loader, 'is_client')), type(rawget(loader, 'is_server')), loader.is_client == not loader.is_server,
    type(rawget(loader, 'has_mod')), type(rawget(loader, 'features')), getmetatable(loader), getmetatable(loader.features),
    type(rawget(loader.features, 'patch')), type(rawget(loader.features, 'export')), loader == builtin.loader
end)

-- shroudforge / builtin.shroudforge: bindings/mod.rs:88-109, create_mod_api.rs:19-115,230 (both pins).
T.call('namespace members', function()
  return keys(shroudforge.settings), keys(shroudforge.i18n), keys(shroudforge.log),
    getmetatable(shroudforge), getmetatable(shroudforge.settings), getmetatable(shroudforge.i18n),
    type(shroudforge.settings.get), type(shroudforge.i18n.get)
end)
T.call('editor aliases are not globals', function()
  return shroudforge_settings, shroudforge_ui, shroudforge_input, shroudforge_notifications,
    shroudforge_i18n, shroudforge_native, shroudforge_log
end)
T.call('builtin.shroudforge identity and slots', function()
  local api = shroudforge
  local same = builtin.shroudforge == api and builtin.shroudforge.settings == api.settings
    and builtin.shroudforge.i18n == api.i18n and builtin.shroudforge.log == api.log
    and builtin.shroudforge.settings.get == api.settings.get and builtin.shroudforge.clock == api.clock
  builtin.shroudforge.sfi_marker = 1
  local through_alias = api.sfi_marker
  api.settings.sfi_marker = 2
  local child = builtin.shroudforge.settings.sfi_marker
  api.sfi_marker, api.settings.sfi_marker = nil, nil
  shroudforge = {}
  local kept = builtin.shroudforge == api
  builtin.shroudforge = {}
  local independent = shroudforge ~= builtin.shroudforge and builtin.shroudforge ~= api
  builtin.shroudforge, shroudforge = api, api
  return same, through_alias, child, kept, independent, shroudforge == builtin.shroudforge
end)

-- shroudforge.settings.get: create_mod_api.rs:95-115, valid_identifier (both pins).
T.call('setting key grammar', function()
  local g = shroudforge.settings.get
  return g(string.rep('a', 80)), g('a.b-c_D9'), g('7'), g(7), g('count', 'fallback'), g('missing', nil),
    g('title', false), select('#', g('missing'))
end)
T.call('setting key too long', shroudforge.settings.get, string.rep('a', 81))
T.call('setting key empty', shroudforge.settings.get, '')
T.call('setting key space', shroudforge.settings.get, 'a b')
T.call('setting key non-ascii', shroudforge.settings.get, 'ä')
T.call('setting key boolean', shroudforge.settings.get, true)

-- shroudforge.i18n.get: create_mod_api.rs:216-230 and resolve_mod_locale.rs (both pins).
T.call('catalog resolution', function()
  local g = shroudforge.i18n.get
  return g('hello', 'de-DE-x'), g('hello', 'en-US'), g('english_only', 'de'), g('english_only', 'fr'),
    g('hello', 'de', nil), g('hello', 'de', 'ignored', 'extra'), g(7, 'de'), g('hello', 7), g('hello', 'en_GB', 'fb'),
    g('hello', 'De'), g('', 'de')
end)
T.call('catalog missing locale', shroudforge.i18n.get, 'hello')
T.call('catalog binary key', shroudforge.i18n.get, string.char(255), 'de')
T.call('catalog binary fallback', shroudforge.i18n.get, 'absent', 'de', string.char(255))
