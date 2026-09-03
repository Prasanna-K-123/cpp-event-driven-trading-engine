# High-Performance C++ Event-Driven Trading / Backtest Engine

[![Validation](https://github.com/Prasanna-K-123/cpp-event-driven-trading-engine/actions/workflows/validation.yml/badge.svg?branch=main)](https://github.com/Prasanna-K-123/cpp-event-driven-trading-engine/actions/workflows/validation.yml)

A C++20 systems project focused on mechanics quantitative developers are expected to reason about directly: deterministic event replay, price-time-priority matching, cancellation semantics, marketable limits, state invariants, reproducible benchmarking, and profiler-guided optimization.

This project is deliberately **not** an alpha claim. It is an execution-systems artifact: correctness first, followed by measured optimization on a controlled workload.

## Recruiter snapshot

| Signal | Verified evidence |
|---|---|
| Matching mechanics | two-sided price-time priority, marketable limits, partial/full cancels, market orders and maker-price fills |
| Cancellation path | live-order hash index + intrusive per-price FIFO; cancellation is **O(1) with respect to queue length after the order-ID hash lookup** |
| Correctness | adversarial unit tests, Debug ASan/UBSan, deep-state checksum and exact 1,000,000-event deterministic replay |
| Accepted optimization | **145.102 → 104.115 ns/event** and **140,001,502 → 100,873,271** replay instructions on the frozen historical runner/workload comparison |
| Semantic preservation | fills, filled quantity, rejected cancels, live orders, resting quantity and final-state checksum remain identical across the accepted optimization sequence |

**Direct evidence:** [`performance validation`](reference/performance_validation.json) · [`reference policy`](reference/README.md) · [`architecture`](docs/ARCHITECTURE.md) · [`benchmark protocol`](docs/BENCHMARK_PROTOCOL.md) · [`profiling methodology`](docs/PROFILING.md)

Wall-clock nanoseconds are intentionally **not** a CI reproducibility gate because hosted-runner timing varies. Deterministic state outputs and replay-only Callgrind instruction evidence are the stronger stable checks.

## What is implemented

- C++20 event model with nanosecond timestamps and strictly increasing sequence numbers;
- two-sided price-time-priority limit order book;
- live-order hash index plus intrusive per-price FIFO links for O(1)-with-respect-to-queue-length cancellation after the order-ID hash lookup;
- marketable limit matching at resting maker prices;
- partial/full cancellation handling;
- market-order execution against best available prices;
- deterministic replay engine with event/fill/accounting statistics;
- caller-owned/reusable fill buffers on the hot event path, while retaining convenience value-returning book APIs;
- non-crossed-book invariant checking after every event in Debug/sanitizer builds, with structural matching invariants and regression tests retained for Release validation;
- deterministic deep-state checksum for replay equivalence tests;
- explicit order-index capacity reservation outside timed replay in the benchmark harness;
- CSV replay CLI for external event streams;
- adversarial tests for FIFO/price priority, crossing behavior, middle-queue cancellation, safe post-removal ID reuse, deep-state determinism, ordering guards, quantity conservation, and fill-buffer reuse;
- fixed-seed synthetic benchmark workload;
- Release benchmark output in machine-readable JSON;
- AddressSanitizer/UndefinedBehaviorSanitizer validation;
- replay-only Valgrind Callgrind profiling with machine-retained hotspot evidence;
- GitHub Actions validation for build, tests, sanitizer gates, benchmark, deterministic checksum repeatability, and replay-only profiling.

## Validated performance snapshot

The current optimization sequence was accepted against the frozen 1,000,000-event workload (`seed=20260902`) on the same GitHub Actions environment (Ubuntu 24.04, GCC 13.3.0, CMake 3.31.6, Valgrind 3.22.0).

| Validation point | Avg. ns/event | Events/s | Callgrind replay instructions |
|---|---:|---:|---:|
| `a86021a4` baseline | 145.102 | 6,891,720 | 140,001,502 |
| `f1a75fff` reusable fill buffer | 130.760 | 7,647,586 | 108,873,262 |
| `7652e893` Release hot-path invariant optimization | **104.115** | **9,604,761** | **100,873,271** |

From `a86021a4` to `7652e893`, the retained validation evidence shows **28.25% lower average replay ns/event**, **39.37% higher throughput**, and **27.95% fewer Callgrind replay instructions**, while fills, filled quantity, rejected cancels, final live orders, resting quantity, and the deterministic final-state checksum remain identical.

These are controlled runner/workload comparisons, **not** exchange-production or round-trip latency claims. The final code-validation snapshot is GitHub Actions run `33627360618` for commit `7652e893b527370d19b1500d4f4de6f06806d5ef`.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For the sanitizer/invariant path:

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DQDEV_SANITIZE=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

## Benchmark

```bash
mkdir -p results
./build/qdev_bench --events 1000000 --seed 20260902 --json results/benchmark.json
```

The benchmark reports wall-clock throughput and average wall-clock nanoseconds per processed event for a fixed synthetic mix. These numbers are **hardware/compiler/workload dependent** and are not presented as exchange-production latency.

## Profiling

CI installs Valgrind, builds the dedicated `qdev_profile` target with `QDEV_CALLGRIND=ON`, and starts Callgrind instrumentation only immediately before `EventEngine::replay`. Synthetic event generation and benchmark setup therefore remain outside the collected instruction-count profile.

The profile answers which engine functions and data-structure operations dominate the replay workload; it is not used as a substitute for the uninstrumented Release benchmark.

## Evidence boundary

- Synthetic events exercise engine mechanics; they do not reproduce any exchange's complete feed semantics.
- The book is a single-instrument in-memory simulator, not a production exchange gateway.
- No kernel bypass, lock-free network stack, NUMA pinning, hardware timestamping, or real exchange protocol is claimed.
- Benchmark results are environment-specific and must include compiler/runner metadata before being compared.
- No profitability, alpha, Sharpe, or trading recommendation claim is made.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), [`docs/BENCHMARK_PROTOCOL.md`](docs/BENCHMARK_PROTOCOL.md), and [`docs/PROFILING.md`](docs/PROFILING.md).
