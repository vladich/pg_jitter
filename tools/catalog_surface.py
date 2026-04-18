#!/usr/bin/env python3
"""Analyze pg_jitter direct-function coverage against PostgreSQL catalogs.

The script is intentionally read-only.  It compares supported PostgreSQL source
trees and reports functions/operators/casts that are common to every selected
version and look safe enough for Tier0/Tier1 consideration:

  * immutable
  * strict
  * small argument count
  * by-value argument and return types

It does not decide correctness.  It produces a stable worklist that still needs
source inspection and regression tests before entries are added to pg_jitter.
"""

from __future__ import annotations

import argparse
import pathlib
import re
from dataclasses import dataclass
from typing import Iterable


@dataclass(frozen=True)
class Proc:
    proname: str
    prosrc: str
    rettype: str
    argtypes: tuple[str, ...]
    volatile: str
    strict: str
    retset: str
    kind: str
    lang: str


@dataclass(frozen=True)
class Operator:
    name: str
    left: str
    right: str
    result: str
    func: str


@dataclass(frozen=True)
class Cast:
    source: str
    target: str
    castfunc: str
    prosrc: str


@dataclass(frozen=True)
class TypeInfo:
    byval: bool
    typtype: str


def dat_records(path: pathlib.Path, first_field: str) -> list[str]:
    text = path.read_text(errors="replace")
    return re.findall(r"\{\s*" + re.escape(first_field) + r"\s*=>.*?\},", text, re.S)


def field(record: str, name: str, default: str = "") -> str:
    match = re.search(r"\b" + re.escape(name) + r"\s*=>\s*'([^']*)'", record)
    return match.group(1) if match else default


def parse_types(pg_src: pathlib.Path) -> dict[str, TypeInfo]:
    result: dict[str, TypeInfo] = {}
    path = pg_src / "src/include/catalog/pg_type.dat"
    for record in dat_records(path, "oid"):
        typname = field(record, "typname")
        if not typname:
            continue
        typbyval = field(record, "typbyval", "f")
        result[typname] = TypeInfo(
            byval=typbyval in {"t", "FLOAT8PASSBYVAL"},
            typtype=field(record, "typtype", "b"),
        )
    return result


def parse_procs(pg_src: pathlib.Path) -> dict[str, list[Proc]]:
    result: dict[str, list[Proc]] = {}
    path = pg_src / "src/include/catalog/pg_proc.dat"
    for record in dat_records(path, "oid"):
        proname = field(record, "proname")
        if not proname:
            continue
        proc = Proc(
            proname=proname,
            prosrc=field(record, "prosrc", proname),
            rettype=field(record, "prorettype"),
            argtypes=tuple(field(record, "proargtypes").split()),
            volatile=field(record, "provolatile", "i"),
            strict=field(record, "proisstrict", "t"),
            retset=field(record, "proretset", "f"),
            kind=field(record, "prokind", "f"),
            lang=field(record, "prolang", "internal"),
        )
        result.setdefault(proc.prosrc, []).append(proc)
    return result


def parse_ops(pg_src: pathlib.Path) -> list[Operator]:
    result: list[Operator] = []
    path = pg_src / "src/include/catalog/pg_operator.dat"
    for record in dat_records(path, "oid"):
        func = field(record, "oprcode")
        if func:
            result.append(
                Operator(
                    name=field(record, "oprname"),
                    left=field(record, "oprleft"),
                    right=field(record, "oprright"),
                    result=field(record, "oprresult"),
                    func=func,
                )
            )
    return result


def resolve_regproc(regproc: str, procs: dict[str, list[Proc]]) -> str:
    if not regproc or regproc == "0":
        return ""
    match = re.match(r"([^()]+)\((.*)\)$", regproc)
    if not match:
        return regproc
    name = match.group(1)
    args = tuple(match.group(2).replace(",", " ").split())
    for candidates in procs.values():
        for proc in candidates:
            if proc.proname == name and proc.argtypes == args:
                return proc.prosrc
    return regproc


def parse_casts(pg_src: pathlib.Path, procs: dict[str, list[Proc]]) -> list[Cast]:
    result: list[Cast] = []
    path = pg_src / "src/include/catalog/pg_cast.dat"
    for record in dat_records(path, "castsource"):
        castfunc = field(record, "castfunc")
        if not castfunc or castfunc == "0":
            continue
        result.append(
            Cast(
                source=field(record, "castsource"),
                target=field(record, "casttarget"),
                castfunc=castfunc,
                prosrc=resolve_regproc(castfunc, procs),
            )
        )
    return result


