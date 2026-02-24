/*
 * pg_jit_mir_blobs.h — Shared native blob loading infrastructure
 *
 * Loads pre-compiled native code blobs (produced by c2mir + MIR_gen at build
 * time), copies them to executable memory, patches relocations, and caches
 * the resulting function pointers. Used by all three JIT backends.
 *
 * No MIR runtime dependency — the blobs are already native machine code.
 * Include this header in any backend that needs MIR-precompiled function
 * support. Requires PG_JITTER_HAVE_MIR_PRECOMPILED to be defined.
 */
#ifndef PG_JIT_MIR_BLOBS_H
#define PG_JIT_MIR_BLOBS_H

#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED

#include <string.h>
#include <sys/mman.h>
#include "pg_jit_funcs.h"
#include "pg_jit_precompiled_mir.h"
#include "common/hashfn.h"

#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#endif

#define MIR_BLOB_CACHE_SIZE 256

/* Page size for mmap allocations */
#ifndef MIR_BLOB_PAGE_SIZE
#define MIR_BLOB_PAGE_SIZE 4096
#endif

typedef struct MirBlobCacheEntry
{
	const char *name;		/* jit_* function name */
	void	   *fn_ptr;		/* callable native function pointer */
} MirBlobCacheEntry;

/* Global cache — shared across all expressions in the backend */
static MirBlobCacheEntry mir_blob_cache[MIR_BLOB_CACHE_SIZE];
static int mir_blob_cache_count = 0;
static bool mir_blobs_loaded = false;

/* Executable memory region for all native blobs */
static void *mir_blob_exec_mem = NULL;
static size_t mir_blob_exec_mem_size = 0;

/*
 * Resolve a symbol name to its actual runtime address.
 * These are the external functions referenced by pre-compiled blobs.
 */
static inline void *
mir_resolve_symbol(const char *name)
{
	if (strcmp(name, "jit_error_int4_overflow") == 0)
		return (void *)jit_error_int4_overflow;
	if (strcmp(name, "jit_error_int8_overflow") == 0)
		return (void *)jit_error_int8_overflow;
	if (strcmp(name, "jit_error_int2_overflow") == 0)
		return (void *)jit_error_int2_overflow;
	if (strcmp(name, "jit_error_division_by_zero") == 0)
		return (void *)jit_error_division_by_zero;
	if (strcmp(name, "hash_bytes_uint32") == 0)
		return (void *)hash_bytes_uint32;
	return NULL;
}

/*
 * Patch relocations in a copied native code blob.
 * The code must be in writable memory when this is called.
 */
static inline void
mir_patch_relocs(uint8_t *code, size_t code_len,
				 const PrecompiledInline *pi)
{
	for (int i = 0; i < pi->n_relocs; i++)
	{
		const PrecompiledReloc *rel = &pi->relocs[i];
		void *target = mir_resolve_symbol(rel->symbol);
		if (!target)
			continue;

		uintptr_t target_addr = (uintptr_t)target;

#if defined(__aarch64__) || defined(_M_ARM64)
		if (rel->type == RELOC_MOVZ_MOVK64)
		{
			/*
			 * Patch MOVZ+3×MOVK sequence (4 instructions, 16 bytes).
			 * Each instruction has a 16-bit immediate in bits [20:5].
			 */
			uint32_t *insn = (uint32_t *)(code + rel->offset);

			/* MOVZ: bits [15:0] */
			insn[0] = (insn[0] & ~(0xFFFF << 5))
					| (((uint32_t)(target_addr & 0xFFFF)) << 5);
			/* MOVK: bits [31:16] */
			insn[1] = (insn[1] & ~(0xFFFF << 5))
					| (((uint32_t)((target_addr >> 16) & 0xFFFF)) << 5);
			/* MOVK: bits [47:32] */
			insn[2] = (insn[2] & ~(0xFFFF << 5))
					| (((uint32_t)((target_addr >> 32) & 0xFFFF)) << 5);
			/* MOVK: bits [63:48] */
			insn[3] = (insn[3] & ~(0xFFFF << 5))
					| (((uint32_t)((target_addr >> 48) & 0xFFFF)) << 5);
		}
		else if (rel->type == RELOC_BRANCH26)
		{
			uint32_t *insn = (uint32_t *)(code + rel->offset);
			int64_t pc_rel = ((int64_t)target_addr - (int64_t)(uintptr_t)insn) >> 2;
			*insn = (*insn & ~0x3FFFFFF) | ((uint32_t)pc_rel & 0x3FFFFFF);
		}
#elif defined(__x86_64__) || defined(_M_X64)
		if (rel->type == RELOC_ABS64)
		{
			/* Patch 8-byte absolute address */
			memcpy(code + rel->offset, &target_addr, 8);
		}
		else if (rel->type == RELOC_PC32)
		{
			uint8_t *instr_addr = code + rel->offset;
			int32_t disp = (int32_t)((int64_t)target_addr
									 - (int64_t)(uintptr_t)(instr_addr + 5));
			memcpy(instr_addr + 1, &disp, 4);
		}
#else
		if (rel->type == RELOC_ABS64)
		{
			memcpy(code + rel->offset, &target_addr, 8);
		}
#endif
	}
}

