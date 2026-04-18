#!/usr/bin/env python3
"""pg_jitter function-surface regression test.

The PostgreSQL pg_regress suite is not designed to exercise every function that
pg_jitter maps to a native direct call or inline operation.  This test is a
pg_jitter-specific baseline comparator: for each manifest case it runs the same
SQL once with jit=off and once with jit=on for the selected backend, then
compares a stable digest of the result rows.

The manifest also records which jit_direct_fns entries each case is intended to
cover.  A coverage check against src/pg_jit_funcs.c fails both when new direct
entries are added without tests and when existing entries disappear from the
fast path unexpectedly.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


COMMON_SETTINGS = """
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;
SET pg_jitter.min_expr_steps = 0;
"""


SETUP_SQL = r"""
DROP SCHEMA IF EXISTS jitter_surface_tmp CASCADE;
CREATE SCHEMA jitter_surface_tmp;

CREATE UNLOGGED TABLE jitter_surface_tmp.t AS
SELECT ord::int4,
       i2a::int2, i2b::int2,
       i4a::int4, i4b::int4,
       i8a::int8, i8b::int8,
       f4a::float4, f4b::float4,
       f8a::float8, f8b::float8,
       b1::bool, b2::bool,
       d1::date, d2::date,
       ts1::timestamp, ts2::timestamp,
       tstz1::timestamptz, tstz2::timestamptz,
       tm1::time, tm2::time,
       iv1::interval, iv2::interval,
       ch1::"char", ch2::"char",
       xid1::xid, xid2::xid,
       xid81::xid8, xid82::xid8,
       cid1::cid, cid2::cid,
       lsn1::pg_lsn, lsn2::pg_lsn,
       oid1::oid, oid2::oid,
       cash1::money, cash2::money,
       txt1::text, txt2::text,
       n1::numeric, n2::numeric,
       u1::uuid, u2::uuid
FROM (VALUES
  (1, 1, 2, 10, 3, 100, 7,
   1.25, 0.5, 10.5, 2.0, true, false,
   '2020-01-01', '2020-01-02',
   '2020-01-01 00:00:00', '2020-01-01 00:01:00',
   '2020-01-01 00:00:00+00', '2020-01-01 00:01:00+00',
   '01:02:03', '02:03:04',
   '1 hour', '30 minutes',
   'a', 'b', '1', '2', '1', '2', '1', '2', '0/10', '0/20',
   '1', '2', '1.23', '0.23', 'alpha', 'alphabet',
   '12.34', '5.6',
   '00000000-0000-0000-0000-000000000001',
   '00000000-0000-0000-0000-000000000002'),
  (2, 3276, -9, -10, 4, -100, 11,
   -4.5, 2.25, -20.25, 3.5, false, true,
   '2020-01-03', '2020-01-01',
   '2020-01-01 00:02:00', '2020-01-01 00:00:00',
   '2020-06-01 00:02:00+00', '2020-06-01 00:00:00+00',
   '03:04:05', '01:00:00',
   '2 days 03:00:00', '1 day 02:00:00',
   'z', 'a', '4294967295', '1', '9223372036854775808', '2', '5', '1',
   'FFFFFFFF/FFFFFFFE', '0/1',
   '4294967295', '1', '9.99', '10.01', 'zeta', 'eta',
   '7799465.7219', '4.31',
   'ffffffff-ffff-ffff-ffff-ffffffffffff',
   '00000000-0000-0000-0000-000000000001'),
  (3, 0, 1, 0, 1, 0, 1,
   0.0, 1.0, 0.0, 1.0, true, true,
   '2020-01-05', '2020-01-05',
   '2020-01-01 00:03:00', '2020-01-01 00:03:00',
   '2020-01-01 00:03:00+00', '2020-01-01 00:03:00+00',
   '04:00:00', '04:00:00',
   '0', '1 second',
   'm', 'm', '3', '3', '3', '3', '3', '3', '0/30', '0/30',
   '3', '3', '0.00', '1.00', 'same', 'same',
   '500', '1',
   '12345678-1234-1234-1234-123456789abc',
   '12345678-1234-1234-1234-123456789abc')
) AS v(ord, i2a, i2b, i4a, i4b, i8a, i8b, f4a, f4b, f8a, f8b,
       b1, b2, d1, d2, ts1, ts2, tstz1, tstz2, tm1, tm2, iv1, iv2,
       ch1, ch2, xid1, xid2, xid81, xid82, cid1, cid2, lsn1, lsn2,
       oid1, oid2, cash1, cash2, txt1, txt2, n1, n2, u1, u2);

