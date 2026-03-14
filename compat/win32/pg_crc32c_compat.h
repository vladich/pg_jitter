/*
 * pg_crc32c_compat.h — Windows x86_64 CRC32C using SSE4.2 intrinsics
 *
 * PostgreSQL's USE_SSE42_CRC32C_WITH_RUNTIME_CHECK path uses a function
 * pointer (pg_comp_crc32c) that is not always exported from postgres.lib
 * on Windows binary installs.  We override COMP_CRC32C to use SSE4.2
 * intrinsics directly, which are always available on x86_64.
 *
 * Include this AFTER "port/pg_crc32c.h".
 */
#ifndef PG_CRC32C_COMPAT_H
#define PG_CRC32C_COMPAT_H

#if defined(_WIN64) && (defined(_M_X64) || defined(_M_AMD64))

#include <nmmintrin.h>

#undef COMP_CRC32C
#define COMP_CRC32C(crc, data, len) \
	((crc) = pg_jitter_crc32c_sse42((crc), (data), (len)))

static inline pg_crc32c
pg_jitter_crc32c_sse42(pg_crc32c crc, const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *) data;

	for (; len >= 8; p += 8, len -= 8)
		crc = (pg_crc32c) _mm_crc32_u64(crc, *(const unsigned __int64 *) p);
	for (; len >= 4; p += 4, len -= 4)
		crc = _mm_crc32_u32(crc, *(const unsigned int *) p);
	for (; len > 0; --len)
		crc = _mm_crc32_u8(crc, *p++);
	return crc;
}

#endif /* _WIN64 && x64 */

#endif /* PG_CRC32C_COMPAT_H */
