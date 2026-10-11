# Bounded filesystem journal: acknowledgment, integrity and recovery

This extension adds a real Linux regular-file journal alongside the original
matching engine and portable in-memory image. It records events, requests file
and directory persistence, replays validated records, refuses uncertain writes,
and checks actual process crashes. It is an independently reproducible systems
prototype with explicit assumptions, developed with AI assistance.

## Evidence and registration

The initial contract, implementation, independent Python oracle and fixed
budgets were published before execution in commit
`0fb303eeee20085f01499d8cb218113929ae9c5f`. The fixed local study completed
without a mismatch. A subsequent source review found that extreme unbounded
quantities could overflow the original matcher's totals. The journal-specific
quantity ceiling and three refusal checks were transparently registered in
amendment `e99d8fe6ab723219599af3884ebe9bf6323a4211` before main publication.
The original matcher stays byte-identical; all eight primary study files remain
byte-identical. Seeds, cases and outputs were not selected or retuned.

| Executed check | Result and exact scope |
|---|---|
| Actual journal appends | 4,096 events, four fixed seeds, file fsync before each returned acknowledgment |
| Actual file reopens | 64; complete canonical state matches uninterrupted replay |
| Independent decoder | All 4,096 frames match the separate input ledger, big-endian fields and standard-library CRC |
| Independent vector matcher | All 64 exported states match quantities, price/time/FIFO order, chronology and seven counters |
| Process crashes | All 288 cases: nine fixed boundaries x 32 variations; pipe proves readiness before SIGKILL |
| Corrupt complete record | All 1,920 individual bit flips in a three-record/240-byte fixture refuse; bytes unchanged |
| Truncated prefix | All 240 shorter prefixes recover their complete valid records, report partial-tail length, allow checked reappend and reopen |
| Resealed malformed input | 13 invalid structure/chronology/domain cases refuse despite recomputed valid CRCs |
| Record splice | Swapped, missing-middle and duplicated records refuse |
| I/O faults | 14 declared scenarios covering short/EINTR I/O, zero writes, ENOSPC, read/sync/truncate failures and recovery refusals |
| Invalid append | Eight scenarios leave file/state unchanged; no poisoned handle for pre-write validation failure |
| Path/size guards | Five cases cover final symlink, FIFO, over-limit file, missing parent and null I/O provider |
| Writer/fork guards | Concurrent cooperating open refuses; inherited child handle and separate child open refuse |
| Local build/checks | All five Release and ASan/UBSan test executables pass; local leak inspection disabled for the execution environment |

The original independent matcher still checks 25,000 events/every fill/all256
ID slots. The separate in-memory recovery study still checks another 25,000
events, 685 restores, 1,304 bit flips, 163 truncations and eight malformed
images. Their source, tests, fixtures, benchmark and original workflow are
unchanged. These are three distinct studies; do not add their counts together
as one independent workload or call test cases statistical confidence evidence.

## What a returned acknowledgment means

The constructor opens a trusted, stable, pre-existing parent directory and
regular file, refuses the final symlink, and takes a nonblocking exclusive
advisory lock. It validates and replays every complete record, then synchronizes
the file and directory before returning a usable handle. It does not create a
directory tree or promise protection against namespace/hardlink races.

For each append:

1. Validate event bounds and process a private candidate restored from the
   current canonical image. Duplicate live IDs, invalid quantity/type/side,
   duplicate/regressing feed sequence and timestamp regression fail before I/O.
2. Encode a complete fixed-width record, continuing short writes and retrying
   EINTR. A zero write or other error fails.
3. Request `fsync(file_fd)`. Only successful completion permits state installation.
4. Move the candidate into the handle with a compile-time checked noexcept move,
   advance ordinal/CRC, and return the journal ordinal.

A write/sync error returns no acknowledgment and poisons the handle. It must
be closed and recovered before another append. For the tested append sync
failure, the previous in-memory state remains unchanged but a complete new
record remains readable and recovers locally. The error is therefore an
**uncertain outcome**, not proof the event never happened. A caller cannot
blindly retry an unacknowledged business operation or infer distributed
exactly-once delivery. This prototype has no external client acknowledgment
ledger or order-routing protocol.

Each record is 80 bytes: magic8/version4/length4/ordinal8/timestamp8/feed-sequence8/
type1/side1/reserved6/ID8/signed-price8/signed-quantity8/previous-CRC4/CRC4.
All integers are big-endian; signed fields preserve their bit representation.
Ordinal is contiguous while feed sequence only needs to increase; the workload
deliberately uses sequence gaps. CRC is IEEE CRC32 over the first76 bytes, and
each record carries the prior CRC. It detects the tested accidental corruption
and splices; it is **not authentication against a writer who can reseal records**.

