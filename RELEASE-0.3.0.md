# pg_jitter v0.3.0 Release Notes

## Highlights

- **460 direct-call functions** (up from ~350): 94 new functions including type casts, cross-type float operations, and aggregate transitions — all bypass fmgr V1 overhead
- **10 inline type cast operations**: Zero-overhead machine instructions for int widening/narrowing and float conversions — no function call at all
- **6 new SLJIT SIMD operations**: ADD, SUB, CMPEQ, CMPGT, MIN_S, MAX_S for both ARM64 NEON and x86 SSE2/AVX2
- **SIMD text equality**: Inline 16-byte CMPEQ+sign comparison for 8-16 byte strings, replacing memcmp call
- **SIMD IN-list with CMPEQ**: 3-instruction compare+movemask replaces 9-instruction XOR+store+scalar-check per chunk
- **Inline prefix LIKE**: Zero function calls for `LIKE 'prefix%'` patterns up to 8 bytes — 1.27x faster
- **Inline int4_avg_accum**: Direct array pointer increment for `avg(int4)` transitions, bypassing V1 fmgr + array validation
- **CASE dead code elimination**: Skip codegen for dead WHEN-condition steps in range CASE binary search
- **Travis CI**: s390x and ppc64le builds with PG14+PG16, FreeBSD x86_64, full regression testing with baseline comparison

## Performance

Additional benchmarks:

### OLAP Sustained Workload (60s, 4 clients, star-schema analytics)

| Backend | TPS Improvement | Latency |
|---------|----------------|---------|
| sljit | **+42%** | 1275ms |
| asmjit | +32% | 1340ms |
| mir | +24% | 1430ms |

### JIT Showcase (CRM analytics, expression-heavy queries)

| Backend | Total Speedup | Geomean |
|---------|--------------|---------|
| sljit | **4.9x** | **3.6x** |
| asmjit | 4.6x | 3.5x |
| mir | 4.5x | 3.3x |

### TPC-H (SF=1, 22 standard queries)

| Backend | Geomean | Total Saved |
|---------|---------|-------------|
| sljit | **1.06x** | 6% faster |
| asmjit | 1.04x | 4% faster |

### TPC-C (OLTP, jit_above_cost=200)

| Backend | Geomean (>1ms queries) | All faster? |
|---------|----------------------|-------------|
| sljit | **1.08x** | 5/5 |

## New Features

### Type Cast Function Inlining

94 new direct-call function entries covering:
- **18 type cast functions**: int widening (int48, int28, i2toi4), int narrowing with range checks (int84, int82, i4toi2), float↔float (ftod, dtof), int→float (i4tod, i4tof, i8tod, i2tod, i2tof), float→int with rint() rounding (dtoi4, ftoi4, dtoi8, dtoi2, ftoi2)
- **12 cross-type float comparisons**: float48eq/ne/lt/le/gt/ge, float84eq/ne/lt/le/gt/ge
- **8 cross-type float arithmetic**: float48pl/mi/mul/div, float84pl/mi/mul/div
- **6 extended hash functions**: hashint2/4/8extended, hashoid/float4/float8extended (DEFERRED for hash correctness)
- **2 aggregate transitions**: int4_avg_accum, int8_avg_accum (direct array pointer manipulation)

10 inline type cast operations emit 1-3 machine instructions with no function call:
- `int4→int8`: single `SLJIT_MOV_S32` (sign-extend)
- `int2→int4`, `int2→int8`: single `SLJIT_MOV_S16`
- `int8→int4`, `int4→int2`, `int8→int2`: compare + overflow error + truncate
- `float4→float8`: `fcopy` + `fconv` + `fcopy` (3 instructions)
- `int4→float8`, `int8→float8`: `fconv` + `fcopy` (2 instructions)

### SLJIT SIMD Extensions

6 new SIMD operations added to the SLJIT library (`sljitLir.h`):

