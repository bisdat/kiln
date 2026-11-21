-- SPDX-License-Identifier: MIT
-- connected_peers in the differential test's state (pregame, a server, readable default settings).
local net = runtime.network
T.value("type.connected_peers", type(net.connected_peers))
T.call("connected_peers", net.connected_peers)
T.call("connected_peers ignores arguments", net.connected_peers, 1, "x", {})
