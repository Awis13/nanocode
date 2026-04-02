#!/usr/bin/env python3
"""
tb_check_regression.py — Compare a TB run against a stored baseline.

Exits 0 if pass rate did not regress beyond threshold.
Exits 1 if pass rate dropped by more than threshold percentage points.
Exits 2 if baseline file is missing (bootstrap mode — copies results as new baseline).

Usage:
    python harbor/scripts/tb_check_regression.py \
        --results tb-results.json \
        --baseline harbor/tb-baseline.json \
        --threshold 5.0
"""

import argparse
import json
import sys
from pathlib import Path


def load_json(path: Path) -> dict:
    return json.loads(path.read_text())


def category_breakdown(results: list[dict]) -> dict[str, dict]:
    """Group task results by inferred category prefix."""
    prefix_map = {
        "hello": "file_editing",
        "create": "file_editing",
        "edit": "file_editing",
        "multi": "file_editing",
        "rename": "file_editing",
        "patch": "file_editing",
        "write-function": "code_generation",
        "implement": "code_generation",
        "add-feature": "code_generation",
        "refactor": "code_generation",
        "complete": "code_generation",
        "run": "shell_scripting",
        "pipe": "shell_scripting",
        "find": "shell_scripting",
        "environment": "shell_scripting",
        "git": "git_operations",
        "fix-syntax": "debugging",
        "fix-logic": "debugging",
        "trace": "debugging",
        "write-unit": "testing",
        "add-test": "testing",
        "fix-failing": "testing",
    }
    cats: dict[str, dict] = {}
    for r in results:
        task = r["task"]
        cat = next((v for k, v in prefix_map.items() if task.startswith(k)), "other")
        if cat not in cats:
            cats[cat] = {"passed": 0, "total": 0}
        cats[cat]["total"] += 1
        if r["passed"]:
            cats[cat]["passed"] += 1
    return cats


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--results", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--threshold", type=float, default=5.0,
                        help="Max allowed regression in percentage points")
    args = parser.parse_args()

    current = load_json(args.results)
    current_rate = current["pass_rate"]
    current_cats = category_breakdown(current["results"])

    print(f"Current pass rate:  {current_rate:.1f}%  ({current['passed']}/{current['total']})")

    print("\nCategory breakdown:")
    for cat, counts in sorted(current_cats.items()):
        pct = counts["passed"] / counts["total"] * 100 if counts["total"] else 0
        print(f"  {cat:<22} {counts['passed']}/{counts['total']}  ({pct:.0f}%)")

    if not args.baseline.exists() or not load_json(args.baseline).get("results"):
        print(f"\nBaseline not found or empty at {args.baseline}.")
        print("Treating current results as bootstrap — no regression check performed.")
        return 2

    baseline = load_json(args.baseline)
    baseline_rate = baseline["pass_rate"]
    baseline_cats = category_breakdown(baseline.get("results", []))

    delta = current_rate - baseline_rate
    print(f"\nBaseline pass rate: {baseline_rate:.1f}%  (run: {baseline.get('run_at', 'unknown')})")
    print(f"Delta:              {delta:+.1f}pp")

    all_cats = sorted(set(list(current_cats) + list(baseline_cats)))
    regressions = []
    print("\nCategory delta:")
    for cat in all_cats:
        cur = current_cats.get(cat, {"passed": 0, "total": 0})
        base = baseline_cats.get(cat, {"passed": 0, "total": 0})
        cur_pct = cur["passed"] / cur["total"] * 100 if cur["total"] else 0
        base_pct = base["passed"] / base["total"] * 100 if base["total"] else 0
        cat_delta = cur_pct - base_pct
        flag = " !!REGRESSION" if cat_delta < -args.threshold else ""
        print(f"  {cat:<22} {cur_pct:.0f}% vs {base_pct:.0f}%  ({cat_delta:+.0f}pp){flag}")
        if cat_delta < -args.threshold:
            regressions.append(cat)

    print()
    if delta < -args.threshold:
        print(f"FAIL: overall pass rate regressed by {abs(delta):.1f}pp (threshold: {args.threshold}pp)")
        if regressions:
            print(f"Regressed categories: {', '.join(regressions)}")
        return 1

    print(f"PASS: no regression beyond {args.threshold}pp threshold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
