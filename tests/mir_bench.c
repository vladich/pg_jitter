/*
 * mir_bench.c — Standalone benchmark comparing MIR API codegen vs c2mir
 *
 * Build:
 *   cc -O2 -I$HOME/PgCypher/mir -I$HOME/PgCypher/mir/c2mir \
 *      -o mir_bench mir_bench.c \
 *      $HOME/PgCypher/mir/mir.c $HOME/PgCypher/mir/mir-gen.c \
 *      $HOME/PgCypher/mir/c2mir/c2mir.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mir.h"
#include "mir-gen.h"
#include "c2mir.h"

static double now_usec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* Build a small function: load, branch, add, call, xor, store, return.
 * ~8 MIR insns — similar to a pg_jitter compiled expression. */
static MIR_item_t build_small_func(MIR_context_t ctx, const char *fname,
                                    MIR_item_t proto, MIR_item_t import) {
    MIR_type_t res_type = MIR_T_I64;
    MIR_item_t fi = MIR_new_func(ctx, fname, 1, &res_type,
                                  3, MIR_T_I64, "a", MIR_T_I64, "b", MIR_T_P, "p");
    MIR_func_t f = fi->u.func;
    MIR_reg_t r_a = MIR_reg(ctx, "a", f);
    MIR_reg_t r_b = MIR_reg(ctx, "b", f);
    MIR_reg_t r_p = MIR_reg(ctx, "p", f);
    MIR_reg_t r_t1 = MIR_new_func_reg(ctx, f, MIR_T_I64, "t1");
    MIR_reg_t r_t2 = MIR_new_func_reg(ctx, f, MIR_T_I64, "t2");
    MIR_reg_t r_ret = MIR_new_func_reg(ctx, f, MIR_T_I64, "ret");
    MIR_insn_t skip = MIR_new_label(ctx);

    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_t1),
            MIR_new_mem_op(ctx, MIR_T_I64, 0, r_p, 0, 1)));
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, skip),
            MIR_new_reg_op(ctx, r_t1), MIR_new_int_op(ctx, 0)));
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_t2),
            MIR_new_reg_op(ctx, r_a), MIR_new_reg_op(ctx, r_b)));
    MIR_append_insn(ctx, fi,
        MIR_new_call_insn(ctx, 4,
            MIR_new_ref_op(ctx, proto), MIR_new_ref_op(ctx, import),
            MIR_new_reg_op(ctx, r_ret), MIR_new_reg_op(ctx, r_p)));
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_XOR, MIR_new_reg_op(ctx, r_t2),
            MIR_new_reg_op(ctx, r_t2), MIR_new_reg_op(ctx, r_ret)));
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_MOV,
            MIR_new_mem_op(ctx, MIR_T_I64, 0, r_p, 0, 1),
            MIR_new_reg_op(ctx, r_t2)));
    MIR_append_insn(ctx, fi, skip);
    MIR_append_insn(ctx, fi,
        MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_t1)));
    MIR_finish_func(ctx);
    return fi;
}

/* Build a larger function: ~60 MIR insns — representative of pg_jitter expressions.
 * Pattern: prologue + 5 steps each with load-addr/store/load-addr/store + fallback call */
