/*
 * StringZilla compile-time configuration for pg_jitter.
 *
 * AVX2 and NEON are acceptable compile-time targets for supported builds.
 * Wider optional backends remain disabled unless this integration grows a
 * runtime-dispatched StringZilla build path.
 */
#ifndef PG_JITTER_STRINGZILLA_CONFIG_H
#define PG_JITTER_STRINGZILLA_CONFIG_H

#undef SZ_DYNAMIC_DISPATCH
#define SZ_DYNAMIC_DISPATCH 0

#if defined(__x86_64__) || defined(_M_X64)
#undef SZ_USE_SKYLAKE
#define SZ_USE_SKYLAKE 0
#undef SZ_USE_ICE
#define SZ_USE_ICE 0
#undef SZ_USE_GOLDMONT
#define SZ_USE_GOLDMONT 0
#elif defined(__aarch64__) || defined(_M_ARM64)
#undef SZ_USE_SVE
#define SZ_USE_SVE 0
#undef SZ_USE_SVE2
#define SZ_USE_SVE2 0
#undef SZ_USE_SVE2_AES
#define SZ_USE_SVE2_AES 0
#undef SZ_USE_NEON_AES
#define SZ_USE_NEON_AES 0
#undef SZ_USE_NEON_SHA
#define SZ_USE_NEON_SHA 0
#endif

#undef SZ_USE_CUDA
#define SZ_USE_CUDA 0
#undef SZ_USE_KEPLER
#define SZ_USE_KEPLER 0
#undef SZ_USE_HOPPER
#define SZ_USE_HOPPER 0

#endif /* PG_JITTER_STRINGZILLA_CONFIG_H */
