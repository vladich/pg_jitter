/*
 * mir_precompile.c — Build-time tool to pre-compile jit functions to native blobs
 *
 * Uses c2mir to compile C source → MIR IR, then MIR_gen() to produce native
 * machine code. Extracts the native bytes and scans for references to external
 * symbols (recorded as relocations). Outputs a C header matching the format
 * used by the LLVM pipeline (PrecompiledInline structs).
 *
 * Architecture-specific: the output contains native code for the build host's
 * architecture. Cross-compilation is not supported; build on each target.
 *
 * Usage:
 *   mir_precompile -I/path/to/pg/include/server -I/path/to/src \
 *                  -o pg_jit_precompiled_mir.h \
 *                  mir_jit_funcs.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "mir.h"
#include "mir-gen.h"
#include "c2mir/c2mir.h"

/* Maximum number of functions we expect to extract */
#define MAX_FUNCTIONS 512

/* Maximum native code size per function (bytes) */
#define MAX_CODE_SIZE (4 * 1024)

/* Maximum relocations per function */
#define MAX_RELOCS 8

/*
 * External symbols that jit functions reference.
 * We assign each a unique sentinel address so we can find references
 * in the generated native code after MIR_gen().
 */
typedef struct {
	const char *name;
	uintptr_t   sentinel;    /* unique dummy address for detection */
} ExternalSymbol;

/*
 * Sentinel addresses: chosen to be distinctive and unlikely to collide.
 * On ARM64, MIR loads 64-bit immediates via MOVZ+3×MOVK, embedding the
 * address across 4 instructions. We scan for the sentinel bit pattern.
 * On x86_64, MIR puts the address in a const_pool accessed via RIP-relative;
 * the 8-byte sentinel appears as data after the code.
 *
 * Sentinels use high bits (0xDEAD0001...) so they're distinguishable from
 * normal small integers that might appear in the code.
 */
/*
 * Every 16-bit chunk must be non-zero to force MIR to emit a full
 * MOVZ+3×MOVK sequence on ARM64 (otherwise it optimizes away zero chunks).
 */
static ExternalSymbol externals[] = {
	{ "jit_error_int4_overflow",    0xDEAD0001BEEF0001ULL },
	{ "jit_error_int8_overflow",    0xDEAD0002BEEF0002ULL },
	{ "jit_error_int2_overflow",    0xDEAD0003BEEF0003ULL },
	{ "jit_error_division_by_zero", 0xDEAD0004BEEF0004ULL },
	{ "hash_bytes_uint32",          0xDEAD0005BEEF0005ULL },
};
#define N_EXTERNALS (sizeof(externals) / sizeof(externals[0]))

/* Dummy implementations that the sentinel addresses point to.
 * These never actually run — they're just targets for MIR_load_external(). */
static void dummy_void_fn(void) { }
static uint32_t dummy_hash_fn(uint32_t k) { (void)k; return 0; }

/* Relocation types — must match extract_inlines.py / pg_jit_precompiled.h */
#define RELOC_UNKNOWN    0
#define RELOC_BRANCH26   1  /* ARM64: BL/B ±128MB */
#define RELOC_PAGE21     2  /* ARM64: ADRP */
#define RELOC_PAGEOFF12  3  /* ARM64: ADD/LDR page offset */
#define RELOC_PC32       4  /* x86_64: RIP-relative 32-bit */
#define RELOC_GOTPCREL   5  /* x86_64: GOT-relative */
#define RELOC_GOT        6  /* ARM64: GOT */
/* New MIR-specific relocation types */
#define RELOC_MOVZ_MOVK64  7  /* ARM64: MOVZ+3×MOVK sequence (16 bytes) */
#define RELOC_ABS64        8  /* x86_64: 8-byte absolute address in code/pool */

typedef struct {
	uint16_t    offset;     /* offset within code */
	uint8_t     type;       /* RELOC_* constant */
	const char *symbol;     /* symbol name for runtime resolution */
} Reloc;

/* Collected function data */
typedef struct {
	char     name[128];
	uint8_t  code[MAX_CODE_SIZE];
	size_t   code_len;
	int      ret_offset;  /* offset of RET instruction, or -1 */
	int      n_relocs;
	Reloc    relocs[MAX_RELOCS];
} FuncBlob;

