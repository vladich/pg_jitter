<p align="center">
  <img width="800px" height="200px" src="img/pg_jitter6.jpeg">
</p>

A lightweight JIT compilation provider for PostgreSQL that adds three alternative JIT backends - **[sljit](https://github.com/zherczeg/sljit)**, **[AsmJit](https://github.com/asmjit/asmjit)** and **[MIR](https://github.com/vnmakarov/mir)** - delivering faster compilation and competitive query execution across PostgreSQL 14–18.

## Why?

JIT compilation was introduced in Postgres 11 in 2018. It solves a problem of Postgres having to interpret expressions and use inefficient per-row loops in run-time in order to do internal data conversions (so-called tuple deforming).
On expression-heavy workloads or just wide tables, it can give a significant performance boost for those operations. However, standard LLVM-based JIT is notoriously slow at compilation.
When it takes tens to hundreds of milliseconds, it may be suitable only for very heavy, OLAP-style queries, in some cases.
For typical OLTP queries, LLVM's JIT overhead can easily exceed the execution time of the query itself.
pg_jitter provides native code generation with microsecond-level compilation times instead of milliseconds, making JIT worthwhile for a much wider range of queries.

## Performance

Typical compilation time:
- **sljit**: tens to low hundreds of microseconds
- **AsmJIT**: hundreds of microseconds
- **MIR**: hundreds of microseconds to single milliseconds
- **LLVM (Postgres default)**: tens to hundreds of milliseconds (seconds to tens(!) of seconds in worst cases)

In reality, the effect of JIT compilation is broader - execution can slow down for up to ~1ms even for sljit, because of other related things, mostly cold processor cache and effects of increased memory pressure (rapid allocations / deallocations related to code generation and JIT compilation). Therefore, on systems executing a lot of queries per second, it's recommended to avoid JIT compilation for very fast queries such as point lookups or queries processing only a few records. By default, `jit_above_cost` parameter is set to a very high number (100'000). This makes sense for LLVM, but doesn't make sense for faster providers.
It's recommended to set this parameter value to something from ~200 to low thousands for pg_jitter (depending on what specific backend you use and your specific workloads).

- **sljit** is the most consistent: 5–25% faster than the interpreter across all workloads. This, and also its phenomenal compilation speed, make it the best choice for most scenarios.
- **AsmJIT** excels on wide-row/deform-heavy queries (up to 32% faster) thanks to specialized tuple deforming
- **MIR** provides solid gains while being the most portable backend
- **LLVM** was supposed to be fast at execution time, due to clang optimization advantages, but in fact, in most cases, it's slower than all 3 pg_jitter backends, even not counting compilation performance differences. This is due to zero-cost inlining using compile-time pre-extracted code and manual instruction-level optimization.

## Benchmarks

There are several scripts in the `tests` folder to run different types of benchmarks, one of them is [tests/bench_comprehensive.sh](tests/bench_comprehensive.sh), another [tests/gen_cross_version_benchmarks.py](tests/gen_cross_version_benchmarks.py).
Here are some results for ARM64 (Apple Silicon M1 Pro) and x86_64 (Ryzen AI 9 HX PRO 370) for different versions of Postgres and different backends.
Some of them are pretty interesting, for example the "super wide table" section for both ARM and x86, where LLVM's performance is simply atrocious (10x-30x of the baseline).

[ARM64](bench/ARM64) -> [PG14](bench/ARM64/BENCHMARK_PG14.md) | [PG15](bench/ARM64/BENCHMARK_PG15.md) | [PG16](bench/ARM64/BENCHMARK_PG16.md) | [PG17](bench/ARM64/BENCHMARK_PG17.md) | [PG18](bench/ARM64/BENCHMARK_PG18.md) * [sljit](bench/ARM64/BENCHMARK_sljit.md) * [AsmJit](bench/ARM64/BENCHMARK_asmjit.md) * [MIR](bench/ARM64/BENCHMARK_mir.md)

[x86_64](bench/x86_64) -> [PG14](bench/x86_64/BENCHMARK_PG14.md) | [PG15](bench/x86_64/BENCHMARK_PG15.md) | [PG16](bench/x86_64/BENCHMARK_PG16.md) | [PG17](bench/x86_64/BENCHMARK_PG17.md) | [PG18](bench/x86_64/BENCHMARK_PG18.md) * [sljit](bench/x86_64/BENCHMARK_sljit.md) * [AsmJit](bench/x86_64/BENCHMARK_asmjit.md) * [MIR](bench/x86_64/BENCHMARK_mir.md)

