# Architecture and correctness model

## Event contract

Every event carries a nanosecond timestamp and an integer sequence. Timestamps may tie, but sequence values must increase strictly. The event engine rejects regressions before mutating state. This makes replay ordering explicit rather than relying on container iteration or wall-clock timing.

`ADD` events contain a side, order ID, limit price in integer ticks and positive quantity. An order ID must be unique while the order is live; it may be reused after the prior order has been completely filled or cancelled. `CANCEL` events reference an order ID and cancel up to the requested quantity. `MARKET` events consume best-priced opposite-side liquidity until filled or depth is exhausted.

## Book representation

Price levels use ordered maps so best bid/ask lookup is deterministic. Each level stores aggregate remaining quantity plus head/tail order IDs. Live orders are stored in a hash index and carry `prev_id` / `next_id` links, producing an intrusive FIFO chain per price level.

This design makes full cancellation **O(1) with respect to queue length** after the order-ID hash lookup: the cancelled node is unlinked directly from its neighbors instead of leaving a tombstone or scanning a deque. It also allows an order ID to be safely reused after the old order has been fully removed, because no stale FIFO entry remains.

The matching path validates that every level head resolves to a live order on the same side and price. Empty-level head/tail/count/quantity invariants are checked when the final node is unlinked.

## Matching semantics

A marketable buy limit consumes asks while the best ask is less than or equal to its limit. A marketable sell limit consumes bids while the best bid is greater than or equal to its limit. Executions occur at the resting maker price. Any unfilled residual rests at the incoming limit price.

## Fill handling

`OrderBook` exposes caller-owned fill-vector overloads for `add_limit` and `execute_market`. The vector is cleared at the start of each successful call and existing capacity is reused. Convenience overloads that return `std::vector<Fill>` remain available for tests and tools.

`EventEngine` owns one reusable fill scratch vector and uses the caller-owned overloads on the replay hot path. Fill contents are transient execution output and are deliberately excluded from persistent book state and the deep-state checksum.

## Invariants and build modes

The matching and unlinking code contains structural invariant checks for invalid FIFO/index/level state. In addition, Debug builds check `book_.is_crossed()` after every processed event. The GitHub sanitizer job is a Debug build, so ASan/UBSan validation retains this per-event crossed-book assertion.

Release builds compile the redundant top-of-book crossed check out of the timed replay hot path under `NDEBUG`. Release correctness is still gated by the same deterministic replay tests, matching semantics, deep-state checksum, and a separate Debug sanitizer/invariant run before an optimization is accepted.

The validation suite checks:

1. price-time priority at equal prices;
2. price priority across multiple levels;
3. no crossed book after controlled event processing;
4. partial cancellation changes level depth exactly;
5. deterministic checksum under identical replay;
6. timestamp/sequence regressions are rejected;
7. quantity is conserved across a controlled execution path;
8. middle-of-FIFO cancellation removes the node without disturbing neighboring priority;
9. an order ID may be safely reused only after the prior order is fully removed;
10. the deterministic checksum distinguishes deep-book state, not only top of book;
11. caller-owned fill buffers replace prior contents and reuse sufficient capacity;
12. fill accounting remains exact when the engine reuses its scratch buffer.

The project prefers explicit correctness properties over a large number of shallow example tests.
