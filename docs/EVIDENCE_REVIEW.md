# Evidence review: C++20 Event-Driven Trading Engine

Independent portfolio research. Updated 2026-10-09. Development and documentation include AI assistance; the committed executable code, data provenance and test outputs establish the work products. No institutional endorsement or third-party authorship review is claimed.

## Research purpose

Quantitative engineering, matching and verification.

## Published evidence

25,000 randomized events; 5 seeds; every fill and all 256 order-ID slots checked against an independent vector-based oracle.

## Interpretation boundary

Synthetic replay and platform-specific historical benchmark; not live exchange latency or production trading.

## Review standard

Check the source data and split before interpreting a score. Compare the strongest result with a simple baseline. Inspect failed diagnostics and uncertainty. Reproduce the calculations from the documented environment; distinguish measured findings from simulated or assumed scenarios. The tests cover specific documented invariants and do not establish complete production correctness.

## Next research extension

Expand the differential corpus, add recovery/snapshot semantics and investigate workload-specific profiling with a fixed baseline. Any performance result requires machine, compiler, input workload and correctness evidence.
