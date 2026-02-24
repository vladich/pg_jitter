#!/usr/bin/env python3
"""
gen_tier2_wrappers.py — Generate LLVM IR wrappers for Tier 2 PG functions

For each Tier 2 function (numeric, text, uuid, interval), generates an LLVM IR
wrapper that:
  1. Builds a minimal FunctionCallInfoBaseData on the stack
  2. Populates args from native parameters
  3. Calls the PG V1 function
  4. Returns the Datum result

When linked with PG's bitcode modules and optimized, LLVM inlines the PG
function bodies into the wrappers and eliminates the fcinfo overhead.

Usage:
    python3 gen_tier2_wrappers.py \
        --pg-include /path/to/pg/include/server \
        --bitcode-dir /path/to/pg/lib/bitcode/postgres \
        --output tier2_wrappers.ll \
        --arch aarch64
"""

import argparse
import sys

# Tier 2 functions to wrap: (pg_func_name, nargs, ret_is_64bit, arg_types)
# arg_types: list of booleans, True = i64 (Datum), False = i32
TIER2_FUNCTIONS = [
    # numeric comparison
    ('numeric_eq',  2, False, [True, True]),
    ('numeric_ne',  2, False, [True, True]),
    ('numeric_lt',  2, False, [True, True]),
    ('numeric_le',  2, False, [True, True]),
    ('numeric_gt',  2, False, [True, True]),
    ('numeric_ge',  2, False, [True, True]),
    ('numeric_cmp', 2, False, [True, True]),
    # numeric arithmetic
    ('numeric_add', 2, True, [True, True]),
    ('numeric_sub', 2, True, [True, True]),
    ('numeric_mul', 2, True, [True, True]),
    # numeric hash
    ('hash_numeric', 1, False, [True]),
    # text comparison
    ('texteq',  2, False, [True, True]),
    ('textne',  2, False, [True, True]),
    ('text_lt', 2, False, [True, True]),
    ('bttextcmp', 2, False, [True, True]),
    ('hashtext',  1, False, [True]),
    # interval
    ('interval_eq',  2, False, [True, True]),
    ('interval_lt',  2, False, [True, True]),
    ('interval_cmp', 2, False, [True, True]),
    # uuid
    ('uuid_eq',   2, False, [True, True]),
    ('uuid_lt',   2, False, [True, True]),
    ('uuid_cmp',  2, False, [True, True]),
    ('uuid_hash', 1, False, [True]),
]


def llvm_type(is_64bit):
    return 'i64' if is_64bit else 'i32'


def generate_wrapper(name, nargs, ret_64, arg_types):
    """Generate LLVM IR for a single Tier 2 wrapper function."""
    ret_type = llvm_type(ret_64)
    arg_strs = [f'{llvm_type(at)} %arg{i}' for i, at in enumerate(arg_types)]
    sig = f'define {ret_type} @jit_{name}_precompiled({", ".join(arg_strs)})'

    lines = [f'{sig} {{', 'entry:']

    # FunctionCallInfoBaseData struct layout (PG18, 64-bit):
    #   Node       tag;           //  0: 8 bytes  (type + padding)
    #   FmgrInfo  *flinfo;        //  8: 8 bytes
    #   fmNodePtr  context;       // 16: 8 bytes
    #   fmNodePtr  resultinfo;    // 24: 8 bytes
    #   bool       isnull;        // 32: 1 byte
    #   short      nargs;         // 34: 2 bytes (after alignment)
    #   [padding]                 // 36: 4 bytes
    #   NullableDatum args[0];    // 40: array of (Datum[8] + bool[1] + pad[7]) = 16 each
    #
    # Total for nargs args: 40 + nargs * 16

    struct_size = 40 + nargs * 16

    # Allocate FunctionCallInfoBaseData on stack
    lines.append(f'    %fcinfo = alloca i8, i64 {struct_size}, align 8')
    # Zero it
    lines.append(f'    call void @llvm.memset.p0.i64('
                 f'ptr %fcinfo, i8 0, i64 {struct_size}, i1 false)')

    # Set nargs (offset 34, i16)
    lines.append(f'    %nargs_ptr = getelementptr i8, ptr %fcinfo, i64 34')
    lines.append(f'    store i16 {nargs}, ptr %nargs_ptr, align 2')

    # Set isnull = false (offset 32, i8) — already zero from memset

    # Set each arg's value and isnull
    for i, at in enumerate(arg_types):
        val_offset = 40 + i * 16
        null_offset = val_offset + 8

        # args[i].value (Datum = i64)
        lines.append(f'    %arg{i}_ptr = getelementptr i8, ptr %fcinfo, i64 {val_offset}')
        if at:  # i64
            lines.append(f'    store i64 %arg{i}, ptr %arg{i}_ptr, align 8')
        else:  # i32 → sext to i64
            lines.append(f'    %arg{i}_ext = sext i32 %arg{i} to i64')
            lines.append(f'    store i64 %arg{i}_ext, ptr %arg{i}_ptr, align 8')

        # args[i].isnull = false — already zero from memset

    # Call the PG function: Datum pg_func(FunctionCallInfo)
    lines.append(f'    %result = call i64 @{name}(ptr %fcinfo)')

    # Return
    if ret_64:
        lines.append(f'    ret i64 %result')
    else:
        lines.append(f'    %result32 = trunc i64 %result to i32')
        lines.append(f'    ret i32 %result32')

    lines.append('}')
    return '\n'.join(lines)


def generate_ir(arch):
    """Generate complete LLVM IR module."""
    lines = []

    # Module header
    if arch == 'aarch64':
        lines.append('target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"')
        lines.append('target triple = "arm64-apple-darwin"')
    else:
        lines.append('target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-'
                      'i64:64-f80:128-n8:16:32:64-S128"')
        lines.append('target triple = "x86_64-unknown-linux-gnu"')

    lines.append('')

    # Declare intrinsics
    lines.append('declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)')
    lines.append('')

    # Declare PG functions (external, will be resolved at link time)
    declared = set()
    for name, nargs, ret_64, arg_types in TIER2_FUNCTIONS:
        if name not in declared:
            lines.append(f'declare i64 @{name}(ptr)')
            declared.add(name)
    lines.append('')

    # Generate wrappers
    for name, nargs, ret_64, arg_types in TIER2_FUNCTIONS:
        lines.append(generate_wrapper(name, nargs, ret_64, arg_types))
        lines.append('')

    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Generate LLVM IR wrappers for Tier 2 PG functions')
    parser.add_argument('--pg-include', required=True,
                        help='PG server include directory')
    parser.add_argument('--bitcode-dir', required=True,
                        help='PG bitcode directory')
    parser.add_argument('--output', required=True,
                        help='Output LLVM IR file')
    parser.add_argument('--arch', required=True,
                        choices=['aarch64', 'x86_64'],
                        help='Target architecture')
    args = parser.parse_args()

    ir = generate_ir(args.arch)

    with open(args.output, 'w') as f:
        f.write(ir)

    print(f'Generated {len(TIER2_FUNCTIONS)} Tier 2 wrappers to {args.output}')
    for name, nargs, ret_64, _ in TIER2_FUNCTIONS:
        ret_str = 'i64' if ret_64 else 'i32'
        print(f'  jit_{name}_precompiled: {nargs} args -> {ret_str}')


if __name__ == '__main__':
    main()
