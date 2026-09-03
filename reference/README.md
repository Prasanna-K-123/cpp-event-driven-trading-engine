# Accepted performance-validation evidence

This directory preserves the compact recruiter-facing evidence behind the performance table in the repository README.

## Frozen workload

- 1,000,000 synthetic events
- seed `20260902`
- identical logical outputs and final deep-state checksum at all three comparison points
- uninstrumented Release wall-clock benchmark and replay-only Valgrind Callgrind profile collected on the same GitHub-hosted environment

`performance_validation.json` records the exact accepted values, full commit SHAs, GitHub Actions run IDs, and SHA-256 digests of the original Actions artifacts.

## Interpretation boundary

Wall-clock numbers are controlled runner/workload measurements, not exchange round-trip latency. Callgrind instruction counts are replay-only dynamic instruction counts, not CPU cycles. The retained comparison supports a profiler-guided optimization claim only; it does not support alpha, profitability, production-exchange latency, or general hardware-independent performance claims.

The accepted final code-validation point is commit `7652e893b527370d19b1500d4f4de6f06806d5ef`, GitHub Actions run `33627360618`.
