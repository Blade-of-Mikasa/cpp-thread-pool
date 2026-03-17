#!/usr/bin/env python3
"""
Run a repeatable stress benchmark suite and collect results into CSV.

This script runs one scenario per process (required because thread_pool is a singleton).
"""

from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Scenario:
    name: str
    args: list[str]


def _run_one(exe: Path, scenario: Scenario) -> dict[str, str]:
    proc = subprocess.run(
        [str(exe), *scenario.args],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    if proc.returncode != 0:
        raise RuntimeError(
            f"scenario '{scenario.name}' failed with exit code {proc.returncode}\n"
            f"stdout:\n{proc.stdout}\n\nstderr:\n{proc.stderr}"
        )

    # stress_bench prints header + one row
    lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        raise RuntimeError(f"scenario '{scenario.name}' produced unexpected output:\n{proc.stdout}")

    header = lines[-2].split(",")
    row = lines[-1].split(",")
    if len(header) != len(row):
        raise RuntimeError(f"scenario '{scenario.name}' produced malformed CSV:\n{proc.stdout}")

    result = dict(zip(header, row, strict=True))
    result["scenario"] = scenario.name
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--exe",
        type=Path,
        default=Path("build/stress_bench.exe"),
        help="Path to stress_bench executable",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("bench_results.csv"),
        help="Output CSV file path",
    )
    parser.add_argument(
        "--threads",
        type=int,
        default=0,
        help="Max worker threads for thread_pool scenarios (0 = auto, default cap=8)",
    )
    args = parser.parse_args()

    exe: Path = args.exe
    if not exe.exists():
        print(f"error: stress_bench not found: {exe}", file=sys.stderr)
        return 2

    max_threads = args.threads
    if max_threads <= 0:
        max_threads = os.cpu_count() or 8
        max_threads = min(max_threads, 8)
    max_threads = max(1, max_threads)
    min_threads_dynamic = 1 if max_threads == 1 else 2

    # Control groups:
    # - serial: no thread pool
    # - fixed: thread_pool with min=max (disables scaling), unbounded queue
    # Treatment group:
    # - dynamic: thread_pool with min<max, unbounded queue
    #
    # Workloads:
    # 1) noop: scheduling overhead dominated (large N)
    # 2) spin: short CPU work per task
    # 3) sleep: IO-like tasks (smaller N to keep runtime reasonable)
    scenarios: list[Scenario] = [
        Scenario(
            "noop_serial_200k",
            ["--impl", "serial", "--work", "noop", "--tasks", "200000", "--work-us", "0"],
        ),
        Scenario(
            "noop_fixed_200k",
            [
                "--impl",
                "thread_pool",
                "--work",
                "noop",
                "--tasks",
                "200000",
                "--producers",
                "4",
                "--min",
                str(max_threads),
                "--max",
                str(max_threads),
                "--capacity",
                "0",
                "--policy",
                "block",
                "--work-us",
                "0",
            ],
        ),
        Scenario(
            "noop_dynamic_200k",
            [
                "--impl",
                "thread_pool",
                "--work",
                "noop",
                "--tasks",
                "200000",
                "--producers",
                "4",
                "--min",
                str(min_threads_dynamic),
                "--max",
                str(max_threads),
                "--idle-ms",
                "200",
                "--capacity",
                "0",
                "--policy",
                "block",
                "--work-us",
                "0",
            ],
        ),
        # API overhead comparison: post() vs submit() (smaller N to keep runtime reasonable).
        Scenario(
            "noop_fixed_post_50k",
            [
                "--impl",
                "thread_pool",
                "--work",
                "noop",
                "--tasks",
                "50000",
                "--producers",
                "4",
                "--min",
                str(max_threads),
                "--max",
                str(max_threads),
                "--capacity",
                "0",
                "--policy",
                "block",
                "--work-us",
                "0",
            ],
        ),
        Scenario(
            "noop_fixed_submit_50k",
            [
                "--impl",
                "thread_pool",
                "--work",
                "noop",
                "--tasks",
                "50000",
                "--producers",
                "4",
                "--min",
                str(max_threads),
                "--max",
                str(max_threads),
                "--capacity",
                "0",
                "--policy",
                "block",
                "--work-us",
                "0",
                "--use-submit",
            ],
        ),
        Scenario(
            "spin50us_serial_50k",
            ["--impl", "serial", "--work", "spin", "--tasks", "50000", "--work-us", "50"],
        ),
        Scenario(
            "spin50us_fixed_50k",
            [
                "--impl",
                "thread_pool",
                "--work",
                "spin",
                "--tasks",
                "50000",
                "--producers",
                "4",
                "--min",
                str(max_threads),
                "--max",
                str(max_threads),
                "--capacity",
                "0",
                "--policy",
                "block",
                "--work-us",
                "50",
            ],
        ),
        Scenario(
            "spin50us_fixed_submit_50k",
            [
                "--impl",
                "thread_pool",
                "--work",
                "spin",
                "--tasks",
                "50000",
                "--producers",
                "4",
                "--min",
                str(max_threads),
                "--max",
                str(max_threads),
                "--capacity",
                "0",
                "--policy",
                "block",
                "--work-us",
                "50",
                "--use-submit",
            ],
        ),
        Scenario(
            "spin50us_dynamic_50k",
            [
                "--impl",
                "thread_pool",
                "--work",
                "spin",
                "--tasks",
                "50000",
                "--producers",
                "4",
                "--min",
                str(min_threads_dynamic),
                "--max",
                str(max_threads),
                "--idle-ms",
                "200",
                "--capacity",
                "0",
                "--policy",
                "block",
                "--work-us",
                "50",
            ],
        ),
        Scenario(
            "spin50us_dynamic_submit_50k",
            [
                "--impl",
                "thread_pool",
                "--work",
                "spin",
                "--tasks",
                "50000",
                "--producers",
                "4",
                "--min",
                str(min_threads_dynamic),
                "--max",
                str(max_threads),
                "--idle-ms",
                "200",
                "--capacity",
                "0",
                "--policy",
                "block",
                "--work-us",
                "50",
                "--use-submit",
            ],
        ),
        Scenario(
            "sleep1ms_fixed_5k",
            [
                "--impl",
                "thread_pool",
                "--work",
                "sleep",
                "--tasks",
                "5000",
                "--producers",
                "4",
                "--min",
                str(max_threads),
                "--max",
                str(max_threads),
                "--capacity",
                "0",
                "--policy",
                "block",
                "--work-us",
                "1000",
            ],
        ),
        Scenario(
            "sleep1ms_dynamic_5k",
            [
                "--impl",
                "thread_pool",
                "--work",
                "sleep",
                "--tasks",
                "5000",
                "--producers",
                "4",
                "--min",
                str(min_threads_dynamic),
                "--max",
                str(max_threads),
                "--idle-ms",
                "200",
                "--capacity",
                "0",
                "--policy",
                "block",
                "--work-us",
                "1000",
            ],
        ),
        # Backpressure comparison: bounded queue + different policy under noop workload.
        Scenario(
            "noop_bounded_block_200k",
            [
                "--impl",
                "thread_pool",
                "--work",
                "noop",
                "--tasks",
                "200000",
                "--producers",
                str(max(4, max_threads)),
                "--min",
                str(max_threads),
                "--max",
                str(max_threads),
                "--capacity",
                "128",
                "--policy",
                "block",
                "--work-us",
                "0",
            ],
        ),
        Scenario(
            "noop_bounded_caller_runs_200k",
            [
                "--impl",
                "thread_pool",
                "--work",
                "noop",
                "--tasks",
                "200000",
                "--producers",
                str(max(4, max_threads)),
                "--min",
                str(max_threads),
                "--max",
                str(max_threads),
                "--capacity",
                "128",
                "--policy",
                "caller_runs",
                "--work-us",
                "0",
            ],
        ),
    ]

    results: list[dict[str, str]] = []
    for scenario in scenarios:
        print(f"[run] {scenario.name}", flush=True)
        results.append(_run_one(exe, scenario))

    # Stable column order: script-added "scenario" first, then whatever stress_bench emitted.
    base_cols = [
        "impl",
        "mode",
        "tasks",
        "producers",
        "work",
        "work_us",
        "min_threads",
        "max_threads",
        "capacity",
        "policy",
        "api",
        "submit_ms",
        "total_ms",
        "throughput_per_sec",
        "avg_wait_us",
        "avg_exec_us",
        "peak_threads",
        "peak_pending_tasks",
        "submitted_total",
        "completed_total",
        "rejected_total",
        "canceled_total",
    ]
    cols = ["scenario", *base_cols]

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=cols)
        writer.writeheader()
        for r in results:
            writer.writerow({k: r.get(k, "") for k in cols})

    print(f"[ok] wrote: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
