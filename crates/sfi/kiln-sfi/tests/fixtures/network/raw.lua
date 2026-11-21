-- SPDX-License-Identifier: MIT
-- runtime.network (status, send, receive, accept) where no network can be used: the argument
-- conversions ShroudForge's typed closures run before the denial chain, and the denial itself
-- (the oracle's state is the pregame phase, so the first gate answers). ShroudForge (MIT,
-- Copyright (c) 2026 bonsaibauer), both pins: modules/transport/lua/network.rs:102-348.
local net = runtime.network
T.value("type.network", type(net))
T.value("type.status", type(net.status))
T.value("type.send", type(net.send))
T.value("type.receive", type(net.receive))
T.value("type.accept", type(net.accept))

T.call("status", net.status)
T.call("status ignores arguments", net.status, 1, "x", {})

T.call("send no arguments", net.send)
T.call("send nil payload", net.send, "1")
T.call("send integer peer", net.send, 76561198000000000, "x")
T.call("send float peer", net.send, 1.5, "x")
T.call("send table peer", net.send, {}, "x")
T.call("send boolean peer", net.send, true, "x")
T.call("send invalid utf8 peer", net.send, "\255", "x")
T.call("send number payload", net.send, "1", 5)
T.call("send boolean payload", net.send, "1", true)
T.call("send table payload", net.send, "1", {})
T.call("send number options", net.send, "1", "x", 5)
T.call("send string options", net.send, "1", "x", "o")
T.call("send nil options", net.send, "1", "x", nil)
T.call("send binary payload", net.send, "1", "\0\255")
T.call("send empty payload", net.send, "1", "")
T.call("send bad peer after the denial", net.send, "0", "x")
T.call("send bad channel after the denial", net.send, "1", "x", { channel = 70000 })
T.call("send string channel after the denial", net.send, "1", "x", { channel = "x" })
T.call("send extra arguments", net.send, "1", "x", {}, 4, 5)

T.call("receive no options", net.receive)
T.call("receive nil options", net.receive, nil)
T.call("receive number options", net.receive, 5)
T.call("receive string options", net.receive, "x")
T.call("receive bad channel after the denial", net.receive, { channel = -1 })
T.call("receive bad limit after the denial", net.receive, { limit = 99 })
T.call("receive string limit after the denial", net.receive, { limit = "x" })

T.call("accept no peer", net.accept)
T.call("accept table", net.accept, {})
T.call("accept integer", net.accept, 7)
T.call("accept invalid utf8", net.accept, "\255")
T.call("accept zero after the denial", net.accept, "0")
T.call("accept", net.accept, "76561198000000000")
