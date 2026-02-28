#!/usr/bin/env python3
"""
parse_explain_json.py — Extract execution time and JIT timing from EXPLAIN JSON.

Reads EXPLAIN (ANALYZE, FORMAT JSON) output from stdin.
Outputs a single CSV line to stdout:

    exec_time,jit_gen,jit_inline,jit_opt,jit_emit,jit_total

All times in milliseconds. If no JIT section, JIT fields are 0.
"""
import json
import sys


def extract_timing(data):
    """Extract execution time and JIT timing from EXPLAIN JSON."""
    if isinstance(data, list):
        data = data[0]

    exec_time = data.get("Execution Time", 0)

    jit = data.get("JIT", {})
    timing = jit.get("Timing", {})

    # Generation can be a number or an object with {"Deform": ..., "Total": ...}
    gen = timing.get("Generation", 0)
    if isinstance(gen, dict):
        jit_gen = gen.get("Total", 0)
    else:
        jit_gen = gen

    jit_inline = timing.get("Inlining", 0)
    jit_opt = timing.get("Optimization", 0)
    jit_emit = timing.get("Emission", 0)
    jit_total = timing.get("Total", 0)

    return exec_time, jit_gen, jit_inline, jit_opt, jit_emit, jit_total


def main():
    raw = sys.stdin.read().strip()
    if not raw:
        print("0,0,0,0,0,0")
        return

    # Strip non-JSON prefix lines (e.g. "SET" from psql)
    idx = raw.find("[")
    if idx < 0:
        idx = raw.find("{")
    if idx > 0:
        raw = raw[idx:]

    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        print("0,0,0,0,0,0", file=sys.stderr)
        sys.exit(1)

    exec_time, jit_gen, jit_inline, jit_opt, jit_emit, jit_total = extract_timing(data)
    print(f"{exec_time},{jit_gen},{jit_inline},{jit_opt},{jit_emit},{jit_total}")


if __name__ == "__main__":
    main()