static MIR_item_t build_large_func(MIR_context_t ctx, const char *fname,
                                    MIR_item_t proto, MIR_item_t import) {
    MIR_type_t res_type = MIR_T_I64;
    MIR_item_t fi = MIR_new_func(ctx, fname, 1, &res_type,
                                  3, MIR_T_I64, "st", MIR_T_I64, "ec", MIR_T_P, "isnp");
    MIR_func_t f = fi->u.func;
    MIR_reg_t r_st = MIR_reg(ctx, "st", f);
    MIR_reg_t r_ec = MIR_reg(ctx, "ec", f);
    MIR_reg_t r_isnp = MIR_reg(ctx, "isnp", f);
    MIR_reg_t r_t1 = MIR_new_func_reg(ctx, f, MIR_T_I64, "t1");
    MIR_reg_t r_t2 = MIR_new_func_reg(ctx, f, MIR_T_I64, "t2");
    MIR_reg_t r_t3 = MIR_new_func_reg(ctx, f, MIR_T_I64, "t3");
    MIR_reg_t r_sl = MIR_new_func_reg(ctx, f, MIR_T_I64, "sl");
    MIR_reg_t r_ret = MIR_new_func_reg(ctx, f, MIR_T_I64, "ret");
    MIR_reg_t r_rv = MIR_new_func_reg(ctx, f, MIR_T_I64, "rv");
    MIR_reg_t r_rn = MIR_new_func_reg(ctx, f, MIR_T_I64, "rn");

    /* Prologue: ~10 insns (add, add, load, branch, load, load, label) */
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_rv),
            MIR_new_reg_op(ctx, r_st), MIR_new_int_op(ctx, 120)));
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_rn),
            MIR_new_reg_op(ctx, r_st), MIR_new_int_op(ctx, 128)));
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_sl),
            MIR_new_mem_op(ctx, MIR_T_P, 136, r_st, 0, 1)));

    MIR_insn_t skip = MIR_new_label(ctx);
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, skip),
            MIR_new_reg_op(ctx, r_sl), MIR_new_int_op(ctx, 0)));
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_t1),
            MIR_new_mem_op(ctx, MIR_T_P, 48, r_sl, 0, 1)));
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_t2),
            MIR_new_mem_op(ctx, MIR_T_P, 56, r_sl, 0, 1)));
    MIR_append_insn(ctx, fi, skip);

    /* 5 "steps", each: load-addr, store-64, load-addr, store-u8 + maybe branch + call (~10 insns each) */
    for (int step = 0; step < 5; step++) {
        MIR_insn_t step_label = MIR_new_label(ctx);
        MIR_append_insn(ctx, fi, step_label);

        /* Load addr into t3, store value */
        MIR_append_insn(ctx, fi,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_t3),
                MIR_new_uint_op(ctx, 0x1000 + step * 16)));
        MIR_append_insn(ctx, fi,
            MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64, 0, r_t3, 0, 1),
                MIR_new_reg_op(ctx, r_t1)));
        /* Load addr into t3, store null byte */
        MIR_append_insn(ctx, fi,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_t3),
                MIR_new_uint_op(ctx, 0x2000 + step * 16)));
        MIR_append_insn(ctx, fi,
            MIR_new_insn(ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_U8, 0, r_t3, 0, 1),
                MIR_new_int_op(ctx, 0)));

        if (step == 2) {
            /* Function call step */
            MIR_append_insn(ctx, fi,
                MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_t3),
                    MIR_new_uint_op(ctx, 0x3000)));
            MIR_append_insn(ctx, fi,
                MIR_new_insn(ctx, MIR_MOV,
                    MIR_new_mem_op(ctx, MIR_T_U8, 64, r_t3, 0, 1),
                    MIR_new_int_op(ctx, 0)));
            MIR_append_insn(ctx, fi,
                MIR_new_call_insn(ctx, 4,
                    MIR_new_ref_op(ctx, proto), MIR_new_ref_op(ctx, import),
                    MIR_new_reg_op(ctx, r_ret), MIR_new_reg_op(ctx, r_t3)));
            MIR_append_insn(ctx, fi,
                MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_t3),
                    MIR_new_uint_op(ctx, 0x1000 + step * 16)));
            MIR_append_insn(ctx, fi,
                MIR_new_insn(ctx, MIR_MOV,
                    MIR_new_mem_op(ctx, MIR_T_I64, 0, r_t3, 0, 1),
                    MIR_new_reg_op(ctx, r_ret)));
        } else {
            /* Load from memory + conditional branch */
            MIR_append_insn(ctx, fi,
                MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_t1),
                    MIR_new_mem_op(ctx, MIR_T_I64, step * 8, r_ec, 0, 1)));
            MIR_append_insn(ctx, fi,
                MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_t2),
                    MIR_new_mem_op(ctx, MIR_T_U8, step, r_ec, 0, 1)));
        }
    }

    /* Return */
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_t1),
            MIR_new_mem_op(ctx, MIR_T_I64, 120, r_st, 0, 1)));
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_t2),
            MIR_new_mem_op(ctx, MIR_T_U8, 128, r_st, 0, 1)));
    MIR_append_insn(ctx, fi,
        MIR_new_insn(ctx, MIR_MOV,
            MIR_new_mem_op(ctx, MIR_T_U8, 0, r_isnp, 0, 1),
            MIR_new_reg_op(ctx, r_t2)));
    MIR_append_insn(ctx, fi,
        MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_t1)));

    MIR_finish_func(ctx);
    return fi;
}

