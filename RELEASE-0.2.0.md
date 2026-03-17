# pg_jitter v0.2.0 Release Notes

## Highlights

- **SIMD acceleration**: StringZilla (fast string operations), simdjson (fast json operations)
- **PCRE2 regex/LIKE acceleration**: PCRE2 JIT for Unicode-correct, SIMD-accelerated pattern matching. 2-5x faster ILIKE/regex on all collations.
- **Text IN-list hash tables**: JIT-compiled hash probe for text `IN (...)` expressions. 1.4x faster for 20-element text IN-lists.
- **Sparse deform for wide nullable tables**: Byte-level bitmap processing with NEON bulk-zero for tables with 64-500 nullable columns. 1.2-1.5x faster deform.
- **CASE binary search**: O(log N) binary search replaces O(N) linear scan for monotonic CASE expressions. 4-7x faster for 50-100 branch CASE.
- **simdjson JSON acceleration**: SIMD-parallel JSON validation and parsing via simdjson for IS JSON, json_in, and jsonb_in.
- **Parallel shared code correctness**: Fixed multiple shared-mode bugs and enabled text hash + PCRE2 optimizations in shared parallel mode.
- **Windows support**: Full MSVC build, Win64 SEH unwind registration, CI on Windows.

## New Features

### Integer IN-list Optimization
- Constant integer `IN (...)` lists extracted at JIT compile time, sorted, and compiled into inline binary search trees
- Up to `pg_jitter.in_bsearch_max` (default 4096) elements: O(log N) inline CMP+branch tree (~5 comparisons for 20 elements vs PG's ~30-instruction Jenkins hash + chained probe)
- Larger lists: CRC32C open-addressing hash table with hardware-accelerated hashing (ARM64 `crc32cw` / x86 `_mm_crc32_u32`), inline linear probe in SLJIT
- SIMD scan for small arrays: NEON `vceqq_s32` / SSE2 `_mm_cmpeq_epi32` for int4/int8 arrays in `SCALARARRAYOP`
- ~1.9x-1.5x faster for integer IN-lists up to `pg_jitter.in_bsearch_max` size. ~1.1x-1.2x for larger lists

### Text IN-list Hash Table
- Open-addressing hash table built at JIT compile time for constant text arrays
- Hybrid hash: hardware CRC32C for strings <= 16 bytes, StringZilla for longer
- Supports deterministic collations (C, en_US.UTF-8, ICU)
- Works in all 3 backends (sljit, asmjit, mir) and in parallel shared mode
- Each parallel worker builds its own process-local table from plan constants
- ~1.4x faster for 20-element text IN-lists

### PCRE2 Regex/LIKE Acceleration
- PCRE2 with `PCRE2_UTF + PCRE2_UCP` handles Unicode POSIX classes correctly
- Direct JIT function call (inline fast path) bypasses `pcre2_jit_match()` overhead
- Pre-allocated match data and JIT stack eliminate per-call allocation
- 2-5x faster than PG's built-in `RE_compile_and_execute`
- Works in parallel shared mode (workers compile own PCRE2 patterns)

### Compiled LIKE Matching
- Multi-segment LIKE patterns parsed into anchor+floating segments with literal pieces
- StringZilla `sz_find` for SIMD-accelerated substring search on longest piece
- Handles escaped characters, multiple wildcards, underscore patterns
- Tiered dispatch: simple patterns → StringZilla, complex → PCRE2, fallback → V1

### CASE Binary Search
- Detects monotonic CASE patterns at JIT compile time (searched and simple CASE)
- Replaces O(N) branch scanning with O(log N) binary search
- Supports all ordered types: int2/int4/int8, float4/float8, date, timestamp, oid, text, numeric, uuid, interval, etc.
- Compact runtime descriptor with pre-sorted thresholds and results
- 4-7x faster for 50-100 branch CASE expressions

### Sparse Deform for Wide Nullable Tables
- C helper function processes null bitmap byte-by-byte with CTZ bit scanning
- NEON bulk-zero on ARM64: 5 instructions for 8 all-null columns
- All-null bytes (0x00) skipped entirely, all-present bytes (0xFF) extracted without bitmap checks
- Activates for nullable tables with 64-500 columns in local mode
- Tables > 500 nullable columns fall back to PG's interpreter (memory-bandwidth limited)

### simdjson SIMD-Accelerated JSON Parsing
- Integrated [simdjson](https://simdjson.org) for hardware-accelerated JSON processing
- **IS JSON validation**: SIMD-parallel structural validation via simdjson DOM parser, replacing PG's byte-by-byte `json_validate()`. Inputs < 64 bytes fall back to PG's native parser.
- **json_in acceleration**: simdjson validates the JSON string before returning the text datum, avoiding PG's character-level parsing overhead
- **jsonb_in acceleration**: simdjson on-demand parser walks the JSON tree and builds binary jsonb via `pushJsonbValue()`, replacing PG's recursive descent parser
- JIT-compiled `EEOP_IS_JSON` and `EEOP_IOCOERCE` (json/jsonb input) opcodes call simdjson wrappers directly
- All three backends (sljit, asmjit, mir) emit direct calls to simdjson wrappers

### JSONB Fast Path
- Direct `getKeyJsonValueFromContainer` call for `doc->>'key'` with constant key
- Bypasses FunctionCallInfo setup and PG_GETARG_TEXT_PP for the key argument
- Handles string, numeric, boolean, and nested JSON values

### Adaptive Backend Selection (experimental)
- `pg_jitter.backend = 'auto'` mode selects the fastest backend per expression profile
- Epsilon-greedy exploration with configurable sample count and exploration rate
- Process-local stats table (no shared memory overhead)
- Statistics only collected when backend is set to `auto`