static FuncBlob func_blobs[MAX_FUNCTIONS];
static int n_func_blobs = 0;

/* Source file reader callback */
typedef struct {
	FILE *fp;
} ReaderData;

static int source_reader(void *data)
{
	ReaderData *rd = (ReaderData *) data;
	return fgetc(rd->fp);
}

/* Check if a function name starts with "jit_" */
static bool is_jit_function(const char *name)
{
	return strncmp(name, "jit_", 4) == 0;
}

/*
 * Scan native code for references to sentinel addresses and record
 * as relocations. Architecture-specific.
 */
#if defined(__aarch64__) || defined(_M_ARM64)

/*
 * ARM64: MIR loads 64-bit immediates via MOVZ+MOVK+MOVK+MOVK sequence.
 * Each instruction encodes 16 bits of the value:
 *   MOVZ Xn, #imm16, LSL #0   → bits [15:0]
 *   MOVK Xn, #imm16, LSL #16  → bits [31:16]
 *   MOVK Xn, #imm16, LSL #32  → bits [47:32]
 *   MOVK Xn, #imm16, LSL #48  → bits [63:48]
 *
 * Followed by BLR Xn (0xD63F0000 | (Rn << 5))
 *
 * MOVZ encoding: 1 10 100101 hw imm16 Rd
 *   hw=00 → LSL #0,  hw=01 → LSL #16, hw=10 → LSL #32, hw=11 → LSL #48
 * MOVK encoding: 1 11 100101 hw imm16 Rd
 *
 * We scan for 4 consecutive instructions targeting the same register
 * that reconstruct a sentinel address.
 */
static void
scan_relocations(FuncBlob *fb)
{
	uint32_t *instrs = (uint32_t *)fb->code;
	int n_instrs = (int)(fb->code_len / 4);

	for (int i = 0; i <= n_instrs - 4; i++)
	{
		uint32_t w0 = instrs[i];
		uint32_t w1 = instrs[i+1];
		uint32_t w2 = instrs[i+2];
		uint32_t w3 = instrs[i+3];

		/* Check for MOVZ Xd, #imm16, LSL #0 (sf=1, opc=10, hw=00) */
		if ((w0 & 0xFFE00000) != 0xD2800000)
			continue;

		uint8_t rd = w0 & 0x1F;

		/* Check MOVK same Rd, hw=01 */
		if ((w1 & 0xFFE0001F) != (0xF2A00000 | rd))
			continue;
		/* Check MOVK same Rd, hw=10 */
		if ((w2 & 0xFFE0001F) != (0xF2C00000 | rd))
			continue;
		/* Check MOVK same Rd, hw=11 */
		if ((w3 & 0xFFE0001F) != (0xF2E00000 | rd))
			continue;

		/* Reconstruct the 64-bit value */
		uint64_t val = 0;
		val |= (uint64_t)((w0 >> 5) & 0xFFFF);         /* bits [15:0] */
		val |= (uint64_t)((w1 >> 5) & 0xFFFF) << 16;   /* bits [31:16] */
		val |= (uint64_t)((w2 >> 5) & 0xFFFF) << 32;   /* bits [47:32] */
		val |= (uint64_t)((w3 >> 5) & 0xFFFF) << 48;   /* bits [63:48] */

		/* Check if this matches any sentinel */
		for (int e = 0; e < (int)N_EXTERNALS; e++)
		{
			if (val == externals[e].sentinel && fb->n_relocs < MAX_RELOCS)
			{
				fb->relocs[fb->n_relocs].offset = (uint16_t)(i * 4);
				fb->relocs[fb->n_relocs].type = RELOC_MOVZ_MOVK64;
				fb->relocs[fb->n_relocs].symbol = externals[e].name;
				fb->n_relocs++;
				break;
			}
		}
	}

	/* Find RET instruction (0xD65F03C0) */
	fb->ret_offset = -1;
	for (int i = 0; i < n_instrs; i++)
	{
		if (instrs[i] == 0xD65F03C0)
		{
			fb->ret_offset = i * 4;
			break;
		}
	}
}