/*
 * Test 1: N modules, 1 function each, link+gen per module (pg_jitter style)
 */
static void bench_per_module(int n, int opt) {
    MIR_context_t ctx = MIR_init();
    MIR_gen_init(ctx);
    MIR_gen_set_optimize_level(ctx, opt);

    double t_build = 0, t_link = 0, t_gen = 0;

    for (int i = 0; i < n; i++) {
        char mn[32], fn[32];
        snprintf(mn, sizeof(mn), "m%d", i);
        snprintf(fn, sizeof(fn), "f%d", i);

        double t0 = now_usec();
        MIR_module_t m = MIR_new_module(ctx, mn);
        MIR_type_t rt = MIR_T_I64;
        MIR_item_t proto = MIR_new_proto(ctx, "p", 1, &rt, 1, MIR_T_P, "a");
        MIR_item_t imp = MIR_new_import(ctx, "ext");
        MIR_item_t fi = build_small_func(ctx, fn, proto, imp);
        MIR_finish_module(ctx);
        double t1 = now_usec();
        t_build += t1 - t0;

        MIR_load_module(ctx, m);
        MIR_load_external(ctx, "ext", (void *)(uintptr_t)0xDEAD);

        double t2 = now_usec();
        MIR_link(ctx, MIR_set_lazy_gen_interface, NULL);
        double t3 = now_usec();
        t_link += t3 - t2;

        void *code = MIR_gen(ctx, fi);
        double t4 = now_usec();
        t_gen += t4 - t3;
        (void)code;
    }

    printf("  per-module (opt=%d):  build=%5.0f  link=%5.0f  gen=%5.0f  total=%5.0f us/func\n",
           opt, t_build/n, t_link/n, t_gen/n, (t_build+t_link+t_gen)/n);

    MIR_gen_finish(ctx);
    MIR_finish(ctx);
}

/*
 * Test 2: 1 module with N functions, link once, gen each
 */
static void bench_one_module(int n, int opt) {
    MIR_context_t ctx = MIR_init();
    MIR_gen_init(ctx);
    MIR_gen_set_optimize_level(ctx, opt);

    MIR_module_t m = MIR_new_module(ctx, "batch");
    MIR_type_t rt = MIR_T_I64;
    MIR_item_t proto = MIR_new_proto(ctx, "p", 1, &rt, 1, MIR_T_P, "a");
    MIR_item_t imp = MIR_new_import(ctx, "ext");

    MIR_item_t *fis = malloc(n * sizeof(MIR_item_t));
    double t0 = now_usec();
    for (int i = 0; i < n; i++) {
        char fn[32];
        snprintf(fn, sizeof(fn), "f%d", i);
        fis[i] = build_small_func(ctx, fn, proto, imp);
    }
    MIR_finish_module(ctx);
    double t1 = now_usec();

    MIR_load_module(ctx, m);
    MIR_load_external(ctx, "ext", (void *)(uintptr_t)0xDEAD);

    double t2 = now_usec();
    MIR_link(ctx, MIR_set_lazy_gen_interface, NULL);
    double t3 = now_usec();

    double t_gen = 0;
    for (int i = 0; i < n; i++) {
        double tg0 = now_usec();
        void *code = MIR_gen(ctx, fis[i]);
        double tg1 = now_usec();
        t_gen += tg1 - tg0;
        (void)code;
    }

    printf("  one-module (opt=%d):  build=%5.0f  link=%5.0f  gen=%5.0f  total=%5.0f us/func\n",
           opt, (t1-t0)/n, (t3-t2)/n, t_gen/n, ((t1-t0)+(t3-t2)+t_gen)/n);

    free(fis);
    MIR_gen_finish(ctx);
    MIR_finish(ctx);
}

/*
 * Test 3: c2mir — compile equivalent C per module
 */
/* Use __attribute__((used)) or unique names to avoid redefinition error */
static const char *c_src_fmt =
    "typedef long int64_t;\n"
    "extern int64_t ext(void *);\n"
    "int64_t test_func_%d(int64_t a, int64_t b, void *p) {\n"
    "  int64_t t1 = *(int64_t *)p;\n"
    "  if (t1 == 0) return t1;\n"
    "  int64_t t2 = a + b;\n"
    "  int64_t ret = ext(p);\n"
    "  t2 ^= ret;\n"
    "  *(int64_t *)p = t2;\n"
    "  return t1;\n"
    "}\n";

