#!/usr/bin/env python3
"""
extract_inlines.py — Extract pre-compiled inline blobs from pg_jit_funcs.o

Parses the compiled object file via llvm-objdump -d -r and for each jit_*
function:
  1. Extracts the instruction bytes
  2. Identifies the ret instruction (ARM64: 0xd65f03c0, x86_64: 0xc3)
  3. Records relocations (offset, type, symbol name)
  4. Outputs a C header with PrecompiledInline structs

Usage:
    python3 extract_inlines.py \
        --objdump /path/to/llvm-objdump \
        --input pg_jit_funcs_precompiled.o \
        --output pg_jit_precompiled.h \
        --arch aarch64
"""

import argparse
import re
import struct
import subprocess
import sys
from dataclasses import dataclass, field


@dataclass
class Relocation:
    offset: int        # byte offset within function (relative)
    type: str          # relocation type string
    symbol: str        # target symbol name
    addend: int = 0


@dataclass
class Function:
    name: str
    offset: int        # absolute offset within section
    code: bytearray = field(default_factory=bytearray)
    relocs: list = field(default_factory=list)
    ret_offset: int = -1  # relative offset of first ret instruction


def run_objdump(objdump_path, input_path):
    """Run llvm-objdump -d -r and capture output."""
    result = subprocess.run(
        [objdump_path, '-d', '-r', input_path],
        capture_output=True, text=True, check=True
    )
    return result.stdout


def parse_objdump_output(text, arch):
    """Parse llvm-objdump output into Function objects.

    llvm-objdump ARM64 format:
        00000000000000d8 <_jit_int4pl>:
              d8: 2b010000     \tadds\tw0, w0, w1
        \t\t00000000000000ec:  ARM64_RELOC_BRANCH26\t_jit_error_int4_overflow

    llvm-objdump x86_64 format:
              0: 01 d8         \taddl\t%ebx, %eax
    """
    functions = {}
    current_fn = None

    # Pattern for function label: "00000000000000d8 <_jit_int4pl>:"
    fn_pattern = re.compile(r'^([0-9a-f]+)\s+<([^>]+)>:\s*$')

    # ARM64 instruction: "      d8: 2b010000     \t..."
    # The hex is a single 8-char word (big-endian encoding of 4-byte instr)
    arm64_instr = re.compile(r'^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s')

    # x86_64 instruction: "       0: 01 d8   ..."
    # Space-separated bytes
    x86_instr = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s)+)')

    # Relocation line: "\t\t00000000000000ec:  ARM64_RELOC_BRANCH26\t_jit_..."
    reloc_pattern = re.compile(
        r'^\s+([0-9a-f]+):\s+'
        r'(\S+)\s+'
        r'(\S+)'
        r'(?:\+0x([0-9a-f]+))?'
    )

    for line in text.split('\n'):
        # Check for function header
        m = fn_pattern.match(line)
        if m:
            abs_offset = int(m.group(1), 16)
            name = m.group(2)
            # Strip leading underscore (Mach-O convention)
            if name.startswith('_'):
                name = name[1:]
            # Only extract jit_* functions
            if name.startswith('jit_'):
                current_fn = Function(name=name, offset=abs_offset)
                functions[name] = current_fn
            else:
                current_fn = None
            continue

        if current_fn is None:
            continue

        # Try ARM64 instruction pattern
        if arch == 'aarch64':
            m = arm64_instr.match(line)
            if m:
                abs_off = int(m.group(1), 16)
                rel_off = abs_off - current_fn.offset
                hex_word = m.group(2)
                # llvm-objdump prints ARM64 as big-endian hex word
                # but ARM64 is little-endian, so the bytes in memory
                # are reversed. The hex word IS the instruction encoding.
                # e.g., "2b010000" means bytes 0x00, 0x00, 0x01, 0x2b
                # Actually no — llvm-objdump prints the instruction word
                # in the target byte order. ARM64 is little-endian, so
                # "2b010000" is stored as bytes 00 00 01 2b.
                word = int(hex_word, 16)
                byte_vals = struct.pack('<I', word)  # little-endian 32-bit

                # Extend code buffer to reach this offset
                while len(current_fn.code) < rel_off:
                    current_fn.code.append(0)
                current_fn.code.extend(byte_vals)

                # Detect ret instruction (0xd65f03c0)
                if word == 0xd65f03c0 and current_fn.ret_offset < 0:
                    current_fn.ret_offset = rel_off
                continue

        # Try x86_64 instruction pattern
        if arch == 'x86_64':
            m = x86_instr.match(line)
            if m:
                abs_off = int(m.group(1), 16)
                rel_off = abs_off - current_fn.offset
                hex_bytes = m.group(2).strip()
                byte_vals = bytes.fromhex(hex_bytes.replace(' ', ''))

                while len(current_fn.code) < rel_off:
                    current_fn.code.append(0)
                current_fn.code.extend(byte_vals)

                # Detect ret (0xc3)
                if byte_vals == b'\xc3' and current_fn.ret_offset < 0:
                    current_fn.ret_offset = rel_off
                continue

        # Check for relocation
        m = reloc_pattern.match(line)
        if m:
            abs_off = int(m.group(1), 16)
            rtype = m.group(2)
            symbol = m.group(3)
            addend = int(m.group(4), 16) if m.group(4) else 0
            # Strip leading underscore
            if symbol.startswith('_'):
                symbol = symbol[1:]
            # Make offset relative to function start
            rel_offset = abs_off - current_fn.offset
            current_fn.relocs.append(Relocation(
                offset=rel_offset, type=rtype, symbol=symbol, addend=addend
            ))

    return functions