#elif defined(__x86_64__) || defined(_M_X64)

/*
 * x86_64: MIR puts 64-bit external addresses into a const_pool, accessed
 * via RIP-relative addressing: call *[rip+disp32]
 *
 * The const_pool data is appended AFTER the code (8-byte aligned).
 * We scan the entire code+pool region for 8-byte sentinel values.
 *
 * Additionally, we look for the RET instruction (0xC3) for ret_offset.
 */
static void
scan_relocations(FuncBlob *fb)
{
	/* Scan for 8-byte sentinel values anywhere in the code region.
	 * On x86_64, MIR embeds 64-bit addresses in the const_pool which
	 * is placed right after the code. The code references pool entries
	 * via RIP-relative FF 15 xx xx xx xx (call *[rip+disp32]).
	 */
	for (size_t off = 0; off + 8 <= fb->code_len; off++)
	{
		uint64_t val;
		memcpy(&val, fb->code + off, 8);
		for (int e = 0; e < (int)N_EXTERNALS; e++)
		{
			if (val == externals[e].sentinel && fb->n_relocs < MAX_RELOCS)
			{
				fb->relocs[fb->n_relocs].offset = (uint16_t)off;
				fb->relocs[fb->n_relocs].type = RELOC_ABS64;
				fb->relocs[fb->n_relocs].symbol = externals[e].name;
				fb->n_relocs++;
				break;
			}
		}
	}

	/* Find first RET instruction (0xC3) */
	fb->ret_offset = -1;
	for (size_t i = 0; i < fb->code_len; i++)
	{
		if (fb->code[i] == 0xC3)
		{
			fb->ret_offset = (int)i;
			break;
		}
	}
}

#else

/* Other architectures: scan for 8-byte sentinels (generic) */
static void
scan_relocations(FuncBlob *fb)
{
	for (size_t off = 0; off + 8 <= fb->code_len; off++)
	{
		uint64_t val;
		memcpy(&val, fb->code + off, 8);
		for (int e = 0; e < (int)N_EXTERNALS; e++)
		{
			if (val == externals[e].sentinel && fb->n_relocs < MAX_RELOCS)
			{
				fb->relocs[fb->n_relocs].offset = (uint16_t)off;
				fb->relocs[fb->n_relocs].type = RELOC_ABS64;
				fb->relocs[fb->n_relocs].symbol = externals[e].name;
				fb->n_relocs++;
				break;
			}
		}
	}
	fb->ret_offset = -1;
}

#endif

/* Detect the current architecture name for the header comment */
static const char *
get_arch_name(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
	return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
	return "x86_64";
#elif defined(__powerpc64__)
	return "ppc64";
#elif defined(__riscv) && __riscv_xlen == 64
	return "riscv64";
#elif defined(__s390x__)
	return "s390x";
#elif defined(__sparc__)
	return "sparc64";
#elif defined(__loongarch64)
	return "loongarch64";
#else
	return "unknown";
#endif
}

