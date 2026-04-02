#!/usr/bin/env python3
"""
tb_run_subset.py — Run a specified subset of Terminal-Bench tasks and write JSON results.

Usage:
    python harbor/scripts/tb_run_subset.py \
        --subset harbor/tb-ci-subset.txt \
        --output tb-results.json

The script invokes `tb run` once per task and aggregates pass/fail into a
structured JSON document that tb_check_regression.py can compare against a
stored baseline.
"""

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


def parse_subset(path: Path) -> list[str]:
    tasks = []
    for line in path.read_text().splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith("#"):
            tasks.append(stripped)
    return tasks


def run_task(task: str, agent_path: str) -> dict:
    """Run a single terminal-bench task and return a result dict."""
    result = {"task": task, "passed": False, "error": None}
    try:
        proc = subprocess.run(
            [
                "tb", "run",
                "--agent-import-path", agent_path,
                "--tasks", task,
                "--output-format", "json",
            ],
            capture_output=True,
            text=True,
            timeout=300,
        )
        if proc.returncode == 0:
            try:
                data = json.loads(proc.stdout)
                # tb run --output-format json returns a list of task results.
                task_results = data if isinstance(data, list) else data.get("results", [])
                passed = all(r.get("passed", False) for r in task_results)
                result["passed"] = passed
                result["raw"] = task_results
            except json.JSONDecodeError:
                # Fall back to exit-code check if JSON parse fails.
                result["passed"] = proc.returncode == 0
        else:
            result["error"] = proc.stderr.strip()[-500:] if proc.stderr else "exit code non-zero"
    except subprocess.TimeoutExpired:
        result["error"] = "timeout after 300s"
    except Exception as exc:
        result["error"] = str(exc)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--subset", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--agent-path",
        default="harbor/agent.py:NanocodeAgent",
        help="Harbor agent import path",
    )
    args = parser.parse_args()

    tasks = parse_subset(args.subset)
    if not tasks:
        print("ERROR: no tasks found in subset file", file=sys.stderr)
        return 1

    print(f"Running {len(tasks)} tasks...")
    results = []
    for i, task in enumerate(tasks, 1):
        print(f"  [{i}/{len(tasks)}] {task}", end=" ", flush=True)
        r = run_task(task, args.agent_path)
        status = "PASS" if r["passed"] else "FAIL"
        print(status)
        results.append(r)

    passed = sum(1 for r in results if r["passed"])
    total = len(results)
    pass_rate = (passed / total * 100) if total else 0.0

    output = {
        "run_at": datetime.now(timezone.utc).isoformat(),
        "pass_rate": round(pass_rate, 2),
        "passed": passed,
        "total": total,
        "results": results,
    }

    args.output.write_text(json.dumps(output, indent=2))
    print(f"\nResults: {passed}/{total} passed ({pass_rate:.1f}%)")
    print(f"Written to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
