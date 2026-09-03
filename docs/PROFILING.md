# Profiling protocol

The profiling evidence uses Valgrind Callgrind rather than relying on sampled symbol attribution. CI installs Valgrind and builds a dedicated `qdev_profile` executable only when `QDEV_CALLGRIND=ON`.

The benchmark prepares the deterministic synthetic event vector and reserves expected order-index capacity before profiling begins. Callgrind is launched with `--instr-atstart=no`; the executable then issues `CALLGRIND_START_INSTRUMENTATION` and `CALLGRIND_ZERO_STATS` immediately before `EventEngine::replay`, and stops instrumentation immediately after replay. The retained `callgrind.out` and `callgrind_annotate` report therefore target engine replay rather than random-number generation or setup work.

Profiling and wall-clock benchmarking answer different questions. The ordinary Release benchmark is uninstrumented and reports throughput/ns-per-event. Callgrind reports deterministic instruction-level hotspot evidence under the same event semantics, but its instrumented runtime is not interpreted as production latency.

## Profile-guided optimization record

The accepted baseline `a86021a4` showed substantial allocation/free cost around per-event fill-vector creation. Replacing that path with an `EventEngine`-owned reusable fill buffer produced commit `f1a75fff`, while retaining caller-friendly value-returning `OrderBook` wrappers. On the same CI environment and fixed workload, the 1,000,000-event benchmark improved from `145.102` to `130.760 ns/event`; replay Callgrind instructions fell from `140,001,502` to `108,873,262`. Semantic outputs and the final checksum remained identical, and the full test/sanitizer pipeline passed.

The new profile then exposed `OrderBook::is_crossed()` as a measurable redundant Release hot-path cost. Commit `7652e893` kept that defensive per-event assertion in Debug/sanitizer builds but compiled it out of Release under `NDEBUG`. The final retained run `33627360618` produced `104.115 ns/event`, `9,604,761.103 events/s`, and `100,873,271` replay instructions, again with identical semantic outputs/checksum and a full CI pass.

Relative to the `a86021a4` baseline, the retained evidence is therefore:

- average replay ns/event: **-28.25%**;
- throughput: **+39.37%**;
- replay instruction count: **-27.95%**.

These values describe this fixed synthetic workload and CI environment only.

## Current residual profile

At `7652e893`, the dominant retained replay instruction costs are primarily structural/data-container work rather than the eliminated per-event fill allocation/check path. The largest entries include `OrderBook::rest`, allocator internals associated with map/hash nodes, event replay/dispatch, `OrderBook::add_limit`, unlinking, matching, and hash lookup.

A more aggressive next step would likely require materially changing allocation/data-structure architecture (for example, pooled/node storage or different index/level representations), not another trivial hot-path edit. Such a redesign carries a larger correctness surface and should be undertaken only if its incremental recruiter/engineering signal justifies the added scope.

## Acceptance rule

Any future optimization must be justified by measured hotspot evidence and re-run against:

1. Release build and unit/invariant tests;
2. Debug ASan/UBSan tests;
3. frozen Release benchmark;
4. deterministic-repeatability gate;
5. replay-only Callgrind profile;
6. semantic-output and deep-state-checksum comparison.
