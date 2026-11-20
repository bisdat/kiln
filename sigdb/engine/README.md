# Engine profiles

Per-build engine profiles of ShroudForge, copied verbatim. Each file is the profile of one exact executable; kiln.engine
(`native/services/engine/`, docs/kiln/services/engine.md) selects one only when the running exe matches it exactly.

Licence: MIT, Copyright (c) 2026 bonsaibauer (`licenses/ShroudForge-LICENSE.txt`, `CREDITS.md`). Source: ShroudForge
`modules/game-runtime/profiles/enshrouded/<side>/<build>.json` at upstream `bonsaibauer/shroudforge` 38ef448d (1.8.0);
byte-identical in the 1.7.9 fork pin d8a394bc.

| File | sha256 of the file | Exe | Exe sha256 | TimeDateStamp | SizeOfImage |
|---|---|---|---|---|---|
| `client/26430625.json` | e6d2afca2636e7ea89f9a3548c1c93ccb97cac78b5cd234522e2c4546bc9ddf7 | enshrouded.exe | 5d9ef2f1…0c77 | 1793791052 | 44298240 |
| `server/26430253.json` | 08a4a910b7e26e8a1d4cfd1b7ff5a8b76f4a72914eeeee3bd6e0f8707b6c87f0 | enshrouded_server.exe | f0d824be…aeb2 | 1793791038 | 31318016 |

Do not edit a profile by hand; copy a new one from ShroudForge, then:

```sh
python3 -I tools/engine/generate.py                     # native/services/engine/profiles.g.h (--check: drift)
python3 -I tools/engine/test_generate.py                # ShroudForge's rules
python3 -I tools/engine/validate.py <profile> <a read-only COPY of that exe>   # never the installed game, never run
```

Static validation (2026-11-15, read-only copies of the two exes): 23/23 checks pass for the client profile, 22/22 for the
server profile (sha256, PE TimeDateStamp and SizeOfImage, file name, the change tracker's 33 guard bytes at its RVA, every
code RVA a `.pdata` function start in `.text`, the thread registry lock/rows/count in `.data`, owner vtables in `.rdata`
pointing into `.text`, the "GameServer" and "KfcResourceContent" thread names present).
