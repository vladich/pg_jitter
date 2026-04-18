#!/usr/bin/env python3
"""
gen_cross_version_benchmarks.py — Generate BENCHMARK_{backend}.md from per-version CSVs.

Reads CSV files produced by bench_comprehensive.sh for PG14-18 and generates
one markdown file per JIT backend showing its performance across all PG versions
relative to the interpreter baseline.

Usage:
    python3 tests/gen_cross_version_benchmarks.py \
        --csv14 tests/bench_pg14.csv \
        --csv15 tests/bench_pg15.csv \
        --csv16 tests/bench_pg16.csv \
        --csv17 tests/bench_pg17.csv \
        --csv18 tests/bench_pg18.csv \
        --outdir bench/ARM64
"""
import argparse
import csv
import os
import platform
import subprocess
import sys
from collections import OrderedDict
from datetime import date


SECTIONS = [
    ("Basic Aggregation", ["SUM_int", "COUNT_star", "GroupBy_5agg", "GroupBy_100K_grp", "COUNT_DISTINCT"]),
    ("Hash Joins", ["HashJoin_single", "HashJoin_composite", "HashJoin_filter", "HashJoin_GroupBy"]),
    ("Outer Joins", ["LeftJoin", "RightJoin", "FullOuterJoin"]),
    ("Semi/Anti Joins", ["EXISTS_semi", "NOT_EXISTS_anti", "IN_subquery"]),
    ("Set Operations", ["INTERSECT", "EXCEPT", "UNION_ALL_agg"]),
    ("Expressions & Filters", ["CASE_simple", "CASE_searched_4way", "COALESCE_NULLIF", "Bool_AND_OR", "Arith_expr",
                               "IN_expr_20", "IN_expr_128", "IN_expr_4096", "IN_expr_4097",
                               "IN_expr_5000_null", "IN_index_20"]),
    ("Subqueries", ["Scalar_subq", "Correlated_subq", "LATERAL_top3"]),
    ("Date/Timestamp", ["Date_extract", "Timestamp_trunc", "Interval_arith", "Timestamp_diff"]),
    ("Text/String", ["Text_EQ_filter", "Text_LIKE", "Text_concat_agg", "Text_length_expr"]),
    ("Numeric", ["Numeric_agg", "Numeric_arith"]),
    ("JSONB", ["JSONB_extract", "JSONB_contains", "JSONB_agg"]),
    ("Arrays", ["Array_overlap", "Array_contains", "Unnest_agg"]),
    ("Wide Row / Deform", ["Wide_10col_sum", "Wide_20col_sum", "Wide_GroupBy_expr"]),
    ("Super-Wide Tables", ["Wide100_sum", "Wide100_groupby", "Wide100_filter",
                           "Wide300_sum", "Wide300_groupby", "Wide300_filter",
                           "Wide1K_sum", "Wide1K_groupby", "Wide1K_filter"]),
    ("Partitioned", ["PartScan_filter", "PartScan_agg_all"]),
]

VERSIONS = ["PG14", "PG15", "PG16", "PG17", "PG18"]


def read_csv(path):
    """Read benchmark CSV, return dict: (query, backend) -> exec_time"""
    data = {}
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            key = (row["query"], row["backend"])
            data[key] = row["exec_time"]
    return data


