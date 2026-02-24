#!/usr/bin/env python3
"""
Generate pre-compiled deform template functions for pg_jitter.

Produces pg_jit_deform_templates.c with 68 deform functions for common
fixed-width byval column patterns (attlen ∈ {1,2,4,8}).

Coverage:
  - attlen ∈ {4,8}, natts 1-5: 2+4+8+16+32 = 62 patterns
  - attlen ∈ {1,2}, natts 1-2 only: 2+4 = 6 patterns
  - Total: 68 functions

Usage:
  python3 gen_deform_templates.py \
      --output-c src/pg_jit_deform_templates.c \
      --output-h src/pg_jit_deform_templates.h
"""

import argparse
import itertools
from dataclasses import dataclass


@dataclass
class DeformPattern:
    attlens: tuple  # e.g. (4, 8)
    offsets: tuple   # computed byte offsets per column
    total_size: int  # total data size after last column


def compute_offsets(attlens):
    """Compute PG TYPEALIGN offsets. For byval, attalignby == attlen."""
    offset = 0
    offsets = []
    for attlen in attlens:
        # Align to attlen boundary
        offset = (offset + attlen - 1) & ~(attlen - 1)
        offsets.append(offset)
        offset += attlen
    return tuple(offsets), offset


def attlen_name(attlen):
    """Map attlen to short name: 1→i1, 2→i2, 4→i4, 8→i8."""
    return f"i{attlen}"


def func_name(attlens):
    """Generate function name from attlen tuple."""
    parts = "_".join(attlen_name(a) for a in attlens)
    return f"jit_deform_{parts}"


def attlen_encode(attlen):
    """Encode attlen to 2 bits: 1→0, 2→1, 4→2, 8→3."""
    return {1: 0, 2: 1, 4: 2, 8: 3}[attlen]


def signature(attlens):
    """Compute signature uint16 for a pattern."""
    natts = len(attlens)
    sig = natts - 1  # bits [2:0]
    for i, al in enumerate(attlens):
        sig |= attlen_encode(al) << (3 + i * 2)
    return sig


def cast_expr(attlen, tp_var, offset):
    """Generate the C expression to extract a value from tuple data.

    Matches PG's fetch_att / CharGetDatum / Int16GetDatum / Int32GetDatum.
    All cast through signed type to match PG's sign-extension behavior.
    """
    if attlen == 1:
        return f"(Datum)(int8)(*(int8 *)({tp_var} + {offset}))"
    elif attlen == 2:
        return f"(Datum)(int16)(*(int16 *)({tp_var} + {offset}))"
    elif attlen == 4:
        return f"(Datum)(int32)(*(int32 *)({tp_var} + {offset}))"
    elif attlen == 8:
        return f"*(Datum *)({tp_var} + {offset})"
    else:
        raise ValueError(f"Unsupported attlen: {attlen}")


def generate_patterns():
    """Generate all 68 deform patterns."""
    patterns = []

    # attlen ∈ {4,8}, natts 1-5
    for natts in range(1, 6):
        for combo in itertools.product([4, 8], repeat=natts):
            offsets, total = compute_offsets(combo)
            patterns.append(DeformPattern(combo, offsets, total))

    # attlen ∈ {1,2}, natts 1-2 (excluding patterns already covered above)
    for natts in range(1, 3):
        for combo in itertools.product([1, 2], repeat=natts):
            offsets, total = compute_offsets(combo)
            patterns.append(DeformPattern(combo, offsets, total))

    return patterns


def generate_function(p: DeformPattern) -> str:
    """Generate a single deform function."""
    name = func_name(p.attlens)
    natts = len(p.attlens)
    lines = []

    lines.append(f"void")
    lines.append(f"{name}(TupleTableSlot *slot)")
    lines.append(f"{{")
    lines.append(f"\tHeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;")
    lines.append(f"\tHeapTupleHeader tup = hslot->tuple->t_data;")
    lines.append(f"\tDatum      *values = slot->tts_values;")
    lines.append(f"\tbool       *isnull = slot->tts_isnull;")
    lines.append(f"\tchar       *tp;")
    lines.append(f"\tint         nvalid = slot->tts_nvalid;")
    lines.append(f"")

    # Guard: HEAP_HASNULL
    lines.append(f"\tif (unlikely(tup->t_infomask & HEAP_HASNULL))")
    lines.append(f"\t{{")
    lines.append(f"\t\tslot_getsomeattrs_int(slot, {natts});")
    lines.append(f"\t\treturn;")
    lines.append(f"\t}}")
    lines.append(f"")

    # Guard: short tuple
    lines.append(f"\tif (unlikely(HeapTupleHeaderGetNatts(tup) < {natts}))")
    lines.append(f"\t{{")
    lines.append(f"\t\tslot_getsomeattrs_int(slot, {natts});")
    lines.append(f"\t\treturn;")
    lines.append(f"\t}}")
    lines.append(f"")

    lines.append(f"\ttp = (char *) tup + tup->t_hoff;")
    lines.append(f"")

    # Switch on nvalid for resume support
    lines.append(f"\tswitch (nvalid)")
    lines.append(f"\t{{")
    for i in range(natts):
        lines.append(f"\t\tcase {i}:")
        lines.append(f"\t\t\tvalues[{i}] = {cast_expr(p.attlens[i], 'tp', p.offsets[i])};")
        lines.append(f"\t\t\tisnull[{i}] = false;")
        if i < natts - 1:
            lines.append(f"\t\t\t/* FALLTHROUGH */")
    lines.append(f"\t\tdefault:")
    lines.append(f"\t\t\tbreak;")
    lines.append(f"\t}}")
    lines.append(f"")

    lines.append(f"\tslot->tts_nvalid = {natts};")
    lines.append(f"")

    # Write back off field (slot-type-dependent)
    lines.append(f"\tif (unlikely(slot->tts_ops == &TTSOpsMinimalTuple))")
    lines.append(f"\t\t((MinimalTupleTableSlot *) slot)->off = {p.total_size};")
    lines.append(f"\telse")
    lines.append(f"\t\thslot->off = {p.total_size};")
    lines.append(f"")
    lines.append(f"\tslot->tts_flags |= TTS_FLAG_SLOW;")
    lines.append(f"}}")

    return "\n".join(lines)


