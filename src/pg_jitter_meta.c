/*
 * pg_jitter_meta.c — Meta JIT provider for runtime backend switching
 *
 * This thin dispatcher registers as a JIT provider (jit_provider = 'pg_jitter')
 * and exposes a pg_jitter.backend GUC (PGC_USERSET) so users can switch between
 * sljit, asmjit, and mir backends at runtime without restarting PostgreSQL.
 *
 * Backend .dylib/.so files are loaded lazily on first use via
 * load_external_function() and cached for the lifetime of the process.
 *
 * Each backend remains independently usable (jit_provider = 'pg_jitter_sljit').
 *
 * Resource owner coordination: each backend dylib compiles its own copy of
 * pg_jitter_common.c, giving each a separate pg_jitter_resowner_desc address.
 * ResourceOwnerForget matches by pointer, so release_context must use the same
 * desc that Remember used. The meta-provider solves this by pre-creating the
 * JIT context (with the meta's own resowner desc) before delegating compile to
 * backends. The backend's pg_jitter_get_context() finds es_jit already set and
 * returns it without re-registering.
 */
#include "postgres.h"
#include "fmgr.h"
#include "jit/jit.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/fd.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

PG_MODULE_MAGIC_EXT(.name = "pg_jitter");

/* ----------------------------------------------------------------
 * PgJitterContext — must match the layout in pg_jitter_common.h
 * ---------------------------------------------------------------- */
typedef struct MetaCompiledCode
{
	struct MetaCompiledCode *next;
	void	(*free_fn)(void *data);
	void   *data;
} MetaCompiledCode;

typedef struct MetaJitterContext
{
	JitContext			base;			/* must be first */
	MetaCompiledCode   *compiled_list;
	ResourceOwner		resowner;
} MetaJitterContext;

/* ----------------------------------------------------------------
 * Resource owner support — the meta's own copy
 * ---------------------------------------------------------------- */
static void MetaResOwnerRelease(Datum res);

static const ResourceOwnerDesc meta_resowner_desc =
{
	.name = "pg_jitter JIT context",
	.release_phase = RESOURCE_RELEASE_BEFORE_LOCKS,
	.release_priority = RELEASE_PRIO_JIT_CONTEXTS,
	.ReleaseResource = MetaResOwnerRelease,
	.DebugPrint = NULL
};

static inline void
MetaRememberContext(ResourceOwner owner, MetaJitterContext *handle)
{
	ResourceOwnerRemember(owner, PointerGetDatum(handle), &meta_resowner_desc);
}

static inline void
MetaForgetContext(ResourceOwner owner, MetaJitterContext *handle)
{
	ResourceOwnerForget(owner, PointerGetDatum(handle), &meta_resowner_desc);
}

static void
MetaResOwnerRelease(Datum res)
{
	MetaJitterContext *context = (MetaJitterContext *) DatumGetPointer(res);

	context->resowner = NULL;
	jit_release_context(&context->base);
}

/* ----------------------------------------------------------------
 * Backend enum and GUC options
 * ---------------------------------------------------------------- */
enum PgJitterBackend
{
	PG_JITTER_BACKEND_SLJIT = 0,
	PG_JITTER_BACKEND_ASMJIT = 1,
	PG_JITTER_BACKEND_MIR = 2,
	PG_JITTER_NUM_BACKENDS
};

static const struct config_enum_entry backend_options[] = {
	{"sljit", PG_JITTER_BACKEND_SLJIT, false},
	{"asmjit", PG_JITTER_BACKEND_ASMJIT, false},
	{"mir", PG_JITTER_BACKEND_MIR, false},
	{NULL, 0, false}
};

static const char *backend_libnames[] = {
	"pg_jitter_sljit",
	"pg_jitter_asmjit",
	"pg_jitter_mir",
};

static int pg_jitter_backend = PG_JITTER_BACKEND_SLJIT;

/* ----------------------------------------------------------------
 * Cached backend state
 * ---------------------------------------------------------------- */
typedef struct BackendEntry
{
	bool				attempted;	/* tried to load */
	bool				available;	/* successfully loaded */
	JitProviderCallbacks cb;
} BackendEntry;

static BackendEntry backends[PG_JITTER_NUM_BACKENDS];

/* ----------------------------------------------------------------
 * Backend loading
 * ---------------------------------------------------------------- */

/*
 * Attempt to load a backend by index. Returns true if available.
 * Probes for the .dylib/.so file first (avoids ERROR from
 * load_external_function on missing files).
 */
static bool
meta_load_backend(int idx)
{
	char			path[MAXPGPATH];
	JitProviderInit init_fn;

	Assert(idx >= 0 && idx < PG_JITTER_NUM_BACKENDS);

	if (backends[idx].attempted)
		return backends[idx].available;

	backends[idx].attempted = true;
	backends[idx].available = false;

	/* Probe: does the shared library file exist? */
	snprintf(path, MAXPGPATH, "%s/%s%s",
			 pkglib_path, backend_libnames[idx], DLSUFFIX);

	if (!pg_file_exists(path))
	{
		elog(DEBUG1, "pg_jitter: backend %s not found at %s",
			 backend_libnames[idx], path);
		return false;
	}

	/* Load and initialize */
	init_fn = (JitProviderInit)
		load_external_function(path, "_PG_jit_provider_init", true, NULL);

	init_fn(&backends[idx].cb);
	backends[idx].available = true;

	elog(DEBUG1, "pg_jitter: loaded backend %s", backend_libnames[idx]);
	return true;
}