## Features

- **Simple-configuration** - set `jit_provider` and go
- **Three independent backends** with different strengths
- **Runtime backend switching** via `SET pg_jitter.backend = 'sljit'` (no restart)
- **PostgreSQL 14–18** support from one codebase
- **Two-tier function optimization** - hot-path PG functions compiled as direct native calls
- **No LLVM dependency** - pure C/C++ with small, embeddable libraries
- **Precompiled function blobs** - optional build-time native code extraction for zero-cost inlining
- **Supported platforms** - aside from AsmJit, other providers (in theory) can be used on most platforms supported by Postgres. But **pg_jitter** was only tested on Linux/MacOS (ARM64) and Linux/Windows (x86_64) so far. Testing it on other platforms is planned, but if you had success (or issues) running it, please let me know at vladimir@churyukin.com. 

## Stability

The current source code can be considered beta-quality. It passes all standard Postgres regression tests and shows good improvements in performance tests. But it lacks large-scale production verification (yet).
Stay tuned.

## Quick Start

### Prerequisites

- PostgreSQL 14–18 (with development headers)
- CMake >= 3.16
- C11 and C++17 compilers
- Backend libraries as sibling directories:

```
parent/
├── pg_jitter/
├── sljit/        
├── asmjit/       
└── mir/          
```
[SLJIT](https://github.com/zherczeg/sljit) | [AsmJit](https://github.com/asmjit/asmjit) | [MIR](https://github.com/vnmakarov/mir)

For MIR, use the patched version from [MIR-patched](https://github.com/vladich/mir-patched) - it has a few changes about tracking the size of the generated native code per function, and per-function memory management.

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

## Configuration

All parameters are user-settable (`SET` in session, `ALTER SYSTEM` for persistent) and take effect without a restart unless noted.

### Backend Selection

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `pg_jitter.backend` | enum | `auto` (if 2+ backends installed, else the single available one) | Active JIT backend: `sljit`, `asmjit`, `mir`, or `auto`. The `auto` mode is **experimental** — it uses adaptive statistics to select the fastest backend per expression profile. |

### Parallel Execution

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `pg_jitter.parallel_mode` | enum | `per_worker` | Controls JIT in parallel workers. `off` — workers use the PG interpreter. `per_worker` — each worker JIT-compiles independently. `shared` — leader compiles once, workers reuse code via DSM (saves compilation time, slightly higher per-row overhead). The `shared` mode is **experimental** |
| `pg_jitter.shared_code_max` | integer (KB) | `4096` (4 MB) | Maximum DSM segment size for shared JIT code. Range: 64 KB – 1 GB. |

### Expression Tuning

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `pg_jitter.min_expr_steps` | integer | `4` | Minimum expression step count for JIT compilation. Expressions with fewer steps skip JIT and use the interpreter. Range: 0–1000. |
| `pg_jitter.deform_cache` | boolean | `on` | Cache compiled deform functions across queries within a backend process. When off, deform is recompiled each query. |
| `pg_jitter.in_bsearch_max` | integer | `4096` | Maximum IN-list elements for inline binary search tree. Larger integer IN-lists use CRC32 hash probe. 0 disables inline binary search. Range: 0–8192. |
| `pg_jitter.in_hash` | enum | `crc32` | Hash table strategy for large integer IN-lists: `pg` (PG's built-in Jenkins hash), `crc32` (hardware CRC32C open-addressing). |

### Adaptive Backend Selection (experimental)

These parameters only take effect when `pg_jitter.backend = 'auto'`. Statistics are not collected when a specific backend is selected.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `pg_jitter.adaptive` | boolean | `on` | Enable adaptive backend selection based on runtime performance statistics. |
| `pg_jitter.adaptive_samples` | integer | `64` | Number of expression evaluations to time before considering a backend profiled. Range: 4–10000. |
| `pg_jitter.adaptive_epsilon` | real | `0.05` | Exploration probability (epsilon-greedy). 0.0 = always pick the best measured backend, 1.0 = always pick randomly. Range: 0.0–1.0. |

### PostgreSQL JIT Parameters

These are standard PostgreSQL parameters that control when JIT compilation is triggered:

| Parameter | Default | Recommended for pg_jitter |
|-----------|---------|---------------------------|
| `jit_above_cost` | `100000` | `200`–`2000` (pg_jitter compiles in microseconds, not milliseconds) |
| `jit_inline_above_cost` | `500000` | `500000` (keep default) |
| `jit_optimize_above_cost` | `500000` | `500000` (keep default) |

## Architecture

### Expression Compilation

pg_jitter implements PostgreSQL's `JitProviderCallbacks` interface. When PostgreSQL decides to JIT-compile a query, it calls `compile_expr()` which:

1. Walks the `ExprState->steps[]` array (PostgreSQL's expression evaluation opcodes)
2. Emits native machine code for ~30 hot-path opcodes (arithmetic, comparisons, variable access, tuple deforming, aggregation, boolean logic, jumps)
3. Delegates remaining opcodes to `pg_jitter_fallback_step()` which calls the corresponding `ExecEval*` C functions
4. Installs the compiled function with a one-time validation wrapper that catches `ALTER COLUMN TYPE` invalidation

### Two-Tier Function Optimization

- **Tier 1**: Pass-by-value operations (int, float, bool, date, timestamp, OID) compiled as direct native calls with inline overflow checking. No `FunctionCallInfo` overhead.
- **Tier 2**: Pass-by-reference operations (numeric, text, interval, uuid) called through `DirectFunctionCall` C wrappers. Optionally LLVM-optimized when built with `-DPG_JITTER_USE_LLVM=ON` or c2mir-optimized when built with `-DPG_JITTER_USE_C2MIR=ON`.

### Three JIT Backends

| | sljit | AsmJIT | MIR |
|---|---|---|---|
| **Language** | C | C++ | C |
| **IR level** | Low-level (register machine) | Low-level (native assembler) | Medium-level (typed ops) |
| **Register allocation** | Manual | Virtual (automatic) | Automatic |
| **Architectures** | arm64, x86_64, s390x, ppc, mips, riscv | arm64, x86_64 | arm64, x86_64, s390x, ppc, mips, riscv |
| **Compilation speed** | Fastest (10s to low 100s of μs) | Fast (x3-x5) of sljit | Still fast (x15-x20 of sljit) |
| **Best for** | General workloads, lowest overhead | Wide rows, deform-heavy queries | Portability and edge cases |
| **Library size** | ~200 KB | ~900 KB | ~900 KB |

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

# Benchmarks
./tests/bench_all_backends.sh
./tests/gen_cross_version_benchmarks.py

# I-cache impact analysis
./tests/bench_cache_compare.sh

# Memory leak detection (10K queries with RSS trend)
./tests/test_leak_trend.sh [port] [backend]

# Multi-version build + test (PG14–18)
./tests/run_all_versions.sh
```

## License

Apache License 2.0. See [LICENSE](LICENSE).

## Copyrights

All copyrights belong to their respective owners.

## Main builds
Full Postgres regression tests (pg_regress) run on each commit to the master branch for each supported combination of main platforms and versions of Postgres (20 combinations total).
For each branch commit, they run for a single version only (PG17). Here is the state of the last master run.

| PG | Linux x86_64 | Linux ARM64 | macOS ARM64 | Windows x86_64 |
|--------|:---:|:---:|:---:|:---:|
| 14 | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg14-linux.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg14-linux-arm.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg14-macos.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg14-windows.yml/badge.svg?branch=master&event=push) |
| 15 | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg15-linux.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg15-linux-arm.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg15-macos.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg15-windows.yml/badge.svg?branch=master&event=push) |
| 16 | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg16-linux.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg16-linux-arm.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg16-macos.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg16-windows.yml/badge.svg?branch=master&event=push) |
| 17 | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg17-linux.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg17-linux-arm.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg17-macos.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg17-windows.yml/badge.svg?branch=master&event=push) |
| 18 | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg18-linux.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg18-linux-arm.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg18-macos.yml/badge.svg?branch=master&event=push) | ![](https://github.com/vladich/pg_jitter/actions/workflows/pg18-windows.yml/badge.svg?branch=master&event=push) |

## PPC64 / s390x / FreeBSD builds
Regression tests for exotic platforms run on each commit to the master branch on Travis CI. Here is the status:

![](https://app.travis-ci.com/vladich/pg_jitter.svg?token=ChVbp9UxCxARfx1f2YNh&branch=master)

Linux/PPC64 and FreeBSD/x86_64 builds run for each supported Postgres version, Linux/s390x builds run for PG14 only
