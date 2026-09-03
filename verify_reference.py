#!/usr/bin/env python3
"""Validate retained QDEV evidence and current deterministic benchmark semantics.

Wall-clock timing is intentionally not a pass/fail gate because it is runner-dependent.
The accepted historical timing values remain auditable in the reference pack.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

REFERENCE = Path("reference/performance_validation.json")
SEMANTIC_KEYS = (
    "fills",
    "filled_quantity",
    "rejected_cancels",
    "live_orders",
    "resting_quantity",
    "state_checksum",
)


def pct_change(new: float, old: float) -> float:
    return (float(new) / float(old) - 1.0) * 100.0


def parse_callgrind_summary(path: Path) -> int:
    summaries: list[int] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("summary:"):
            token = line.split(":", 1)[1].strip().split()[0].replace(",", "")
            summaries.append(int(token))
    if not summaries:
        raise AssertionError(f"Callgrind summary not found in {path}")
    collected = max(summaries)
    if collected <= 0:
        raise AssertionError(f"Callgrind collected no replay instructions in {path}: {summaries}")
    return collected


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark", type=Path)
    parser.add_argument("--callgrind", type=Path)
    parser.add_argument("--output", type=Path, default=Path("results/reference_verification.json"))
    args = parser.parse_args()

    ref = json.loads(REFERENCE.read_text(encoding="utf-8"))
    workload = ref["reference_workload"]
    assert workload == {"events": 1_000_000, "seed": 20260902}, workload

    points = ref["validation_points"]
    expected_labels = ["baseline", "reusable_fill_buffer", "release_hot_path"]
    assert [p["label"] for p in points] == expected_labels
    assert points[0]["commit"] == "a86021a4a7d05848a9d8abef6f704b5901434d6c"
    assert points[1]["commit"] == "f1a75fff7dc5aae958f66659c5a333fa1e96a48a"
    assert points[2]["commit"] == "7652e893b527370d19b1500d4f4de6f06806d5ef"
    assert points[0]["github_actions_run"] == 33625315672
    assert points[1]["github_actions_run"] == 33626558691
    assert points[2]["github_actions_run"] == 33627360618

    semantic_snapshots = [tuple(p["benchmark"][k] for k in SEMANTIC_KEYS) for p in points]
    assert semantic_snapshots[0] == semantic_snapshots[1] == semantic_snapshots[2], semantic_snapshots

    baseline = points[0]
    final = points[-1]
    accepted = ref["accepted_change_vs_baseline"]
    recomputed = {
        "average_ns_per_event_pct": pct_change(
            final["benchmark"]["average_ns_per_event"], baseline["benchmark"]["average_ns_per_event"]
        ),
        "throughput_pct": pct_change(
            final["benchmark"]["throughput_events_per_sec"], baseline["benchmark"]["throughput_events_per_sec"]
        ),
        "callgrind_instruction_pct": pct_change(
            final["callgrind_replay_instructions"], baseline["callgrind_replay_instructions"]
        ),
    }
    for key, value in recomputed.items():
        assert math.isclose(value, float(accepted[key]), rel_tol=0.0, abs_tol=1e-4), (key, value, accepted[key])
    assert accepted["semantic_output_identity"] is True

    result: dict[str, object] = {
        "reference_evidence": "PASS",
        "accepted_final_commit": final["commit"],
        "accepted_final_run": final["github_actions_run"],
        "accepted_state_checksum": final["benchmark"]["state_checksum"],
        "accepted_callgrind_replay_instructions": final["callgrind_replay_instructions"],
    }

    if args.benchmark is not None:
        current = json.loads(args.benchmark.read_text(encoding="utf-8"))
        assert int(current["events"]) == int(workload["events"])
        assert int(current["seed"]) == int(workload["seed"])
        for key in SEMANTIC_KEYS:
            assert int(current[key]) == int(final["benchmark"][key]), (
                key,
                current[key],
                final["benchmark"][key],
            )
        result["current_semantic_state"] = "PASS"
        result["current_state_checksum"] = current["state_checksum"]
        result["current_average_ns_per_event_observed"] = current["average_ns_per_event"]
        result["current_throughput_events_per_sec_observed"] = current["throughput_events_per_sec"]
        result["wall_clock_gate"] = "NOT_APPLIED_RUNNER_DEPENDENT"

    if args.callgrind is not None:
        current_instructions = parse_callgrind_summary(args.callgrind)
        accepted_instructions = int(final["callgrind_replay_instructions"])
        ratio = current_instructions / accepted_instructions
        # Callgrind instruction count is substantially more stable than wall-clock timing,
        # but compiler/stdlib updates can move it. Treat >10% growth as a regression signal,
        # not an exact cross-toolchain reproducibility requirement.
        assert ratio <= 1.10, (current_instructions, accepted_instructions, ratio)
        result["current_callgrind_replay_instructions"] = current_instructions
        result["current_vs_accepted_instruction_ratio"] = ratio
        result["instruction_regression_guard"] = "PASS"

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