def get_system_info():
    os_info = f"{platform.system()} {platform.release()} {platform.machine()}"
    cpu = "unknown"
    ram = "unknown"
    try:
        cpu = subprocess.check_output(
            ["sysctl", "-n", "machdep.cpu.brand_string"],
            stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        try:
            with open("/proc/cpuinfo") as f:
                for line in f:
                    if line.startswith("model name"):
                        cpu = line.split(":", 1)[1].strip()
                        break
        except Exception:
            pass
    try:
        mem = int(subprocess.check_output(
            ["sysctl", "-n", "hw.memsize"],
            stderr=subprocess.DEVNULL).decode().strip())
        ram = f"{mem // (1024**3)} GB"
    except Exception:
        try:
            with open("/proc/meminfo") as f:
                for line in f:
                    if line.startswith("MemTotal"):
                        kb = int(line.split()[1])
                        ram = f"{kb // (1024**2)} GB"
                        break
        except Exception:
            pass
    return os_info, cpu, ram


def pct(val, baseline):
    """Compute percentage, return string like '85%' or '' on error."""
    try:
        v = float(val)
        b = float(baseline)
        if b <= 0:
            return ""
        return f"{v * 100 / b:.0f}%"
    except (ValueError, TypeError):
        return ""


def generate_backend_md(backend, version_data, outdir, os_info, cpu, ram):
    """Generate BENCHMARK_{backend}.md"""
    # Collect all queries and backends available
    all_backends = set()
    for vdata in version_data.values():
        for (q, b) in vdata:
            all_backends.add(b)

    if backend not in all_backends:
        print(f"  Skipping {backend}: not found in CSV data")
        return

    path = os.path.join(outdir, f"BENCHMARK_{backend}.md")
    with open(path, "w") as f:
        f.write(f"# pg_jitter {backend} — Cross-Version Benchmark Results\n\n")
        f.write(f"Performance of the **{backend}** JIT backend compared against the "
                f"PostgreSQL interpreter (No JIT) across PostgreSQL 14 through 18.\n\n")

        f.write("## Environment\n\n")
        f.write("| Parameter | Value |\n")
        f.write("|-----------|-------|\n")
        f.write(f"| OS | {os_info} |\n")
        f.write(f"| CPU | {cpu} |\n")
        f.write(f"| RAM | {ram} |\n")
        f.write(f"| Backend | {backend} |\n")
        f.write(f"| Runs per query | 5 (median) |\n")
        f.write(f"| Warmup runs | 3 |\n")
        f.write(f"| Parallel workers | 0 (disabled) |\n")
        f.write(f"| Date | {date.today()} |\n\n")

        f.write("## Methodology\n\n")
        f.write("- Each cell shows: **absolute time in ms** and **(% of No JIT)** for that PG version\n")
        f.write("- `<100%` = JIT is faster than interpreter, `>100%` = JIT is slower\n")
        f.write("- No JIT baseline is the interpreter time for that specific PG version\n")
        f.write("- All queries run with `jit_above_cost = 0` to force JIT on every query\n\n")

        f.write("## Results\n\n")

        for section_name, queries in SECTIONS:
            # Check if any queries from this section exist in the data
            has_data = False
            for q in queries:
                for ver, vdata in version_data.items():
                    if (q, backend) in vdata:
                        has_data = True
                        break
                if has_data:
                    break
            if not has_data:
                continue

            f.write(f"### {section_name}\n\n")
            f.write("| Query |")
            for ver in VERSIONS:
                f.write(f" {ver} | |")
            f.write("\n")
            f.write("|-------|")
            for _ in VERSIONS:
                f.write("------:|---:|")
            f.write("\n")
            f.write("| |")
            for _ in VERSIONS:
                f.write(" ms | % |")
            f.write("\n")

            for q in queries:
                f.write(f"| {q} |")
                for ver in VERSIONS:
                    vdata = version_data.get(ver, {})
                    val = vdata.get((q, backend), "")
                    baseline = vdata.get((q, "interp"), "")
                    if val == "CRASH":
                        f.write(" CRASH | - |")
                    elif val and val != "":
                        p = pct(val, baseline)
                        f.write(f" {val} | {p} |")
                    else:
                        f.write(" - | - |")
                f.write("\n")
            f.write("\n")

        # Interpreter baseline table
        f.write("## Interpreter Baseline (No JIT)\n\n")
        f.write("Absolute interpreter times per version, for reference.\n\n")
        f.write("| Query |")
        for ver in VERSIONS:
            f.write(f" {ver} |")
        f.write("\n")
        f.write("|-------|")
        for _ in VERSIONS:
            f.write("-----:|")
        f.write("\n")

        all_queries = []
        for _, queries in SECTIONS:
            all_queries.extend(queries)

        for q in all_queries:
            f.write(f"| {q} |")
            for ver in VERSIONS:
                vdata = version_data.get(ver, {})
                val = vdata.get((q, "interp"), "")
                if val and val != "CRASH":
                    f.write(f" {val} |")
                else:
                    f.write(" - |")
            f.write("\n")
        f.write("\n")

        f.write(f"---\n\n*Generated by `tests/gen_cross_version_benchmarks.py` on {date.today()}*\n")

    print(f"  Generated {path}")


def main():
    parser = argparse.ArgumentParser(description="Generate cross-version benchmark markdown")
    parser.add_argument("--outdir", required=True, help="Output directory for BENCHMARK_{backend}.md files")
    for v in [14, 15, 16, 17, 18]:
        parser.add_argument(f"--csv{v}", required=True, help=f"CSV file from PG{v} benchmark")
    parser.add_argument("--backends", default="sljit,asmjit,mir",
                        help="Comma-separated list of backends (default: sljit,asmjit,mir)")
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    # Read all CSV files
    version_data = {}
    for v in [14, 15, 16, 17, 18]:
        csv_path = getattr(args, f"csv{v}")
        if os.path.exists(csv_path):
            version_data[f"PG{v}"] = read_csv(csv_path)
            print(f"  Read {csv_path}: {len(version_data[f'PG{v}'])} entries")
        else:
            print(f"  WARNING: {csv_path} not found, skipping PG{v}")

    os_info, cpu, ram = get_system_info()
    backends = args.backends.split(",")

    for backend in backends:
        generate_backend_md(backend, version_data, args.outdir, os_info, cpu, ram)

    print("\nDone.")


if __name__ == "__main__":
    main()