def classify_reloc_type(rtype, arch):
    """Map relocation type string to our enum constants."""
    if arch == 'aarch64':
        if 'BRANCH26' in rtype:
            return 'RELOC_BRANCH26'
        elif 'PAGE21' in rtype or 'ADRP' in rtype:
            return 'RELOC_PAGE21'
        elif 'PAGEOFF12' in rtype:
            return 'RELOC_PAGEOFF12'
        elif 'GOT' in rtype:
            return 'RELOC_GOT'
        return 'RELOC_UNKNOWN'
    elif arch == 'x86_64':
        if 'PC32' in rtype or 'PLT32' in rtype:
            return 'RELOC_PC32'
        elif 'GOTPCREL' in rtype:
            return 'RELOC_GOTPCREL'
        return 'RELOC_UNKNOWN'
    return 'RELOC_UNKNOWN'


def generate_header(functions, arch):
    """Generate the C header content."""
    lines = []
    lines.append('/* Auto-generated by extract_inlines.py — do not edit */')
    lines.append('#ifndef PG_JIT_PRECOMPILED_H')
    lines.append('#define PG_JIT_PRECOMPILED_H')
    lines.append('')
    lines.append('#include <stdint.h>')
    lines.append('')
    lines.append('/* Relocation types */')
    lines.append('#define RELOC_BRANCH26   1  /* ARM64: BL/B ±128MB */')
    lines.append('#define RELOC_PAGE21     2  /* ARM64: ADRP */')
    lines.append('#define RELOC_PAGEOFF12  3  /* ARM64: ADD/LDR page offset */')
    lines.append('#define RELOC_PC32       4  /* x86_64: RIP-relative 32-bit */')
    lines.append('#define RELOC_GOTPCREL   5  /* x86_64: GOT-relative */')
    lines.append('#define RELOC_GOT        6  /* ARM64: GOT */')
    lines.append('#define RELOC_MOVZ_MOVK64 7 /* ARM64: MOVZ+3xMOVK 64-bit imm */')
    lines.append('#define RELOC_ABS64      8  /* 8-byte absolute address */')
    lines.append('#define RELOC_UNKNOWN    0')
    lines.append('')
    lines.append('typedef struct PrecompiledReloc {')
    lines.append('    uint16_t    offset;     /* offset within code */')
    lines.append('    uint8_t     type;       /* RELOC_* constant */')
    lines.append('    const char *symbol;     /* symbol name for runtime resolution */')
    lines.append('} PrecompiledReloc;')
    lines.append('')
    lines.append('typedef struct PrecompiledInline {')
    lines.append('    const uint8_t  *code;       /* instruction bytes */')
    lines.append('    uint16_t        code_len;   /* total length */')
    lines.append('    int16_t         ret_offset; /* offset of ret instruction (-1 if none) */')
    lines.append('    uint8_t         n_relocs;   /* number of relocations */')
    lines.append('    PrecompiledReloc relocs[8];  /* relocations */')
    lines.append('} PrecompiledInline;')
    lines.append('')

    # Sort functions and filter to those with code
    sorted_fns = [fn for fn in sorted(functions.values(), key=lambda f: f.name)
                  if fn.code]

    # Emit code byte arrays
    for fn in sorted_fns:
        lines.append(f'static const uint8_t code_{fn.name}[] = {{')
        for i in range(0, len(fn.code), 16):
            chunk = fn.code[i:i+16]
            hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
            lines.append(f'    {hex_str},')
        lines.append('};')
        lines.append('')

    # Emit PrecompiledInline structs
    for fn in sorted_fns:
        # Filter to only relocations we can handle (max 8)
        useful_relocs = []
        for r in fn.relocs:
            rclass = classify_reloc_type(r.type, arch)
            if rclass != 'RELOC_UNKNOWN' and len(useful_relocs) < 8:
                useful_relocs.append((r, rclass))

        lines.append(f'static const PrecompiledInline precompiled_{fn.name} = {{')
        lines.append(f'    .code = code_{fn.name},')
        lines.append(f'    .code_len = {len(fn.code)},')
        lines.append(f'    .ret_offset = {fn.ret_offset},')
        lines.append(f'    .n_relocs = {len(useful_relocs)},')
        if useful_relocs:
            lines.append('    .relocs = {')
            for r, rclass in useful_relocs:
                lines.append(f'        {{ .offset = {r.offset},'
                             f' .type = {rclass},'
                             f' .symbol = "{r.symbol}" }},')
            lines.append('    },')
        else:
            lines.append('    .relocs = {{0}},')
        lines.append('};')
        lines.append('')

    # Emit lookup table mapping function names to PrecompiledInline pointers
    lines.append('typedef struct PrecompiledEntry {')
    lines.append('    const char            *name;')
    lines.append('    const PrecompiledInline *blob;')
    lines.append('} PrecompiledEntry;')
    lines.append('')
    lines.append('static const PrecompiledEntry precompiled_inlines[] = {')
    for fn in sorted_fns:
        lines.append(f'    {{ "{fn.name}", &precompiled_{fn.name} }},')
    lines.append('};')
    lines.append('')
    lines.append(f'static const int precompiled_inlines_count = {len(sorted_fns)};')
    lines.append('')
    lines.append('#endif /* PG_JIT_PRECOMPILED_H */')
    lines.append('')

    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Extract pre-compiled inline blobs from pg_jit_funcs.o')
    parser.add_argument('--objdump', required=True,
                        help='Path to llvm-objdump')
    parser.add_argument('--input', required=True,
                        help='Input object file')
    parser.add_argument('--output', required=True,
                        help='Output C header file')
    parser.add_argument('--arch', required=True,
                        choices=['aarch64', 'x86_64'],
                        help='Target architecture')
    args = parser.parse_args()

    # Run objdump
    try:
        text = run_objdump(args.objdump, args.input)
    except subprocess.CalledProcessError as e:
        print(f'Error running llvm-objdump: {e.stderr}', file=sys.stderr)
        sys.exit(1)

    # Parse output
    functions = parse_objdump_output(text, args.arch)

    if not functions:
        print('Warning: no jit_* functions found in objdump output',
              file=sys.stderr)

    # Filter to functions with actual code
    with_code = {k: v for k, v in functions.items() if v.code}
    without_code = {k: v for k, v in functions.items() if not v.code}

    # Generate header
    header = generate_header(functions, args.arch)

    # Write output
    with open(args.output, 'w') as f:
        f.write(header)

    print(f'Extracted {len(with_code)} functions '
          f'({len(without_code)} skipped, no code) to {args.output}')
    for fn in sorted(with_code.values(), key=lambda f: f.name):
        reloc_str = f', {len(fn.relocs)} relocs' if fn.relocs else ''
        ret_str = f', ret@{fn.ret_offset}' if fn.ret_offset >= 0 else ', no ret'
        print(f'  {fn.name}: {len(fn.code)} bytes{ret_str}{reloc_str}')


if __name__ == '__main__':
    main()
