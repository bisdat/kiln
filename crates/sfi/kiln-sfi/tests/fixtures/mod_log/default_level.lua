-- SPDX-License-Identifier: MIT
-- shroudforge.log at ShroudForge's default minimum level, INFO (logging/allows_log_level.rs:
-- 3-19; the differential test's subscriber is INFO too). Disabled trace and
-- debug return before converting anything (create_mod_api.rs:31-43 in 1.7.9, :32-44 in
-- 1.8.0); info, warn, error and print convert every argument.
local count = 0
local counted = setmetatable({}, {__tostring = function() count = count + 1; return 'counted' end})
local boom = setmetatable({}, {__tostring = function() error('conversion sentinel') end})
local function conversions(f, ...)
    count = 0
    local results = select('#', f(...))
    return results, count
end
T.call('trace converts nothing', conversions, shroudforge.log.trace, counted, counted)
T.call('debug converts nothing', conversions, shroudforge.log.debug, counted, counted)
T.call('info converts', conversions, shroudforge.log.info, counted, counted)
T.call('warn converts', conversions, shroudforge.log.warn, counted, counted)
T.call('error converts', conversions, shroudforge.log.error, counted, counted)
T.call('print converts at debug level', conversions, print, counted, counted)
T.call('trace hides a conversion error', shroudforge.log.trace, boom, string.char(255))
T.call('debug hides a conversion error', shroudforge.log.debug, boom, string.char(255))
T.call('info raises a conversion error', shroudforge.log.info, boom)
T.call('info raises a binary string error', shroudforge.log.info, string.char(255))
T.call('trace with no arguments', shroudforge.log.trace)
T.call('info with no arguments', shroudforge.log.info)
