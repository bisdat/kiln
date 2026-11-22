#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A built-in marker receiver over kiln.net's lab transport (dossier chapter 04 §4.4, wire v1), for kiln.markers' lab test.

It joins a lab server as a Kiln client that has the channel kiln:markers v1 and NOT the sending mod
(mods/tests/markers-send-test, a server-only mod), speaks the receiver's side of wire v1 (HELLO, CATALOG when asked, snapshot
staging with its digest, DELTA, POSE, ACK, RESYNC on a gap, HEARTBEAT), keeps the received view and counts one logical event
per changed key as the built-in receiver does; then a vanilla lab peer joins, which must get no Kiln frame at all.

    tools/nettest/markerstest.py --port <labPort> --lab <lab dir> --catalog <catalog rows JSON> --client-exe <sha256 hex>

The catalog file (rows: id, image GUID, flags, name; and its sha256) is read at run time: game data is never kept in the repo.
The server accepts it only when its digest is the verified catalog of the server's build pair (catalog.h), sent by that
pair's client exe hash. Prints "markers-peer: PASS <case>" / "FAIL <case>" and exits 1 on any FAIL.
"""
import argparse
import hashlib
import json
import os
import random
import struct
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nettest as nt  # noqa: E402

CHANNEL = ("kiln:markers", "1")
HELLO, WELCOME, CATALOG, BEGIN, CHUNK, END, DELTA, POSE, ACK, RESYNC, HEARTBEAT = range(11)
NAMES = ["HELLO", "WELCOME", "CATALOG", "SNAP_BEGIN", "SNAP_CHUNK", "SNAP_END", "DELTA", "POSE", "ACK", "RESYNC", "HEARTBEAT"]
CAPS = 1 | 2 | 4 | 16  # map, compass, world, follow poses


def header(epoch, nonce, seq):
    return struct.pack("<HH16s16sQ", 1, 0, epoch, nonce, seq)


class Reader:
    def __init__(self, b, at=0):
        self.b, self.p = b, at

    def take(self, n):
        if self.p + n > len(self.b):
            raise ValueError("short")
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def u(self, fmt):
        n = struct.calcsize(fmt)
        return struct.unpack("<" + fmt, self.take(n))[0]

    def str16(self):
        return self.take(self.u("H")).decode("utf-8")


def read_record(r):
    start = r.p
    rec = {}
    rec["wire"], rec["gen"] = r.u("Q"), r.u("Q")
    rec["ns"], rec["key"] = r.str16(), r.str16()
    rec["rev"], rec["surfaces"], rec["priority"], rec["rgba"], rec["icon"] = r.u("Q"), r.u("I"), r.u("h"), r.u("I"), r.u("I")
    rec["label"], rec["followed"], rec["reason"] = r.u("B"), r.u("B"), r.u("H")
    rec["x"], rec["y"], rec["z"] = r.u("d"), r.u("d"), r.u("d")
    rec["height"], rec["max_distance"] = r.u("f"), r.u("f")
    rec["text"] = r.str16()
    return rec, bytes(r.b[start:r.p])


class MarkerPeer(nt.Client):
    def __init__(self, port, name, verbose=False):
        super().__init__(port, name, verbose)
        self.nonce = bytes(random.getrandbits(8) for _ in range(16))
        self.epoch = bytes(16)
        self.applied = 0
        self.view = {}        # (ns, gen, key) -> record
        self.events = []      # (kind, key)
        self.staging = None
        self.welcomes = []
        self.counts = {}
        self.acks = 0
        self.resyncs = 0
        self.bad = []
        self.digest_ok = 0

    def set_table(self, table):
        self.table = table
        self.channels = {n: i for i, n, v, _ in table if (n, v) == CHANNEL}

    def send_marker(self, msg, body):
        return self.data(CHANNEL[0], msg, body)

    def hello(self_, *a, **k):  # the Kiln handshake (nettest's), not the marker HELLO
        return nt.Client.hello(self_, *a, **k)

    def marker_hello(self, exe, digest):
        body = header(bytes(16), self.nonce, 0) + struct.pack("<Q32sIHI32s", 1, exe, CAPS, 512, 16384, digest)
        self.send_marker(HELLO, body)

    def ack(self, seq, status=0, reason=b""):
        self.acks += 1
        self.send_marker(ACK, header(self.epoch, self.nonce, self.applied) + struct.pack("<QBH", seq, status, len(reason)) + reason)

    def resync(self, reason):
        self.resyncs += 1
        self.send_marker(RESYNC, header(self.epoch, self.nonce, self.applied) + struct.pack("<QB", self.applied, reason))

    def apply_view(self, new, removes=()):
        """Diff old and new views by key, one event per changed key (as kiln.markers' receiver)."""
        old = self.view
        for k in removes:
            if k in old:
                self.events.append(("removed", k[2]))
        for k, rec in new.items():
            o = old.get(k)
            if o is None:
                self.events.append(("added", k[2]))
            elif (o["wire"], o["rev"]) != (rec["wire"], rec["rev"]):
                self.events.append(("updated", k[2]))
        self.view = new

    def handle(self, msg, body, catalog_body):
        self.counts[NAMES[msg] if msg < 11 else str(msg)] = self.counts.get(NAMES[msg] if msg < 11 else str(msg), 0) + 1
        r = Reader(body)
        schema, flags, epoch, nonce, seq = struct.unpack("<HH16s16sQ", r.take(44))
        if schema != 1 or flags != 0 or nonce != self.nonce:
            return
        if msg == WELCOME:
            accepted, maxr, need, _ = r.u("I"), r.u("H"), r.u("B"), r.u("B")
            self.epoch = epoch
            self.welcomes.append((accepted, need))
            if need:
                self.send_marker(CATALOG, header(self.epoch, self.nonce, self.applied) + catalog_body)
            return
        if epoch != self.epoch:
            return
        if msg == BEGIN:
            sid, total, chunks = r.u("Q"), r.u("H"), r.u("H")
            if seq > self.applied:
                self.staging = {"seq": seq, "id": sid, "total": total, "chunks": {}, "n": chunks, "end": None}
        elif msg == CHUNK:
            sid, idx, cnt = r.u("Q"), r.u("H"), r.u("H")
            st = self.staging
            if st and st["seq"] == seq and st["id"] == sid:
                recs = [read_record(r) for _ in range(cnt)]
                st["chunks"][idx] = recs
        elif msg == END:
            sid, digest = r.u("Q"), r.take(32)
            st = self.staging
            if seq <= self.applied:
                self.ack(self.applied)
            elif st and st["seq"] == seq and st["id"] == sid and len(st["chunks"]) == st["n"]:
                recs = [x for i in range(st["n"]) for x in st["chunks"][i]]
                if hashlib.sha256(b"".join(raw for _, raw in recs)).digest() != bytes(digest):
                    self.bad.append("snapshot digest")
                    self.resync(2)
                    return
                self.digest_ok += 1
                new = {(x["ns"], x["gen"], x["key"]): x for x, _ in recs}
                self.apply_view(new, removes=[k for k in self.view if k not in new])
                self.applied = seq
                self.staging = None
                self.ack(seq)
        elif msg == DELTA:
            if seq <= self.applied:
                self.ack(self.applied)
                return
            if seq != self.applied + 1:
                self.resync(1)
                return
            n = r.u("H")
            new = dict(self.view)
            by_wire = {v["wire"]: k for k, v in new.items()}
            removes = []
            for _ in range(n):
                op = r.u("B")
                if op == 0:
                    rec, _ = read_record(r)
                    k = (rec["ns"], rec["gen"], rec["key"])
                    new[k] = rec
                else:
                    wid = r.u("Q")
                    if wid in by_wire:
                        k = by_wire[wid]
                        removes.append(k)
                        new.pop(k, None)
            self.apply_view(new, removes)
            self.applied = seq
            self.ack(seq)
        elif msg == POSE:
            if seq <= self.applied:
                self.ack(self.applied)
            elif seq != self.applied + 1:
                self.resync(1)
            else:
                self.applied = seq
                self.ack(seq)
        elif msg == HEARTBEAT:
            if seq > self.applied and not (self.staging and self.staging["seq"] >= seq):
                self.resync(3)

    def drain(self, catalog_body):
        cid = self.channels.get(CHANNEL[0])
        keep = []
        for m in self.messages:
            if m[0] == cid:
                try:
                    self.handle(m[1], m[2], catalog_body)
                except (ValueError, struct.error, UnicodeDecodeError) as e:
                    self.bad.append(f"{NAMES[m[1]] if m[1] < 11 else m[1]}: {e}")
            else:
                keep.append(m)
        self.messages = keep


def catalog_message(path):
    data = json.load(open(path))
    rows = sorted(data["rows"], key=lambda r: r["id"])
    body = b""
    digest_in = b""
    for r in rows:
        name = r["name"].encode()
        row = struct.pack("<I16sI", r["id"], uuid.UUID(r["image"]).bytes_le, r["flags"]) + struct.pack("<H", len(name)) + name
        body += row
        digest_in += row
    digest = hashlib.sha256(digest_in).digest()
    if digest.hex() != data["sha256"]:
        raise SystemExit(f"the catalog file's digest {digest.hex()} is not its own sha256 {data['sha256']}")
    return digest, digest + struct.pack("<H", len(rows)) + body, len(rows)


def log_lines(lab, mark):
    p = os.path.join(lab, "kiln", "logs", "kiln.log")
    try:
        with open(p, "rb") as f:
            f.seek(mark)
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--lab", required=True)
    ap.add_argument("--catalog", required=True)
    ap.add_argument("--client-exe", required=True)
    ap.add_argument("--demo", action="store_true", help="check the mod's demo mode (kiln/control/markers-send-test.demo) instead")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    passed = failed = 0

    def check(name, ok, detail=""):
        nonlocal passed, failed
        if ok:
            passed += 1
            print(f"markers-peer: PASS {name}", flush=True)
        else:
            failed += 1
            print(f"markers-peer: FAIL {name} {detail}", flush=True)
        return ok

    digest, catalog_body, rows = catalog_message(a.catalog)
    exe = bytes.fromhex(a.client_exe)

    def wait_log(text, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if text in log_lines(a.lab, 0):
                return True
            time.sleep(0.5)
        return False

    if a.demo:
        if not check("demo.started", wait_log("markers-send-test: demo at", 240), "no demo line"):
            return 1
    elif not check("lab.mod_phase1", wait_log("markers-send-test: INFO phase 1", 240), "the mod did not reach phase 1"):
        return 1
    p = MarkerPeer(a.port, "markers-peer", a.verbose)
    p.hello([], [(CHANNEL[0], CHANNEL[1], False)])
    if not check("kiln.handshake", p.wait(lambda: p.welcome is not None or p.reject is not None, 10) and p.welcome is not None and CHANNEL[0] in p.channels,
                 f"welcome {p.welcome and p.welcome.get('table')} reject {p.reject}"):
        return 1

    def pump(seconds, until=None):
        end = time.time() + seconds
        last_hello = 0
        while time.time() < end:
            if not p.welcomes and time.time() - last_hello > 1:
                p.marker_hello(exe, digest)
                last_hello = time.time()
            p.pump(0.05)
            p.drain(catalog_body)
            if until and until():
                return True
        return until() if until else True

    if a.demo:
        keys = lambda: {k[2] for k in p.view}
        ok = pump(40, lambda: {"demo-map", "demo-compass", "demo-world"} <= keys())
        recs = {k[2]: v for k, v in p.view.items()}
        check("demo.three_markers", ok and recs["demo-map"]["surfaces"] == 1 and recs["demo-compass"]["surfaces"] == 2 and
              recs["demo-compass"]["rgba"] == 0xff0000ff and recs["demo-world"]["surfaces"] == 4 and recs["demo-world"]["text"] == "Server label",
              f"view {recs}")
        open(os.path.join(a.lab, "kiln", "control", "markers-send-test.demo-clear"), "w").close()
        ok = pump(30, lambda: not p.view and "markers-send-test: demo cleared" in log_lines(a.lab, 0))
        check("demo.cleared", ok, f"view {list(p.view)}")
        adds = [k for kind, k in p.events if kind == "added"]
        check("demo.one_added_per_key", sorted(adds) == ["demo-compass", "demo-map", "demo-world"], f"events {p.events}")
        print(f"markers-peer: {passed} PASS, {failed} FAIL", flush=True)
        return 1 if failed else 0
    ok = pump(30, lambda: len(p.welcomes) >= 2 and p.applied > 0)
    check("marker.hello_welcome_catalog", ok and p.welcomes[0][1] == 1 and p.welcomes[-1][1] == 0 and p.welcomes[-1][0] & 1, f"welcomes {p.welcomes}")
    check("marker.snapshot_complete", p.digest_ok >= 1 and any(k[2] == "lab_a" for k in p.view), f"view {list(p.view)} counts {p.counts}")
    ok = pump(60, lambda: any(k[2] == "lab_map" for k in p.view))
    rec = next((v for k, v in p.view.items() if k[2] == "lab_map"), None)
    check("marker.map_marker_received", ok and rec is not None and rec["icon"] == 0xffe5e444 and rec["surfaces"] & 1,
          f"view {list(p.view)} counts {p.counts}")
    # a vanilla lab peer joins: the mod's partial send; it must get no Kiln frame
    v = nt.Client(a.port, "markers-vanilla", a.verbose)
    v.send(nt.LAB_VANILLA)
    time.sleep(0.3)
    v.send(nt.LAB_READY)
    vframes = {}

    def pump_both(seconds, until=None):
        end = time.time() + seconds
        alive = time.time()
        while time.time() < end:
            pump(0.1)
            v.pump(0.05)
            if time.time() - alive > 15:  # the lab transport drops a peer silent for 60 s
                v.send(nt.LAB_VANILLA)
                alive = time.time()
            if until and until():
                return True
        return until() if until else True

    pump_both(120, lambda: "markers-send-test done" in log_lines(a.lab, 0))
    for t, n in v.frame_len.items():
        vframes[t] = n
    check("vanilla.no_kiln_frame", not v.messages and not v.welcome and nt.T_DATA not in v.frame_len and nt.T_FRAG not in v.frame_len,
          f"frames seen {v.frame_len}")
    pump(4)
    adds = [k for kind, k in p.events if kind == "added"]
    check("received.one_added_per_key", sorted(adds) == sorted(set(adds)) and {"lab_a", "lab_map", "lab_b"} <= set(adds), f"events {p.events}")
    removed = [k for kind, k in p.events if kind == "removed"]
    check("received.removed_after_unsend_and_clear", sorted(removed) == ["lab_a", "lab_b", "lab_map"] and not p.view, f"events {p.events} view {list(p.view)}")
    check("received.no_bad_frames", not p.bad, str(p.bad))
    print(f"markers-peer: counts {p.counts}, acks {p.acks}, resyncs {p.resyncs}, events {p.events}", flush=True)
    print(f"markers-peer: {passed} PASS, {failed} FAIL", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