/* Emit the header file with all collected native blobs */
static void emit_header(FILE *out)
{
	const char *arch = get_arch_name();

	fprintf(out, "/*\n");
	fprintf(out, " * pg_jit_precompiled_mir.h — Auto-generated native code blobs (via c2mir + MIR_gen)\n");
	fprintf(out, " *\n");
	fprintf(out, " * Generated by tools/mir_precompile from mir_jit_funcs.c\n");
	fprintf(out, " * DO NOT EDIT — regenerate with: cmake --build . --target mir_precompiled_header\n");
	fprintf(out, " *\n");
	fprintf(out, " * Architecture: %s\n", arch);
	fprintf(out, " * Contains %d pre-compiled native functions.\n", n_func_blobs);
	fprintf(out, " * At runtime: copy bytes into JIT buffer + fixup relocations.\n");
	fprintf(out, " */\n\n");
	fprintf(out, "#ifndef PG_JIT_PRECOMPILED_MIR_H\n");
	fprintf(out, "#define PG_JIT_PRECOMPILED_MIR_H\n\n");
	fprintf(out, "#include <stdint.h>\n\n");

	/* Relocation type defines */
	fprintf(out, "/* Relocation types */\n");
	fprintf(out, "#ifndef RELOC_BRANCH26\n");
	fprintf(out, "#define RELOC_BRANCH26   1  /* ARM64: BL/B ±128MB */\n");
	fprintf(out, "#define RELOC_PAGE21     2  /* ARM64: ADRP */\n");
	fprintf(out, "#define RELOC_PAGEOFF12  3  /* ARM64: ADD/LDR page offset */\n");
	fprintf(out, "#define RELOC_PC32       4  /* x86_64: RIP-relative 32-bit */\n");
	fprintf(out, "#define RELOC_GOTPCREL   5  /* x86_64: GOT-relative */\n");
	fprintf(out, "#define RELOC_GOT        6  /* ARM64: GOT */\n");
	fprintf(out, "#define RELOC_MOVZ_MOVK64 7 /* ARM64: MOVZ+3×MOVK 64-bit imm */\n");
	fprintf(out, "#define RELOC_ABS64      8  /* 8-byte absolute address */\n");
	fprintf(out, "#define RELOC_UNKNOWN    0\n");
	fprintf(out, "#endif\n\n");

	/* Struct definitions (guarded so they don't clash with LLVM pipeline header) */
	fprintf(out, "#ifndef PRECOMPILED_RELOC_DEFINED\n");
	fprintf(out, "#define PRECOMPILED_RELOC_DEFINED\n");
	fprintf(out, "typedef struct PrecompiledReloc {\n");
	fprintf(out, "    uint16_t    offset;     /* offset within code */\n");
	fprintf(out, "    uint8_t     type;       /* RELOC_* constant */\n");
	fprintf(out, "    const char *symbol;     /* symbol name for runtime resolution */\n");
	fprintf(out, "} PrecompiledReloc;\n\n");

	fprintf(out, "typedef struct PrecompiledInline {\n");
	fprintf(out, "    const uint8_t  *code;       /* instruction bytes */\n");
	fprintf(out, "    uint16_t        code_len;   /* total length */\n");
	fprintf(out, "    int16_t         ret_offset; /* offset of ret instruction (-1 if none) */\n");
	fprintf(out, "    uint8_t         n_relocs;   /* number of relocations */\n");
	fprintf(out, "    PrecompiledReloc relocs[8];  /* relocations */\n");
	fprintf(out, "} PrecompiledInline;\n");
	fprintf(out, "#endif\n\n");

	/* Emit code byte arrays */
	for (int i = 0; i < n_func_blobs; i++)
	{
		FuncBlob *fb = &func_blobs[i];
		fprintf(out, "/* %s: %zu bytes, ret@%d, %d reloc(s) */\n",
				fb->name, fb->code_len, fb->ret_offset, fb->n_relocs);
		fprintf(out, "static const uint8_t mir_code_%s[] = {\n    ", fb->name);
		for (size_t j = 0; j < fb->code_len; j++)
		{
			fprintf(out, "0x%02x", fb->code[j]);
			if (j < fb->code_len - 1)
			{
				fprintf(out, ",");
				if ((j + 1) % 16 == 0)
					fprintf(out, "\n    ");
				else
					fprintf(out, " ");
			}
		}
		fprintf(out, "\n};\n\n");
	}

	/* Emit PrecompiledInline structs */
	for (int i = 0; i < n_func_blobs; i++)
	{
		FuncBlob *fb = &func_blobs[i];
		fprintf(out, "static const PrecompiledInline mir_precompiled_%s = {\n", fb->name);
		fprintf(out, "    .code = mir_code_%s,\n", fb->name);
		fprintf(out, "    .code_len = %zu,\n", fb->code_len);
		fprintf(out, "    .ret_offset = %d,\n", fb->ret_offset);
		fprintf(out, "    .n_relocs = %d,\n", fb->n_relocs);
		if (fb->n_relocs > 0)
		{
			fprintf(out, "    .relocs = {\n");
			for (int r = 0; r < fb->n_relocs; r++)
			{
				fprintf(out, "        { .offset = %u, .type = %u, .symbol = \"%s\" },\n",
						fb->relocs[r].offset, fb->relocs[r].type, fb->relocs[r].symbol);
			}
			fprintf(out, "    },\n");
		}
		else
		{
			fprintf(out, "    .relocs = {{0}},\n");
		}
		fprintf(out, "};\n\n");
	}

	/* Emit lookup table */
	fprintf(out, "#ifndef PRECOMPILED_ENTRY_DEFINED\n");
	fprintf(out, "#define PRECOMPILED_ENTRY_DEFINED\n");
	fprintf(out, "typedef struct PrecompiledMirEntry {\n");
	fprintf(out, "    const char            *name;\n");
	fprintf(out, "    const PrecompiledInline *blob;\n");
	fprintf(out, "} PrecompiledMirEntry;\n");
	fprintf(out, "#endif\n\n");

	fprintf(out, "static const PrecompiledMirEntry precompiled_mir_inlines[] = {\n");
	for (int i = 0; i < n_func_blobs; i++)
	{
		FuncBlob *fb = &func_blobs[i];
		fprintf(out, "    { \"%s\", &mir_precompiled_%s },\n", fb->name, fb->name);
	}
	fprintf(out, "};\n\n");
	fprintf(out, "static const int precompiled_mir_inlines_count = %d;\n\n", n_func_blobs);

	fprintf(out, "#endif /* PG_JIT_PRECOMPILED_MIR_H */\n");
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-I<dir>]... [-D<macro>]... -o <output.h> <source.c>\n", prog);
	exit(1);
}