ANALYZE jitter_surface_tmp.t;
"""


@dataclass(frozen=True)
class Case:
    name: str
    covers: tuple[str, ...]
    sql: str
    requires: tuple[str, ...] = field(default_factory=tuple)


def c(name: str, covers: Iterable[str], sql: str,
      requires: Iterable[str] = ()) -> Case:
    return Case(name, tuple(covers), sql.strip().rstrip(";"), tuple(requires))


CASES: tuple[Case, ...] = (
    c(
        "int4_ops",
        ["int4pl", "int4mi", "int4mul", "int4div", "int4mod",
         "int4abs", "int4inc", "int4um", "int4up"],
        """
        SELECT ord,
               i4a + i4b AS add_v,
               i4a - i4b AS sub_v,
               i4a * i4b AS mul_v,
               i4a / NULLIF(i4b, 0) AS div_v,
               i4a % NULLIF(i4b, 0) AS mod_v,
               abs(i4a) AS abs_v,
               pg_catalog.int4inc(i4a) AS inc_v,
               -i4a AS neg_v,
               +i4a AS pos_v
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "int8_ops",
        ["int8pl", "int8mi", "int8mul", "int8div", "int8mod",
         "int8abs", "int8inc", "int8dec", "int8um", "int8up"],
        """
        SELECT ord,
               i8a + i8b AS add_v,
               i8a - i8b AS sub_v,
               i8a * i8b AS mul_v,
               i8a / NULLIF(i8b, 0) AS div_v,
               i8a % NULLIF(i8b, 0) AS mod_v,
               abs(i8a) AS abs_v,
               pg_catalog.int8inc(i8a) AS inc_v,
               pg_catalog.int8dec(i8a) AS dec_v,
               -i8a AS neg_v,
               +i8a AS pos_v
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "int2_ops",
        ["int2pl", "int2mi", "int2mul", "int2div", "int2mod",
         "int2abs", "int2um", "int2up"],
        """
        SELECT ord,
               i2a + i2b AS add_v,
               i2a - i2b AS sub_v,
               i2a * i2b AS mul_v,
               i2a / NULLIF(i2b, 0) AS div_v,
               i2a % NULLIF(i2b, 0) AS mod_v,
               abs(i2a) AS abs_v,
               -i2a AS neg_v,
               +i2a AS pos_v
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "int_cmp",
        ["int4eq", "int4ne", "int4lt", "int4le", "int4gt", "int4ge",
         "int8eq", "int8ne", "int8lt", "int8le", "int8gt", "int8ge",
         "int2eq", "int2ne", "int2lt", "int2le", "int2gt", "int2ge"],
        """
        SELECT ord,
               (i4a = i4b)::int, (i4a <> i4b)::int, (i4a < i4b)::int,
               (i4a <= i4b)::int, (i4a > i4b)::int, (i4a >= i4b)::int,
               (i8a = i8b)::int, (i8a <> i8b)::int, (i8a < i8b)::int,
               (i8a <= i8b)::int, (i8a > i8b)::int, (i8a >= i8b)::int,
               (i2a = i2b)::int, (i2a <> i2b)::int, (i2a < i2b)::int,
               (i2a <= i2b)::int, (i2a > i2b)::int, (i2a >= i2b)::int
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "cross_int_cmp",
        ["int48eq", "int48ne", "int48lt", "int48le", "int48gt", "int48ge",
         "int84eq", "int84ne", "int84lt", "int84le", "int84gt", "int84ge",
         "int24eq", "int24ne", "int24lt", "int24le", "int24gt", "int24ge",
         "int42eq", "int42ne", "int42lt", "int42le", "int42gt", "int42ge",
         "int28eq", "int28ne", "int28lt", "int28le", "int28gt", "int28ge",
         "int82eq", "int82ne", "int82lt", "int82le", "int82gt", "int82ge"],
        """
        SELECT ord,
               (i4a = i8b)::int, (i4a <> i8b)::int, (i4a < i8b)::int,
               (i4a <= i8b)::int, (i4a > i8b)::int, (i4a >= i8b)::int,
               (i8a = i4b)::int, (i8a <> i4b)::int, (i8a < i4b)::int,
               (i8a <= i4b)::int, (i8a > i4b)::int, (i8a >= i4b)::int,
               (i2a = i4b)::int, (i2a <> i4b)::int, (i2a < i4b)::int,
               (i2a <= i4b)::int, (i2a > i4b)::int, (i2a >= i4b)::int,
               (i4a = i2b)::int, (i4a <> i2b)::int, (i4a < i2b)::int,
               (i4a <= i2b)::int, (i4a > i2b)::int, (i4a >= i2b)::int,
               (i2a = i8b)::int, (i2a <> i8b)::int, (i2a < i8b)::int,
               (i2a <= i8b)::int, (i2a > i8b)::int, (i2a >= i8b)::int,
               (i8a = i2b)::int, (i8a <> i2b)::int, (i8a < i2b)::int,
               (i8a <= i2b)::int, (i8a > i2b)::int, (i8a >= i2b)::int
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "cross_int_arith",
        ["int24pl", "int24mi", "int24mul", "int24div",
         "int42pl", "int42mi", "int42mul", "int42div",
         "int48pl", "int48mi", "int48mul", "int48div",
         "int84pl", "int84mi", "int84mul", "int84div",
         "int28pl", "int28mi", "int28mul", "int28div",
         "int82pl", "int82mi", "int82mul", "int82div"],
        """
        SELECT ord,
               i2a + i4b AS int24pl_v, i2a - i4b AS int24mi_v,
               i2a * i4b AS int24mul_v, i2a / NULLIF(i4b, 0) AS int24div_v,
               i4a + i2b AS int42pl_v, i4a - i2b AS int42mi_v,
               i4a * i2b AS int42mul_v, i4a / NULLIF(i2b, 0) AS int42div_v,
               i4a + i8b AS int48pl_v, i4a - i8b AS int48mi_v,
               i4a * i8b AS int48mul_v, i4a / NULLIF(i8b, 0) AS int48div_v,
               i8a + i4b AS int84pl_v, i8a - i4b AS int84mi_v,
               i8a * i4b AS int84mul_v, i8a / NULLIF(i4b, 0) AS int84div_v,
               i2a + i8b AS int28pl_v, i2a - i8b AS int28mi_v,
               i2a * i8b AS int28mul_v, i2a / NULLIF(i8b, 0) AS int28div_v,
               i8a + i2b AS int82pl_v, i8a - i2b AS int82mi_v,
               i8a * i2b AS int82mul_v, i8a / NULLIF(i2b, 0) AS int82div_v
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "int_minmax_bit",
        ["int2larger", "int2smaller", "int4larger", "int4smaller",
         "int8larger", "int8smaller",
         "int2and", "int2or", "int2xor", "int2not", "int2shl", "int2shr",
         "int4and", "int4or", "int4xor", "int4not", "int4shl", "int4shr",
         "int8and", "int8or", "int8xor", "int8not", "int8shl", "int8shr"],
        """
        SELECT ord,
               pg_catalog.int2larger(i2a, i2b), pg_catalog.int2smaller(i2a, i2b),
               pg_catalog.int4larger(i4a, i4b), pg_catalog.int4smaller(i4a, i4b),
               pg_catalog.int8larger(i8a, i8b), pg_catalog.int8smaller(i8a, i8b),
               i2a & i2b, i2a | i2b, i2a # i2b, ~i2a, i2a << 1, i2a >> 1,
               i4a & i4b, i4a | i4b, i4a # i4b, ~i4a, i4a << 1, i4a >> 1,
               i8a & i8b, i8a | i8b, i8a # i8b, ~i8a, i8a << 1, i8a >> 1
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "float_ops",
        ["float8pl", "float8mi", "float8mul", "float8div", "float8abs",
         "float8um", "float8up",
         "float4pl", "float4mi", "float4mul", "float4div", "float4abs",
         "float4um", "float4up",
         "float4eq", "float4ne", "float4lt", "float4le", "float4gt", "float4ge",
         "float8eq", "float8ne", "float8lt", "float8le", "float8gt", "float8ge",
         "float4larger", "float4smaller", "float8larger", "float8smaller"],
        """
        SELECT ord,
               f8a + f8b, f8a - f8b, f8a * f8b, f8a / NULLIF(f8b, 0.0::float8),
               abs(f8a), -f8a, +f8a,
               f4a + f4b, f4a - f4b, f4a * f4b, f4a / NULLIF(f4b, 0.0::float4),
               abs(f4a), -f4a, +f4a,
               (f4a = f4b)::int, (f4a <> f4b)::int, (f4a < f4b)::int,
               (f4a <= f4b)::int, (f4a > f4b)::int, (f4a >= f4b)::int,
               (f8a = f8b)::int, (f8a <> f8b)::int, (f8a < f8b)::int,
               (f8a <= f8b)::int, (f8a > f8b)::int, (f8a >= f8b)::int,
               pg_catalog.float4larger(f4a, f4b), pg_catalog.float4smaller(f4a, f4b),
               pg_catalog.float8larger(f8a, f8b), pg_catalog.float8smaller(f8a, f8b)
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "cross_float_ops",
        ["float48eq", "float48ne", "float48lt", "float48le", "float48gt", "float48ge",
         "float84eq", "float84ne", "float84lt", "float84le", "float84gt", "float84ge",
         "float48pl", "float48mi", "float48mul", "float48div",
         "float84pl", "float84mi", "float84mul", "float84div"],
        """
        SELECT ord,
               (f4a = f8b)::int, (f4a <> f8b)::int, (f4a < f8b)::int,
               (f4a <= f8b)::int, (f4a > f8b)::int, (f4a >= f8b)::int,
               (f8a = f4b)::int, (f8a <> f4b)::int, (f8a < f4b)::int,
               (f8a <= f4b)::int, (f8a > f4b)::int, (f8a >= f4b)::int,
               f4a + f8b, f4a - f8b, f4a * f8b, f4a / NULLIF(f8b, 0.0::float8),
               f8a + f4b, f8a - f4b, f8a * f4b, f8a / NULLIF(f4b, 0.0::float4)
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "casts",
        ["int48", "int84", "i2toi4", "i4toi2", "int28", "int82",
         "ftod", "dtof", "i4tod", "dtoi4", "i4tof", "ftoi4",
         "i8tod", "i8tof", "dtoi8", "ftoi8", "i2tod", "dtoi2", "i2tof",
         "ftoi2", "bool_int4", "int4_bool", "chartoi4", "i4tochar",
         "oidtoi8", "xid8toxid"],
        """
        SELECT ord,
               i4a::bigint, i8b::integer, i2a::integer, i4b::smallint,
               i2a::bigint, i8b::smallint,
               f4a::float8, f8b::float4, i4a::float8, f8b::integer,
               i4a::float4, f4b::integer, i8a::float8, f8b::bigint,
               i8a::float4, f4a::bigint,
               i2a::float8, f8b::smallint, i2a::float4, f4b::smallint,
               b1::integer, i4a::boolean, ch1::integer, i4b::"char",
               oid1::bigint, xid81::xid
        FROM jitter_surface_tmp.t
        WHERE i8b BETWEEN -32768 AND 32767
          AND i4b BETWEEN -32768 AND 32767
          AND f8b BETWEEN -32768 AND 32767
          AND f4b BETWEEN -32768 AND 32767
        ORDER BY ord
        """,
    ),
    c(
        "bool_system_scalars",
        ["booleq", "boolne", "boollt", "boolgt", "boolle", "boolge",
         "booland_statefunc", "boolor_statefunc",
         "chareq", "charne", "charlt", "charle", "chargt", "charge",
         "xideq", "xidneq", "cideq",
         "xid8eq", "xid8ne", "xid8lt", "xid8le", "xid8gt", "xid8ge", "xid8cmp",
         "pg_lsn_eq", "pg_lsn_ne", "pg_lsn_lt", "pg_lsn_le", "pg_lsn_gt", "pg_lsn_ge",
         "pg_lsn_cmp", "pg_lsn_larger", "pg_lsn_smaller", "pg_lsn_hash",
         "pg_lsn_hash_extended",
         "oideq", "oidne", "oidlt", "oidle", "oidgt", "oidge",
         "oidlarger", "oidsmaller"],
        """
        SELECT ord,
               (b1 = b2)::int, (b1 <> b2)::int, (b1 < b2)::int,
               (b1 > b2)::int, (b1 <= b2)::int, (b1 >= b2)::int,
               pg_catalog.booland_statefunc(b1, b2), pg_catalog.boolor_statefunc(b1, b2),
               (ch1 = ch2)::int, (ch1 <> ch2)::int, (ch1 < ch2)::int,
               (ch1 <= ch2)::int, (ch1 > ch2)::int, (ch1 >= ch2)::int,
               (xid1 = xid2)::int, (xid1 <> xid2)::int, (cid1 = cid2)::int,
               (xid81 = xid82)::int, (xid81 <> xid82)::int,
               (xid81 < xid82)::int, (xid81 <= xid82)::int,
               (xid81 > xid82)::int, (xid81 >= xid82)::int,
               pg_catalog.xid8cmp(xid81, xid82),
               (lsn1 = lsn2)::int, (lsn1 <> lsn2)::int,
               (lsn1 < lsn2)::int, (lsn1 <= lsn2)::int,
               (lsn1 > lsn2)::int, (lsn1 >= lsn2)::int,
               pg_catalog.pg_lsn_cmp(lsn1, lsn2),
               pg_catalog.pg_lsn_larger(lsn1, lsn2),
               pg_catalog.pg_lsn_smaller(lsn1, lsn2),
               pg_catalog.pg_lsn_hash(lsn1),
               pg_catalog.pg_lsn_hash_extended(lsn1, 42::bigint),
               (oid1 = oid2)::int, (oid1 <> oid2)::int,
               (oid1 < oid2)::int, (oid1 <= oid2)::int,
               (oid1 > oid2)::int, (oid1 >= oid2)::int,
               pg_catalog.oidlarger(oid1, oid2), pg_catalog.oidsmaller(oid1, oid2)
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "timestamp_date_time",
        ["timestamp_eq", "timestamp_ne", "timestamp_lt", "timestamp_le",
         "timestamp_gt", "timestamp_ge", "timestamp_cmp", "timestamp_larger",
         "timestamp_smaller", "timestamp_finite", "timestamp_hash",
         "timestamp_pl_interval", "timestamp_mi_interval",
         "timestamptz_pl_interval", "timestamptz_mi_interval", "timestamp_mi",
         "date_eq", "date_ne", "date_lt", "date_le", "date_gt", "date_ge",
         "date_larger", "date_smaller", "date_pli", "date_mii", "date_mi",
         "date_finite",
         "date_eq_timestamp", "date_ne_timestamp", "date_lt_timestamp",
         "date_le_timestamp", "date_gt_timestamp", "date_ge_timestamp",
         "date_cmp_timestamp", "timestamp_eq_date", "timestamp_ne_date",
         "timestamp_lt_date", "timestamp_le_date", "timestamp_gt_date",
         "timestamp_ge_date", "timestamp_cmp_date",
         "time_eq", "time_ne", "time_lt", "time_le", "time_gt", "time_ge", "time_cmp",
         "time_larger", "time_smaller", "time_hash", "time_hash_extended",
         "date_timestamp"],
        """
        SELECT ord,
               (ts1 = ts2)::int, (ts1 <> ts2)::int, (ts1 < ts2)::int,
               (ts1 <= ts2)::int, (ts1 > ts2)::int, (ts1 >= ts2)::int,
               pg_catalog.timestamp_cmp(ts1, ts2),
               pg_catalog.timestamp_larger(ts1, ts2), pg_catalog.timestamp_smaller(ts1, ts2),
               pg_catalog.isfinite(ts1), pg_catalog.isfinite(tstz1),
               pg_catalog.timestamp_hash(ts1),
               ts1 + iv1, ts1 - iv1, tstz1 + interval '30 minutes',
               tstz1 - interval '30 minutes', ts1 - ts2,
               (d1 = d2)::int, (d1 <> d2)::int, (d1 < d2)::int,
               (d1 <= d2)::int, (d1 > d2)::int, (d1 >= d2)::int,
               pg_catalog.date_larger(d1, d2), pg_catalog.date_smaller(d1, d2),
               d1 + 2, d1 - 2, d1 - d2, pg_catalog.isfinite(d1),
               (d1 = ts2)::int, (d1 <> ts2)::int, (d1 < ts2)::int,
               (d1 <= ts2)::int, (d1 > ts2)::int, (d1 >= ts2)::int,
               pg_catalog.date_cmp_timestamp(d1, ts2),
               (ts1 = d2)::int, (ts1 <> d2)::int, (ts1 < d2)::int,
               (ts1 <= d2)::int, (ts1 > d2)::int, (ts1 >= d2)::int,
               pg_catalog.timestamp_cmp_date(ts1, d2),
               (tm1 = tm2)::int, (tm1 <> tm2)::int, (tm1 < tm2)::int,
               (tm1 <= tm2)::int, (tm1 > tm2)::int, (tm1 >= tm2)::int,
               pg_catalog.time_cmp(tm1, tm2),
               pg_catalog.time_larger(tm1, tm2), pg_catalog.time_smaller(tm1, tm2),
               pg_catalog.time_hash(tm1), pg_catalog.time_hash_extended(tm1, 42::bigint),
               d1::timestamp
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "timestamp_tz_hash_pg18",
        ["timestamptz_hash"],
        """
        SELECT ord, pg_catalog.timestamptz_hash(tstz1)
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
        requires=["pg_catalog.timestamptz_hash(timestamp with time zone)"],
    ),
    c(
        "btree_cmp",
        ["btboolcmp", "btcharcmp", "btint2cmp", "btint4cmp", "btint8cmp",
         "btint24cmp", "btint42cmp", "btint28cmp", "btint48cmp",
         "btint82cmp", "btint84cmp",
         "btfloat4cmp", "btfloat8cmp", "btfloat48cmp", "btfloat84cmp",
         "btoidcmp", "date_cmp"],
        """
        SELECT ord,
               pg_catalog.btboolcmp(b1, b2),
               pg_catalog.btcharcmp(ch1, ch2),
               pg_catalog.btint2cmp(i2a, i2b),
               pg_catalog.btint4cmp(i4a, i4b),
               pg_catalog.btint8cmp(i8a, i8b),
               pg_catalog.btint24cmp(i2a, i4b),
               pg_catalog.btint42cmp(i4a, i2b),
               pg_catalog.btint28cmp(i2a, i8b),
               pg_catalog.btint48cmp(i4a, i8b),
               pg_catalog.btint82cmp(i8a, i2b),
               pg_catalog.btint84cmp(i8a, i4b),
               pg_catalog.btfloat4cmp(f4a, f4b),
               pg_catalog.btfloat8cmp(f8a, f8b),
               pg_catalog.btfloat48cmp(f4a, f8b),
               pg_catalog.btfloat84cmp(f8a, f4b),
               pg_catalog.btoidcmp(oid1, oid2),
               pg_catalog.date_cmp(d1, d2)
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "cash_ops",
        ["cash_eq", "cash_ne", "cash_lt", "cash_le", "cash_gt", "cash_ge",
         "cash_cmp", "cash_pl", "cash_mi", "cashlarger", "cashsmaller"],
        """
        SELECT ord,
               (cash1 = cash2)::int, (cash1 <> cash2)::int,
               (cash1 < cash2)::int, (cash1 <= cash2)::int,
               (cash1 > cash2)::int, (cash1 >= cash2)::int,
               pg_catalog.cash_cmp(cash1, cash2),
               cash1 + cash2, cash1 - cash2,
               pg_catalog.cashlarger(cash1, cash2), pg_catalog.cashsmaller(cash1, cash2)
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "range_subdiff",
        ["int4range_subdiff", "int8range_subdiff", "daterange_subdiff",
         "tsrange_subdiff", "tstzrange_subdiff"],
        """
        SELECT ord,
               pg_catalog.int4range_subdiff(i4a, i4b),
               pg_catalog.int8range_subdiff(i8a, i8b),
               pg_catalog.daterange_subdiff(d1, d2),
               pg_catalog.tsrange_subdiff(ts1, ts2),
               pg_catalog.tstzrange_subdiff(tstz1, tstz2)
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "text_ops",
        ["textlen", "textoctetlen", "text_starts_with",
         "texteq", "textne", "text_lt", "text_le", "text_gt", "text_ge",
         "bttextcmp", "text_larger", "text_smaller", "hashtext",
         "text_pattern_lt", "text_pattern_le", "text_pattern_ge",
         "text_pattern_gt", "bttext_pattern_cmp"],
        """
        SELECT ord,
               length(txt1), octet_length(txt1), pg_catalog.starts_with(txt2, txt1),
               (txt1 COLLATE "C" = txt2 COLLATE "C")::int,
               (txt1 COLLATE "C" <> txt2 COLLATE "C")::int,
               (txt1 COLLATE "C" < txt2 COLLATE "C")::int,
               (txt1 COLLATE "C" <= txt2 COLLATE "C")::int,
               (txt1 COLLATE "C" > txt2 COLLATE "C")::int,
               (txt1 COLLATE "C" >= txt2 COLLATE "C")::int,
               pg_catalog.bttextcmp(txt1 COLLATE "C", txt2 COLLATE "C"),
               pg_catalog.text_larger(txt1 COLLATE "C", txt2 COLLATE "C"),
               pg_catalog.text_smaller(txt1 COLLATE "C", txt2 COLLATE "C"),
               pg_catalog.hashtext(txt1),
               pg_catalog.text_pattern_lt(txt1, txt2),
               pg_catalog.text_pattern_le(txt1, txt2),
               pg_catalog.text_pattern_ge(txt1, txt2),
               pg_catalog.text_pattern_gt(txt1, txt2),
               pg_catalog.bttext_pattern_cmp(txt1, txt2)
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "numeric_ops",
        ["numeric_eq", "numeric_ne", "numeric_lt", "numeric_le", "numeric_gt",
         "numeric_ge", "numeric_cmp", "numeric_larger", "numeric_smaller",
         "hash_numeric", "numeric_abs", "numeric_uminus",
         "numeric_add", "numeric_sub", "numeric_mul"],
        """
        SELECT ord,
               (n1 = n2)::int, (n1 <> n2)::int, (n1 < n2)::int,
               (n1 <= n2)::int, (n1 > n2)::int, (n1 >= n2)::int,
               pg_catalog.numeric_cmp(n1, n2),
               pg_catalog.numeric_larger(n1, n2), pg_catalog.numeric_smaller(n1, n2),
               pg_catalog.hash_numeric(n1),
               abs(n1), -n1, n1 + n2, n1 - n2, n1 * n2
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "uuid_ops",
        ["uuid_eq", "uuid_lt", "uuid_cmp", "uuid_hash"],
        """
        SELECT ord,
               (u1 = u2)::int, (u1 < u2)::int,
               pg_catalog.uuid_cmp(u1, u2), pg_catalog.uuid_hash(u1)
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "hash_ops",
        ["hashint2", "hashint4", "hashint8", "hashoid",
         "hashint2extended", "hashint4extended", "hashint8extended", "hashoidextended",
         "hashfloat4", "hashfloat8"],
        """
        SELECT ord,
               pg_catalog.hashint2(i2a), pg_catalog.hashint4(i4a),
               pg_catalog.hashint8(i8a), pg_catalog.hashoid(oid1),
               pg_catalog.hashint2extended(i2a, 42::bigint),
               pg_catalog.hashint4extended(i4a, 42::bigint),
               pg_catalog.hashint8extended(i8a, 42::bigint),
               pg_catalog.hashoidextended(oid1, 42::bigint),
               pg_catalog.hashfloat4(f4a), pg_catalog.hashfloat8(f8a)
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
    c(
        "hash_ops_pg18",
        ["hashbool", "hashdate", "hashdateextended"],
        """
        SELECT ord,
               pg_catalog.hashbool(b1),
               pg_catalog.hashdate(d1),
               pg_catalog.hashdateextended(d1, 42::bigint)
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
        requires=[
            "pg_catalog.hashbool(boolean)",
            "pg_catalog.hashdate(date)",
            "pg_catalog.hashdateextended(date,bigint)",
        ],
    ),
    c(
        "aggregate_ops",
        ["int8inc_any", "int8dec_any", "int2_sum", "int4_sum",
         "int4_avg_accum", "int8_avg_accum"],
        """
        SELECT 'direct' AS kind,
               pg_catalog.int8inc_any(i8a, i4a)::text,
               pg_catalog.int8dec_any(i8a, i4a)::text,
               NULL::text,
               NULL::text,
               NULL::text
        FROM jitter_surface_tmp.t
        UNION ALL
        SELECT 'aggregate' AS kind,
               count(i4a)::text,
               sum(i2a)::text,
               sum(i4a)::text,
               avg(i4a)::text,
               avg(i8a)::text
        FROM jitter_surface_tmp.t
        ORDER BY kind
        """,
    ),
    c(
        "interval_ops",
        ["interval_eq", "interval_ne", "interval_lt", "interval_le",
         "interval_gt", "interval_ge", "interval_cmp", "interval_smaller",
         "interval_larger", "interval_hash", "interval_pl", "interval_mi"],
        """
        SELECT ord,
               (iv1 = iv2)::int, (iv1 <> iv2)::int, (iv1 < iv2)::int,
               (iv1 <= iv2)::int, (iv1 > iv2)::int, (iv1 >= iv2)::int,
               pg_catalog.interval_cmp(iv1, iv2),
               pg_catalog.interval_smaller(iv1, iv2),
               pg_catalog.interval_larger(iv1, iv2),
               pg_catalog.interval_hash(iv1),
               iv1 + iv2, iv1 - iv2
        FROM jitter_surface_tmp.t
        ORDER BY ord
        """,
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare pg_jitter function-surface output with jit=off baseline."
    )
    parser.add_argument("--pg-config", default=os.environ.get("PG_CONFIG", "pg_config"))
    parser.add_argument("--host", default=os.environ.get("PGHOST", "/tmp"))
    parser.add_argument("--port", default=os.environ.get("PGPORT", "5432"))
    parser.add_argument("--db", default=os.environ.get("PGDB", "postgres"))
    parser.add_argument(
        "--backend",
        default=os.environ.get("BACKEND", "all"),
        help="Backend to test: sljit, asmjit, mir, auto, or all",
    )
    parser.add_argument(
        "--project-dir",
        default=str(Path(__file__).resolve().parents[1]),
        help="pg_jitter source directory for coverage checking",
    )
    parser.add_argument(
        "--no-coverage-check",
        action="store_true",
        help="Do not fail when jit_direct_fns contains entries absent from the manifest",
    )
    return parser.parse_args()


class Psql:
    def __init__(self, pg_config: str, host: str, port: str, db: str):
        self.pg_config = pg_config
        self.host = host
        self.port = port
        self.db = db
        self.bindir = self._pg_config("--bindir")
        self.pkglibdir = self._pg_config("--pkglibdir")
        self.libdir = self._pg_config("--libdir")
        self.pkglib_candidates = [Path(self.pkglibdir)]
        lib_postgresql = Path(self.libdir) / "postgresql"
        if lib_postgresql.is_dir() and lib_postgresql != Path(self.pkglibdir):
            self.pkglib_candidates.append(lib_postgresql)
        self.psql = str(Path(self.bindir) / "psql")

    def _pg_config(self, arg: str) -> str:
        return subprocess.check_output([self.pg_config, arg], text=True).strip()

    def run(self, sql: str) -> str:
        cmd = [
            self.psql,
            "-h", self.host,
            "-p", self.port,
            "-d", self.db,
            "-X",
            "-q",
            "-t",
            "-A",
            "-v", "ON_ERROR_STOP=1",
            "-c", sql,
        ]
        proc = subprocess.run(cmd, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)
        if proc.returncode != 0:
            raise RuntimeError(proc.stdout.strip())
        return proc.stdout.strip()


def detect_backends(pkglibdirs: list[Path], pg_version_num: int) -> list[str]:
    if sys.platform == "darwin":
        ext = "dylib"
    elif os.name == "nt":
        ext = "dll"
    else:
        ext = "so"

    found: list[str] = []
    for backend in ("sljit", "asmjit", "mir"):
        if any(Path(pkglibdir, f"pg_jitter_{backend}.{ext}").exists()
               for pkglibdir in pkglibdirs):
            found.append(backend)
    if pg_version_num >= 170000 and any(Path(pkglibdir, f"pg_jitter.{ext}").exists()
                                        for pkglibdir in pkglibdirs):
        found.append("auto")
    if not found:
        paths = ", ".join(str(path) for path in pkglibdirs)
        raise RuntimeError(f"no pg_jitter provider libraries found in {paths}")
    return found


def wrap_case_sql(sql: str) -> str:
    return f"""
WITH case_rows AS MATERIALIZED (
{sql}
)
SELECT md5(coalesce(string_agg(row_to_json(case_rows)::text, E'\\n'
                               ORDER BY row_to_json(case_rows)::text), ''))
FROM case_rows;
"""


def backend_settings(backend: str) -> str:
    return f"SET pg_jitter.backend = '{backend}';\n"


def settings_for_backend(backend: str) -> str:
    if backend == "baseline":
        return f"{COMMON_SETTINGS}\nSET jit = off;\n"
    return (
        f"{backend_settings(backend)}"
        f"{COMMON_SETTINGS}\n"
        "SET jit = on;\n"
    )


def run_case(psql: Psql, backend: str, case: Case) -> str:
    return psql.run(settings_for_backend(backend) + wrap_case_sql(case.sql))


def regprocedure_exists(psql: Psql, regprocedure: str) -> bool:
    escaped = regprocedure.replace("'", "''")
    sql = f"SELECT to_regprocedure('{escaped}') IS NOT NULL;"
    return psql.run(sql) == "t"


def _eval_pg_version_if(line: str, pg_version_num: int) -> bool | None:
    match = re.match(r"#if\s+PG_VERSION_NUM\s*([<>]=?|==|!=)\s*(\d+)", line)
    if not match:
        return None
    op, raw_value = match.groups()
    value = int(raw_value)
    if op == ">=":
        return pg_version_num >= value
    if op == ">":
        return pg_version_num > value
    if op == "<=":
        return pg_version_num <= value
    if op == "<":
        return pg_version_num < value
    if op == "==":
        return pg_version_num == value
    if op == "!=":
        return pg_version_num != value
    return None


def extract_direct_fns(project_dir: Path, pg_version_num: int) -> set[str]:
    path = project_dir / "src" / "pg_jit_funcs.c"
    text = path.read_text()
    try:
        body = text.split("const JitDirectFn jit_direct_fns[] = {", 1)[1]
        body = body.split("};", 1)[0]
    except IndexError as exc:
        raise RuntimeError(f"could not locate jit_direct_fns in {path}") from exc

    active_lines: list[str] = []
    active_stack = [True]
    for raw_line in body.splitlines():
        line = raw_line.strip()
        pg_version_if = _eval_pg_version_if(line, pg_version_num)
        if pg_version_if is not None:
            active_stack.append(active_stack[-1] and pg_version_if)
            continue
        if line.startswith("#ifdef") or line.startswith("#ifndef"):
            active_stack.append(active_stack[-1])
            continue
        if line.startswith("#else"):
            if len(active_stack) > 1:
                parent = active_stack[-2]
                active_stack[-1] = parent and not active_stack[-1]
            continue
        if line.startswith("#endif"):
            if len(active_stack) > 1:
                active_stack.pop()
            continue
        if active_stack[-1]:
            active_lines.append(raw_line)

    active_body = "\n".join(active_lines)
    pattern = re.compile(
        r"\b(?:E[0-4]|EI[0-4]|EC[0-4]|EIC[0-4])\s*\(\s*"
        r"([A-Za-z_][A-Za-z0-9_]*)"
    )
    return {match.group(1) for match in pattern.finditer(active_body)}


def manifest_coverage(psql: Psql) -> set[str]:
    covered: set[str] = set()
    for case in CASES:
        missing = [req for req in case.requires if not regprocedure_exists(psql, req)]
        if missing:
            continue
        covered.update(case.covers)
    return covered


def run_backend(psql: Psql, backend: str) -> tuple[int, set[str]]:
    print(f"=== backend: {backend} ===")
    failures = 0
    executed: set[str] = set()

    for case in CASES:
        missing = [req for req in case.requires
                   if not regprocedure_exists(psql, req)]
        if missing:
            print(f"SKIP: {case.name} ({', '.join(missing)} absent)")
            continue

        try:
            baseline = run_case(psql, "baseline", case)
        except RuntimeError as exc:
            print(f"FAIL: {case.name} baseline error")
            print(str(exc))
            failures += 1
            continue

        try:
            observed = run_case(psql, backend, case)
        except RuntimeError as exc:
            print(f"FAIL: {case.name} {backend} error")
            print(str(exc))
            failures += 1
            continue

        if baseline != observed:
            print(f"FAIL: {case.name}")
            print(f"  baseline: {baseline}")
            print(f"  observed: {observed}")
            failures += 1
        else:
            print(f"PASS: {case.name}")
            executed.update(case.covers)

    print(f"executed_manifest_entries={len(executed)}")
    return failures, executed


def main() -> int:
    args = parse_args()
    psql = Psql(args.pg_config, args.host, args.port, args.db)

    pg_version_num = int(psql.run("SHOW server_version_num;"))

    if args.backend == "all":
        backends = detect_backends(psql.pkglib_candidates, pg_version_num)
    else:
        backends = args.backend.split()

    project_dir = Path(args.project_dir)
    direct_fns = extract_direct_fns(project_dir, pg_version_num)
    declared = manifest_coverage(psql)
    missing_from_manifest = sorted(direct_fns - declared)
    extra_in_manifest = sorted(declared - direct_fns)

    failures = 0
    if missing_from_manifest:
        print("FAIL: manifest does not cover direct-function entries:")
        for name in missing_from_manifest:
            print(f"  {name}")
        failures += 1
    if extra_in_manifest:
        print("FAIL: manifest names not present in current direct table:")
        for name in extra_in_manifest:
            print(f"  {name}")
        failures += 1

    if (missing_from_manifest or extra_in_manifest) and not args.no_coverage_check:
        return failures

    psql.run(SETUP_SQL)

    all_executed: set[str] = set()
    for backend in backends:
        backend_failures, executed = run_backend(psql, backend)
        failures += backend_failures
        all_executed.update(executed)

    not_executed = sorted((direct_fns & declared) - all_executed)
    if not_executed:
        print("NOTE: manifest-covered direct entries not executed in this run:")
        for name in not_executed:
            print(f"  {name}")

    print(f"direct_table_entries={len(direct_fns)}")
    print(f"manifest_entries={len(declared)}")
    print(f"executed_entries={len(all_executed)}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