def parse_direct_table(path: pathlib.Path) -> set[str]:
    text = path.read_text(errors="replace")
    start = text.index("const JitDirectFn jit_direct_fns[]")
    end = text.index("const int jit_direct_fns_count", start)
    direct: set[str] = set()
    for line in text[start:end].splitlines():
        match = re.match(r"\s*(E0|E1|E2|EI1|EI2|EC1|EC2|EIC2)\((.*)\),?", line)
        if match:
            direct.add(match.group(2).split(",", 1)[0].strip())
    return direct


def is_scalar_byval(types: dict[str, TypeInfo], typname: str) -> bool:
    info = types.get(typname)
    return bool(info and info.byval and info.typtype == "b")


def is_proc_candidate(types: dict[str, TypeInfo], proc: Proc) -> bool:
    return (
        proc.kind == "f"
        and proc.lang != "sql"
        and proc.retset != "t"
        and proc.volatile == "i"
        and proc.strict == "t"
        and len(proc.argtypes) <= 3
        and is_scalar_byval(types, proc.rettype)
        and all(is_scalar_byval(types, arg) for arg in proc.argtypes)
    )


def has_matching_proc(
    types: dict[str, TypeInfo],
    procs: dict[str, list[Proc]],
    prosrc: str,
    rettype: str,
    argtypes: tuple[str, ...],
) -> bool:
    return any(
        proc.rettype == rettype
        and proc.argtypes == argtypes
        and is_proc_candidate(types, proc)
        for proc in procs.get(prosrc, ())
    )


AUTO_RULES: dict[str, str] = {
    "bool_int4": "bool/int by-value cast",
    "int4_bool": "bool/int by-value cast",
    "chartoi4": "signed char/int by-value cast",
    "i4tochar": "range-checked char/int by-value cast",
    "i8tof": "integer to float4 by-value cast",
    "ftoi8": "range-checked float4 to int8 by-value cast",
    "oidtoi8": "unsigned OID widening cast",
    "xid8toxid": "FullTransactionId low-xid cast",
    "int2um": "checked unary integer operator",
    "int2up": "identity unary integer operator",
    "int4um": "checked unary integer operator",
    "int4up": "identity unary integer operator",
    "int8um": "checked unary integer operator",
    "int8up": "identity unary integer operator",
    "float4up": "identity unary float operator",
    "float8up": "identity unary float operator",
    "btcharcmp": "simple btree comparator",
    "btint24cmp": "simple cross-integer btree comparator",
    "btint28cmp": "simple cross-integer btree comparator",
    "pg_lsn_cmp": "unsigned pg_lsn comparator",
    "pg_lsn_larger": "unsigned pg_lsn max",
    "pg_lsn_smaller": "unsigned pg_lsn min",
    "pg_lsn_hash": "pg_lsn delegates to hashint8",
    "pg_lsn_hash_extended": "pg_lsn delegates to hashint8extended",
    "time_larger": "time max on int64 representation",
    "time_smaller": "time min on int64 representation",
    "time_hash": "time delegates to hashint8",
    "time_hash_extended": "time delegates to hashint8extended",
    "date_finite": "finite check against date sentinel values",
    "timestamp_finite": "finite check against timestamp sentinel values",
    "int4range_subdiff": "range subdiff as float8 difference",
    "int8range_subdiff": "range subdiff as float8 difference",
    "daterange_subdiff": "range subdiff as float8 difference",
    "tsrange_subdiff": "timestamp range subdiff in seconds",
    "tstzrange_subdiff": "timestamptz range subdiff in seconds",
}

TIMEZONE_OR_TYPMOD = {
    "date_timestamptz",
    "time_scale",
    "timestamp_date",
    "timestamp_scale",
    "timestamp_time",
    "timestamp_timestamptz",
    "timestamptz_date",
    "timestamptz_scale",
    "timestamptz_time",
    "timestamptz_timestamp",
}