int main(int argc, char **argv)
{
	const char *output_file = NULL;
	const char *source_file = NULL;

	/* Collect include dirs and macro defs */
	const char *include_dirs[64];
	int n_include_dirs = 0;
	struct c2mir_macro_command macros[64];
	int n_macros = 0;

	/* Always define PG_JITTER_PRECOMPILE_ONLY to exclude non-function code */
	macros[n_macros].def_p = 1;
	macros[n_macros].name = "PG_JITTER_PRECOMPILE_ONLY";
	macros[n_macros].def = "1";
	n_macros++;

	/* Parse command line */
	for (int i = 1; i < argc; i++)
	{
		if (strncmp(argv[i], "-I", 2) == 0)
		{
			if (argv[i][2] != '\0')
				include_dirs[n_include_dirs++] = argv[i] + 2;
			else if (i + 1 < argc)
				include_dirs[n_include_dirs++] = argv[++i];
		}
		else if (strncmp(argv[i], "-D", 2) == 0)
		{
			char *eq = strchr(argv[i] + 2, '=');
			macros[n_macros].def_p = 1;
			if (eq)
			{
				*eq = '\0';
				macros[n_macros].name = argv[i] + 2;
				macros[n_macros].def = eq + 1;
			}
			else
			{
				macros[n_macros].name = argv[i] + 2;
				macros[n_macros].def = "1";
			}
			n_macros++;
		}
		else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
		{
			output_file = argv[++i];
		}
		else if (argv[i][0] != '-')
		{
			source_file = argv[i];
		}
		else
		{
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			usage(argv[0]);
		}
	}

	if (!output_file || !source_file)
		usage(argv[0]);

	/* Open source file */
	FILE *src_fp = fopen(source_file, "r");
	if (!src_fp)
	{
		fprintf(stderr, "Cannot open source: %s\n", source_file);
		return 1;
	}

	/* Initialize MIR context */
	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);

	/* Set up c2mir options */
	struct c2mir_options opts;
	memset(&opts, 0, sizeof(opts));
	opts.message_file = stderr;
	opts.debug_p = 0;
	opts.verbose_p = 0;
	opts.ignore_warnings_p = 1;
	opts.module_num = 0;
	opts.include_dirs = include_dirs;
	opts.include_dirs_num = n_include_dirs;
	opts.macro_commands = macros;
	opts.macro_commands_num = n_macros;

	/* Compile C source to MIR modules */
	ReaderData rd = { .fp = src_fp };
	int ok = c2mir_compile(ctx, &opts, source_reader, &rd, source_file, NULL);
	fclose(src_fp);

	if (!ok)
	{
		fprintf(stderr, "c2mir compilation failed for %s\n", source_file);
		c2mir_finish(ctx);
		MIR_finish(ctx);
		return 1;
	}

	fprintf(stderr, "c2mir compilation succeeded for %s\n", source_file);

	/*
	 * Load all modules and register external symbols with sentinel addresses.
	 * MIR_gen() will resolve external calls to these sentinel addresses,
	 * which we can then detect in the generated native code.
	 */
	DLIST(MIR_module_t) *modules = MIR_get_module_list(ctx);
	MIR_module_t module;

	for (module = DLIST_HEAD(MIR_module_t, *modules);
		 module != NULL;
		 module = DLIST_NEXT(MIR_module_t, module))
	{
		MIR_load_module(ctx, module);
	}

	/* Register external symbols with sentinel addresses */
	for (int e = 0; e < (int)N_EXTERNALS; e++)
	{
		/*
		 * We pass the sentinel value as the function pointer.
		 * MIR will use this address in the generated code, allowing us
		 * to find and record it as a relocation.
		 *
		 * The sentinel is a valid-looking pointer value but will never
		 * be called during build-time compilation — we only extract the
		 * generated bytes, we don't execute them.
		 */
		MIR_load_external(ctx, externals[e].name,
						  (void *)externals[e].sentinel);
	}

	/* Initialize code generation */
	MIR_gen_init(ctx);
	MIR_gen_set_optimize_level(ctx, 2);  /* maximize code quality */

	/* Link all modules */
	MIR_link(ctx, MIR_set_gen_interface, NULL);

	/*
	 * MIR_gen() each jit_* function and extract native code.
	 */
	for (module = DLIST_HEAD(MIR_module_t, *modules);
		 module != NULL;
		 module = DLIST_NEXT(MIR_module_t, module))
	{
		MIR_item_t item;
		for (item = DLIST_HEAD(MIR_item_t, module->items);
			 item != NULL;
			 item = DLIST_NEXT(MIR_item_t, item))
		{
			if (item->item_type != MIR_func_item)
				continue;
			if (!is_jit_function(item->u.func->name))
				continue;
			if (n_func_blobs >= MAX_FUNCTIONS)
			{
				fprintf(stderr, "Warning: exceeded MAX_FUNCTIONS (%d)\n", MAX_FUNCTIONS);
				break;
			}

			/* Generate native code */
			MIR_gen(ctx, item);

			/* Extract native bytes */
			void  *machine_code = item->u.func->machine_code;
			size_t code_len     = item->u.func->machine_code_len;

			if (!machine_code || code_len == 0)
			{
				fprintf(stderr, "  SKIP %s: no machine code generated\n",
						item->u.func->name);
				continue;
			}

			if (code_len > MAX_CODE_SIZE)
			{
				fprintf(stderr, "  SKIP %s: code too large (%zu bytes > %d)\n",
						item->u.func->name, code_len, MAX_CODE_SIZE);
				continue;
			}

			FuncBlob *fb = &func_blobs[n_func_blobs];
			snprintf(fb->name, sizeof(fb->name), "%s", item->u.func->name);
			memcpy(fb->code, machine_code, code_len);
			fb->code_len = code_len;
			fb->n_relocs = 0;

			/* Scan for sentinel references and RET */
			scan_relocations(fb);

			fprintf(stderr, "  %s: %zu bytes, ret@%d, %d reloc(s)\n",
					fb->name, fb->code_len, fb->ret_offset, fb->n_relocs);

			n_func_blobs++;
		}
	}

	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);

	if (n_func_blobs == 0)
	{
		fprintf(stderr, "Error: no jit_* functions found\n");
		return 1;
	}

	/* Write output header */
	FILE *out = fopen(output_file, "w");
	if (!out)
	{
		fprintf(stderr, "Cannot open output: %s\n", output_file);
		return 1;
	}

	emit_header(out);
	fclose(out);

	fprintf(stderr, "Generated %s: %d native functions for %s\n",
			output_file, n_func_blobs, get_arch_name());
	return 0;
}
