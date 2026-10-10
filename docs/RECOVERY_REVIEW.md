# Portable snapshot recovery and adversarial image checks

`EventEngine::snapshot()` exports a canonical version 1 big-endian image. `EventEngine::restore(bytes)` returns a new engine, so a rejected image cannot mutate an existing engine. The image preserves every live order, price/time/FIFO arrival, counters and the last accepted timestamp/sequence. Hash-map iteration order is never serialized as FIFO order.

Format: QDEVSNAP magic,32-bit version, 1-byte started flag,64-bit timestamp/sequence, seven 64-bit counters, 64-bit live-order count, 33-byte order records (ID,side,price,quantity, arrival),and a 32-bit IEEE CRC. Images are capped at 16 MiB; lengths/counts are checked before allocations. Unknown versions/flags/sides, zero/negative prices or quantities, duplicate IDs/arrival sequences, crossed books, aggregate quantity overflow, inconsistent counters/chronology, trailing/truncated bytes and CRC mismatches are rejected. Event validation also rejects invalid side/type enums.

CRC detects accidental corruption; **it is not cryptographic authentication or protection against an attacker who can reseal an otherwise valid image**. This is an in-memory image/restore API. It does not implement a durable journal, filesystem writes, fsync, power-loss recovery, network feed resumption,an exchange protocol or an exactly-once distributed system. Such claims remain out of scope.

## Executed checks

- 25,000 events across five seeds, with **685 restorations**. After every event, uninterrupted and repeatedly restored engines have identical canonical bytes/checksum and all 256 live-ID slots, including previous/next FIFO links.
- All **1,304 single-bit flips** in a163-byte two-order fixture rejected.
- All **163 truncation prefixes** rejected.
- Eight correctly resealed malformed structure cases rejected: version,flags,count,side,duplicate ID,repeated arrival,zero price,zero quantity.
- After restore, duplicate sequence and time regression still refuse without changing state; subsequent market execution preserves maker FIFO.
- All four optimized C++ test executables passed, including the separate existing vector matching oracle's 25,000 events.
- New snapshot checks passed ASan/UBSan locally with `detect_leaks=0` because this environment's LeakSanitizer process inspection is restricted. Remote full sanitizer state is reported separately from actual CI; do not infer it from this local workaround.
- [Independent Python decoder](../verify_snapshot_fixture.py) checks C++ output with standard-library `zlib.crc32`, big-endian `struct` decoding and an immutable [format fixture](../reference/recovery/fixture.hex). This reduces shared-implementation self-check risk; it is an automated oracle, not a practitioner endorsement.

## Reproduce

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
python3 verify_snapshot_fixture.py --exe build/qdev_snapshot_tests
```

The release workflow also runs the four tests under ASan/UBSan, million-event semantic gates and the existing performance regression protocol. Snapshot methods are not invoked on the ordinary benchmark event path; enum checks are a source change, so the historical 145.1→104.1ns result remains explicitly historical. No new universal latency record is claimed. The byte-format fixture verifies compatibility of this version, not every future compiler/platform. Code and documentation used AI assistance.
