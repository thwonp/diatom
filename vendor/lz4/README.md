# lz4, vendored

LZ4 block compression for the rewind ring (`src/rewind.c`, plorpos-gkd.59):
each snapshot is XORed against the previous one and LZ4-compressed on a worker
thread, so the same memory budget holds far more history.

- Upstream: https://github.com/lz4/lz4
- Version: `v1.10.0` - `lib/lz4.c`, `lib/lz4.h`, `lib/LICENSE`, unmodified.
- **BSD 2-Clause** - see `LICENSE`. Permissive; compatible with this fork's
  license and brings no copyleft in.

Vendored rather than linked because the GKD's ROCKNIX image ships no
`liblz4.so`, and a two-file library compiled in costs less than shipping and
locating a shared object on every port.

Only the block format is used (`LZ4_compress_default`, `LZ4_decompress_safe`).
The frame format (`lz4frame.c`) and HC (`lz4hc.c`) are deliberately not
vendored: rewind entries never leave memory, and HC trades the speed rewind
needs for ratio it does not.

Do not patch these files - a local fix goes invisible at the next update.