struct str_rd { const char *s; int pos; };
static int sgetc(void *d) { struct str_rd *r = d; return r->s[r->pos] ? (unsigned char)r->s[r->pos++] : EOF; }

static void bench_c2mir(int n, int opt) {
    MIR_context_t ctx = MIR_init();
    MIR_gen_init(ctx);
    MIR_gen_set_optimize_level(ctx, opt);
    c2mir_init(ctx);

    struct c2mir_options ops = {0};
    double t_compile = 0, t_link = 0, t_gen = 0;

    for (int i = 0; i < n; i++) {
        char c_buf[512];
        snprintf(c_buf, sizeof(c_buf), c_src_fmt, i);
        struct str_rd rd = { .s = c_buf, .pos = 0 };
        ops.module_num = i;

        double t0 = now_usec();
        c2mir_compile(ctx, &ops, sgetc, &rd, "t.c", NULL);
        double t1 = now_usec();
        t_compile += t1 - t0;

        /* Find last module in list */
        MIR_module_t m = NULL;
        for (MIR_module_t mm = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx));
             mm != NULL; mm = DLIST_NEXT(MIR_module_t, mm))
            m = mm;

        MIR_load_module(ctx, m);
        MIR_load_external(ctx, "ext", (void *)(uintptr_t)0xDEAD);

        double t2 = now_usec();
        MIR_link(ctx, MIR_set_lazy_gen_interface, NULL);
        double t3 = now_usec();
        t_link += t3 - t2;

        for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->items); item != NULL;
             item = DLIST_NEXT(MIR_item_t, item)) {
            if (item->item_type == MIR_func_item) {
                double tg0 = now_usec();
                void *code = MIR_gen(ctx, item);
                double tg1 = now_usec();
                t_gen += tg1 - tg0;
                (void)code;
                break;
            }
        }
    }

    printf("  c2mir       (opt=%d):  compile=%3.0f  link=%5.0f  gen=%5.0f  total=%5.0f us/func\n",
           opt, t_compile/n, t_link/n, t_gen/n, (t_compile+t_link+t_gen)/n);

    c2mir_finish(ctx);
    MIR_gen_finish(ctx);
    MIR_finish(ctx);
}

int main(void) {
    int n = 200;
    printf("=== MIR Compilation Benchmark (%d functions) ===\n\n", n);

    for (int opt = 0; opt <= 2; opt++) {
        printf("Optimize level %d:\n", opt);
        bench_per_module(n, opt);
        bench_one_module(n, opt);
        bench_c2mir(n, opt);

        /* Large function test (per-module) */
        {
            MIR_context_t ctx = MIR_init();
            MIR_gen_init(ctx);
            MIR_gen_set_optimize_level(ctx, opt);
            double t_build = 0, t_link = 0, t_gen = 0;
            for (int i = 0; i < n; i++) {
                char mn[32], fn[32];
                snprintf(mn, sizeof(mn), "m%d", i);
                snprintf(fn, sizeof(fn), "f%d", i);
                double ta = now_usec();
                MIR_module_t m = MIR_new_module(ctx, mn);
                MIR_type_t rt = MIR_T_I64;
                MIR_item_t proto = MIR_new_proto(ctx, "p", 1, &rt, 1, MIR_T_P, "a");
                MIR_item_t imp = MIR_new_import(ctx, "ext");
                MIR_item_t fi = build_large_func(ctx, fn, proto, imp);
                MIR_finish_module(ctx);
                double tb = now_usec();
                t_build += tb - ta;
                MIR_load_module(ctx, m);
                MIR_load_external(ctx, "ext", (void *)(uintptr_t)0xDEAD);
                double tc = now_usec();
                MIR_link(ctx, MIR_set_lazy_gen_interface, NULL);
                double td = now_usec();
                t_link += td - tc;
                void *code = MIR_gen(ctx, fi);
                double te = now_usec();
                t_gen += te - td;
                (void)code;
            }
            printf("  LARGE per-mod(opt=%d):  build=%5.0f  link=%5.0f  gen=%5.0f  total=%5.0f us/func (~60 insns)\n",
                   opt, t_build/n, t_link/n, t_gen/n, (t_build+t_link+t_gen)/n);
            MIR_gen_finish(ctx);
            MIR_finish(ctx);
        }
        printf("\n");
    }
    return 0;
}
