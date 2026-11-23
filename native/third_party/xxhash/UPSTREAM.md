# xxHash

- Source: https://github.com/Cyan4973/xxHash, release tag `v0.8.3`.
- Files: `xxhash.h` and `LICENSE`, fetched from the official repository's tagged release. Unmodified.
- Licence: BSD-2-Clause (`LICENSE`, copied from upstream).
- Used by: kiln.items player storage witness. Included with `XXH_INLINE_ALL`; no external xxHash symbols.
- The release notices collector (`tools/release/lib/notices.py`) includes this directory's `LICENSE` automatically,
  like the other vendored code under `native/third_party`.
