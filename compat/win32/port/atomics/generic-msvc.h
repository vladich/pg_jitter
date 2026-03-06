/*
 * generic-msvc.h - C++-compatible shim for PG's MSVC atomics
 *
 * PG's port/atomics/generic-msvc.h passes volatile uint64* to
 * _InterlockedCompareExchange64 etc. which expect volatile LONG64*.
 * In C these pointer types are implicitly compatible; in C++ MSVC
 * treats them as distinct, causing error C2664.
 *
 * This file shadows PG's version (via compat/win32 BEFORE include
 * priority in CMakeLists.txt) and adds the necessary casts.
 *
 * Compatible with PostgreSQL 14-18.
 */
#include <intrin.h>

/* intentionally no include guards, should only be included by atomics.h */
#ifndef INSIDE_ATOMICS_H
#error "should be included via atomics.h"
#endif

#pragma intrinsic(_ReadWriteBarrier)
#define pg_compiler_barrier_impl()	_ReadWriteBarrier()

#ifndef pg_memory_barrier_impl
#define pg_memory_barrier_impl()	MemoryBarrier()
#endif

/*
 * PG 14-17 gate on HAVE_ATOMICS; PG 18 removed it.
 * Always define -- MSVC on Windows always has atomics.
 */

#define PG_HAVE_ATOMIC_U32_SUPPORT
typedef struct pg_atomic_uint32
{
	volatile uint32 value;
} pg_atomic_uint32;

#define PG_HAVE_ATOMIC_U64_SUPPORT
/* pg_attribute_aligned was added in PG 17; use __declspec directly for MSVC */
typedef struct __declspec(align(8)) pg_atomic_uint64
{
	volatile uint64 value;
} pg_atomic_uint64;


#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U32
static inline bool
pg_atomic_compare_exchange_u32_impl(volatile pg_atomic_uint32 *ptr,
									uint32 *expected, uint32 newval)
{
	bool	ret;
	uint32	current;
	current = InterlockedCompareExchange(&ptr->value, newval, *expected);
	ret = current == *expected;
	*expected = current;
	return ret;
}

#define PG_HAVE_ATOMIC_EXCHANGE_U32
static inline uint32
pg_atomic_exchange_u32_impl(volatile pg_atomic_uint32 *ptr, uint32 newval)
{
	return InterlockedExchange(&ptr->value, newval);
}

#define PG_HAVE_ATOMIC_FETCH_ADD_U32
static inline uint32
pg_atomic_fetch_add_u32_impl(volatile pg_atomic_uint32 *ptr, int32 add_)
{
	return InterlockedExchangeAdd(&ptr->value, add_);
}

#pragma intrinsic(_InterlockedCompareExchange64)

#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U64
static inline bool
pg_atomic_compare_exchange_u64_impl(volatile pg_atomic_uint64 *ptr,
									uint64 *expected, uint64 newval)
{
	bool	ret;
	uint64	current;
	/* [CAST] volatile uint64* -> volatile long long* for C++ compat */
	current = _InterlockedCompareExchange64((volatile long long *)&ptr->value, newval, *expected);
	ret = current == *expected;
	*expected = current;
	return ret;
}

/* Only implemented on 64bit builds */
#ifdef _WIN64

#pragma intrinsic(_InterlockedExchange64)

#define PG_HAVE_ATOMIC_EXCHANGE_U64
static inline uint64
pg_atomic_exchange_u64_impl(volatile pg_atomic_uint64 *ptr, uint64 newval)
{
	/* [CAST] volatile uint64* -> volatile long long* for C++ compat */
	return _InterlockedExchange64((volatile long long *)&ptr->value, newval);
}

#pragma intrinsic(_InterlockedExchangeAdd64)

#define PG_HAVE_ATOMIC_FETCH_ADD_U64
static inline uint64
pg_atomic_fetch_add_u64_impl(volatile pg_atomic_uint64 *ptr, int64 add_)
{
	/* [CAST] volatile uint64* -> volatile long long* for C++ compat */
	return _InterlockedExchangeAdd64((volatile long long *)&ptr->value, add_);
}

#endif /* _WIN64 */