MATH_ERROR_SEMANTICS = {
    "dacos", "dasin", "datan", "datan2", "dcbrt", "dceil", "dcos", "dcot",
    "ddegrees", "dexp", "dfloor", "dlog1", "dlog10", "dpow", "dradians",
    "dsign", "dsin", "dsqrt", "dtan",
}


def classify_candidate(name: str) -> tuple[str, str]:
    if name in AUTO_RULES:
        return "auto", AUTO_RULES[name]
    if name in TIMEZONE_OR_TYPMOD:
        return "manual", "timezone, typmod, or special-value semantics"
    if name in MATH_ERROR_SEMANTICS:
        return "manual", "math domain/NaN/Inf/error semantics need source-specific handling"
    if name.startswith("cash_") or name.endswith("_cash") or name in {"int4_cash", "int8_cash"}:
        return "manual", "money rounding, overflow, division, and locale scale semantics"
    if name in {"btequalimage", "btvarstrequalimage"}:
        return "manual", "btree support function, not a normal scalar expression hot path"
    if name.startswith("generate_series") or name.startswith("window_"):
        return "manual", "set-returning or window function"
    if name.startswith("hash") and name.endswith("extended"):
        return "manual", "hash exactness affects hash partitioning; verify algorithm first"
    return "manual", "requires PostgreSQL source inspection and regression case"


def common_keys(items: Iterable[set[str]]) -> set[str]:
    iterator = iter(items)
    try:
        common = set(next(iterator))
    except StopIteration:
        return set()
    for item in iterator:
        common &= item
    return common


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pg-root", default=str(pathlib.Path.home() / "postgres_versions"))
    parser.add_argument("--versions", default="14,15,16,17,18")
    parser.add_argument("--direct", default="src/pg_jit_funcs.c")
    parser.add_argument("--limit", type=int, default=80)
    args = parser.parse_args()

    versions = [v.strip().removeprefix("v") for v in args.versions.split(",") if v.strip()]
    pg_root = pathlib.Path(args.pg_root).expanduser()
    direct = parse_direct_table(pathlib.Path(args.direct))

    per_version = {}
    for version in versions:
        pg_src = pg_root / f"v{version}"
        if not pg_src.exists():
            raise SystemExit(f"missing PostgreSQL source tree: {pg_src}")
        types = parse_types(pg_src)
        procs = parse_procs(pg_src)
        ops = parse_ops(pg_src)
        casts = parse_casts(pg_src, procs)

        proc_candidates = {
            proc.prosrc
            for candidates in procs.values()
            for proc in candidates
            if is_proc_candidate(types, proc)
        }

        op_candidates = {
            op.func
            for op in ops
            if has_matching_proc(
                types,
                procs,
                op.func,
                op.result,
                tuple(arg for arg in (op.left, op.right) if arg),
            )
            and is_scalar_byval(types, op.result)
            and all(is_scalar_byval(types, arg) for arg in (op.left, op.right) if arg)
        }

        cast_candidates = {
            cast.prosrc
            for cast in casts
            if is_scalar_byval(types, cast.source)
            and is_scalar_byval(types, cast.target)
            and has_matching_proc(types, procs, cast.prosrc, cast.target, (cast.source,))
        }

        per_version[version] = {
            "proc": proc_candidates,
            "operator": op_candidates,
            "cast": cast_candidates,
        }

    print(f"direct_table_unique={len(direct)}")
    print(f"versions={','.join(versions)}")

    for category in ("operator", "cast", "proc"):
        common = common_keys(per_version[v][category] for v in versions)
        missing = sorted(common - direct)
        auto = [name for name in missing if classify_candidate(name)[0] == "auto"]
        manual = [name for name in missing if classify_candidate(name)[0] != "auto"]
        print()
        print(
            f"{category}_common_candidates={len(common)} "
            f"{category}_missing_from_direct={len(missing)} "
            f"{category}_auto_addable={len(auto)} "
            f"{category}_manual_review={len(manual)}"
        )
        shown = 0
        for name in auto[: args.limit]:
            _, reason = classify_candidate(name)
            print(f"  AUTO   {name}  # {reason}")
            shown += 1
        remaining_limit = max(args.limit - shown, 0)
        for name in manual[: remaining_limit]:
            _, reason = classify_candidate(name)
            print(f"  MANUAL {name}  # {reason}")
            shown += 1
        if len(missing) > shown:
            print(f"  ... {len(missing) - shown} more")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
