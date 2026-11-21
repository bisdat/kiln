-- SPDX-License-Identifier: MIT
-- The environment cases: require's raised error roots and argument conversion.
-- Fixed seed: the differential test's seed package. No case depends on OS I/O error text (the empty path
-- reaches the package tree lookup, whose 'entity not found' is not an OS message).
T.call('unknown mod', require, 'mods.absent')
T.call('unknown mod with path', require, 'mods.absent.module')
T.call('table argument', require, {})
T.call('missing argument', require)
T.call('boolean argument', require, true)
T.call('binary argument', require, string.char(255))
T.call('empty path', require, '')
T.call('cycle', require, 'cycle')
T.call('cycle is retryable', require, 'cycle')
T.call('extra arguments ignored', function()
  return require('cached', 'ignored', {}) == require('cached'), select('#', require('nil_value', 1))
end)
T.call('qualified self import', function()
  return require('mods.oracle.cached') == require('cached'), require('mods.oracle.nil_value')
end)