| Operation | ARM64 NEON | x86 SSE2/SSE4.1 |
|-----------|-----------|-----------------|
| `SLJIT_SIMD_OP2_ADD` | ADD v.4s | PADDD |
| `SLJIT_SIMD_OP2_SUB` | SUB v.4s | PSUBD |
| `SLJIT_SIMD_OP2_CMPEQ` | CMEQ v.4s | PCMPEQD |
| `SLJIT_SIMD_OP2_CMPGT` | CMGT v.4s | PCMPGTD |
| `SLJIT_SIMD_OP2_MIN_S` | SMIN v.4s | PMINSD |
| `SLJIT_SIMD_OP2_MAX_S` | SMAX v.4s | PMAXSD |

All operations support 8/16/32-bit element sizes (64-bit for ADD/SUB/CMPEQ/CMPGT on ARM64). Unsupported operations return `SLJIT_ERR_UNSUPPORTED` with proper `default:` fallback in the switch — safe on platforms without implementations.

### SIMD Text Equality

Inline SLJIT comparison for short-header text values (8-16 bytes):
1. Load 16 bytes from each string into SIMD registers
2. CMPEQ byte-by-byte: matching bytes → 0xFF, different → 0x00
3. Extract sign bits (MSB of each byte)
4. Mask to `data_len` bits, check if all set

Runtime probe (`SLJIT_SIMD_TEST`) ensures graceful fallback to `memcmp` on platforms without SIMD CMPEQ support.

### SIMD IN-list with CMPEQ

Replaced the XOR+store+4-scalar-checks pattern (9 instructions per chunk) with CMPEQ+sign+test (3 instructions):
- Old: `XOR` → store to stack → 4× (load lane, compare to zero)
- New: `CMPEQ` → `simd_sign` → test non-zero

### Inline Prefix LIKE

For `LIKE 'prefix%'` patterns with prefix ≤ 8 bytes, the fast path emits inline machine instructions:
1. Load varlena header byte, check short-header form
2. Extract data_len, compare ≥ pattern_len
3. Load 8 bytes from data, mask to pattern_len, compare against immediate
4. Slow path for 4-byte headers falls back to `simd_like_match_text`

Zero function calls on the fast path (~8 instructions total).

### CASE Dead Code Elimination

When CASE binary search is active, the linear WHEN-condition steps are dead code. A skip bitset marks dead steps during compilation — only THEN expressions, JUMPs, and ELSE steps are compiled. Reduces compilation time and I-cache pressure for large CASE expressions.

### Inline int4_avg_accum

`avg(int4)` and `avg(int8)` transition functions bypass V1 fmgr entirely:
- Direct pointer access to `Int8TransTypeData` inside the ArrayType
- Two increments: `count++; sum += newval;`
- No `AggCheckCallContext`, no array validation, no `ARR_DATA_PTR` indirection

## CI/CD

### Travis CI

- **ppc64le**, **FreeBSD x86_64**: PG14-18 builds with full regression testing
- **s390x**: PG14 (Ubuntu Jammy) + PG16 (Ubuntu Noble) with baseline comparison
- Baseline comparison: runs regression tests with jit=off first, then compares JIT output — only fails if JIT introduces new regressions
- Uses `vladich/sljit` fork for SIMD extensions

### Build System

- CMake extracts `PG_VERSION_NUM` from `pg_config --version` for feature gating
- `PG_JITTER_HAVE_YYJSON` + `PG_VERSION_NUM >= 160000` guards for IS JSON support
- Separate `-Wl,-u` linker flags for PG14/15 (no `pg_jitter_yj_is_json_datum`)

## Regression Testing

- All Postgres core regression tests pass on PG14, PG15, PG16, PG17, PG18
- All 4 backends (sljit, asmjit, mir, auto) pass on all versions
- New `tests/sql/jit_slt_failures.sql`: 50+ regression tests covering cross-type float underflow patterns
- Parallel regression runner (`run_pg_regress.sh`) with per-port work directories, `DROP DATABASE` cleanup between backends, version-specific `pg_regress` binary, and `testtablespace` directory creation

## Compatibility

| PostgreSQL | Status |
|-----------|--------|
| PG14 | All backends pass |
| PG15 | All backends pass |
| PG16 | All backends pass |
| PG17 | All backends pass |
| PG18 | All backends pass |

| Platform | Status |
|----------|--------|
| ARM64 (Apple M1/M2, Linux aarch64) | Full support |
| x86_64 (Linux, FreeBSD, Windows) | Full support |
| ppc64le (Linux) | Full support |
| s390x (Linux) | Full support |