/* ----------------------------------------------------------------
 * GUC assign hook — eagerly attempt load on SET
 * ---------------------------------------------------------------- */
static void
meta_backend_assign(int newval, void *extra)
{
	if (!meta_load_backend(newval))
		ereport(WARNING,
				(errmsg("pg_jitter backend \"%s\" is not installed, will fall back",
						backend_libnames[newval])));
}

/* ----------------------------------------------------------------
 * Context pre-creation
 *
 * Pre-create the JIT context with the meta-provider's resowner desc
 * so that release_context can use the matching desc for ForgetContext.
 * The backend's pg_jitter_get_context() sees es_jit already set and
 * returns it without re-registering with its own desc.
 * ---------------------------------------------------------------- */
static void
meta_ensure_context(ExprState *state)
{
	PlanState	   *parent = state->parent;
	MetaJitterContext *ctx;

	Assert(parent != NULL);

	if (parent->state->es_jit)
		return;		/* context already exists */

	ResourceOwnerEnlarge(CurrentResourceOwner);

	ctx = (MetaJitterContext *)
		MemoryContextAllocZero(TopMemoryContext, sizeof(MetaJitterContext));
	ctx->base.flags = parent->state->es_jit_flags;
	ctx->compiled_list = NULL;
	ctx->resowner = CurrentResourceOwner;

	MetaRememberContext(CurrentResourceOwner, ctx);

	parent->state->es_jit = &ctx->base;
}

/* ----------------------------------------------------------------
 * Provider callbacks — dispatch to loaded backend
 * ---------------------------------------------------------------- */

static bool
meta_compile_expr(ExprState *state)
{
	int		idx = pg_jitter_backend;

	/*
	 * Pre-create the context with our resowner desc before the backend
	 * gets a chance to create it with its own desc.
	 */
	if (state->parent)
		meta_ensure_context(state);

	/* Try selected backend first */
	if (meta_load_backend(idx))
		return backends[idx].cb.compile_expr(state);

	/* Fallback: try each in order */
	for (idx = 0; idx < PG_JITTER_NUM_BACKENDS; idx++)
	{
		if (meta_load_backend(idx))
			return backends[idx].cb.compile_expr(state);
	}

	/* No backend available — PG interpreter will run */
	return false;
}

static void
meta_release_context(JitContext *context)
{
	MetaJitterContext *ctx = (MetaJitterContext *) context;
	MetaCompiledCode *cc, *next;

	/* Free all compiled code — each node carries its own free_fn */
	for (cc = ctx->compiled_list; cc != NULL; cc = next)
	{
		next = cc->next;
		if (cc->free_fn)
			cc->free_fn(cc->data);
		pfree(cc);
	}
	ctx->compiled_list = NULL;

	if (ctx->resowner)
		MetaForgetContext(ctx->resowner, ctx);
}

static void
meta_reset_after_error(void)
{
	/* Call reset_after_error on ALL loaded backends */
	for (int idx = 0; idx < PG_JITTER_NUM_BACKENDS; idx++)
	{
		if (backends[idx].available)
			backends[idx].cb.reset_after_error();
	}
}

/* ----------------------------------------------------------------
 * SQL function: pg_jitter_backend() returns text
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(pg_jitter_current_backend);

Datum
pg_jitter_current_backend(PG_FUNCTION_ARGS)
{
	const char *name = backend_options[pg_jitter_backend].name;

	PG_RETURN_TEXT_P(cstring_to_text(name));
}

/* ----------------------------------------------------------------
 * Provider entry point
 * ---------------------------------------------------------------- */
/*
 * Probe which backend .dylib/.so files are installed and return the
 * highest-priority one as the boot default.  Priority: sljit > asmjit > mir.
 * Does NOT load anything — just checks file existence.
 */
static int
meta_detect_default(void)
{
	char	path[MAXPGPATH];

	for (int idx = 0; idx < PG_JITTER_NUM_BACKENDS; idx++)
	{
		snprintf(path, MAXPGPATH, "%s/%s%s",
				 pkglib_path, backend_libnames[idx], DLSUFFIX);
		if (pg_file_exists(path))
			return idx;
	}

	/* Nothing installed — keep sljit as nominal default; fallback handles it */
	return PG_JITTER_BACKEND_SLJIT;
}

void
_PG_jit_provider_init(JitProviderCallbacks *cb)
{
	int		boot_default;

	cb->reset_after_error = meta_reset_after_error;
	cb->release_context = meta_release_context;
	cb->compile_expr = meta_compile_expr;

	boot_default = meta_detect_default();
	pg_jitter_backend = boot_default;

	DefineCustomEnumVariable("pg_jitter.backend",
							 "Selects the active pg_jitter JIT backend.",
							 NULL,
							 &pg_jitter_backend,
							 boot_default,
							 backend_options,
							 PGC_USERSET,
							 0,
							 NULL,
							 meta_backend_assign,
							 NULL);

	MarkGUCPrefixReserved("pg_jitter");
}