/*
 * Find a cached pre-compiled function pointer by name.
 * Returns NULL if no cached entry exists.
 */
static inline void *
mir_find_precompiled_fn(const char *name)
{
	for (int i = 0; i < mir_blob_cache_count; i++)
	{
		if (strcmp(mir_blob_cache[i].name, name) == 0)
			return mir_blob_cache[i].fn_ptr;
	}
	return NULL;
}

/*
 * Find a pre-compiled native blob by name (for inline emission by sljit).
 * Returns NULL if not found.
 */
static inline const PrecompiledInline *
mir_find_precompiled_blob(const char *name)
{
	for (int i = 0; i < precompiled_mir_inlines_count; i++)
	{
		if (strcmp(precompiled_mir_inlines[i].name, name) == 0)
			return precompiled_mir_inlines[i].blob;
	}
	return NULL;
}

/*
 * Load all pre-compiled native blobs, copy to executable memory,
 * patch relocations, and cache function pointers.
 *
 * The context parameter is ignored (kept for API compatibility).
 */
static inline void
mir_load_precompiled_blobs(void *unused_ctx)
{
	(void)unused_ctx;

	if (mir_blobs_loaded)
		return;
	mir_blobs_loaded = true;

	/*
	 * Calculate total code size needed (with alignment padding).
	 * Each function is aligned to 16 bytes for performance.
	 */
	size_t total_size = 0;
	for (int i = 0; i < precompiled_mir_inlines_count; i++)
	{
		total_size += (precompiled_mir_inlines[i].blob->code_len + 15) & ~15;
	}

	if (total_size == 0)
		return;

	/* Round up to page size */
	size_t alloc_size = (total_size + MIR_BLOB_PAGE_SIZE - 1)
						& ~(MIR_BLOB_PAGE_SIZE - 1);

	/* Allocate RW memory (we'll make it RX after patching) */
#if defined(__APPLE__) && defined(__aarch64__)
	/*
	 * macOS ARM64 with MAP_JIT: memory starts as writable per-thread.
	 * After patching, we toggle to executable.
	 */
	mir_blob_exec_mem = mmap(NULL, alloc_size,
							 PROT_READ | PROT_WRITE | PROT_EXEC,
							 MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
	if (mir_blob_exec_mem == MAP_FAILED)
	{
		mir_blob_exec_mem = NULL;
		elog(WARNING, "MIR precompiled: mmap(MAP_JIT) failed");
		return;
	}
	pthread_jit_write_protect_np(0);  /* writable mode */
#else
	/* Linux/others: mmap RWX */
	mir_blob_exec_mem = mmap(NULL, alloc_size,
							 PROT_READ | PROT_WRITE | PROT_EXEC,
							 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mir_blob_exec_mem == MAP_FAILED)
	{
		mir_blob_exec_mem = NULL;
		elog(WARNING, "MIR precompiled: mmap failed");
		return;
	}
#endif
	mir_blob_exec_mem_size = alloc_size;

	/* Copy each blob, patch relocations, cache function pointer */
	uint8_t *dest = (uint8_t *)mir_blob_exec_mem;
	for (int i = 0; i < precompiled_mir_inlines_count; i++)
	{
		const PrecompiledMirEntry *entry = &precompiled_mir_inlines[i];
		const PrecompiledInline *pi = entry->blob;

		if (mir_blob_cache_count >= MIR_BLOB_CACHE_SIZE)
			break;

		/* Copy native code bytes */
		memcpy(dest, pi->code, pi->code_len);

		/* Patch relocations in place */
		mir_patch_relocs(dest, pi->code_len, pi);

		/* Cache the function pointer */
		mir_blob_cache[mir_blob_cache_count].name = entry->name;
		mir_blob_cache[mir_blob_cache_count].fn_ptr = (void *)dest;
		mir_blob_cache_count++;

		/* Advance to next aligned position */
		dest += (pi->code_len + 15) & ~15;
	}

#if defined(__APPLE__) && defined(__aarch64__)
	/* Switch to executable mode and flush I-cache */
	pthread_jit_write_protect_np(1);
	sys_icache_invalidate(mir_blob_exec_mem, alloc_size);
#elif defined(__aarch64__)
	/* Non-Apple ARM64: make executable and flush I-cache */
	mprotect(mir_blob_exec_mem, alloc_size, PROT_READ | PROT_EXEC);
	__builtin___clear_cache((char *)mir_blob_exec_mem,
							(char *)mir_blob_exec_mem + alloc_size);
#else
	/* x86_64 and others: already RWX from mmap, no I-cache flush needed */
#endif

	elog(DEBUG1, "MIR precompiled: loaded %d native functions (%zu bytes)",
		 mir_blob_cache_count, total_size);
}

/*
 * Free the executable memory region for pre-compiled blobs.
 */
static inline void
mir_destroy_blob_ctx(void)
{
	if (mir_blob_exec_mem)
	{
		munmap(mir_blob_exec_mem, mir_blob_exec_mem_size);
		mir_blob_exec_mem = NULL;
		mir_blob_exec_mem_size = 0;
	}
	mir_blobs_loaded = false;
	mir_blob_cache_count = 0;
}

#endif /* PG_JITTER_HAVE_MIR_PRECOMPILED */

#endif /* PG_JIT_MIR_BLOBS_H */
