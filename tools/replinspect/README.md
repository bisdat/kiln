# replinspect

An offline parser for the capture files of `kiln.repl`: `repl capture <peer> <seconds>` on the server writes
`kiln/logs/repl-<peer>-<utc>.jsonl`, every replication message the peer was sent in that time. replinspect parses each
message's bytes by the wire formats of the replication chapter (§2.3: entity messages, their bit-stream payload,
generic deltas, destroy lists) and checks that **every payload byte is accounted for**. Python 3, standard library only.

```
python3 tools/replinspect/replinspect.py check <capture.jsonl> [--json]
python3 tools/replinspect/replinspect.py show <capture.jsonl> [--entity N] [--kind frame|handshake|create|update|destroy]
python3 tools/replinspect/replinspect.py registry <capture.jsonl>
```

- `check` prints a summary (messages and bytes per kind and channel, entities, gaps, dropped records), every warning
  and every problem with its line number. Exit 0 when every message accounts for every byte and agrees with its JSON
  fields, 1 when a problem was found, 2 on bad usage or a file without a header line. `--json` prints the summary as
  one JSON document (`ok`, `messages`, `clean`, `bytes`, `by_kind`, `by_channel`, `entities`, `gaps`,
  `dropped`, `complete`, `problems` and `warnings` as `{"line", "text"}` lists).
- `show` prints one line per message: line number, tick, seq, channel, kind, entity, size, the baseline of an update
  (`(A)` = the acknowledged state, `template` = a delta against the template), the removed, added and changed
  components by name, each changed one with its blob size, `raw` or `delta` and its version. `--entity` also
  matches destroy lists naming the id.
- `registry` prints the header's registry as TSV: `index name hash size flags` (hash as hex).

Tests (hand-made byte vectors, one per format rule):
`python3 -m unittest discover -s tools/replinspect -p 'test_*.py'` from the repository root.

## The capture file (version 1)

JSON Lines, UTF-8. Readers ignore fields they do not know, and lines of a type they do not know.

**Line 1, the header:**

```
{"type":"header","version":1,"peer":<u32 peer handle>,"seconds":<n>,"utc":"<ISO 8601>",
 "registry":[{"index":<registry index>,"name":"<component name>","hash":<u32>,"size":<bytes of one component>,
              "flags":<bits>}, ...]}
```

Registry flags: 1 replicated, 2 versioned, 4 custom codec, 8 may be on the client channel, 16 may be on the prediction
channel.

**One line per message the peer was sent:**

```
{"type":"msg","t":<utc ms>,"peer":<u32>,"ch":4|5,"msg":<entry type>,"kind":"<kind>","entity":<u32, 0 when none>,
 "tick":<u32>,"seq":<u32>,"base":<u32>,"bytes":<whole message size>,"gap":<bool>,"raw":"<hex of the whole message>",
 "components":{"<name>":"<hex>", ...}}
```

| field | meaning |
|---|---|
| `ch` | 4 the client channel, 5 the prediction channel |
| `msg` | entry type: 0 frame, 2 handshake, 4 entity, 5 destroy list |
| `kind` | `frame` (msg 0), `handshake` (msg 2), `create` or `update` (msg 4), `destroy` (msg 5) |
| `tick`, `seq` | the message header's server tick and message counter |
| `base` | the baseline tick of an update, else 0 (replinspect accepts it with or without bit 31, the "acked state A" bit) |
| `bytes` | the size of the whole message (`raw`) |
| `gap` | records were dropped before this one |
| `components` | optional, entity messages only: the full component values of the snapshot the server coded (the bytes before delta coding) |

A destroy message also has `"a":[ids destroyed]` and `"b":[ids that left relevance]`.

**Last line:** `{"type":"end","messages":<n>,"dropped":<n>,"utc":"<ISO 8601>"}`. A file without it (the server stopped
during the capture) is still read, with a warning; so is a cut-off last line.

## What check verifies

Entity message (entry type 4), little-endian:
`u32 (tick << 3) | kind` (2 create, 3 update), `u32 seq`, `u32 entity`; a create then `u32 parent`, `GUID[16]`,
`u8 0`; an update `u32 baseline` (bit 31 = against the acknowledged state A) and `GUID[16]` only when the baseline
(without bit 31) is 0; then `u16 length` and the payload, with nothing after it.

Payload, a bit stream read least significant bit first: 3 flag bits (removed, added, changed lists present); the
counts (removed 6 bits, added 6, changed 8) of the lists present; removed entries `idx:10`; added entries
`idx:10 v:1 [version:8]`; changed entries `idx:10 v:1 [version:8] (size << 1 | isDelta):16`; padding to a byte; then
the changed components' blobs in order. **Header bytes + blob sizes must equal the payload length.**

- A raw blob (isDelta 0) must have the registry size and, when the record carries the component's value in
  `components`, equal it.
- A delta of a component without the custom-codec flag is a generic delta: runs of
  `[unchanged count][changed count][changed bytes XOR baseline]`, each count as 0xFF bytes (255 each) then one byte
  below 0xFF, nothing after the last changed byte. The runs must end exactly at the blob's end, change at least one
  byte each and fit the component's size. A generic delta not smaller than the raw bytes is a warning (the writer
  sends those raw).
- A custom-codec delta is only sized.
- Every registry index named must exist in the registry.

Destroy list (entry type 5): `u32 (tick << 3) | 4`, `u32 seq`, `u8 nA`, `u8 nB`, `(nA + nB) x u32` ids (list A =
destroyed, list B = left relevance), nothing after. Frames and handshakes are counted, not parsed.

Every message: `bytes` equals the raw length, `ch` is 4 or 5, `kind` goes with `msg`, and `kind`, `tick`, `seq`,
`entity`, `base` (and `a`, `b`) agree with the raw header.

Warnings (do not fail the check): no end line, an end line whose count differs, non-zero padding bits, a create byte
that is not 0, a version on a component the registry does not flag versioned, a `components` name not in the
registry, a peer other than the header's.
