-- SPDX-License-Identifier: MIT
-- runtime.network.send_mod / receive_mod where no network can be used: the argument conversions
-- ShroudForge's typed closures run before the denial chain (Rust String, so UTF-8, for peer,
-- target and payload; Option<usize> for the limit), and the denial itself (the oracle's state is
-- the pregame phase, so the first gate answers; the target length, peer and limit checks come
-- after it). ShroudForge (MIT, Copyright (c) 2026 bonsaibauer), both pins:
-- modules/transport/lua/network.rs:201-232,350-409.
local net = runtime.network
T.value("type.send_mod", type(net.send_mod))
T.value("type.receive_mod", type(net.receive_mod))

T.call("send_mod no arguments", net.send_mod)
T.call("send_mod nil target", net.send_mod, "1")
T.call("send_mod nil payload", net.send_mod, "1", "m")
T.call("send_mod table target", net.send_mod, "1", {}, "p")
T.call("send_mod boolean target", net.send_mod, "1", true, "p")
T.call("send_mod invalid utf8 target", net.send_mod, "1", "\255", "p")
T.call("send_mod invalid utf8 peer", net.send_mod, "\255", "m", "p")
T.call("send_mod invalid utf8 payload", net.send_mod, "1", "m", "\255")
T.call("send_mod binary but valid utf8 payload", net.send_mod, "1", "m", "\0\195\169")
T.call("send_mod table payload", net.send_mod, "1", "m", {})
T.call("send_mod boolean payload", net.send_mod, "1", "m", false)
T.call("send_mod number options", net.send_mod, "1", "m", "p", 5)
T.call("send_mod string options", net.send_mod, "1", "m", "p", "o")
T.call("send_mod number peer", net.send_mod, 76561198000000000, "m", "p")
T.call("send_mod number target", net.send_mod, "1", 5, "p")
T.call("send_mod number payload", net.send_mod, "1", "m", 7)
T.call("send_mod nil options", net.send_mod, "1", "m", "p", nil)
T.call("send_mod", net.send_mod, "1", "m", "p", { reliable = false })
T.call("send_mod zero peer after the denial", net.send_mod, "0", "m", "p")
T.call("send_mod empty target after the denial", net.send_mod, "1", "", "p")
T.call("send_mod long target after the denial", net.send_mod, "1", string.rep("t", 97), "p")
T.call("send_mod channel option ignored", net.send_mod, "1", "m", "p", { channel = "x" })
T.call("send_mod big payload after the denial", net.send_mod, "1", "m", string.rep("z", 524288))
T.call("send_mod extra arguments", net.send_mod, "1", "m", "p", {}, 5, 6)

T.call("receive_mod", net.receive_mod)
T.call("receive_mod nil", net.receive_mod, nil)
T.call("receive_mod negative", net.receive_mod, -1)
T.call("receive_mod float", net.receive_mod, 1.9)
T.call("receive_mod negative fraction", net.receive_mod, -0.5)
T.call("receive_mod zero after the denial", net.receive_mod, 0)
T.call("receive_mod 33 after the denial", net.receive_mod, 33)
T.call("receive_mod huge", net.receive_mod, 1e300)
T.call("receive_mod string", net.receive_mod, "x")
T.call("receive_mod numeric string", net.receive_mod, "3")
T.call("receive_mod table", net.receive_mod, {})
T.call("receive_mod boolean", net.receive_mod, true)
T.call("receive_mod extra arguments", net.receive_mod, 4, "x", {})
