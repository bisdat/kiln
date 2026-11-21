-- SPDX-License-Identifier: MIT
-- Fixed seed: the differential test's seed package. No native, input or UI success claims.
T.call('identity', function() return shroudforge.version, shroudforge.mod_id, shroudforge.mod_kind,
    rawget(shroudforge, 'mod_kind'), shroudforge == builtin.shroudforge,
    shroudforge.settings.get == builtin.shroudforge.settings.get end)
T.call('settings kinds', function()
    local g = shroudforge.settings.get
    return g('enabled_flag'), g('count'), math.type(g('count')), g('fraction'),
        math.type(g('fraction')), g('title'), g('missing'), g('missing', false)
end)
T.call('arbitrary fallback', function()
    local fallback = {}
    return shroudforge.settings.get('absent', fallback) == fallback
end)
T.call('invalid setting key', shroudforge.settings.get, '../bad')
T.call('setting key type', shroudforge.settings.get, {})
T.call('setting missing key', shroudforge.settings.get)
T.call('setting binary key', shroudforge.settings.get, string.char(255))
T.call('catalogs', function()
    local g = shroudforge.i18n.get
    return shroudforge.locale(), g('hello','de-DE'), g('hello','en'),
        g('hello','fr'), g('hello','DE','fallback'), g('absent','de','')
end)
T.call('catalog argument', shroudforge.i18n.get, 'hello', {})
T.call('loader metadata', function()
    return loader == builtin.loader, loader.is_client, loader.is_server,
        loader.features.patch, loader.features.export, loader.has_mod('oracle'),
        loader.has_mod('ORACLE'), loader.has_mod('absent'), loader.has_mod(7)
end)
T.call('loader argument', loader.has_mod, {})
T.call('loader missing argument', loader.has_mod)
T.call('print', print, '', 'x', '', 'a\0b\r\n', 1.0)
T.call('warn', warn, '@off', '', 'x')
T.call('log trace', shroudforge.log.trace, '', 'x')
T.call('log debug', shroudforge.log.debug, '', 'x')
T.call('log info', shroudforge.log.info, '', 'x')
T.call('log warn', shroudforge.log.warn, '', 'x')
T.call('log error', shroudforge.log.error, '', 'x')
T.call('print binary error', print, string.char(255))
T.call('warn binary error', warn, string.char(255))
T.call('log binary error', shroudforge.log.info, string.char(255))
T.call('log tostring error', function()
    shroudforge.log.info(setmetatable({}, {__tostring = function() error('conversion sentinel') end}))
end)
T.call('clock nonnegative', function() return shroudforge.clock() >= 0 end)