The journal is bounded at100,000 records/8,000,000 bytes and positive event
quantity<=1,000,000,000. Total submitted, filled or resting quantity is therefore
<=100,000,000,000,000; counters and level totals remain far below64-bit limits.
This restriction belongs to the journal API. It does not repair or certify the
original unbounded matcher for arbitrary hostile numeric inputs. Access must
be serialized by the caller; a handle inherited across fork cannot append.

## Recovery and the failure boundary

A complete record must pass magic, version, length, ordinal, reserved fields,
prior CRC, current CRC, domain and original engine sequence/state validation.
Any invalid complete record refuses the entire open before tail repair; the
tests confirm those bytes are untouched. There is no silent interior skip or
"best effort" truncation of a damaged complete record.

Under the append-only partial-write model, fewer than80 final bytes are an
incomplete unacknowledged tail. Recovery reports the exact length, truncates to
the validated prefix, synchronizes it, and only then allows more writes. If
truncate or synchronization fails, construction refuses. A storage failure
that removes a whole acknowledged record, or makes acknowledged data appear
as an incomplete tail, lies outside this recovery assumption. Without an
external retained high-water mark the journal cannot detect complete suffix
deletion. The240-prefix study exposes this limitation rather than claiming
every shorter file proves safe data loss.

All nine process boundaries were reached32 times: before write; after1/7/40/79
bytes; after complete write before sync; after sync before applying memory;
after memory before acknowledgment; and after append returned. The parent
receives a pipe notification, sends SIGKILL, verifies the actual killed status,
then opens a new handle. Partial cases recover the previous three records and
report the exact torn bytes. Complete cases recover all four records, including
the unacknowledged pre-sync case.

**SIGKILL leaves the kernel and page cache alive.** In particular, recovery of
the complete pre-sync write says nothing about persistence across a power cut,
kernel crash or reboot. Tests do not force the physical storage to lose cached
data. The primary environment records filesystem magic2035054128 (overlayfs);
CI records its actual filesystem separately. Passing on these filesystems does
not validate every device, controller, mount or filesystem configuration.

## Compared acknowledgment policies

The [99-row model](../reference/journal/policy_model.json) compares three policies
at all0..32 written-record boundaries under an explicit idealized model: successful
sync advances the durable prefix and a modeled power cut discards every unsynced
byte. This is a mathematical state model, **not a physical power-loss experiment**.

| Policy | Maximum modeled acknowledged loss | Tradeoff in this fixed model |
|---|---:|---|
| Acknowledge after write only | 32 records | Negative control violates the chosen persistence contract |
| Sync each record, then acknowledge | 0 | Implemented prototype; no latency result inferred |
| Sync groups of16, then acknowledge the group | 0 | Up to15 written events remain unacknowledged; policy model only |

No group-commit implementation, latency advantage or production throughput is
claimed. The candidate-image copy and per-record sync favor transparent
correctness over speed and run outside the original benchmark event path.
Historical145.1→104.1ns timing remains tied to its original runner/workload.

## Reproduce and review

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
python3 verify_snapshot_fixture.py --exe build/qdev_snapshot_tests
./build/qdev_journal_study results/journal-replay
python3 verify_journal.py --exe build/qdev_journal_tests --replay results/journal-replay
```

Use a fresh output directory: the study refuses overwriting an earlier executed
ledger. The Python verifier uses independent standard-library struct/zlib
decoding, a vector matcher and independently constructed canonical bytes.
All seven semantic result files must reproduce byte-for-byte; summary differs
only where it records the actual filesystem magic. The registration receipt
guards all nine method/configuration identities. The original registration CI
executed tests/study/sanitizers but skipped result comparison because reference
outputs were not yet committed; it is not the final exact-reference check.
The [current workflow](../.github/workflows/journal.yml) repeats the full fixed
study and exact comparison when published results exist. The historical
workflow additionally runs million-event semantic and Callgrind regression gates.

Primary references: Linux man-pages for [write](https://man7.org/linux/man-pages/man2/write.2.html),
[fsync](https://man7.org/linux/man-pages/man2/fsync.2.html),
[flock](https://man7.org/linux/man-pages/man2/flock.2.html), and
[open](https://man7.org/linux/man-pages/man2/open.2.html). In particular, write
completion alone does not establish persistence; the directory entry needs a
separate synchronization request; the file lock protects cooperating writers.

Future distinct work includes real restart/power-loss injection on controlled
storage, batching with measured latency, retained external acknowledgment
high-water marks, snapshot/journal rotation with namespace crash semantics,
and genuine feed/session reconciliation. No exchange integration, authentication,
high availability, practitioner endorsement or production trading is implied.
