# Benchmark protocol

## Purpose

The benchmark measures the event engine on a deterministic synthetic workload so changes can be compared without silently changing the input distribution. It is a systems benchmark, not a market-quality or strategy benchmark.

## Frozen reference workload

- seed: `20260902`
- reference events: `1,000,000`
- initial two-sided depth: 64 price levels per side
- subsequent event mix: approximately 70% passive adds, 15% cancels, 15% market orders
- integer price ticks centered around 100,000
- quantity: uniform integer 1–20 units for generated events

The generator permits some rejected cancels because fills may remove IDs before the generator retires them locally. Rejected cancels are counted and surfaced rather than hidden.

## Reported measurements

The Release executable records:

- total processed events;
- elapsed wall-clock milliseconds;
- events/second;
- average wall-clock nanoseconds/event;
- fills and filled quantity;
- rejected cancels;
- final live-order count and resting quantity;
- deterministic final-state checksum.

The timing section covers replay only; event-vector generation and hash-table capacity reservation happen before the timer starts. Pre-sizing is explicit benchmark setup, not hidden timed work.

## Comparison gate

A performance change is not accepted from timing alone. For the frozen workload, compare the benchmark's semantic outputs and final-state checksum as well as wall-clock performance. The CI deterministic-repeatability gate separately runs the same 200,000-event input twice and requires equality of events, seed, fills, filled quantity, rejected cancels, live orders, resting quantity, and state checksum.

Profiler evidence is complementary: the replay-only Callgrind target measures deterministic instruction counts and hotspot attribution. A credible optimization should have a mechanism consistent with the profile and must pass the normal test and Debug ASan/UBSan jobs.

## Retained validation snapshot

For GitHub Actions run `33627360618` at code commit `7652e893b527370d19b1500d4f4de6f06806d5ef`:

- events: `1,000,000`
- seed: `20260902`
- elapsed: `104.115 ms`
- average: `104.115 ns/event`
- throughput: `9,604,761.103 events/s`
- fills: `284,852`
- filled quantity: `1,573,724`
- rejected cancels: `41,397`
- live orders: `491,440`
- resting quantity: `5,005,317`
- state checksum: `17584781756899873247`

The accepted `a86021a4` comparison point on the same CI environment produced `145.102 ns/event`, `6,891,720.201 events/s`, and the same semantic outputs/checksum. This retained pair is evidence for the optimization under the frozen harness; it is not a universal latency claim.

## Interpretation limits

The reference number depends on runner CPU, compiler, standard library, frequency scaling, and virtualization. It must not be described as exchange round-trip latency or production feed-handler latency. Performance comparisons are valid only when the environment and workload are held sufficiently constant.
