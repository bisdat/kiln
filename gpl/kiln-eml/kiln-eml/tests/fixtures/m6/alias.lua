-- SPDX-License-Identifier: GPL-3.0-or-later
-- The real old-EML require cache shares these mapped userdata with the entrypoint.
local d = game.assets.get_resource("01234567-89ab-cdef-0123-456789abcdef", "m6.Root").data
return {child=d.child, children=d.children}
