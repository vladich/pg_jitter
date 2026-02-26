# pg_jitter

A lightweight JIT compilation provider for PostgreSQL that adds three alternative JIT backends — **sljit**, **AsmJIT**, and **MIR** — delivering faster compilation and competitive query execution across PostgreSQL 14–18.

## Why?

JIT compilation was introduced in Postgres 11 in 2018. It solves a problem of Postgres having to interpret expressions and use inefficient per-row loops in run-time in order to do internal data conversions (so-called tuple deforming).
On expression-heavy workloads or just wide tables, it can give a significant performance boost for those operations. However, standard LLVM-based JIT is notoriously slow at compilation.
When your compilation lasts tens to hundreds of milliseconds, it may be suitable only for very heavy queries, in some cases.
On typical OLTP queries, LLVM's JIT overhead can exceed the execution time of the query itself.
pg_jitter provides native code generation with microsecond-level compilation times instead of milliseconds, making JIT worthwhile for a much wider range of queries.

## Performance

Typical compilation time:
- **sljit**: tens to low hundreds of microseconds
- **AsmJIT**: hundreds of microseconds
- **MIR**: hundreds of microseconds to single milliseconds

In reality, the effect of JIT compilation is broader - execution can slow down for up to ~1ms even for sljit, because of other related things, mostly cold processor cache.
Therefore, on system executing a lot of queries per second, it's recommended to avoid JIT compilation for very fast queries such as point lookups or queries processing only a few records.
Default `jit_above_cost` parameter is set to a very high cost (100'000). This makes sense for LLVM, but doesn't make sense for these providers.
It's recommended to set this parameter value to something from few hundreds to few thousands.

- pg_jitter backends are **always faster than LLVM**, if we count compilation speed (often 2–3x faster)
- **sljit** is the most consistent: 5–25% faster than the interpreter across all workloads. this, and also its phenomenal compilation speed makes it the best choice for most scenarios.
- **AsmJIT** excels on wide-row/deform-heavy queries (up to 32% faster) thanks to specialized tuple deforming
- **MIR** provides solid gains while being the most portable backend
- **LLVM was supposed to be fast** due to clang optimization advantages, but in fact in most cases it's slower than all 3 supported backends, even not counting compilation performance differences. This is due to zero-cost inlining using compile-time pre-extracted code and manual instruction-level optimization.

## Features

- **Zero-config** — set `jit_provider` and go
- **Three independent backends** with different strengths
- **Runtime backend switching** via `SET pg_jitter.backend = 'sljit'` (no restart)
- **PostgreSQL 14–18** support from one codebase
- **Two-tier function optimization** — 350+ hot-path PG functions compiled as direct native calls
- **No LLVM dependency** — pure C/C++ with small, embeddable libraries
- **Precompiled function blobs** — optional build-time native code extraction for zero-cost inlining
- **Leak-free** — verified stable RSS across 10,000 compile/release cycles
- **Supported platforms** - 2 out of 3 providers can be used on all Postgres-supported platforms. AsmJit is ARM64 and x86 only.

## Quick Start

### Prerequisites

- PostgreSQL 14–18 (with development headers)
- CMake >= 3.16
- C11 and C++17 compilers
- Backend libraries as sibling directories:

```
parent/
├── pg_jitter/
├── sljit/        # https://github.com/nicktehrany/sljit
├── asmjit/       # https://github.com/asmjit/asmjit
└── mir/          # https://github.com/nicktehrany/mir
```

### Build

```bash
# Build all backends
./build.sh

# Build a single backend
./build.sh sljit

# Custom PostgreSQL installation
./build.sh --pg-config /opt/pg17/bin/pg_config all

# With precompiled function blobs (optional, pick one)
./build.sh all -DPG_JITTER_USE_LLVM=ON      # requires clang + llvm-objdump
./build.sh all -DPG_JITTER_USE_C2MIR=ON     # uses MIR (no extra deps)

# Custom dependency paths
./build.sh all -DSLJIT_DIR=/path/to/sljit -DMIR_DIR=/path/to/mir
```

### Install

```bash
# Install all backends and restart PostgreSQL
./install.sh

# Custom paths
./install.sh --pg-config /opt/pg17/bin/pg_config --pgdata /var/lib/postgresql/data all
```

### Configure

```sql
-- Use a specific backend directly
ALTER SYSTEM SET jit_provider = 'pg_jitter_sljit';
SELECT pg_reload_conf();

-- Or use the meta provider for runtime switching (no restart needed)
ALTER SYSTEM SET jit_provider = 'pg_jitter';
SELECT pg_reload_conf();

SET pg_jitter.backend = 'asmjit';  -- switch on the fly
```

## Architecture

### Expression Compilation

pg_jitter implements PostgreSQL's `JitProviderCallbacks` interface. When PostgreSQL decides to JIT-compile a query, it calls `compile_expr()` which:

1. Walks the `ExprState->steps[]` array (PostgreSQL's expression evaluation opcodes)
2. Emits native machine code for ~30 hot-path opcodes (arithmetic, comparisons, variable access, tuple deforming, aggregation, boolean logic, jumps)
3. Delegates remaining opcodes to `pg_jitter_fallback_step()` which calls the corresponding `ExecEval*` C functions
4. Installs the compiled function with a one-time validation wrapper that catches `ALTER COLUMN TYPE` invalidation

### Two-Tier Function Optimization

- **Tier 1** (~350 functions): Pass-by-value operations (int, float, bool, date, timestamp, OID) compiled as direct native calls with inline overflow checking. No `FunctionCallInfo` overhead.
- **Tier 2**: Pass-by-reference operations (numeric, text, interval, uuid) called through `DirectFunctionCall` C wrappers. Optionally LLVM-optimized when built with `-DPG_JITTER_USE_LLVM=ON`.

### Three JIT Backends

| | sljit | AsmJIT | MIR |
|---|---|---|---|
| **Language** | C | C++ | C |
| **IR level** | Low-level (register machine) | Low-level (native assembler) | Medium-level (typed ops) |
| **Register allocation** | Manual | Virtual (automatic) | Automatic |
| **Architectures** | arm64, x86_64, s390x, ppc, mips, riscv | arm64, x86_64 | Portable (any POSIX) |
| **Compilation speed** | Fastest | Fast | Fast (~1.4ms init/query) |
| **Best for** | General workloads, lowest overhead | Wide rows, deform-heavy queries | Portability |
| **Library size** | ~100 KB | ~300 KB | ~200 KB |

### Meta Provider

The meta provider (`jit_provider = 'pg_jitter'`) is a thin dispatcher that:

- Exposes a `pg_jitter.backend` GUC (user-settable, no restart required)
- Lazily loads backend shared libraries on first use
- Caches loaded backends for process lifetime
- Falls back to the next available backend if the selected one isn't installed

Each backend remains independently usable by setting `jit_provider = 'pg_jitter_sljit'` directly.

### Memory Management

JIT-compiled code is tied to PostgreSQL's ResourceOwner system:

1. A `PgJitterContext` is created per query, extending PostgreSQL's `JitContext`
2. Each compiled function is registered on a linked list with a backend-specific free callback
3. When the query's ResourceOwner is released, all compiled code is freed:
   - **sljit**: `sljit_free_code()` — releases mmap'd executable memory
   - **AsmJIT**: `JitRuntime::release()` — frees the code buffer
   - **MIR**: `MIR_gen_finish()` + `MIR_finish()` — tears down the entire MIR context
4. Verified leak-free: RSS remains flat across 10,000 compile/release cycles for all backends

### Version Compatibility

A single codebase supports PostgreSQL 14–18 via compile-time `#if PG_VERSION_NUM` guards in `src/pg_jitter_compat.h`. Key differences handled:

- **PG17+**: Generic ResourceOwner API (`ResourceOwnerDesc`)
- **PG14–16**: JIT-specific ResourceOwner API (`ResourceOwnerEnlargeJIT`/`RememberJIT`/`ForgetJIT`)
- **PG18**: `CompactAttribute`, split `EEOP_DONE`, `CompareType` rename, new opcodes

### Precompiled Function Blobs

Two optional build-time pipelines extract native code for hot functions and embed them directly into the shared library:

- **LLVM pipeline** (`-DPG_JITTER_USE_LLVM=ON`): clang compiles → `extract_inlines.py` extracts native blobs → embeds in header. Supports deep inlining with PG bitcode.
- **c2mir pipeline** (`-DPG_JITTER_USE_C2MIR=ON`): c2mir compiles → MIR_gen emits native code → embeds in header. No LLVM toolchain required.

Without either pipeline, all three backends still work — Tier 1 functions use direct calls and Tier 2 uses C wrappers.

## Testing

```bash
# Correctness: 203 JIT-compiled functions (all types, overflow, NULL propagation, 100K-row validation)
psql -d postgres -f tests/test_precompiled.sql

# Benchmarks: 49 queries across 5 backends
./tests/bench_all_backends.sh

# I-cache impact analysis
./tests/bench_cache_compare.sh

# Memory leak detection (10K queries with RSS trend)
./tests/test_leak_trend.sh [port] [backend]

# Multi-version build + test (PG14–18)
./tests/run_all_versions.sh
```

All 15 combinations (5 PG versions × 3 backends) pass the full PostgreSQL regression test suite.

## Project Structure

```
src/
├── pg_jitter_common.c/h     # Shared: context management, resource owner, fallback dispatch
├── pg_jitter_compat.h        # PG14–18 compatibility layer
├── pg_jitter_sljit.c         # sljit backend
├── pg_jitter_asmjit.cpp      # AsmJIT backend
├── pg_jitter_asmjit_arm64.inc  # AsmJIT arm64 code generation
├── pg_jitter_asmjit_x86.inc   # AsmJIT x86_64 code generation
├── pg_jitter_mir.c            # MIR backend
├── pg_jitter_meta.c           # Meta provider (runtime backend switching)
├── pg_jit_funcs.c/h           # Tier 1 hot-path function table (350+ functions)
├── pg_jit_tier2_wrappers.c/h  # Tier 2 pass-by-ref wrappers
└── pg_jit_deform_templates.c/h  # 68 specialized tuple deform functions (generated)

cmake/
├── CMakeLists.txt             # Per-backend build (used by build.sh)
├── sljit.cmake                # sljit backend configuration
├── asmjit.cmake               # AsmJIT backend configuration
├── mir.cmake                  # MIR backend configuration
└── precompiled.cmake          # Precompilation pipeline (LLVM / c2mir)

tools/
├── gen_deform_templates.py    # Generates specialized deform functions
├── gen_tier2_wrappers.py      # Generates LLVM IR for Tier 2 wrappers
├── extract_inlines.py         # Extracts native blobs from .o files
└── mir_precompile.c           # Standalone c2mir → native code compiler

tests/
├── test_precompiled.sql       # Correctness suite (203 functions)
├── test_leak.sql              # Memory leak detection (single session)
├── test_leak_trend.sh         # RSS trend across 10K queries
├── bench_all_backends.sh      # Full benchmark suite (49 queries)
├── bench_setup.sql            # Benchmark table creation (1M+ rows)
├── bench_setup_extra.sql      # Additional benchmark tables
└── run_all_versions.sh        # Multi-version test runner (PG14–18)
```

## License

Apache License 2.0. See [LICENSE](LICENSE).

## Copyrights

All copyrights belong to their respective owners.
