/*
 * pg_jitter_functions.sql — SQL function definitions for pg_jitter
 *
 * Run this after installing the pg_jitter shared libraries:
 *   psql -f pg_jitter_functions.sql
 *
 * These functions are provided by the pg_jitter meta provider (pg_jitter.so/.dylib).
 * The JIT provider must be loaded first:
 *   ALTER SYSTEM SET jit_provider = 'pg_jitter';
 */

-- Returns the currently active backend name (sljit, asmjit, mir, or auto).
CREATE FUNCTION pg_jitter_current_backend() RETURNS text
  LANGUAGE c STRICT AS 'pg_jitter', 'pg_jitter_current_backend';

-- Returns per-expression adaptive statistics collected during auto mode.
-- Each row represents one expression profile with timing data per backend.
CREATE FUNCTION pg_jitter_adaptive_stats(
  OUT profile_hash text,
  OUT nsteps int,
  OUT fetchsome_natts int,
  OUT n_hashdatum int,
  OUT n_funcexpr int,
  OUT n_qual int,
  OUT n_agg int,
  OUT sljit_calls int,
  OUT sljit_compile_ns float8,
  OUT sljit_avg_ns float8,
  OUT asmjit_calls int,
  OUT asmjit_compile_ns float8,
  OUT asmjit_avg_ns float8,
  OUT mir_calls int,
  OUT mir_compile_ns float8,
  OUT mir_avg_ns float8,
  OUT selected text,
  OUT margin_pct float8
) RETURNS SETOF record
  LANGUAGE c STRICT AS 'pg_jitter', 'pg_jitter_adaptive_stats';

-- Resets all adaptive statistics. Useful for benchmarking or after
-- workload changes.
CREATE FUNCTION pg_jitter_adaptive_stats_reset() RETURNS void
  LANGUAGE c STRICT AS 'pg_jitter', 'pg_jitter_adaptive_stats_reset';
