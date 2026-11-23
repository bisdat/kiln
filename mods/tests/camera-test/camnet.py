#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Lab-transport Kiln clients for the channel kiln.camera v1 (chapter 03 4.6, S8), for camera-test's remote checks.

Four fake Kiln peers on kiln.net's lab transport (tools/nettest); camera-test's OnLoad stands each in for a player by its
nettest name (a lab peer has no game player):
  A  camtest-A  kiln.camera, Hello with every local part (pose, fov, profile, shake)
  N  camtest-N  kiln.camera, Hello without camera.local_fov
  V  camtest-V  no kiln.camera (a Kiln without the channel)
  S  camtest-S  kiln.camera, never says Hello
A and N answer as a Kiln client's receiver does: Hello until Welcome, an Ack for each Apply/Update/End of their binding
(frame 4242; a part they did not offer is refused ClientCapability), the cached Ack again for a repeated serial, Status for
each Heartbeat. An Apply of exactly 3333 ms is ended by the client right after its Ack (Ended, Expired), as a client
whose lease ran out; an Update of exactly 4444 ms is never answered, as a client that hangs. Needs "labPort" in kiln/config/network.json.

    camnet.py --port P [--until FILE] [--seconds N]

Runs until FILE exists (camera-test's report) or N seconds passed; prints what each peer saw and exits 0, or 1 when a
peer never finished its handshake.
"""
import argparse
import os
import random
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools", "nettest"))
import nettest as nt  # noqa: E402

CH = "kiln.camera"
HELLO, WELCOME, APPLY, UPDATE, END, ACK, ENDED, HEARTBEAT, STATUS = range(9)
POSE, FOV, PROFILE, SHAKE = 1 << 1, 1 << 2, 1 << 3, 1 << 4
PART_NEEDS = ((2, POSE), (4, PROFILE), (8, SHAKE), (16, FOV))  # request part -> the local row it needs
HEADER = 84
CLIENT_CAPABILITY = 25
EXPIRED = 2
# camera-test's LabPlayers (CameraTest.cs): the server binds a Hello only for the player its connection stands for
LAB_PLAYERS = {"camtest-A": 910001, "camtest-N": 910002, "camtest-V": 910003, "camtest-S": 910004}


class Peer(nt.Client):
    def __init__(self, port, name, caps, say_hello=True):
        super().__init__(port, name)
        self.caps, self.say_hello = caps, say_hello
        self.cam_id = None
        self.nonce = random.getrandbits(62) | 1
        self.binding = None
        self.player = 0
        self.allowed = 0
        self.acks = {}
        self.highest = 0
        self.active = (0, 0)
        self.last_hello = 0.0
        self.seen = {"Welcome": 0, "Apply": 0, "Update": 0, "End": 0, "Heartbeat": 0, "Ack": 0, "Ended": 0, "Refused": 0, "Replayed": 0, "Withheld": 0}

    def set_table(self, table):
        self.table = table
        self.cam_id = next((i for i, n, v, _ in table if n == CH and v == "1"), None)
        self.channels = {CH: self.cam_id} if self.cam_id is not None else {}

    def pump(self, seconds):
        super().pump(seconds)
        keep = []
        for m in self.messages:
            if self.cam_id is not None and m[0] == self.cam_id:
                self.on_camera(m[1], m[2])
            else:
                keep.append(m)
        self.messages = keep

    def tick(self):
        if self.say_hello and self.cam_id is not None and self.welcome is not None and self.binding is None and time.time() - self.last_hello >= 1.0:
            self.last_hello = time.time()
            self.data(self.cam_id, HELLO, self.hello_body())

    def hello_body(self):
        return struct.pack("<HQIIQHI", 1, self.nonce, 1, LAB_PLAYERS[self.name], self.caps, 256, 16384)

    def cam(self, mid, body):
        self.data(self.cam_id, mid, body)

    def on_camera(self, mid, body):
        if mid == WELCOME and len(body) == 58:
            cn, ce, sn, se, player, inc, token, allowed, _maxbody, _maxkeys = struct.unpack("<QIQIIQQQIH", body)
            if cn != self.nonce:
                return
            self.binding = struct.pack("<QQIQIIQ", token, sn, se, cn, ce, player, inc)
            self.player, self.allowed = player, allowed
            self.seen["Welcome"] += 1
        elif mid in (APPLY, UPDATE, END):
            if self.binding is None or len(body) < HEADER or body[:44] != self.binding:
                return
            owner, lease, _rid, serial, rev = struct.unpack_from("<QQQQQ", body, 44)
            if serial in self.acks:
                self.seen["Replayed"] += 1
                self.cam(ACK, self.acks[serial])
                return
            if serial <= self.highest:
                return
            self.highest = serial
            self.seen[{APPLY: "Apply", UPDATE: "Update", END: "End"}[mid]] += 1
            ok, reason, scope, duration = 1, 0, 2, 0
            if mid != END:
                at = HEADER + (1 if mid == UPDATE else 0)
                parts = struct.unpack_from("<I", body, at)[0]
                duration = struct.unpack_from("<I", body, at + 5)[0]
                if mid == UPDATE and duration == 4444:
                    # a client that never answers (a hitch, a loading screen): no Ack, also not for the resends of this
                    # serial; the server times out and ends the lease, and its End (a higher serial) is answered
                    self.seen["Withheld"] += 1
                    return
                need = 0
                for part, row in PART_NEEDS:
                    if parts & part:
                        need |= row
                if need & ~self.caps:
                    ok, reason, scope = 0, CLIENT_CAPABILITY, 0
                    self.seen["Refused"] += 1
            else:
                scope = 0
            ack = body[:HEADER] + struct.pack("<BHBIQIIH", ok, reason, scope, 0, 4242 if ok else 0, 0, duration if ok else 0, 0)
            self.acks[serial] = ack
            self.cam(ACK, ack)
            self.seen["Ack"] += 1
            if not ok:
                return
            if mid == END:
                if self.active[0] == lease:
                    self.active = (0, 0)
                return
            self.active = (lease, rev)
            if mid == APPLY and duration == 3333:
                # this client's lease ran out by itself: Ended, checked against owner, lease and revision by the server
                header = self.binding + struct.pack("<QQQQQ", owner, lease, 0, self.highest, rev)
                self.cam(ENDED, header + struct.pack("<BHQ", EXPIRED, 0, 4243))
                self.active = (0, 0)
                self.seen["Ended"] += 1
        elif mid == HEARTBEAT and len(body) == 76:
            if self.binding is None or body[:44] != self.binding:
                return
            seq = struct.unpack_from("<Q", body, 44)[0]
            self.seen["Heartbeat"] += 1
            lease, rev = self.active
            self.cam(STATUS, self.binding + struct.pack("<QQQII", seq, lease, rev, 0, 1000 if lease else 0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--until", default=None)
    ap.add_argument("--seconds", type=float, default=240)
    a = ap.parse_args()
    peers = [
        Peer(a.port, "camtest-A", POSE | FOV | PROFILE | SHAKE),
        Peer(a.port, "camtest-N", POSE | PROFILE),
        Peer(a.port, "camtest-V", 0, say_hello=False),
        Peer(a.port, "camtest-S", POSE | FOV | PROFILE | SHAKE, say_hello=False),
    ]
    for p in peers:
        p.hello([], [] if p.name == "camtest-V" else [(CH, "1", False)])
    end = time.time() + a.seconds
    ok = True
    for p in peers:
        if not p.wait(lambda p=p: p.welcome is not None, 30):
            print(f"camnet: FAIL {p.name}: no Kiln handshake", flush=True)
            ok = False
    if not ok:
        sys.exit(1)
    print("camnet: handshakes " + ", ".join(f"{p.name} declared kiln.camera {'no' if p.name == 'camtest-V' else 'yes'}" for p in peers), flush=True)
    while time.time() < end:
        for p in peers:
            p.tick()
            p.pump(0.02)
        if a.until and os.path.exists(a.until):
            break
    for p in peers:
        print(f"camnet: {p.name} player {p.player} allowed {p.allowed:#x} saw {p.seen}", flush=True)
    a_peer = peers[0]
    print(f"camnet: {'PASS' if a_peer.seen['Welcome'] >= 1 and a_peer.seen['Apply'] >= 1 else 'FAIL'} A bound and served", flush=True)


if __name__ == "__main__":
    main()