def generate_c_file(patterns):
    """Generate the complete .c file."""
    lines = []

    lines.append("/*-------------------------------------------------------------------------")
    lines.append(" *")
    lines.append(" * pg_jit_deform_templates.c")
    lines.append(" *\t\tPre-compiled deform functions for common fixed-width column patterns.")
    lines.append(" *")
    lines.append(" * GENERATED FILE — do not edit by hand.")
    lines.append(" * Generated by tools/gen_deform_templates.py")
    lines.append(" *")
    lines.append(" *-------------------------------------------------------------------------")
    lines.append(" */")
    lines.append('#include "postgres.h"')
    lines.append("")
    lines.append('#include "access/htup_details.h"')
    lines.append('#include "executor/tuptable.h"')
    lines.append("")
    lines.append('#include "pg_jit_deform_templates.h"')
    lines.append("")
    lines.append("")

    # Forward declarations
    for p in patterns:
        lines.append(f"static void {func_name(p.attlens)}(TupleTableSlot *slot);")
    lines.append("")
    lines.append("")

    # Function implementations
    for p in patterns:
        lines.append(f"static {generate_function(p)}")
        lines.append("")

    # Lookup table (sorted by signature)
    sorted_patterns = sorted(patterns, key=lambda p: signature(p.attlens))
    lines.append("const DeformTemplate jit_deform_templates[] = {")
    for p in sorted_patterns:
        sig = signature(p.attlens)
        name = func_name(p.attlens)
        lines.append(f"\t{{ 0x{sig:04X}, {name} }},")
    lines.append("};")
    lines.append("")
    lines.append(f"const int jit_deform_templates_count = {len(patterns)};")
    lines.append("")

    # Binary search lookup function
    lines.append("deform_template_fn")
    lines.append("jit_deform_find_template(uint16 sig)")
    lines.append("{")
    lines.append("\tint\tlo = 0;")
    lines.append("\tint\thi = jit_deform_templates_count - 1;")
    lines.append("")
    lines.append("\twhile (lo <= hi)")
    lines.append("\t{")
    lines.append("\t\tint\t\tmid = (lo + hi) / 2;")
    lines.append("\t\tuint16\tmid_sig = jit_deform_templates[mid].signature;")
    lines.append("")
    lines.append("\t\tif (mid_sig == sig)")
    lines.append("\t\t\treturn jit_deform_templates[mid].fn;")
    lines.append("\t\telse if (mid_sig < sig)")
    lines.append("\t\t\tlo = mid + 1;")
    lines.append("\t\telse")
    lines.append("\t\t\thi = mid - 1;")
    lines.append("\t}")
    lines.append("")
    lines.append("\treturn NULL;")
    lines.append("}")
    lines.append("")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Generate pre-compiled deform template functions")
    parser.add_argument("--output-c", required=True,
                        help="Output path for .c file")
    parser.add_argument("--output-h",
                        help="Output path for .h file (optional, for verification)")
    parser.add_argument("--pg-includedir",
                        help="PG server include directory (unused, for CMake compat)")
    args = parser.parse_args()

    patterns = generate_patterns()
    print(f"Generated {len(patterns)} deform template patterns")

    # Verify no duplicate signatures
    sigs = [signature(p.attlens) for p in patterns]
    assert len(sigs) == len(set(sigs)), "Duplicate signatures detected!"

    c_content = generate_c_file(patterns)
    with open(args.output_c, "w") as f:
        f.write(c_content)
    print(f"Wrote {args.output_c}")

    # Print summary
    by_natts = {}
    for p in patterns:
        n = len(p.attlens)
        by_natts.setdefault(n, []).append(p)
    for n in sorted(by_natts):
        print(f"  natts={n}: {len(by_natts[n])} patterns")


if __name__ == "__main__":
    main()
